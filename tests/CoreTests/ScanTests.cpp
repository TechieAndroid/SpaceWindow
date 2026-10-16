// Tests for the scanner, against a real folder tree built in %TEMP%.
#include "pch.h"
#include "TestHarness.h"

#include "../../src/Core/FolderTree.h"
#include "../../src/Core/FolderWatch.h"
#include "../../src/Core/IoCancellation.h"

namespace
{
    constexpr int kDeepLevels = 150;  // deep enough for a path far beyond MAX_PATH

    std::wstring Extended(std::wstring const& path)
    {
        return sm::ToExtendedLengthPath(path);
    }

    void WriteFileOfSize(std::wstring const& path, std::size_t size)
    {
        wil::unique_hfile const file(CreateFileW(Extended(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                                 FILE_ATTRIBUTE_NORMAL, nullptr));
        CHECK(static_cast<bool>(file));
        std::vector<char> const data(size, 'x');
        DWORD written = 0;
        CHECK(WriteFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &written, nullptr));
        CHECK_EQ(static_cast<std::size_t>(written), size);
    }

    void MakeDirectory(std::wstring const& path)
    {
        CHECK(CreateDirectoryW(Extended(path).c_str(), nullptr));
    }

    // Creates a directory junction (no privilege needed, unlike symbolic links).
    void MakeJunction(std::wstring const& link, std::wstring const& target)
    {
        MakeDirectory(link);
        wil::unique_hfile const handle(CreateFileW(Extended(link).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                                   FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        CHECK(static_cast<bool>(handle));

        // REPARSE_DATA_BUFFER.MountPointReparseBuffer, which only the DDK declares.
        std::wstring const substitute = L"\\??\\" + target;
        std::wstring const& print = target;
        std::size_t const pathBytes = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
        std::vector<BYTE> buffer(8 + 8 + pathBytes);
        auto const put16 = [&buffer](std::size_t offset, std::size_t value) {
            auto const v = static_cast<WORD>(value);
            std::memcpy(buffer.data() + offset, &v, sizeof(v));
        };
        DWORD const tag = IO_REPARSE_TAG_MOUNT_POINT;
        std::memcpy(buffer.data(), &tag, sizeof(tag));
        put16(4, 8 + pathBytes);                               // ReparseDataLength
        put16(8, 0);                                           // SubstituteNameOffset
        put16(10, substitute.size() * sizeof(wchar_t));        // SubstituteNameLength
        put16(12, (substitute.size() + 1) * sizeof(wchar_t));  // PrintNameOffset
        put16(14, print.size() * sizeof(wchar_t));             // PrintNameLength
        std::memcpy(buffer.data() + 16, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
        std::memcpy(buffer.data() + 16 + (substitute.size() + 1) * sizeof(wchar_t), print.c_str(),
                    (print.size() + 1) * sizeof(wchar_t));

        DWORD returned = 0;
        CHECK(DeviceIoControl(handle.get(), FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
                              nullptr, 0, &returned, nullptr));
    }

    // Deletes a tree without following junctions.  Iterative, like the scanner,
    // because the test tree is deeper than a recursive helper should go.
    void DeleteTree(std::wstring const& root)
    {
        std::vector<std::wstring> folders{ root };
        std::vector<std::wstring> visitOrder;
        while (!folders.empty())
        {
            std::wstring folder = std::move(folders.back());
            folders.pop_back();
            WIN32_FIND_DATAW data{};
            wil::unique_hfind const find(FindFirstFileW(Extended(folder + L"*").c_str(), &data));
            if (find)
            {
                do
                {
                    std::wstring_view const name(data.cFileName);
                    if (name == L"." || name == L"..")
                    {
                        continue;
                    }
                    std::wstring const path = folder + std::wstring(name);
                    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    {
                        (void)RemoveDirectoryW(Extended(path).c_str());  // removes the junction, not its target
                    }
                    else if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    {
                        folders.push_back(path + L"\\");
                    }
                    else
                    {
                        (void)DeleteFileW(Extended(path).c_str());
                    }
                } while (FindNextFileW(find.get(), &data));
            }
            visitOrder.push_back(std::move(folder));
        }
        for (std::wstring const& folder : visitOrder | std::views::reverse)
        {
            (void)RemoveDirectoryW(Extended(folder).c_str());
        }
    }

    // A small tree with known contents:
    //   a.bin (10000)  b.bin (1 MiB)  empty.txt (0)
    //   sub1\c.bin (4096)  sub1\sub2\d.bin (300000)
    //   ファイル Ünïcödé\Ünïcödé.txt (5000)
    //   link -> sub1 (a junction: must not be followed or counted)
    //   deep\d\d\...\d\leaf.bin (2000), kDeepLevels levels of "d"
    class TestTree
    {
    public:
        TestTree()
        {
            wchar_t temp[MAX_PATH + 1]{};
            CHECK(GetTempPathW(static_cast<DWORD>(std::size(temp)), temp) != 0);
            GUID guid{};
            (void)CoCreateGuid(&guid);
            wchar_t id[40]{};
            (void)StringFromGUID2(guid, id, static_cast<int>(std::size(id)));
            std::wstring const root = std::wstring(temp) + L"SpaceWindowTest-" + id;
            MakeDirectory(root);
            root_ = sm::NormalizeScanPath(root);  // once it exists, as the scanner will see it

            WriteFileOfSize(root_ + L"a.bin", 10000);
            WriteFileOfSize(root_ + L"b.bin", 1 << 20);
            WriteFileOfSize(root_ + L"empty.txt", 0);
            MakeDirectory(root_ + L"sub1");
            WriteFileOfSize(root_ + L"sub1\\c.bin", 4096);
            MakeDirectory(root_ + L"sub1\\sub2");
            WriteFileOfSize(root_ + L"sub1\\sub2\\d.bin", 300000);
            MakeDirectory(root_ + L"ファイル Ünïcödé");
            WriteFileOfSize(root_ + L"ファイル Ünïcödé\\Ünïcödé.txt", 5000);
            MakeJunction(root_ + L"link", root_ + L"sub1");

            std::wstring deep = root_ + L"deep\\";
            MakeDirectory(deep);
            for (int i = 0; i < kDeepLevels; ++i)
            {
                deep += L"d\\";
                MakeDirectory(deep);
            }
            leaf_ = deep + L"leaf.bin";
            WriteFileOfSize(leaf_, 2000);
        }
        TestTree(TestTree const&) = delete;
        TestTree& operator=(TestTree const&) = delete;
        ~TestTree()
        {
            try
            {
                DeleteTree(root_);
            }
            catch (...)  // NOLINT(bugprone-empty-catch): best-effort cleanup; a destructor must not throw
            {
            }
        }

        std::wstring const& Root() const noexcept { return root_; }
        std::wstring const& Leaf() const noexcept { return leaf_; }

        static constexpr std::uint64_t kFiles = 7;
        static constexpr std::uint64_t kFolders = 4 + kDeepLevels;  // sub1, sub2, ファイル..., deep, d x N
        static constexpr std::uint64_t kLogicalBytes = 10000 + (1 << 20) + 0 + 4096 + 300000 + 5000 + 2000;

    private:
        std::wstring root_;
        std::wstring leaf_;
    };

    sm::ScanResult Scan(std::wstring const& path, unsigned threads = 0, sm::ScanProgress* progress = nullptr)
    {
        sm::ScanProgress local;
        sm::ScanOptions options;
        options.threads = threads;
        return sm::ScanFolder(path, progress != nullptr ? *progress : local, std::stop_token{}, options);
    }

    sm::Folder* FindChild(sm::Folder& folder, std::wstring_view name)
    {
        for (std::size_t i = 0; i < folder.entries.size(); ++i)
        {
            if (folder.Name(i) == name && folder.entries[i].child)
            {
                return folder.entries[i].child.get();
            }
        }
        return nullptr;
    }

    // Checks the invariants every finalized folder must satisfy and returns
    // the number of folders visited.
    std::uint64_t CheckConsistency(sm::Folder const& folder)
    {
        std::uint64_t visited = 1;
        std::uint64_t files = 0;
        std::uint64_t fileBytes = 0;
        std::uint64_t folderBytes = 0;
        for (std::size_t i = 0; i < folder.entries.size(); ++i)
        {
            sm::Entry const& entry = folder.entries[i];
            if (i > 0)
            {
                CHECK(folder.entries[i - 1].size >= entry.size);  // largest first
            }
            if (entry.child)
            {
                CHECK_EQ(entry.child->parent, &folder);
                CHECK_EQ(entry.child->indexInParent, static_cast<std::uint32_t>(i));  // 1.4.0 got this wrong
                CHECK_EQ(entry.size, entry.child->SizeTotal());
                folderBytes += entry.size;
                files += entry.child->fileCount;
                visited += CheckConsistency(*entry.child);
            }
            else if (entry.kind == sm::EntryKind::File)
            {
                ++files;
                fileBytes += entry.size;
            }
        }
        CHECK_EQ(folder.fileCount, files);
        CHECK_EQ(folder.sizeFiles, fileBytes);
        CHECK_EQ(folder.sizeSubfolders, folderBytes);
        return visited;
    }

    // Every entry as "path|size", for comparing two scans.
    void Flatten(sm::FolderTree const& tree, sm::Folder const& folder, std::vector<std::wstring>& out)
    {
        for (std::size_t i = 0; i < folder.entries.size(); ++i)
        {
            out.push_back(tree.PathOf(folder, i) + L"|" + std::to_wstring(folder.entries[i].size));
            if (folder.entries[i].child)
            {
                Flatten(tree, *folder.entries[i].child, out);
            }
        }
    }
}

TEST_CASE(ScanCountsEverythingAndSkipsJunctions)
{
    TestTree const tree;
    sm::ScanProgress progress;
    sm::ScanResult const result = Scan(tree.Root(), 0, &progress);
    CHECK(result.status == sm::ScanStatus::Completed);
    CHECK(result.tree != nullptr);

    sm::Folder& root = *result.tree->Root();
    CHECK_EQ(root.fileCount, TestTree::kFiles);
    CHECK_EQ(root.folderCount, TestTree::kFolders);
    CHECK_EQ(root.logicalTotal, TestTree::kLogicalBytes);
    CHECK_EQ(progress.files.load(), TestTree::kFiles);
    CHECK_EQ(progress.folders.load(), TestTree::kFolders);
    CHECK_EQ(progress.bytes.load(), root.SizeTotal());
    CHECK_EQ(CheckConsistency(root), TestTree::kFolders + 1);
    CHECK_EQ(result.tree->unreadableFolders, std::uint64_t{ 0 });
    CHECK(!result.tree->IsWholeVolume());

    // The junction would have added sub1's 304096 bytes a second time.
    CHECK(FindChild(root, L"link") == nullptr);

    // Files report what they occupy on disk, which is at least their length
    // once they are too big to live inside the MFT record.
    for (sm::Entry const& entry : root.entries)
    {
        if (entry.kind == sm::EntryKind::File && entry.logicalSize >= 4096)
        {
            CHECK(entry.size >= entry.logicalSize);
        }
    }
}

TEST_CASE(ScanCountsHardLinksOnce)
{
    TestTree const tree;
    sm::ScanResult const without = Scan(tree.Root());
    // A second name for sub1\c.bin (4096 bytes), in the root folder.
    if (!CreateHardLinkW(Extended(tree.Root() + L"c-link.bin").c_str(), Extended(tree.Root() + L"sub1\\c.bin").c_str(),
                         nullptr))
    {
        return;  // the temporary folder is not on NTFS
    }
    sm::ScanResult const with = Scan(tree.Root());
    sm::Folder& root = *with.tree->Root();
    CHECK_EQ(root.fileCount, without.tree->Root()->fileCount + 1);  // listed under both names...
    CHECK_EQ(root.SizeTotal(), without.tree->Root()->SizeTotal());  // ...counted once
    sm::Entry const* link = nullptr;
    for (std::size_t i = 0; i < root.entries.size(); ++i)
    {
        if (root.Name(i) == L"c-link.bin")
        {
            link = &root.entries[i];
        }
    }
    sm::Folder const* const sub1 = FindChild(root, L"sub1");
    CHECK(link != nullptr && sub1 != nullptr);
    sm::Entry const* original = nullptr;
    for (std::size_t i = 0; i < sub1->entries.size(); ++i)
    {
        if (sub1->Name(i) == L"c.bin")
        {
            original = &sub1->entries[i];
        }
    }
    CHECK(original != nullptr);
    // Counted under the name nearer the root.
    CHECK(!link->countedElsewhere && link->size == 4096);
    CHECK(original->countedElsewhere && original->size == 0);
    CHECK_EQ(CheckConsistency(root), TestTree::kFolders + 1);
}

namespace
{
    // Every entry of a tree as "path|size", sorted: two trees of the same folder compare equal.
    std::vector<std::wstring> Contents(sm::FolderTree const& tree)
    {
        std::vector<std::wstring> entries;
        Flatten(tree, *tree.Root(), entries);
        std::ranges::sort(entries);
        return entries;
    }

    void ExpectSameContents(sm::FolderTree const& tree, std::wstring const& root)
    {
        std::vector<std::wstring> const have = Contents(tree);
        std::vector<std::wstring> const fresh = Contents(*Scan(root).tree);
        if (have != fresh)
        {
            std::vector<std::wstring> onlyHave;
            std::vector<std::wstring> onlyFresh;
            std::ranges::set_difference(have, fresh, std::back_inserter(onlyHave));
            std::ranges::set_difference(fresh, have, std::back_inserter(onlyFresh));
            for (std::wstring const& entry : onlyHave | std::views::take(5))
            {
                std::printf("      only in the tree: %s\n", test::ToUtf8(entry).c_str());
            }
            for (std::wstring const& entry : onlyFresh | std::views::take(5))
            {
                std::printf("      only in a fresh scan: %s\n", test::ToUtf8(entry).c_str());
            }
        }
        CHECK(have == fresh);
    }
}

TEST_CASE(ReloadOfWatchedFoldersMatchesAFreshScan)
{
    TestTree const tree;
    sm::ScanProgress progress;
    sm::ScanOptions options;
    options.watch = true;
    sm::ScanResult const result = sm::ScanFolder(tree.Root(), progress, std::stop_token{}, options);
    CHECK(result.tree != nullptr && result.tree->watch != nullptr);
    sm::FolderTree& scanned = *result.tree;

    // Files made, rewritten and deleted; folders made, moved, renamed and deleted.
    WriteFileOfSize(tree.Root() + L"new.bin", 70000);
    WriteFileOfSize(tree.Root() + L"a.bin", 50000);
    CHECK(DeleteFileW(Extended(tree.Root() + L"b.bin").c_str()));
    MakeDirectory(tree.Root() + L"made");
    WriteFileOfSize(tree.Root() + L"made\\inside.bin", 9000);
    CHECK(MoveFileW(Extended(tree.Root() + L"sub1\\sub2").c_str(), Extended(tree.Root() + L"moved").c_str()));
    CHECK(MoveFileW(Extended(tree.Root() + L"sub1").c_str(), Extended(tree.Root() + L"renamed").c_str()));
    CHECK(DeleteFileW(Extended(tree.Root() + L"ファイル Ünïcödé\\Ünïcödé.txt").c_str()));
    CHECK(RemoveDirectoryW(Extended(tree.Root() + L"ファイル Ünïcödé").c_str()));
    Sleep(300);  // the watch records changes on a thread of its own

    sm::FolderWatch::Changes const changes = scanned.watch->Take();
    CHECK(changes.complete);
    CHECK(!changes.folders.empty());
    std::optional<sm::FolderReload> reload =
        sm::PrepareReload(scanned, changes.folders, changes.files, progress, std::stop_token{});
    CHECK(reload.has_value());
    sm::ApplyReload(scanned, std::move(*reload));

    ExpectSameContents(scanned, tree.Root());
    CheckConsistency(*scanned.Root());
    CHECK(FindChild(*scanned.Root(), L"renamed") != nullptr && FindChild(*scanned.Root(), L"sub1") == nullptr);
}

TEST_CASE(ReloadReadsFilesKeptOpen)
{
    TestTree const tree;
    sm::ScanProgress progress;
    sm::ScanOptions options;
    options.watch = true;
    sm::ScanResult const result = sm::ScanFolder(tree.Root(), progress, std::stop_token{}, options);
    sm::FolderTree& scanned = *result.tree;

    // A download in progress: a folder listing shows it as it was when last closed.
    wil::unique_hfile const download(CreateFileW(Extended(tree.Root() + L"download.part").c_str(), GENERIC_WRITE,
                                                 FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                                 nullptr));
    CHECK(static_cast<bool>(download));
    std::vector<char> const chunk(std::size_t{ 1 } << 20, 'x');
    auto const write = [&](int megabytes) {
        for (int i = 0; i < megabytes; ++i)
        {
            DWORD written = 0;
            CHECK(WriteFile(download.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &written, nullptr));
        }
    };
    auto const reload = [&] {
        Sleep(300);  // the watch records changes on a thread of its own
        sm::FolderWatch::Changes const changes = scanned.watch->Take();
        std::optional<sm::FolderReload> prepared =
            sm::PrepareReload(scanned, changes.folders, changes.files, progress, std::stop_token{});
        CHECK(prepared.has_value());
        sm::ApplyReload(scanned, std::move(*prepared));
        sm::Folder const& root = *scanned.Root();
        for (std::size_t i = 0; i < root.entries.size(); ++i)
        {
            if (root.Name(i) == L"download.part")
            {
                return root.entries[i].logicalSize;
            }
        }
        return std::uint64_t{ 0 };
    };

    write(3);
    CHECK_EQ(reload(), std::uint64_t{ 3 } << 20);
    // Nothing reports it growing further while it stays open, but it is read again.
    write(2);
    CHECK_EQ(reload(), std::uint64_t{ 5 } << 20);
    CHECK(!scanned.growingFiles.empty());
    CheckConsistency(*scanned.Root());
}

TEST_CASE(LiveSnapshotsShowWhatWasFoundSoFar)
{
    TestTree const tree;
    for (int folder = 0; folder < 30; ++folder)  // enough for the scan to take a while
    {
        std::wstring const path = tree.Root() + L"many" + std::to_wstring(folder) + L"\\";
        MakeDirectory(path);
        for (int file = 0; file < 20; ++file)
        {
            WriteFileOfSize(path + std::to_wstring(file) + L".bin", 100);
        }
    }
    sm::ScanProgress progress;
    std::atomic<bool> done = false;
    std::vector<std::unique_ptr<sm::FolderTree>> snapshots;
    std::thread taker([&] {
        while (!done)
        {
            // A small budget: most folders come as boxes of the size found in them.
            if (std::unique_ptr<sm::FolderTree> snapshot = progress.Snapshot(20))
            {
                snapshots.push_back(std::move(snapshot));
            }
            // The first comes as soon as the root is listed; after it, a snapshot
            // blocks finalizing folders, so taking them back to back would stall the scan.
            if (snapshots.empty())
            {
                std::this_thread::yield();
            }
            else
            {
                Sleep(1);
            }
        }
    });
    sm::ScanOptions options;
    options.threads = 1;  // one folder at a time: the 150-level chain takes a while
    sm::ScanResult const result = sm::ScanFolder(tree.Root(), progress, std::stop_token{}, options);
    done = true;
    taker.join();

    CHECK(result.status == sm::ScanStatus::Completed);
    CHECK(progress.Snapshot(20) == nullptr);  // only while the scan runs
    CHECK(!snapshots.empty());
    sm::Folder const& final = *result.tree->Root();
    for (std::unique_ptr<sm::FolderTree> const& snapshot : snapshots)
    {
        sm::Folder const& root = *snapshot->Root();
        // Each folder adds up - but for the boxes, which hold totals and no entries.
        std::vector<sm::Folder const*> pending{ &root };
        while (!pending.empty())
        {
            sm::Folder const& folder = *pending.back();
            pending.pop_back();
            if (folder.entries.empty())
            {
                continue;
            }
            std::uint64_t files = 0;
            std::uint64_t bytes = 0;
            for (std::size_t i = 0; i < folder.entries.size(); ++i)
            {
                sm::Entry const& entry = folder.entries[i];
                CHECK(i == 0 || folder.entries[i - 1].size >= entry.size);
                if (entry.child)
                {
                    CHECK_EQ(entry.size, entry.child->SizeTotal());
                    files += entry.child->fileCount;
                    pending.push_back(entry.child.get());
                }
                else
                {
                    ++files;
                }
                bytes += entry.size;
            }
            CHECK_EQ(folder.fileCount, files);
            CHECK_EQ(folder.SizeTotal(), bytes);
        }
        CHECK(root.SizeTotal() <= final.SizeTotal());
        CHECK(root.fileCount <= final.fileCount);
        CHECK(root.folderCount <= final.folderCount);
    }
}

TEST_CASE(RescanningAFolderReplacesWhatItHolds)
{
    TestTree const tree;
    sm::ScanResult const result = Scan(tree.Root());
    sm::FolderTree& scanned = *result.tree;
    sm::Folder* const sub1 = FindChild(*scanned.Root(), L"sub1");
    CHECK(sub1 != nullptr);

    WriteFileOfSize(tree.Root() + L"sub1\\more.bin", 123456);
    CHECK(DeleteFileW(Extended(tree.Root() + L"sub1\\c.bin").c_str()));
    sm::ScanProgress progress;
    std::unique_ptr<sm::Folder> fresh = sm::ScanAgain(scanned, *sub1, progress, std::stop_token{});
    CHECK(fresh != nullptr);
    sm::ReplaceContents(scanned, *sub1, std::move(fresh));

    ExpectSameContents(scanned, tree.Root());
    CheckConsistency(*scanned.Root());
}

TEST_CASE(UnreadableFoldersAreListed)
{
    TestTree const tree;
    std::wstring const locked = tree.Root() + L"locked\\";
    MakeDirectory(locked);
    // Nobody may list it (an administrator neither, without the backup privilege); anyone may delete it.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;;0x1;;;WD)(A;;FA;;;WD)", SDDL_REVISION_1,
                                                               &descriptor, nullptr));
    BOOL const set = SetFileSecurityW(Extended(locked).c_str(), DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    CHECK(set);

    sm::ScanResult const result = Scan(tree.Root());
    CHECK_EQ(result.tree->unreadableFolders, std::uint64_t{ 1 });
    std::vector<std::wstring> const unreadable = result.tree->UnreadableFolders();
    CHECK_EQ(unreadable.size(), std::size_t{ 1 });
    CHECK(unreadable.front() == locked);
}

TEST_CASE(StoppingBreaksOffBlockedIo)
{
    // A read from a pipe nobody writes to blocks, like a folder listing on a
    // network share that stopped answering - until it is cancelled.
    wil::unique_handle readEnd;
    wil::unique_handle writeEnd;
    CHECK(CreatePipe(readEnd.put(), writeEnd.put(), nullptr, 0));
    std::stop_source stop;
    sm::IoCancellation io(stop.get_token());
    std::atomic<bool> enlisted = false;
    std::atomic<DWORD> error = ERROR_SUCCESS;
    std::thread reader([&] {
        sm::IoCancellation::Enlistment const enlistment(io);
        enlisted = true;
        char byte = 0;
        DWORD read = 0;
        if (!ReadFile(readEnd.get(), &byte, 1, &read, nullptr))
        {
            error = GetLastError();
        }
    });
    while (!enlisted)
    {
        Sleep(1);
    }
    Sleep(100);  // time to block in ReadFile
    auto const stopped = std::chrono::steady_clock::now();
    stop.request_stop();
    // Should the read not be cancelled, closing the pipe ends it: the test fails rather than hangs.
    if (WaitForSingleObject(reader.native_handle(), 5000) == WAIT_TIMEOUT)
    {
        writeEnd.reset();
    }
    reader.join();
    CHECK_EQ(error.load(), static_cast<DWORD>(ERROR_OPERATION_ABORTED));
    CHECK(std::chrono::steady_clock::now() - stopped < std::chrono::seconds(2));
}

TEST_CASE(ScanMeasuresFilesCompressedByWindows)
{
    // compact.exe's compression: folder listings show such a file taking no space.
    TestTree const tree;
    std::wstring const path = tree.Root() + L"compact.txt";
    std::size_t const length = std::size_t{ 1 } << 20;
    {
        wil::unique_hfile const file(CreateFileW(Extended(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                                 FILE_ATTRIBUTE_NORMAL, nullptr));
        CHECK(static_cast<bool>(file));
        std::string text;
        while (text.size() < length)
        {
            text += "Lorem ipsum dolor sit amet, consectetur adipiscing elit. ";
        }
        DWORD written = 0;
        CHECK(WriteFile(file.get(), text.data(), static_cast<DWORD>(length), &written, nullptr) && written == length);
    }
    {
        wil::unique_hfile const file(
            CreateFileW(Extended(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        CHECK(static_cast<bool>(file));
        struct
        {
            WOF_EXTERNAL_INFO wof;
            FILE_PROVIDER_EXTERNAL_INFO_V1 provider;
        } info{ { WOF_CURRENT_VERSION, WOF_PROVIDER_FILE },
                { FILE_PROVIDER_CURRENT_VERSION, FILE_PROVIDER_COMPRESSION_XPRESS4K, 0 } };
        DWORD returned = 0;
        if (!DeviceIoControl(file.get(), FSCTL_SET_EXTERNAL_BACKING, &info, sizeof(info), nullptr, 0, &returned,
                             nullptr))
        {
            return;  // not offered on this drive
        }
    }
    DWORD high = 0;
    DWORD const low = GetCompressedFileSizeW(Extended(path).c_str(), &high);
    std::uint64_t const real = (std::uint64_t{ high } << 32) | low;
    CHECK(real > 0 && real < length);

    sm::ScanResult const result = Scan(tree.Root());
    sm::Folder const& root = *result.tree->Root();
    sm::Entry const* found = nullptr;
    for (std::size_t i = 0; i < root.entries.size(); ++i)
    {
        if (root.Name(i) == L"compact.txt")
        {
            found = &root.entries[i];
        }
    }
    CHECK(found != nullptr);
    CHECK_EQ(found->logicalSize, std::uint64_t{ length });
    CHECK_EQ(found->size, real);
}

TEST_CASE(ScanHandlesUnicodeAndLongPaths)
{
    TestTree const tree;
    sm::ScanResult const result = Scan(tree.Root());
    CHECK(result.status == sm::ScanStatus::Completed);
    sm::FolderTree const& folderTree = *result.tree;

    sm::Folder* const unicode = FindChild(*folderTree.Root(), L"ファイル Ünïcödé");
    CHECK(unicode != nullptr);
    CHECK_EQ(unicode->entries.size(), std::size_t{ 1 });
    CHECK_EQ(unicode->Name(0), std::wstring_view(L"Ünïcödé.txt"));
    CHECK_EQ(unicode->entries[0].logicalSize, std::uint64_t{ 5000 });

    sm::Folder* folder = FindChild(*folderTree.Root(), L"deep");
    for (int i = 0; folder != nullptr && i < kDeepLevels; ++i)
    {
        folder = FindChild(*folder, L"d");
    }
    CHECK(folder != nullptr);
    CHECK_EQ(folder->entries.size(), std::size_t{ 1 });
    CHECK_EQ(folder->entries[0].logicalSize, std::uint64_t{ 2000 });
    CHECK(tree.Leaf().size() > MAX_PATH);
    CHECK_EQ(folderTree.PathOf(*folder, 0), tree.Leaf());
}

TEST_CASE(ScanIsDeterministicAcrossThreadCounts)
{
    TestTree const tree;
    sm::ScanResult const single = Scan(tree.Root(), 1);
    sm::ScanResult const parallel = Scan(tree.Root(), 8);
    CHECK(single.status == sm::ScanStatus::Completed);
    CHECK(parallel.status == sm::ScanStatus::Completed);
    CHECK_EQ(single.tree->threads, 1u);
    CHECK_EQ(parallel.tree->threads, 8u);

    std::vector<std::wstring> a;
    std::vector<std::wstring> b;
    Flatten(*single.tree, *single.tree->Root(), a);
    Flatten(*parallel.tree, *parallel.tree->Root(), b);
    CHECK_EQ(a.size(), b.size());
    CHECK(a == b);
}

TEST_CASE(RemoveUpdatesEveryAncestor)
{
    TestTree const tree;
    sm::ScanResult const result = Scan(tree.Root());
    sm::FolderTree& folderTree = *result.tree;
    sm::Folder& root = *folderTree.Root();

    sm::Folder* const sub1 = FindChild(root, L"sub1");
    sm::Folder* const sub2 = FindChild(*sub1, L"sub2");
    std::uint64_t const rootBefore = root.SizeTotal();
    std::uint64_t const sub1Before = sub1->SizeTotal();
    std::uint64_t const removed = sub2->entries[0].size;

    folderTree.Remove(*sub2, 0);  // d.bin

    CHECK(sub2->entries.empty());
    CHECK_EQ(sub2->SizeTotal(), std::uint64_t{ 0 });
    CHECK_EQ(sub1->SizeTotal(), sub1Before - removed);
    CHECK_EQ(root.SizeTotal(), rootBefore - removed);
    CHECK_EQ(root.fileCount, TestTree::kFiles - 1);
    CHECK_EQ(root.logicalTotal, TestTree::kLogicalBytes - 300000);
    CHECK_EQ(CheckConsistency(root), TestTree::kFolders + 1);

    // Removing a whole folder takes its sub-folders and files with it.
    std::uint32_t const sub1Index = sub1->indexInParent;
    folderTree.Remove(root, sub1Index);  // sub1 and sub2, c.bin
    CHECK(FindChild(root, L"sub1") == nullptr);
    CHECK_EQ(root.fileCount, TestTree::kFiles - 2);
    CHECK_EQ(root.folderCount, TestTree::kFolders - 2);
    CHECK_EQ(CheckConsistency(root), TestTree::kFolders - 1);
}

TEST_CASE(ScanReportsMissingPaths)
{
    TestTree const tree;
    CHECK(Scan(tree.Root() + L"does-not-exist").status == sm::ScanStatus::NotFound);
    CHECK(Scan(tree.Root() + L"a.bin").status == sm::ScanStatus::NotFound);  // a file, not a folder
    CHECK(Scan(L"").status == sm::ScanStatus::NotFound);
}

TEST_CASE(ScanCanBeCancelled)
{
    TestTree const tree;
    std::stop_source stop;
    stop.request_stop();
    sm::ScanProgress progress;
    sm::ScanResult const before = sm::ScanFolder(tree.Root(), progress, stop.get_token());
    CHECK(before.status == sm::ScanStatus::Cancelled);
    CHECK(before.tree == nullptr);

    // Cancel a long scan part-way: it must stop promptly.
    wchar_t windows[MAX_PATH + 1]{};
    CHECK(GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows))) != 0);
    std::stop_source midway;
    sm::ScanProgress progress2;
    auto const started = std::chrono::steady_clock::now();
    std::jthread const canceller([&midway] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        midway.request_stop();
    });
    sm::ScanResult const during = sm::ScanFolder(windows, progress2, midway.get_token());
    auto const elapsed = std::chrono::steady_clock::now() - started;
    if (during.status == sm::ScanStatus::Cancelled)
    {
        CHECK(elapsed < std::chrono::seconds(5));
    }
    else
    {
        CHECK(during.status == sm::ScanStatus::Completed);  // a very fast machine finished first
    }
}

TEST_CASE(WholeDriveGetsFreeAndUnscannedSpace)
{
    wchar_t windows[MAX_PATH + 1]{};
    CHECK(GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows))) != 0);
    std::wstring const drive = std::wstring(windows, 3);  // "C:\"

    // Scanning a whole system drive is slow, so build the tree by hand instead:
    // what matters is how a whole-volume tree treats its pseudo-entries.
    auto root = std::make_unique<sm::Folder>();
    sm::Entry file;
    file.size = file.logicalSize = 4096;
    root->entries.push_back(std::move(file));
    root->sizeFiles = 4096;
    root->fileCount = 1;

    sm::VolumeInfo volume;
    volume.root = drive;
    volume.totalBytes = 1'000'000;
    volume.freeBytes = 600'000;
    sm::FolderTree tree(std::move(root), drive, volume, true);

    CHECK(tree.IsWholeVolume());
    sm::Folder const& folder = *tree.Root();
    CHECK_EQ(folder.entries.size(), std::size_t{ 3 });
    CHECK(folder.entries[0].kind == sm::EntryKind::FreeSpace);
    CHECK_EQ(folder.entries[0].size, std::uint64_t{ 600'000 });
    CHECK(folder.entries[1].kind == sm::EntryKind::Unscanned);
    CHECK_EQ(folder.entries[1].size, std::uint64_t{ 400'000 - 4096 });
    CHECK_EQ(folder.SizeTotal(), std::uint64_t{ 4096 });  // pseudo-entries are not files
    CHECK_EQ(tree.PathOf(folder, 0), drive);

    tree.Remove(*tree.Root(), 0);  // pseudo-entries cannot be removed
    CHECK_EQ(tree.Root()->entries.size(), std::size_t{ 3 });
}

TEST_CASE(NormalizeScanPathCanonicalizes)
{
    wchar_t windows[MAX_PATH + 1]{};
    CHECK(GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows))) != 0);
    std::wstring const drive(windows, 2);  // "C:"

    CHECK_EQ(sm::NormalizeScanPath(drive), drive + L"\\");  // the root, not the current directory
    CHECK_EQ(sm::NormalizeScanPath(L"  \"" + drive + L"\\Windows\\..\"  "), drive + L"\\");
    CHECK_EQ(sm::NormalizeScanPath(drive + L"/Windows/System32"), drive + L"\\Windows\\System32\\");
    CHECK_EQ(sm::NormalizeScanPath(L"\\\\?\\" + drive + L"\\Windows"), drive + L"\\Windows\\");
    CHECK_EQ(sm::NormalizeScanPath(L"\\\\?\\UNC\\server\\share\\x"), std::wstring(L"\\\\server\\share\\x\\"));
    CHECK_EQ(sm::NormalizeScanPath(L"   "), std::wstring());
}

TEST_CASE(NormalizeScanPathExpandsShortNames)
{
    wchar_t windows[MAX_PATH + 1]{};
    CHECK(GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows))) != 0);
    std::wstring const longPath = std::wstring(windows, 3) + L"Program Files\\Common Files\\";
    wchar_t shortPath[MAX_PATH + 1]{};
    DWORD const length = GetShortPathNameW(longPath.c_str(), shortPath, static_cast<DWORD>(std::size(shortPath)));
    CHECK(length != 0 && length < std::size(shortPath));

    // "C:\PROGRA~1\COMMON~1\" where the volume has 8.3 names, else the long path itself.
    CHECK_EQ(sm::NormalizeScanPath(shortPath), longPath);
    CHECK_EQ(sm::NormalizeScanPath(std::wstring(L"\\\\?\\") + shortPath), longPath);
    CHECK_EQ(sm::NormalizeScanPath(std::wstring(shortPath) + L"NoSuchFolder~1"),
             std::wstring(shortPath) + L"NoSuchFolder~1\\");  // missing paths keep their form
}

TEST_CASE(ExtendedLengthPaths)
{
    CHECK_EQ(sm::ToExtendedLengthPath(L"C:\\x\\"), std::wstring(L"\\\\?\\C:\\x\\"));
    CHECK_EQ(sm::ToExtendedLengthPath(L"\\\\server\\share\\"), std::wstring(L"\\\\?\\UNC\\server\\share\\"));
    CHECK_EQ(sm::ToExtendedLengthPath(L"\\\\?\\C:\\x\\"), std::wstring(L"\\\\?\\C:\\x\\"));
    CHECK_EQ(sm::ToExtendedLengthPath(L"\\\\.\\C:"), std::wstring(L"\\\\.\\C:"));
}

TEST_CASE(RoundUpToClusterRoundsUp)
{
    // 1.4.0 meant to round up but rounded down, so every file smaller than a
    // cluster counted as nothing at all.
    CHECK_EQ(sm::RoundUpToCluster(0, 4096), std::uint64_t{ 0 });
    CHECK_EQ(sm::RoundUpToCluster(1, 4096), std::uint64_t{ 4096 });
    CHECK_EQ(sm::RoundUpToCluster(4096, 4096), std::uint64_t{ 4096 });
    CHECK_EQ(sm::RoundUpToCluster(4097, 4096), std::uint64_t{ 8192 });
    CHECK_EQ(sm::RoundUpToCluster(5000, 3000), std::uint64_t{ 6000 });  // non-power-of-two clusters too
    CHECK_EQ(sm::RoundUpToCluster(12345, 0), std::uint64_t{ 12345 });
    CHECK_EQ(sm::RoundUpToCluster(UINT64_MAX - 1, 4096), UINT64_MAX - 1);  // no overflow
}

TEST_CASE(DeepTreesAreFreedWithoutRecursion)
{
    // 100,000 nested folders: recursive destruction would overflow the 1 MB stack.
    auto root = std::make_unique<sm::Folder>();
    sm::Folder* tip = root.get();
    for (int i = 0; i < 100'000; ++i)
    {
        sm::Entry entry;
        entry.kind = sm::EntryKind::Folder;
        entry.child = std::make_unique<sm::Folder>();
        entry.child->parent = tip;
        sm::Folder* const next = entry.child.get();
        tip->entries.push_back(std::move(entry));
        tip = next;
    }
    root.reset();
    CHECK(root == nullptr);
}

TEST_CASE(ReloadKeepsHardLinksAndUnreadableFoldersAsAFreshScanWould)
{
    // A reload recounts only the hard links of the files whose names came or
    // went, and keeps the count of unreadable folders without walking the
    // tree: after each change, the tree must look like a fresh scan.
    TestTree const tree;
    sm::ScanProgress progress;
    sm::ScanOptions options;
    options.watch = true;
    sm::ScanResult const result = sm::ScanFolder(tree.Root(), progress, std::stop_token{}, options);
    CHECK(result.tree != nullptr && result.tree->watch != nullptr);
    sm::FolderTree& scanned = *result.tree;
    auto const reload = [&] {
        Sleep(300);  // the watch records changes on a thread of its own
        sm::FolderWatch::Changes const changes = scanned.watch->Take();
        CHECK(changes.complete);
        std::optional<sm::FolderReload> prepared =
            sm::PrepareReload(scanned, changes.folders, changes.files, progress, std::stop_token{});
        CHECK(prepared.has_value());
        sm::ApplyReload(scanned, std::move(*prepared));
        ExpectSameContents(scanned, tree.Root());
        CheckConsistency(*scanned.Root());
        CHECK_EQ(scanned.unreadableFolders, Scan(tree.Root()).tree->unreadableFolders);
    };

    // A second name for sub1\c.bin, nearer the root: the space goes with it.
    if (!CreateHardLinkW(Extended(tree.Root() + L"c-link.bin").c_str(), Extended(tree.Root() + L"sub1\\c.bin").c_str(),
                         nullptr))
    {
        return;  // the temporary folder is not on NTFS
    }
    reload();
    CHECK(scanned.hardLinks);
    // That name goes: the space comes back to sub1\c.bin, read from the file.
    CHECK(DeleteFileW(Extended(tree.Root() + L"c-link.bin").c_str()));
    reload();

    // A folder nobody may list comes, and goes.
    std::wstring const locked = tree.Root() + L"locked\\";
    MakeDirectory(locked);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;;0x1;;;WD)(A;;FA;;;WD)", SDDL_REVISION_1,
                                                               &descriptor, nullptr));
    BOOL const set = SetFileSecurityW(Extended(locked).c_str(), DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    CHECK(set);
    reload();
    CHECK_EQ(scanned.unreadableFolders, std::uint64_t{ 1 });
    CHECK(RemoveDirectoryW(Extended(locked).c_str()));
    reload();
    CHECK_EQ(scanned.unreadableFolders, std::uint64_t{ 0 });
}
