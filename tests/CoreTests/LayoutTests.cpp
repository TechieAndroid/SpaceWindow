// Tests for the treemap layout, on folder trees built in memory.
#include "pch.h"
#include "TestHarness.h"

#include "TreeBuilder.h"
#include "../../src/Core/Settings.h"
#include "../../src/Core/TreemapLayout.h"

namespace
{
    using test::AddFile;
    using test::AddFolder;
    using test::AddSpecial;
    using test::Finish;

    long long Area(sm::DisplayItem const& item)
    {
        return static_cast<long long>(item.w) * item.h;
    }

    bool InteriorsOverlap(sm::DisplayItem const& a, sm::DisplayItem const& b)
    {
        // Neighbours share their outline pixels, so only the interiors must be disjoint.
        return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
    }

    void CheckInvariants(sm::TreemapLayout const& layout, int width, int height)
    {
        auto const items = layout.Items();
        sm::LayoutOptions const& options = layout.Options();
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            sm::DisplayItem const& item = items[i];
            CHECK(item.w >= 0 && item.h >= 0);
            CHECK(item.x >= 0 && item.y >= 0);
            CHECK(item.x + item.w <= width - 1 && item.y + item.h <= height - 1);
            CHECK(item.subtreeEnd > i && item.subtreeEnd <= items.size());
            if (!item.IsFolder())
            {
                CHECK_EQ(item.subtreeEnd, static_cast<std::uint32_t>(i + 1));
                continue;
            }
            for (std::size_t j = i + 1; j < item.subtreeEnd; ++j)
            {
                // Contents sit inside the folder's border and below its title band.
                CHECK(items[j].x >= item.x + options.border);
                CHECK(items[j].y >= item.y + options.header);
                CHECK(items[j].x + items[j].w <= item.x + item.w - options.border);
                CHECK(items[j].y + items[j].h <= item.y + item.h - options.border);
                CHECK(items[j].depth > item.depth);
            }
        }

        // Leaves (files and filler boxes) never overlap.
        std::vector<sm::DisplayItem const*> leaves;
        for (sm::DisplayItem const& item : items)
        {
            if (!item.IsFolder())
            {
                leaves.push_back(&item);
            }
        }
        for (std::size_t i = 0; i < leaves.size(); ++i)
        {
            for (std::size_t j = i + 1; j < leaves.size(); ++j)
            {
                CHECK(!InteriorsOverlap(*leaves[i], *leaves[j]));
            }
        }
    }

    sm::LayoutOptions Options()
    {
        return sm::LayoutOptions::ForDensity(0, 1.0);
    }
}

TEST_CASE(LayoutSplitsInProportion)
{
    // Worked example of 1.4.0's algorithm: 50/30/20 in a 1000x1000 area deals
    // 50 into the first list and 30+20 into the second, cuts the square in
    // half horizontally, then cuts the lower half 600:400 vertically.
    sm::Folder root;
    AddFile(root, L"fifty", 50);
    AddFile(root, L"thirty", 30);
    AddFile(root, L"twenty", 20);
    Finish(root);

    sm::TreemapLayout layout;
    layout.Build(&root, 0, 1001, 1001, Options());
    auto const items = layout.Items();
    CHECK_EQ(items.size(), std::size_t{ 3 });
    CHECK_EQ(root.Name(items[0].index), std::wstring_view(L"fifty"));
    CHECK(items[0].x == 0 && items[0].y == 0 && items[0].w == 1000 && items[0].h == 500);
    CHECK(items[1].x == 0 && items[1].y == 500 && items[1].w == 600 && items[1].h == 500);
    CHECK(items[2].x == 600 && items[2].y == 500 && items[2].w == 400 && items[2].h == 500);
    CHECK_EQ(Area(items[0]) + Area(items[1]) + Area(items[2]), 1000LL * 1000);
    CheckInvariants(layout, 1001, 1001);
}

TEST_CASE(LayoutHitTestingFollowsTheOriginalRules)
{
    sm::Folder root;
    sm::Folder& folder = AddFolder(root, L"folder");
    AddFile(folder, L"inner-a", 600);
    AddFile(folder, L"inner-b", 200);
    AddFile(root, L"file", 200);
    AddSpecial(root, sm::EntryKind::FreeSpace, 1000);
    Finish(root);

    sm::TreemapLayout layout;
    sm::LayoutOptions const options = Options();
    layout.Build(&root, 0, 801, 601, options);
    CheckInvariants(layout, 801, 601);
    auto const items = layout.Items();

    int folderItem = -1;
    int fileItem = -1;
    int freeItem = -1;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        if (items[i].IsSpecial())
        {
            freeItem = static_cast<int>(i);
            CHECK_EQ(items[i].depth, -1);
        }
        else if (items[i].IsFolder())
        {
            folderItem = static_cast<int>(i);
        }
        else if (items[i].source == &root)
        {
            fileItem = static_cast<int>(i);
        }
    }
    CHECK(folderItem >= 0 && fileItem >= 0 && freeItem >= 0);

    auto const centre = [&items](int i) {
        sm::DisplayItem const& item = items[static_cast<std::size_t>(i)];
        return std::pair{ item.x + item.w / 2, item.y + item.h / 2 };
    };

    auto const [fx, fy] = centre(fileItem);
    CHECK_EQ(layout.HitTest(fx, fy), fileItem);

    // The free-space block cannot be selected.
    auto const [sx, sy] = centre(freeItem);
    CHECK_EQ(layout.HitTest(sx, sy), -1);

    // A folder is selected through its title band; its middle belongs to its contents.
    sm::DisplayItem const& box = items[static_cast<std::size_t>(folderItem)];
    CHECK_EQ(layout.HitTest(box.x + box.w / 2, box.y + options.header / 2), folderItem);
    int const inner = layout.HitTest(box.x + box.w / 2, box.y + box.h / 2);
    CHECK(inner > folderItem);
    CHECK(items[static_cast<std::size_t>(inner)].source == &folder);

    // The outline shared by two boxes belongs to neither.
    CHECK_EQ(layout.HitTest(box.x, box.y + box.h / 2), -1);

    // The rollover chain runs from the folder down to the file under the point.
    std::vector<int> chain;
    layout.ContainersAt(box.x + box.w / 2, box.y + box.h / 2, chain);
    CHECK_EQ(chain.size(), std::size_t{ 2 });
    CHECK_EQ(chain[0], folderItem);
    CHECK_EQ(chain[1], inner);

    CHECK_EQ(layout.FindFolder(&folder), folderItem);
    CHECK_EQ(layout.Find(&root, items[static_cast<std::size_t>(fileItem)].index), fileItem);
}

TEST_CASE(LayoutHidesFreeSpaceOnRequest)
{
    sm::Folder root;
    AddFile(root, L"a", 300);
    AddFile(root, L"b", 100);
    AddSpecial(root, sm::EntryKind::FreeSpace, 600);
    AddSpecial(root, sm::EntryKind::Unscanned, 50);
    Finish(root);

    sm::LayoutOptions options = Options();
    options.showFreeSpace = false;
    sm::TreemapLayout layout;
    layout.Build(&root, 0, 641, 481, options);
    CheckInvariants(layout, 641, 481);
    long long area = 0;
    for (sm::DisplayItem const& item : layout.Items())
    {
        CHECK(!item.IsSpecial());
        area += Area(item);
    }
    CHECK_EQ(area, 640LL * 480);  // the files take all the room
}

TEST_CASE(LayoutSkipsEmptyFilesAndLumpsTinyOnes)
{
    sm::Folder root;
    AddFile(root, L"zero-1", 0);
    AddFile(root, L"zero-2", 0);
    for (int i = 0; i < 2000; ++i)
    {
        AddFile(root, L"tiny", 1);
    }
    Finish(root);

    sm::TreemapLayout layout;
    sm::LayoutOptions const options = Options();
    layout.Build(&root, 0, 201, 151, options);
    CheckInvariants(layout, 201, 151);

    std::size_t fillers = 0;
    for (sm::DisplayItem const& item : layout.Items())
    {
        if (!item.HasEntry())
        {
            ++fillers;
            continue;
        }
        CHECK(root.entries[item.index].size != 0);
        CHECK(item.w > options.minWidth && item.h > options.minHeight);  // only roomy boxes are labelled
    }
    CHECK(fillers > 0);
}

TEST_CASE(LayoutHandlesHugeSizesAndCoordinates)
{
    // 2^60 bytes times the width overflowed 1.4.0's 64-bit split arithmetic,
    // and its 16-bit coordinates could not describe a 40,000 pixel area.
    sm::Folder root;
    AddFile(root, L"big", std::uint64_t{ 1 } << 60);
    AddFile(root, L"half", std::uint64_t{ 1 } << 59);
    Finish(root);

    sm::TreemapLayout layout;
    layout.Build(&root, 0, 40001, 301, Options());
    CheckInvariants(layout, 40001, 301);
    auto const items = layout.Items();
    CHECK_EQ(items.size(), std::size_t{ 2 });
    CHECK_EQ(items[0].w, 26666);  // two thirds of 40,000
    CHECK_EQ(items[1].x, 26666);
    CHECK_EQ(items[1].x + items[1].w, 40000);
}

TEST_CASE(LayoutBiasChangesTheCut)
{
    sm::Folder root;
    AddFile(root, L"a", 1);
    AddFile(root, L"b", 1);
    Finish(root);

    sm::LayoutOptions options = Options();
    sm::TreemapLayout layout;

    options.bias = sm::Settings::kMinBias;  // favours horizontal strips
    layout.Build(&root, 0, 1001, 401, options);
    CHECK(layout.Items()[0].w == 1000 && layout.Items()[0].h == 200);

    options.bias = sm::Settings::kMaxBias;  // favours vertical strips
    layout.Build(&root, 0, 401, 1001, options);
    CHECK(layout.Items()[0].w == 200 && layout.Items()[0].h == 1000);
}

TEST_CASE(LayoutOfANestedTreeKeepsItsInvariants)
{
    // A pseudo-random tree, several levels deep, at a few sizes and densities.
    sm::Folder root;
    std::uint32_t seed = 12345;
    auto const next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    std::vector<sm::Folder*> folders{ &root };
    for (int i = 0; i < 400; ++i)
    {
        sm::Folder& parent = *folders[next() % folders.size()];
        if (next() % 4 == 0)
        {
            folders.push_back(&AddFolder(parent, L"dir"));
        }
        else
        {
            AddFile(parent, L"file", 1 + next() % 1'000'000);
        }
    }
    Finish(root);

    for (int density = sm::Settings::kMinDensity; density <= sm::Settings::kMaxDensity; ++density)
    {
        for (auto const [width, height] : { std::pair{ 1921, 1081 }, std::pair{ 3841, 2161 }, std::pair{ 50, 40 } })
        {
            sm::TreemapLayout layout;
            layout.Build(&root, 0, width, height, sm::LayoutOptions::ForDensity(density, 1.5));
            CheckInvariants(layout, width, height);
        }
    }
}

TEST_CASE(LayoutOfAnEmptyFolderIsEmpty)
{
    sm::Folder root;
    sm::TreemapLayout layout;
    layout.Build(&root, 0, 800, 600, Options());
    CHECK(layout.Items().empty());
    layout.Build(nullptr, 0, 800, 600, Options());
    CHECK(layout.Items().empty());
    CHECK_EQ(layout.HitTest(10, 10), -1);
}
