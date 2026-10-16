// Tests that read a real drive: the master file table scan and change journal
// rescans, checked against the folder scanner on files made for the purpose in
// the temporary folder.  They run only with --volume, as administrator, with
// the temporary folder on an NTFS drive.  The drive itself is only read.
#include "pch.h"
#include "Compare.h"
#include "TestHarness.h"

#include "../../src/Core/ChangeJournal.h"
#include "../../src/Core/FolderTree.h"
#include "../../src/Core/Ntfs.h"

namespace
{
    template <typename T>
    T Field(std::span<std::byte const> bytes, std::size_t offset)
    {
        T value{};
        if (offset + sizeof(T) <= bytes.size())
        {
            std::memcpy(&value, bytes.data() + offset, sizeof(T));
        }
        return value;
    }

    // The drive holding the temporary folder, such as L"C:\\".
    std::wstring TempDrive()
    {
        wchar_t temp[MAX_PATH + 1]{};
        wchar_t drive[MAX_PATH + 1]{};
        CHECK(GetTempPathW(static_cast<DWORD>(std::size(temp)), temp) != 0);
        CHECK(GetVolumePathNameW(temp, drive, static_cast<DWORD>(std::size(drive))));
        return drive;
    }

    // A folder for a test's files in the temporary folder, removed afterwards.
    class Sandbox
    {
    public:
        Sandbox()
        {
            wchar_t temp[MAX_PATH + 1]{};
            CHECK(GetTempPathW(static_cast<DWORD>(std::size(temp)), temp) != 0);
            std::wstring const folder =
                std::wstring(temp) + L"SpaceWindow volume test " + std::to_wstring(GetCurrentProcessId());
            Remove(folder);  // left over from an interrupted run
            CHECK(CreateDirectoryW(folder.c_str(), nullptr));
            // The long form: the master file table has no short (8.3) names to look up.
            wchar_t full[MAX_PATH + 1]{};
            DWORD const length = GetLongPathNameW(folder.c_str(), full, static_cast<DWORD>(std::size(full)));
            CHECK(length != 0 && length < std::size(full));
            folder_ = std::wstring(full) + L"\\";
            drive_ = TempDrive();
            CHECK(folder_.size() > drive_.size());
        }

        Sandbox(Sandbox const&) = delete;
        Sandbox& operator=(Sandbox const&) = delete;
        ~Sandbox()
        {
            try
            {
                Remove(folder_);
            }
            catch (...)  // NOLINT(bugprone-empty-catch): best-effort cleanup; a destructor must not throw
            {
            }
        }

        std::wstring operator[](std::wstring_view name) const { return folder_ + std::wstring(name); }
        std::wstring const& Folder() const noexcept { return folder_; }
        std::wstring const& Drive() const noexcept { return drive_; }
        std::wstring_view WithinDrive() const noexcept { return std::wstring_view(folder_).substr(drive_.size()); }

    private:
        static void Remove(std::wstring const& folder)
        {
            std::error_code ignored;
            std::filesystem::remove_all(folder, ignored);
        }

        std::wstring folder_;
        std::wstring drive_;
    };

    // Appends `size` bytes that do not compress, or that compress well.
    void Append(HANDLE file, std::uint64_t size, bool compressible = false)
    {
        std::vector<std::byte> chunk(std::size_t{ 64 } * 1024);
        std::string_view const phrase = "Lorem ipsum dolor sit amet, consectetur adipiscing elit. ";
        std::uint32_t seed = 2463534242u;
        while (size > 0)
        {
            for (std::size_t i = 0; i < chunk.size(); ++i)
            {
                seed ^= seed << 13u;
                seed ^= seed >> 17u;
                seed ^= seed << 5u;
                chunk[i] = static_cast<std::byte>(compressible ? static_cast<std::uint8_t>(phrase[i % phrase.size()])
                                                               : static_cast<std::uint8_t>(seed));
            }
            auto const piece = static_cast<DWORD>(std::min<std::uint64_t>(size, chunk.size()));
            DWORD written = 0;
            CHECK(::WriteFile(file, chunk.data(), piece, &written, nullptr) && written == piece);
            size -= piece;
        }
    }

    wil::unique_hfile Create(std::wstring const& path, DWORD access = GENERIC_WRITE)
    {
        wil::unique_hfile file(
            CreateFileW(path.c_str(), access, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        CHECK(file.is_valid());
        return file;
    }

    // A file of `size` bytes, flushed, so that its listing and its record agree.
    void MakeFile(std::wstring const& path, std::uint64_t size, bool compressible = false)
    {
        wil::unique_hfile const file = Create(path);
        Append(file.get(), size, compressible);
        CHECK(FlushFileBuffers(file.get()));
    }

    void MakeSparseFile(std::wstring const& path)
    {
        wil::unique_hfile const file = Create(path, GENERIC_READ | GENERIC_WRITE);
        DWORD returned = 0;
        CHECK(DeviceIoControl(file.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr));
        LARGE_INTEGER at{};
        at.QuadPart = std::int64_t{ 64 } << 20;
        CHECK(SetFilePointerEx(file.get(), at, nullptr, FILE_BEGIN) && SetEndOfFile(file.get()));
        at.QuadPart = std::int64_t{ 32 } << 20;
        CHECK(SetFilePointerEx(file.get(), at, nullptr, FILE_BEGIN));
        Append(file.get(), 100'000);
        CHECK(FlushFileBuffers(file.get()));
    }

    // NTFS compression - where the drive offers it (clusters of 4 KB at most);
    // otherwise a plain file.
    void MakeCompressedFile(std::wstring const& path)
    {
        wil::unique_hfile const file = Create(path, GENERIC_READ | GENERIC_WRITE);
        USHORT format = COMPRESSION_FORMAT_DEFAULT;
        DWORD returned = 0;
        (void)DeviceIoControl(file.get(), FSCTL_SET_COMPRESSION, &format, sizeof(format), nullptr, 0, &returned,
                              nullptr);
        Append(file.get(), 1 << 20, true);
        CHECK(FlushFileBuffers(file.get()));
    }

    // Windows' own file compression, as compact.exe applies it - where the
    // system offers it; otherwise a plain file.
    void MakeWofFile(std::wstring const& path)
    {
        MakeFile(path, 1 << 20, true);
        wil::unique_hfile const file(
            CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        CHECK(file.is_valid());
        struct
        {
            WOF_EXTERNAL_INFO wof;
            FILE_PROVIDER_EXTERNAL_INFO_V1 provider;
        } info{ { WOF_CURRENT_VERSION, WOF_PROVIDER_FILE },
                { FILE_PROVIDER_CURRENT_VERSION, FILE_PROVIDER_COMPRESSION_XPRESS4K, 0 } };
        DWORD returned = 0;
        (void)DeviceIoControl(file.get(), FSCTL_SET_EXTERNAL_BACKING, &info, sizeof(info), nullptr, 0, &returned,
                              nullptr);
    }

    std::uint64_t RecordOf(std::wstring const& path)
    {
        wil::unique_hfile const file(
            CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        CHECK(file.is_valid());
        BY_HANDLE_FILE_INFORMATION info{};
        CHECK(GetFileInformationByHandle(file.get(), &info));
        return sm::ntfs::RecordOf((std::uint64_t{ info.nFileIndexHigh } << 32) | info.nFileIndexLow);
    }

    // Names long enough that a few fill a file record.
    std::wstring LinkName(std::wstring_view file, int link)
    {
        return std::wstring(file) + L" link " + std::to_wstring(link) + L" " + std::wstring(90, L'x');
    }

    // A file with `links` more names, which spill out of its record into
    // others, listed in an attribute list.  With dozens of them, the list
    // itself no longer fits in the record and goes to the disk - which NTFS
    // writes a few seconds later, so wait until the file can be read whole.
    void MakeLinkedFile(Sandbox const& sandbox, std::wstring const& name, int links)
    {
        MakeFile(sandbox[name], 5000);
        for (int link = 0; link < links; ++link)
        {
            CHECK(CreateHardLinkW(sandbox[LinkName(name, link)].c_str(), sandbox[name].c_str(), nullptr));
        }
        std::optional<sm::ntfs::Volume> const volume = sm::ntfs::Volume::Open(sandbox.Drive());
        bool const opened = volume.has_value();
        CHECK(opened);  // needs administrator rights and NTFS
        std::uint64_t const record = RecordOf(sandbox[name]);
        sm::ntfs::FileRecord file;
        for (int attempt = 0; attempt < 600; ++attempt)
        {
            if (volume->ReadFile(record, file) && file.names.size() == static_cast<std::size_t>(links) + 1)
            {
                return;
            }
            Sleep(100);
        }
        test::Fail("the attribute list of " + test::ToUtf8(name) + " never reached the disk",
                   std::source_location::current());
    }

    // Files of every kind the two scanners must agree on.
    void MakeFiles(Sandbox const& sandbox)
    {
        MakeFile(sandbox[L"empty.txt"], 0);
        MakeFile(sandbox[L"tiny.txt"], 13);  // kept inside its record
        MakeFile(sandbox[L"big.bin"], std::uint64_t{ 3 } << 20);
        MakeFile(sandbox[L"na\u00EFve \u2713 \u65E5\u672C.txt"], 5000);
        MakeSparseFile(sandbox[L"sparse.bin"]);
        MakeCompressedFile(sandbox[L"compressed.txt"]);
        MakeWofFile(sandbox[L"wof.txt"]);
        MakeFile(sandbox[L"streams.txt"], 100);
        MakeFile(sandbox[L"streams.txt:extra"], 200'000);  // a named stream: not counted by either
        MakeFile(sandbox[L"hidden.txt"], 10);
        CHECK(SetFileAttributesW(sandbox[L"hidden.txt"].c_str(), FILE_ATTRIBUTE_HIDDEN));
        CHECK(CreateDirectoryW(sandbox[L"empty"].c_str(), nullptr));
        for (std::wstring_view const folder : { L"a", L"a\\b", L"a\\b\\c", L"a\\b\\c\\d" })
        {
            CHECK(CreateDirectoryW(sandbox[folder].c_str(), nullptr));
        }
        MakeFile(sandbox[L"a\\b\\c\\d\\deep.txt"], 777);
        CHECK(CreateDirectoryW(sandbox[L"sub"].c_str(), nullptr));
        MakeFile(sandbox[L"sub\\linked.bin"], 20'000);
        CHECK(CreateHardLinkW(sandbox[L"link to linked.bin"].c_str(), sandbox[L"sub\\linked.bin"].c_str(), nullptr));
        CHECK(CreateSymbolicLinkW(sandbox[L"alias"].c_str(), sandbox[L"sub"].c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY));
        CHECK(CreateDirectoryW(sandbox[L"links"].c_str(), nullptr));
        MakeLinkedFile(sandbox, L"links\\twelve", 12);
        MakeLinkedFile(sandbox, L"links\\forty", 40);
    }

    // The whole drive, read through its master file table.
    std::unique_ptr<sm::FolderTree> ScanDrive(Sandbox const& sandbox)
    {
        sm::ScanProgress progress;
        sm::ScanResult result = sm::ScanFolder(sandbox.Drive(), progress, std::stop_token{});
        bool const readTheTable = result.tree && result.tree->method == sm::ScanMethod::MasterFileTable;
        CHECK(readTheTable);                      // needs administrator rights and NTFS
        CHECK(result.tree->journal.has_value());  // caught up with the changes made while it was read
        return std::move(result.tree);
    }

    // The sandbox in `drive` - a tree of its whole drive - is what the folder scanner lists now.
    void ExpectSameAsListing(Sandbox const& sandbox, sm::FolderTree const& drive)
    {
        sm::ScanProgress progress;
        sm::ScanOptions options;
        options.masterFileTable = false;
        sm::ScanResult const listed = sm::ScanFolder(sandbox.Folder(), progress, std::stop_token{}, options);
        CHECK(listed.tree != nullptr);
        sm::Folder const* const within = test::FindFolder(*drive.Root(), sandbox.WithinDrive());
        CHECK(within != nullptr);

        std::uint64_t files = 0;
        std::vector<test::Difference> const differences =
            test::CompareTrees(*listed.tree->Root(), *within, sandbox.Folder(), &files);
        for (test::Difference const& difference : differences | std::views::take(10))
        {
            std::printf("      %s: %s (listed %llu, table %llu)\n", difference.what.c_str(),
                        test::ToUtf8(difference.path).c_str(), static_cast<unsigned long long>(difference.listed),
                        static_cast<unsigned long long>(difference.table));
        }
        CHECK(differences.empty());
        CHECK(files > 50);  // the comparison did reach the files made
        sm::Folder const& root = *listed.tree->Root();
        CHECK_EQ(within->SizeTotal(), root.SizeTotal());
        CHECK_EQ(within->logicalTotal, root.logicalTotal);
        CHECK_EQ(within->fileCount, root.fileCount);
        CHECK_EQ(within->folderCount, root.folderCount);
    }

    // Every folder's totals are the sums of what it holds, every entry is in
    // order and knows its place - from the root down.
    void CheckConsistent(sm::Folder const& root)
    {
        std::size_t problems = 0;
        std::vector<sm::Folder const*> pending{ &root };
        while (!pending.empty())
        {
            sm::Folder const& folder = *pending.back();
            pending.pop_back();
            std::uint64_t fileBytes = 0;
            std::uint64_t folderBytes = 0;
            std::uint64_t logical = 0;
            std::uint64_t files = 0;
            std::uint64_t folders = 0;
            for (std::size_t i = 0; i < folder.entries.size(); ++i)
            {
                sm::Entry const& entry = folder.entries[i];
                if (i > 0 && entry.size > folder.entries[i - 1].size)
                {
                    ++problems;  // out of order: largest first
                }
                if (!entry.IsSpecial())
                {
                    logical += entry.logicalSize;
                }
                if (entry.child)
                {
                    sm::Folder const& child = *entry.child;
                    if (child.parent != &folder || child.indexInParent != i || entry.size != child.SizeTotal() ||
                        entry.logicalSize != child.logicalTotal)
                    {
                        ++problems;
                    }
                    folderBytes += entry.size;
                    files += child.fileCount;
                    folders += child.folderCount + 1;
                    pending.push_back(&child);
                }
                else if (entry.kind == sm::EntryKind::File)
                {
                    fileBytes += entry.size;
                    ++files;
                }
            }
            if (fileBytes != folder.sizeFiles || folderBytes != folder.sizeSubfolders ||
                logical != folder.logicalTotal || files != folder.fileCount || folders != folder.folderCount)
            {
                ++problems;
            }
        }
        CHECK_EQ(problems, std::size_t{ 0 });
    }

    // Two copies of a record are the same but for the update sequence array,
    // renewed each time the record is written.
    bool SameRecord(std::span<std::byte const> disk, std::span<std::byte const> current)
    {
        std::size_t const array = Field<std::uint16_t>(disk, 0x04);
        std::size_t const arrayEnd = array + Field<std::uint16_t>(disk, 0x06) * std::size_t{ 2 };
        std::size_t const used = Field<std::uint32_t>(disk, 0x18);
        return used <= disk.size() && used <= current.size() && arrayEnd <= used &&
               Field<std::uint32_t>(current, 0x18) == used &&
               std::equal(disk.begin(), disk.begin() + static_cast<std::ptrdiff_t>(array), current.begin()) &&
               std::equal(disk.begin() + static_cast<std::ptrdiff_t>(arrayEnd),
                          disk.begin() + static_cast<std::ptrdiff_t>(used),
                          current.begin() + static_cast<std::ptrdiff_t>(arrayEnd));
    }
}

VOLUME_TEST_CASE(MasterFileTableIsReadInFull)
{
    std::optional<sm::ntfs::Volume> const volume = sm::ntfs::Volume::Open(TempDrive());
    bool const opened = volume.has_value();
    CHECK(opened);  // needs administrator rights and NTFS
    std::uint32_t const size = volume->RecordSize();
    std::uint64_t records = 0;
    std::uint64_t formatted = 0;
    std::uint64_t misplaced = 0;
    std::uint64_t compared = 0;
    std::uint64_t differing = 0;
    std::vector<std::byte> current;
    sm::ntfs::Volume::ReadStatus const status =
        volume->ReadAll(std::stop_token{}, [&](std::uint64_t first, std::span<std::byte> block) {
            for (std::size_t i = 0; i < block.size() / size; ++i)
            {
                std::uint64_t const number = first + i;
                std::span<std::byte> const record = block.subspan(i * size, size);
                ++records;
                // Each record NTFS has formatted holds its own number (NTFS 3.1,
                // array at 0x30): proof that it was read from the right place.
                if (Field<std::uint16_t>(record, 0x04) < 0x30 || !sm::ntfs::ApplyFixups(record))
                {
                    continue;
                }
                ++formatted;
                if (Field<std::uint32_t>(record, 0x2C) != static_cast<std::uint32_t>(number))
                {
                    if (++misplaced <= 5)
                    {
                        std::printf("      record %llu holds record %lu\n", static_cast<unsigned long long>(number),
                                    static_cast<unsigned long>(Field<std::uint32_t>(record, 0x2C)));
                    }
                    continue;
                }
                // Now and then, the same record through the file system: unless
                // it changed since it was written (its log sequence number moved
                // on), it is the same.
                if (number % 997 != 0 || (Field<std::uint16_t>(record, 0x16) & 1) == 0 ||
                    volume->ReadCurrentRecord(number, current) != sm::ntfs::Volume::RecordRead::Ok ||
                    Field<std::uint64_t>(current, 0x08) != Field<std::uint64_t>(record, 0x08))
                {
                    continue;
                }
                ++compared;
                if (!SameRecord(record, current) && ++differing <= 5)
                {
                    std::printf("      record %llu differs from the file system's copy\n",
                                static_cast<unsigned long long>(number));
                }
            }
        });
    std::printf("      %llu records, %llu formatted, %llu compared with the file system's copy\n",
                static_cast<unsigned long long>(records), static_cast<unsigned long long>(formatted),
                static_cast<unsigned long long>(compared));
    CHECK(status == sm::ntfs::Volume::ReadStatus::Done);
    CHECK_EQ(records, volume->RecordCount());
    CHECK(formatted > sm::ntfs::kFirstUserRecord);
    CHECK_EQ(misplaced, std::uint64_t{ 0 });
    CHECK(compared > 0);
    CHECK_EQ(differing, std::uint64_t{ 0 });
}

VOLUME_TEST_CASE(MasterFileTableScanMatchesTheFolderScanner)
{
    Sandbox const sandbox;
    MakeFiles(sandbox);
    std::unique_ptr<sm::FolderTree> const drive = ScanDrive(sandbox);
    ExpectSameAsListing(sandbox, *drive);
    CheckConsistent(*drive->Root());
}

VOLUME_TEST_CASE(RescanSeesFilesGrowingWhileOpen)
{
    // The journal names a changed file once until it is closed.
    Sandbox const sandbox;
    // A download in progress: written before the scan, and still after it.
    wil::unique_hfile const download(CreateFileW(sandbox[L"download.part"].c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                                 nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    // A virtual machine's disk, open since long before the scan.
    wil::unique_hfile const disk(CreateFileW(sandbox[L"disk.vhdx"].c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    CHECK(download.is_valid() && disk.is_valid());
    Append(download.get(), std::uint64_t{ 1 } << 20);
    Append(disk.get(), std::uint64_t{ 20 } << 20);

    std::unique_ptr<sm::FolderTree> const drive = ScanDrive(sandbox);
    sm::JournalPosition position = *drive->journal;
    std::uint32_t const downloadRecord = static_cast<std::uint32_t>(RecordOf(sandbox[L"download.part"]));
    std::uint32_t const diskRecord = static_cast<std::uint32_t>(RecordOf(sandbox[L"disk.vhdx"]));
    CHECK(std::ranges::find(position.openFiles, downloadRecord) != position.openFiles.end());
    // Opened long before: the journal never saw the disk change, as far as the scan knows.
    std::erase(position.openFiles, diskRecord);

    Append(download.get(), std::uint64_t{ 10 } << 20);
    Append(disk.get(), std::uint64_t{ 10 } << 20);
    sm::JournalRead const read = sm::ReadJournalChanges(position, std::stop_token{}, 0, 0, sm::LargeFiles(*drive));
    CHECK(read.status == sm::JournalReadStatus::Ready);
    sm::ApplyJournalChanges(*drive, read.changes);

    sm::Folder const* const within = test::FindFolder(*drive->Root(), sandbox.WithinDrive());
    CHECK(within != nullptr);
    std::uint64_t downloaded = 0;
    std::uint64_t diskSize = 0;
    for (sm::Entry const& entry : within->entries)
    {
        if (within->Name(entry) == L"download.part")
        {
            downloaded = entry.size;
        }
        else if (within->Name(entry) == L"disk.vhdx")
        {
            diskSize = entry.size;
        }
    }
    CHECK(downloaded >= (std::uint64_t{ 11 } << 20));
    CHECK(diskSize >= (std::uint64_t{ 30 } << 20));
    CHECK(std::ranges::find(read.changes.position.openFiles, downloadRecord) != read.changes.position.openFiles.end());
    CheckConsistent(*drive->Root());
}

VOLUME_TEST_CASE(ChangeJournalRescanMatchesTheFolderScanner)
{
    Sandbox const sandbox;
    MakeFiles(sandbox);
    std::unique_ptr<sm::FolderTree> const drive = ScanDrive(sandbox);
    sm::JournalPosition const scanned = *drive->journal;

    // Files and folders made, renamed, moved, grown, shrunk and deleted.
    CHECK(MoveFileW(sandbox[L"tiny.txt"].c_str(), sandbox[L"renamed.txt"].c_str()));
    {
        wil::unique_hfile const file(
            CreateFileW(sandbox[L"big.bin"].c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        CHECK(file.is_valid());
        CHECK(SetFilePointerEx(file.get(), LARGE_INTEGER{}, nullptr, FILE_END));
        Append(file.get(), 1 << 20);
        CHECK(FlushFileBuffers(file.get()));
    }
    MakeFile(sandbox[L"empty.txt"], 70'000);
    MakeFile(sandbox[L"na\u00EFve \u2713 \u65E5\u672C.txt"], 10);
    CHECK(DeleteFileW(sandbox[L"compressed.txt"].c_str()));
    CHECK(CreateDirectoryW(sandbox[L"new"].c_str(), nullptr));
    MakeFile(sandbox[L"new\\fresh.txt"], 5000);
    CHECK(MoveFileW(sandbox[L"sub"].c_str(), sandbox[L"new\\sub"].c_str()));  // with what it holds
    CHECK(MoveFileW(sandbox[L"a"].c_str(), sandbox[L"a2"].c_str()));
    CHECK(DeleteFileW(sandbox[L"link to linked.bin"].c_str()));  // one of two names
    CHECK(RemoveDirectoryW(sandbox[L"empty"].c_str()));
    MakeFile(sandbox[L"short-lived.txt"], 100);
    CHECK(DeleteFileW(sandbox[L"short-lived.txt"].c_str()));
    CHECK(SetFileAttributesW(sandbox[L"hidden.txt"].c_str(), FILE_ATTRIBUTE_NORMAL));
    for (int link = 0; link < 10; ++link)
    {
        CHECK(DeleteFileW(sandbox[LinkName(L"links\\forty", link)].c_str()));
    }

    sm::JournalRead const read = sm::ReadJournalChanges(scanned, std::stop_token{});
    CHECK(read.status == sm::JournalReadStatus::Ready);
    auto const changed = [&read](std::wstring const& path) {
        std::uint64_t const record = RecordOf(path);
        return std::ranges::any_of(read.changes.files,
                                   [record](sm::ChangedFile const& file) { return file.record == record; });
    };
    CHECK(changed(sandbox[L"renamed.txt"]));
    CHECK(changed(sandbox[L"new\\fresh.txt"]));
    CHECK(changed(sandbox[L"new\\sub"]));
    CHECK(changed(sandbox[L"links\\forty"]));
    sm::ApplyJournalChanges(*drive, read.changes);
    CHECK(drive->journal.has_value() && drive->journal->nextUsn > scanned.nextUsn);
    ExpectSameAsListing(sandbox, *drive);
    CheckConsistent(*drive->Root());
}
