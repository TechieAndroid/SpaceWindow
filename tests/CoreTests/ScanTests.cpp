// Tests for the scanner, against a real folder tree built in %TEMP%.
#include "pch.h"
#include "TestHarness.h"

#include "../../src/Core/FolderTree.h"
#include "../../src/Core/FolderWatch.h"
#include "../../src/Core/IoCancellation.h"
#include "../../src/Core/ScopeExit.h"
#ifndef _WIN32
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#endif

namespace
{
    constexpr int kDeepLevels = 150;  // deep enough for a path far beyond MAX_PATH
#ifdef _WIN32
    constexpr std::wstring_view kDeepName = L"d";
    constexpr std::size_t kLongPath = MAX_PATH;
#else
    // Linux takes paths of up to PATH_MAX (4096) bytes in one piece: names this
    // long make the deep tree's path longer than that.
    constexpr std::wstring_view kDeepName = L"a-somewhat-long-folder-name";
    constexpr std::size_t kLongPath = PATH_MAX;
    static_assert(kDeepLevels * (kDeepName.size() + 1) > PATH_MAX);
#endif

    // A path below a folder, written with '/' whatever the system's separator.
    std::wstring Rel(std::wstring_view relative)
    {
        std::wstring path(relative);
        std::ranges::replace(path, L'/', sm::kPathSeparator);
        return path;
    }

#ifdef _WIN32
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

    // A directory junction, which the scanner must not follow.
    void MakeAlias(std::wstring const& link, std::wstring const& target)
    {
        MakeJunction(link, target);
    }

    bool DeletePath(std::wstring const& path)
    {
        return DeleteFileW(Extended(path).c_str()) != FALSE;
    }

    bool RemoveFolder(std::wstring const& path)
    {
        return RemoveDirectoryW(Extended(path).c_str()) != FALSE;
    }

    bool MovePath(std::wstring const& from, std::wstring const& to)
    {
        return MoveFileW(Extended(from).c_str(), Extended(to).c_str()) != FALSE;
    }

    // False where the file system has no hard links.
    bool MakeHardLink(std::wstring const& link, std::wstring const& target)
    {
        return CreateHardLinkW(Extended(link).c_str(), Extended(target).c_str(), nullptr) != FALSE;
    }

    void Pause(int milliseconds)
    {
        Sleep(static_cast<DWORD>(milliseconds));
    }

    // A new folder in %TEMP%.
    std::wstring NewTempFolder(std::wstring_view prefix)
    {
        wchar_t temp[MAX_PATH + 1]{};
        CHECK(GetTempPathW(static_cast<DWORD>(std::size(temp)), temp) != 0);
        GUID guid{};
        (void)CoCreateGuid(&guid);
        wchar_t id[40]{};
        (void)StringFromGUID2(guid, id, static_cast<int>(std::size(id)));
        std::wstring const root = std::wstring(temp) + std::wstring(prefix) + id;
        MakeDirectory(root);
        return root;
    }

    // Nobody may list it (an administrator neither, without the backup privilege); anyone may delete it.
    bool CanLockFolders()
    {
        return true;
    }

    void LockFolder(std::wstring const& folder)
    {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;;0x1;;;WD)(A;;FA;;;WD)", SDDL_REVISION_1,
                                                                   &descriptor, nullptr));
        BOOL const set = SetFileSecurityW(Extended(folder).c_str(), DACL_SECURITY_INFORMATION, descriptor);
        LocalFree(descriptor);
        CHECK(set);
    }
#else
    // The folder holding `path`, open, and the name in it: so that paths of any
    // length can be made and removed (one call takes at most PATH_MAX bytes).
    struct InFolder
    {
        int folder = -1;
        std::string name;

        InFolder() = default;
        InFolder(InFolder const&) = delete;
        InFolder& operator=(InFolder const&) = delete;
        ~InFolder()
        {
            if (folder >= 0)
            {
                close(folder);
            }
        }
    };

    std::unique_ptr<InFolder> Locate(std::wstring const& path)
    {
        std::string native = sm::posix::ToNative(path);
        while (native.size() > 1 && native.back() == '/')
        {
            native.pop_back();
        }
        std::size_t const slash = native.rfind('/');
        auto at = std::make_unique<InFolder>();
        if (slash != std::string::npos)
        {
            at->folder = sm::posix::OpenDirectory(slash == 0 ? std::string("/") : native.substr(0, slash));
            at->name = native.substr(slash + 1);
        }
        return at;
    }

    void WriteFileOfSize(std::wstring const& path, std::size_t size)
    {
        auto const at = Locate(path);
        CHECK(at->folder >= 0);
        int const file = openat(at->folder, at->name.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        CHECK(file >= 0);
        std::vector<char> const data(size, 'x');
        std::size_t written = 0;
        while (written < size)
        {
            ssize_t const now = write(file, data.data() + written, size - written);
            if (now <= 0)
            {
                break;
            }
            written += static_cast<std::size_t>(now);
        }
        close(file);
        CHECK_EQ(written, size);
    }

    void MakeDirectory(std::wstring const& path)
    {
        auto const at = Locate(path);
        CHECK(at->folder >= 0 && mkdirat(at->folder, at->name.c_str(), 0755) == 0);
    }

    // A symbolic link, which the scanner must not follow.
    void MakeAlias(std::wstring const& link, std::wstring const& target)
    {
        CHECK(symlink(sm::posix::ToNative(target).c_str(), sm::posix::ToNative(link).c_str()) == 0);
    }

    bool DeletePath(std::wstring const& path)
    {
        auto const at = Locate(path);
        return at->folder >= 0 && unlinkat(at->folder, at->name.c_str(), 0) == 0;
    }

    bool RemoveFolder(std::wstring const& path)
    {
        auto const at = Locate(path);
        return at->folder >= 0 && unlinkat(at->folder, at->name.c_str(), AT_REMOVEDIR) == 0;
    }

    bool MovePath(std::wstring const& from, std::wstring const& to)
    {
        return rename(sm::posix::ToNative(from).c_str(), sm::posix::ToNative(to).c_str()) == 0;
    }

    // False where the file system has no hard links.
    bool MakeHardLink(std::wstring const& link, std::wstring const& target)
    {
        return ::link(sm::posix::ToNative(target).c_str(), sm::posix::ToNative(link).c_str()) == 0;
    }

    void Pause(int milliseconds)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }

    // A new folder in $TMPDIR (or /tmp).
    std::wstring NewTempFolder(std::wstring_view prefix)
    {
        static std::atomic<unsigned> serial = 0;
        std::string const name = sm::posix::ToNative(prefix) + std::to_string(getpid()) + "-" +
                                 std::to_string(serial++) + "-" +
                                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::path const path = std::filesystem::temp_directory_path() / name;
        CHECK(mkdir(path.c_str(), 0755) == 0);
        return sm::posix::FromNative(path.string());
    }

    // Nobody may list it - but the superuser, who lists everything.
    bool CanLockFolders()
    {
        return geteuid() != 0;
    }

    void LockFolder(std::wstring const& folder)
    {
        CHECK(chmod(sm::posix::ToNative(folder).c_str(), 0) == 0);
    }

    // Deletes a tree without following links, whatever its depth and the length
    // of its paths: one open folder per level, as the scanner does.
    void DeleteTree(std::wstring const& root)
    {
        struct Level
        {
            DIR* listing;
            std::string name;  // in the level above
        };
        int const top = sm::posix::OpenDirectory(sm::posix::ToNative(root));
        DIR* const first = top >= 0 ? fdopendir(top) : nullptr;
        if (first == nullptr)
        {
            return;
        }
        std::vector<Level> levels{ { first, {} } };
        while (!levels.empty())
        {
            DIR* const listing = levels.back().listing;
            dirent const* const entry = readdir(listing);
            if (entry == nullptr)
            {
                std::string const name = levels.back().name;
                closedir(listing);
                levels.pop_back();
                if (!levels.empty())
                {
                    (void)unlinkat(dirfd(levels.back().listing), name.c_str(), AT_REMOVEDIR);
                }
                continue;
            }
            std::string_view const name(entry->d_name);
            if (name == "." || name == "..")
            {
                continue;
            }
            int const folder = dirfd(listing);
            struct stat status{};
            if (fstatat(folder, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(status.st_mode))
            {
                [[maybe_unused]] int const unlocked = fchmodat(folder, entry->d_name, 0700, 0);  // a locked folder too
                int const inside = openat(folder, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                DIR* const next = inside >= 0 ? fdopendir(inside) : nullptr;
                if (next != nullptr)
                {
                    levels.push_back({ next, std::string(name) });
                }
                else
                {
                    (void)unlinkat(folder, entry->d_name, AT_REMOVEDIR);
                }
            }
            else
            {
                (void)unlinkat(folder, entry->d_name, 0);
            }
        }
        (void)RemoveFolder(root);
    }
#endif

    // A small tree with known contents:
    //   a.bin (10000)  b.bin (1 MiB)  empty.txt (0)
    //   sub1\c.bin (4096)  sub1\sub2\d.bin (300000)
    //   ファイル Ünïcödé\Ünïcödé.txt (5000)
    //   link -> sub1 (a junction, or a symbolic link: must not be followed or counted)
    //   deep\d\d\...\d\leaf.bin (2000), kDeepLevels levels of kDeepName
    class TestTree
    {
    public:
        TestTree()
        {
            root_ = sm::NormalizeScanPath(
                NewTempFolder(L"SpaceWindowTest-"));  // once it exists, as the scanner will see it

            WriteFileOfSize(root_ + L"a.bin", 10000);
            WriteFileOfSize(root_ + L"b.bin", 1 << 20);
            WriteFileOfSize(root_ + L"empty.txt", 0);
            MakeDirectory(root_ + L"sub1");
            WriteFileOfSize(root_ + Rel(L"sub1/c.bin"), 4096);
            MakeDirectory(root_ + Rel(L"sub1/sub2"));
            WriteFileOfSize(root_ + Rel(L"sub1/sub2/d.bin"), 300000);
            MakeDirectory(root_ + L"ファイル Ünïcödé");
            WriteFileOfSize(root_ + Rel(L"ファイル Ünïcödé/Ünïcödé.txt"), 5000);
            MakeAlias(root_ + L"link", root_ + L"sub1");

            std::wstring deep = root_ + L"deep" + sm::kPathSeparator;
            MakeDirectory(deep);
            for (int i = 0; i < kDeepLevels; ++i)
            {
                deep.append(kDeepName).push_back(sm::kPathSeparator);
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

    // The junction (or symbolic link) would have added sub1's 304096 bytes a second time.
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
    if (!MakeHardLink(tree.Root() + L"c-link.bin", tree.Root() + Rel(L"sub1/c.bin")))
    {
        return;  // the temporary folder's file system has no hard links
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
    CHECK(DeletePath(tree.Root() + L"b.bin"));
    MakeDirectory(tree.Root() + L"made");
    WriteFileOfSize(tree.Root() + Rel(L"made/inside.bin"), 9000);
    CHECK(MovePath(tree.Root() + Rel(L"sub1/sub2"), tree.Root() + L"moved"));
    CHECK(MovePath(tree.Root() + L"sub1", tree.Root() + L"renamed"));
    CHECK(DeletePath(tree.Root() + Rel(L"ファイル Ünïcödé/Ünïcödé.txt")));
    CHECK(RemoveFolder(tree.Root() + L"ファイル Ünïcödé"));
    Pause(300);  // the watch records changes on a thread of its own

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
#ifdef _WIN32
    wil::unique_hfile const download(CreateFileW(Extended(tree.Root() + L"download.part").c_str(), GENERIC_WRITE,
                                                 FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                                 nullptr));
    CHECK(static_cast<bool>(download));
#else
    int const download = open(sm::posix::ToNative(tree.Root() + L"download.part").c_str(),
                              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    CHECK(download >= 0);
    sm::ScopeExit const closeDownload([download]() noexcept { close(download); });
#endif
    std::vector<char> const chunk(std::size_t{ 1 } << 20, 'x');
    auto const write = [&](int megabytes) {
        for (int i = 0; i < megabytes; ++i)
        {
#ifdef _WIN32
            DWORD written = 0;
            CHECK(WriteFile(download.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &written, nullptr));
#else
            CHECK(::write(download, chunk.data(), chunk.size()) == static_cast<ssize_t>(chunk.size()));
#endif
        }
    };
    auto const reload = [&] {
        Pause(300);  // the watch records changes on a thread of its own
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
#ifdef _WIN32
    CHECK(!scanned.growingFiles.empty());  // (a Linux listing never lags behind a file)
#endif
    CheckConsistency(*scanned.Root());
}

TEST_CASE(LiveSnapshotsShowWhatWasFoundSoFar)
{
    TestTree const tree;
    for (int folder = 0; folder < 30; ++folder)  // enough for the scan to take a while
    {
        std::wstring const path = tree.Root() + L"many" + std::to_wstring(folder) + sm::kPathSeparator;
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
                Pause(1);
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

    WriteFileOfSize(tree.Root() + Rel(L"sub1/more.bin"), 123456);
    CHECK(DeletePath(tree.Root() + Rel(L"sub1/c.bin")));
    sm::ScanProgress progress;
    std::unique_ptr<sm::Folder> fresh = sm::ScanAgain(scanned, *sub1, progress, std::stop_token{});
    CHECK(fresh != nullptr);
    sm::ReplaceContents(scanned, *sub1, std::move(fresh));

    ExpectSameContents(scanned, tree.Root());
    CheckConsistency(*scanned.Root());
}

TEST_CASE(UnreadableFoldersAreListed)
{
    if (!CanLockFolders())
    {
        return;
    }
    TestTree const tree;
    std::wstring const locked = tree.Root() + L"locked" + sm::kPathSeparator;
    MakeDirectory(locked);
    LockFolder(locked);

    sm::ScanResult const result = Scan(tree.Root());
    CHECK_EQ(result.tree->unreadableFolders, std::uint64_t{ 1 });
    std::vector<std::wstring> const unreadable = result.tree->UnreadableFolders();
    CHECK_EQ(unreadable.size(), std::size_t{ 1 });
    CHECK(unreadable.front() == locked);
}

#ifdef _WIN32
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
#else
TEST_CASE(StoppingBreaksOffBlockedIo)
{
    // A read from a pipe nobody writes to blocks, like a listing on a FUSE file
    // system that stopped answering - until the stop interrupts it.
    std::array<int, 2> ends{ -1, -1 };
    CHECK(pipe(ends.data()) == 0);
    std::stop_source stop;  // NOLINT(misc-const-correctness): request_stop() is not const in the standard
    sm::IoCancellation io(stop.get_token());
    std::atomic<bool> enlisted = false;
    std::atomic<bool> finished = false;
    std::atomic<int> error = 0;
    std::thread reader([&] {
        sm::IoCancellation::Enlistment const enlistment(io);
        enlisted = true;
        char byte = 0;
        if (read(ends[0], &byte, 1) < 0)
        {
            error = errno;
        }
        finished = true;
    });
    while (!enlisted)
    {
        Pause(1);
    }
    Pause(100);  // time to block in read
    auto const stopped = std::chrono::steady_clock::now();
    stop.request_stop();
    // Should the read not be interrupted, closing the pipe ends it: the test fails rather than hangs.
    for (int i = 0; i < 500 && !finished; ++i)
    {
        Pause(10);
    }
    if (!finished)
    {
        close(ends[1]);
        ends[1] = -1;
    }
    reader.join();
    for (int const end : ends)
    {
        if (end >= 0)
        {
            close(end);
        }
    }
    CHECK_EQ(error.load(), EINTR);
    CHECK(std::chrono::steady_clock::now() - stopped < std::chrono::seconds(2));
}
#endif

#ifdef _WIN32
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
#else
TEST_CASE(ScanMeasuresSparseFiles)
{
    // A file 64 MiB long that holds a hole and a byte: it takes a block or two.
    TestTree const tree;
    std::wstring const path = tree.Root() + L"sparse.img";
    std::uint64_t const length = std::uint64_t{ 64 } << 20;
    {
        int const file = open(sm::posix::ToNative(path).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        CHECK(file >= 0);
        CHECK(pwrite(file, "x", 1, static_cast<off_t>(length - 1)) == 1);
        close(file);
    }
    struct stat status{};
    CHECK(stat(sm::posix::ToNative(path).c_str(), &status) == 0);
    std::uint64_t const real = static_cast<std::uint64_t>(status.st_blocks) * 512;
    if (real >= length)
    {
        return;  // a file system without holes
    }

    sm::ScanResult const result = Scan(tree.Root());
    sm::Folder const& root = *result.tree->Root();
    sm::Entry const* found = nullptr;
    for (std::size_t i = 0; i < root.entries.size(); ++i)
    {
        if (root.Name(i) == L"sparse.img")
        {
            found = &root.entries[i];
        }
    }
    CHECK(found != nullptr);
    CHECK_EQ(found->logicalSize, length);
    CHECK_EQ(found->size, real);
    CHECK((found->attributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0);
    CHECK_EQ(root.logicalTotal, TestTree::kLogicalBytes + length);
}
#endif

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
        folder = FindChild(*folder, kDeepName);
    }
    CHECK(folder != nullptr);
    CHECK_EQ(folder->entries.size(), std::size_t{ 1 });
    CHECK_EQ(folder->entries[0].logicalSize, std::uint64_t{ 2000 });
    CHECK(tree.Leaf().size() > kLongPath);
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
    std::stop_source stop;  // NOLINT(misc-const-correctness): request_stop() is not const in the standard
    stop.request_stop();
    sm::ScanProgress progress;
    sm::ScanResult const before = sm::ScanFolder(tree.Root(), progress, stop.get_token());
    CHECK(before.status == sm::ScanStatus::Cancelled);
    CHECK(before.tree == nullptr);

    // Cancel a long scan part-way: it must stop promptly.
#ifdef _WIN32
    wchar_t windows[MAX_PATH + 1]{};
    CHECK(GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows))) != 0);
#else
    std::wstring const windows = L"/usr";  // big enough to take a while
#endif
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
#ifdef _WIN32
    wchar_t windows[MAX_PATH + 1]{};
    CHECK(GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows))) != 0);
    std::wstring const drive = std::wstring(windows, 3);  // "C:\"
#else
    std::wstring const drive = L"/";
#endif

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

#ifdef _WIN32
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
#else
TEST_CASE(NormalizeScanPathCanonicalizes)
{
    TestTree const tree;
    std::wstring const& root = tree.Root();
    CHECK_EQ(sm::NormalizeScanPath(L"  \"" + root + L"sub1/../sub1//sub2\"  "), root + L"sub1/sub2/");
    CHECK_EQ(sm::NormalizeScanPath(root + L"link"), root + L"sub1/");             // a symbolic link: where it leads
    CHECK_EQ(sm::NormalizeScanPath(root + L"missing/x/.."), root + L"missing/");  // missing paths keep their form
    CHECK_EQ(sm::NormalizeScanPath(L"/"), std::wstring(L"/"));
    CHECK_EQ(sm::NormalizeScanPath(L"   "), std::wstring());

    // A relative path is relative to the current folder.
    std::filesystem::path const current = std::filesystem::current_path();
    std::filesystem::current_path(sm::posix::ToNative(root));
    std::wstring const relative = sm::NormalizeScanPath(L"sub1");
    std::filesystem::current_path(current);
    CHECK_EQ(relative, root + L"sub1/");
}

TEST_CASE(NamesThatAreNotUtf8RoundTrip)
{
    // Linux file names are bytes: one that is not UTF-8 is still listed, and still reached.
    TestTree const tree;
    std::string const name = "caf\xE9-latin1.txt";  // ISO 8859-1, as old archives have it
    std::string const path = sm::posix::ToNative(tree.Root()) + name;
    int const file = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    CHECK(file >= 0);
    CHECK(write(file, "hello", 5) == 5);
    close(file);

    sm::ScanResult const result = Scan(tree.Root());
    sm::Folder const& root = *result.tree->Root();
    std::optional<std::size_t> found;
    for (std::size_t i = 0; i < root.entries.size(); ++i)
    {
        if (sm::posix::ToNative(root.Name(i)) == name)
        {
            found = i;
        }
    }
    CHECK(found.has_value());
    CHECK_EQ(root.entries[*found].logicalSize, std::uint64_t{ 5 });
    CHECK(sm::posix::ToNative(result.tree->PathOf(root, *found)) == path);
    CHECK(sm::ReadSizeOnDisk(result.tree->PathOf(root, *found), 0).has_value());
}

TEST_CASE(NamesThatAreNotUtf8GoToTheTrashWhole)
{
    // The freedesktop.org trash, in a scratch $XDG_DATA_HOME on the tree's file system.
    TestTree const tree;
    std::string const root = sm::posix::ToNative(tree.Root());
    std::string const trash = root + "data/Trash";
    char const* const saved = std::getenv("XDG_DATA_HOME");
    std::optional<std::string> const before = saved != nullptr ? std::optional<std::string>(saved) : std::nullopt;
    CHECK(setenv("XDG_DATA_HOME", (root + "data").c_str(), 1) == 0);
    sm::ScopeExit const restore([&before]() noexcept {
        (void)(before ? setenv("XDG_DATA_HOME", before->c_str(), 1) : unsetenv("XDG_DATA_HOME"));
    });
    auto const read = [](std::string const& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    auto const escaped = [](std::string_view path) {
        std::string text;
        for (char const c : path)
        {
            auto const byte = static_cast<unsigned char>(c);
            if ((std::isalnum(byte) != 0 && byte < 0x80) || std::string_view("/-._~").find(c) != std::string_view::npos)
            {
                text.push_back(c);
                continue;
            }
            std::array<char, 4> hex{};
            (void)std::snprintf(hex.data(), hex.size(), "%%%02X", byte);
            text += hex.data();
        }
        return text;
    };

    std::string const name = "caf\xE9.txt";  // ISO 8859-1
    for (int round = 1; round <= 2; ++round)
    {
        WriteFileOfSize(sm::posix::FromNative(root + name), 5);
        CHECK(sm::posix::MoveToTrash(sm::posix::FromNative(root + name)));
        CHECK(access((root + name).c_str(), F_OK) != 0);
        // The second goes beside the first, under a name of its own; both remember where they were.
        std::string const kept = round == 1 ? name : name + ".2";
        CHECK(access(std::string(trash).append("/files/").append(kept).c_str(), F_OK) == 0);
        std::string const info = read(std::string(trash).append("/info/").append(kept).append(".trashinfo"));
        CHECK(info.starts_with("[Trash Info]\nPath=" + escaped(root + name) + "\nDeletionDate=20"));
    }
    // A folder goes with what it holds.
    CHECK(sm::posix::MoveToTrash(tree.Root() + L"sub1"));
    CHECK(access((trash + "/files/sub1/sub2/d.bin").c_str(), F_OK) == 0);
    CHECK(!sm::posix::MoveToTrash(tree.Root() + L"no such file"));
}

TEST_CASE(MountPointsAreReadAsTheSystemNamesThem)
{
    // /proc/self/mountinfo escapes space, tab, newline and backslash, and
    // leaves every other byte as it is - a name that is not UTF-8 too.
    std::string const text = "36 35 98:0 /mnt1 /mnt/parent rw,noatime master:1 - ext3 /dev/root rw,errors=continue\n"
                             "29 1 259:2 / / rw,relatime shared:1 - xfs /dev/nvme0n1p2 rw\n"
                             "51 29 8:17 / /run/media/me/My\\040Drive rw,nosuid - vfat /dev/sdb1 rw\n"
                             "52 29 8:18 / /run/media/me/caf\xE9 rw - ext4 /dev/sdb2 rw\n"
                             "53 29 0:45 / /mnt/back\\134slash rw - tmpfs tmpfs rw\n"
                             "not a mount\n";
    std::vector<sm::posix::MountEntry> const mounts = sm::posix::ParseMountInfo(text);
    CHECK_EQ(mounts.size(), std::size_t{ 5 });
    CHECK(mounts[0].major == 98 && mounts[0].minor == 0 && mounts[0].point == "/mnt/parent");
    CHECK(mounts[0].type == "ext3" && mounts[0].source == "/dev/root");
    CHECK(mounts[1].point == "/" && mounts[1].major == 259 && mounts[1].minor == 2);
    CHECK(mounts[2].point == "/run/media/me/My Drive");
    CHECK(mounts[3].point == "/run/media/me/caf\xE9");
    CHECK(mounts[4].point == "/mnt/back\\slash" && mounts[4].type == "tmpfs");
    CHECK(!sm::posix::MountedFileSystems().empty());  // this system's own
}

TEST_CASE(FoldersThatListButCannotBeSearchedAreUnreadable)
{
    if (!CanLockFolders())
    {
        return;
    }
    // Read but no search permission (r--): the names list, nothing about them does.
    TestTree const tree;
    std::wstring const listed = tree.Root() + L"names-only" + sm::kPathSeparator;
    MakeDirectory(listed);
    WriteFileOfSize(listed + L"inside.bin", 4096);
    CHECK(chmod(sm::posix::ToNative(listed).c_str(), 0444) == 0);

    sm::ScanResult const result = Scan(tree.Root());
    CHECK_EQ(result.tree->unreadableFolders, std::uint64_t{ 1 });
    std::vector<std::wstring> const unreadable = result.tree->UnreadableFolders();
    CHECK(unreadable.size() == 1 && unreadable.front() == listed);
}

TEST_CASE(TreesWithMoreFoldersThanTheWatchTakesAreNotWatched)
{
    // Past the watches it may have, the watch gives up - and says why, so that
    // scans of the tree again do not try to watch it all once more.
    TestTree const tree;
    sm::ScanProgress progress;
    sm::ScanOptions options;
    options.watch = true;
    options.watchLimit = 2;
    sm::ScanResult const big = sm::ScanFolder(tree.Root(), progress, std::stop_token{}, options);
    CHECK(big.status == sm::ScanStatus::Completed);
    CHECK(big.tree->watch == nullptr && big.tree->tooBigToWatch);

    options.watchLimit = 10'000;
    sm::ScanResult const watched = sm::ScanFolder(tree.Root(), progress, std::stop_token{}, options);
    CHECK(watched.tree->watch != nullptr && !watched.tree->tooBigToWatch);
}
#endif

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
        Pause(300);  // the watch records changes on a thread of its own
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
    if (!MakeHardLink(tree.Root() + L"c-link.bin", tree.Root() + Rel(L"sub1/c.bin")))
    {
        return;  // the temporary folder's file system has no hard links
    }
    reload();
    CHECK(scanned.hardLinks);
    // That name goes: the space comes back to sub1\c.bin, read from the file.
    CHECK(DeletePath(tree.Root() + L"c-link.bin"));
    reload();

    // A folder nobody may list comes, and goes.
    if (!CanLockFolders())
    {
        return;
    }
    std::wstring const locked = tree.Root() + L"locked" + sm::kPathSeparator;
    MakeDirectory(locked);
    LockFolder(locked);
    reload();
    CHECK_EQ(scanned.unreadableFolders, std::uint64_t{ 1 });
    CHECK(RemoveFolder(locked));
    reload();
    CHECK_EQ(scanned.unreadableFolders, std::uint64_t{ 0 });
}
