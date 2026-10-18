// Tests for the folder tree: totals, walks, and counting hard links once.
#include "pch.h"
#include "TestHarness.h"
#include "TreeBuilder.h"
#ifndef _WIN32
#include "../../src/Core/Posix.h"
#endif

namespace
{
    sm::Entry& Last(sm::Folder& folder)
    {
        return folder.entries.back();
    }

    sm::Entry* Named(sm::Folder& folder, std::wstring_view name)
    {
        for (std::size_t i = 0; i < folder.entries.size(); ++i)
        {
            if (folder.Name(i) == name)
            {
                return &folder.entries[i];
            }
        }
        return nullptr;
    }

    // root (record 5)
    //   X (10)             link.bin (file 100, 4096 bytes)
    //   Y (11)  Z (12)     link.bin (file 100)
    //   solo.bin (file 101, 8192 bytes)
    std::unique_ptr<sm::FolderTree> LinkedTree()
    {
        auto root = std::make_unique<sm::Folder>();
        sm::Folder& x = test::AddFolder(*root, L"X");
        Last(*root).record = 10;
        test::AddFile(x, L"link.bin", 4096);
        Last(x).record = 100;
        sm::Folder& y = test::AddFolder(*root, L"Y");
        Last(*root).record = 11;
        sm::Folder& z = test::AddFolder(y, L"Z");
        Last(y).record = 12;
        test::AddFile(z, L"link.bin", 4096);
        Last(z).record = 100;
        test::AddFile(*root, L"solo.bin", 8192);
        Last(*root).record = 101;
        test::Finish(*root);
        auto tree = std::make_unique<sm::FolderTree>(std::move(root), L"C:\\", sm::VolumeInfo{}, false);
        tree->rootRecord = 5;
        return tree;
    }

    // Every entry's size and whether it is counted elsewhere, in tree order.
    std::vector<std::pair<std::uint64_t, bool>> Counting(sm::Folder const& root)
    {
        std::vector<std::pair<std::uint64_t, bool>> counting;
        sm::ForEachFolder(root, [&counting](sm::Folder const& folder) {
            for (sm::Entry const& entry : folder.entries)
            {
                counting.emplace_back(entry.size, entry.countedElsewhere);
            }
        });
        return counting;
    }
}

TEST_CASE(UpdateTotalsUpwardFixesEveryFolderAbove)
{
    auto root = std::make_unique<sm::Folder>();
    sm::Folder& a = test::AddFolder(*root, L"A");
    sm::Folder& b = test::AddFolder(a, L"B");
    test::AddFile(b, L"f1", 100);
    test::AddFile(a, L"f2", 10);
    sm::Folder& c = test::AddFolder(*root, L"C");
    test::AddFile(c, L"f3", 500);
    test::Finish(*root);
    CHECK(root->Name(0) == L"C");  // largest first: C (500) before A (110)

    // B's file grows: B, A and the root take it in, and are sorted again.
    Named(b, L"f1")->size = 1000;
    std::array<sm::Folder*, 2> const changed{ &b, &b };  // twice: no matter
    sm::UpdateTotalsUpward(*root, changed);
    CHECK_EQ(b.SizeTotal(), std::uint64_t{ 1000 });
    CHECK_EQ(a.SizeTotal(), std::uint64_t{ 1010 });
    CHECK_EQ(root->SizeTotal(), std::uint64_t{ 1510 });
    CHECK(root->Name(0) == L"A" && root->entries[0].size == 1010);
    CHECK_EQ(a.indexInParent, std::uint32_t{ 0 });
    CHECK_EQ(c.indexInParent, std::uint32_t{ 1 });
    CHECK(a.Name(0) == L"B");

    // A folder no longer in the tree is left alone, and so is the tree.
    sm::Folder loose;
    test::AddFile(loose, L"g", 7);
    std::array<sm::Folder*, 1> const elsewhere{ &loose };
    sm::UpdateTotalsUpward(*root, elsewhere);
    CHECK_EQ(loose.SizeTotal(), std::uint64_t{ 0 });
    CHECK_EQ(root->SizeTotal(), std::uint64_t{ 1510 });
}

TEST_CASE(TreesAreWalkedFoldersFirst)
{
    std::unique_ptr<sm::FolderTree> const tree = LinkedTree();
    sm::Folder& root = *tree->Root();
    sm::Folder& y = *Named(root, L"Y")->child;
    sm::Folder const& z = *Named(y, L"Z")->child;

    std::vector<sm::Folder const*> seen;
    sm::ForEachFolder(std::as_const(root), [&seen](sm::Folder const& folder) { seen.push_back(&folder); });
    CHECK_EQ(seen.size(), std::size_t{ 4 });
    CHECK(seen.front() == &root);
    auto const position = [&seen](sm::Folder const* folder) { return std::ranges::find(seen, folder) - seen.begin(); };
    CHECK(position(&y) < position(&z));  // a folder before what it holds

    // Returning false passes over what a folder holds.
    std::size_t visited = 0;
    sm::ForEachFolder(root, [&visited, &y](sm::Folder& folder) {
        ++visited;
        return &folder != &y;
    });
    CHECK_EQ(visited, std::size_t{ 3 });  // not Z

    CHECK(z.LevelsBelow(&root) == std::optional<std::size_t>(2));
    CHECK(z.LevelsBelow(&y) == std::optional<std::size_t>(1));
    CHECK(z.LevelsBelow(&z) == std::optional<std::size_t>(0));
    CHECK(!y.LevelsBelow(&z).has_value());
    CHECK(z.IsWithin(&root) && z.IsWithin(&z) && !root.IsWithin(&z));

    CHECK(sm::FileRecords(root) == (std::vector<std::uint32_t>{ 100, 101 }));
    CHECK(sm::FileRecords(y) == (std::vector<std::uint32_t>{ 100 }));
}

TEST_CASE(RecountingSomeHardLinksMatchesRecountingAll)
{
    // As scanned: counted under X\link.bin, nearer the root than Y\Z\link.bin.
    std::unique_ptr<sm::FolderTree> const some = LinkedTree();
    std::unique_ptr<sm::FolderTree> const all = LinkedTree();
    sm::CountHardLinksOnce(*some);
    sm::CountHardLinksOnce(*all);
    CHECK(some->hardLinks);
    sm::Folder& x = *Named(*some->Root(), L"X")->child;
    CHECK(Named(x, L"link.bin")->size == 4096 && !Named(x, L"link.bin")->countedElsewhere);
    CHECK_EQ(some->Root()->SizeTotal(), std::uint64_t{ 4096 + 8192 });

    // A third name, in the root: the file's space moves to it.
    for (sm::FolderTree const* const tree : { some.get(), all.get() })
    {
        test::AddFile(*tree->Root(), L"top.bin", 4096);
        Last(*tree->Root()).record = 100;
        test::Finish(*tree->Root());
    }
    std::array<std::uint32_t, 1> const file{ 100 };
    sm::CountHardLinksOnce(*some, file);
    sm::CountHardLinksOnce(*all);
    CHECK(Counting(*some->Root()) == Counting(*all->Root()));
    CHECK(Named(*some->Root(), L"top.bin")->size == 4096 && Named(x, L"link.bin")->countedElsewhere);
    CHECK_EQ(some->Root()->SizeTotal(), std::uint64_t{ 4096 + 8192 });

    // It goes again: the space comes back to X\link.bin, as read from the file.
    std::size_t asked = 0;
    auto const lookup = [&asked](sm::Folder const&, std::size_t) {
        ++asked;
        return std::optional<std::uint64_t>(4096);
    };
    for (sm::FolderTree* const tree : { some.get(), all.get() })
    {
        sm::Folder& root = *tree->Root();
        tree->Remove(root, static_cast<std::size_t>(Named(root, L"top.bin") - root.entries.data()));
    }
    sm::CountHardLinksOnce(*some, file, lookup);
    sm::CountHardLinksOnce(*all, lookup);
    CHECK(Counting(*some->Root()) == Counting(*all->Root()));
    CHECK(Named(x, L"link.bin")->size == 4096 && !Named(x, L"link.bin")->countedElsewhere);
    CHECK_EQ(some->Root()->SizeTotal(), std::uint64_t{ 4096 + 8192 });
    CHECK_EQ(asked, std::size_t{ 2 });  // once for each tree
}

#ifndef _WIN32
TEST_CASE(HardLinksAtTheSameDepthGoToTheFolderWithTheLowerInode)
{
    // A reload numbers the files new to the tree in the order it meets them,
    // not in that of their inodes, as a scan does: here folder Q got its
    // number first.  The file is still counted where a fresh scan counts it,
    // in P, the folder with the lower inode.
    auto inodes = std::make_shared<sm::InodeTable>();
    std::uint32_t const device = inodes->Device(42);
    std::uint32_t const rootRecord = inodes->Intern(device, 10);
    std::uint32_t const q = inodes->Intern(device, 300);
    std::uint32_t const p = inodes->Intern(device, 200);
    std::uint32_t const file = inodes->Intern(device, 400);
    CHECK(q < p);

    auto root = std::make_unique<sm::Folder>();
    for (auto const& [name, record] : { std::pair{ L"P", p }, std::pair{ L"Q", q } })
    {
        sm::Folder& folder = test::AddFolder(*root, name);
        Last(*root).record = record;
        test::AddFile(folder, L"link.bin", 4096);
        Last(folder).record = file;
    }
    test::Finish(*root);
    auto tree = std::make_unique<sm::FolderTree>(std::move(root), L"/data/", sm::VolumeInfo{}, false);
    tree->rootRecord = rootRecord;
    tree->inodes = inodes;
    sm::CountHardLinksOnce(*tree);

    sm::Entry const* const inP = Named(*Named(*tree->Root(), L"P")->child, L"link.bin");
    sm::Entry const* const inQ = Named(*Named(*tree->Root(), L"Q")->child, L"link.bin");
    CHECK(!inP->countedElsewhere && inP->size == 4096);
    CHECK(inQ->countedElsewhere && inQ->size == 0);
}
#endif
