// Tests for reading NTFS directly: file records, run lists, the tree built from
// them, and patching a tree with what the change journal reports.  The records
// are written here byte by byte, laid out as NTFS stores them on the disk.
#include "pch.h"
#include "TestHarness.h"

#include "../../src/Core/ChangeJournal.h"
#include "../../src/Core/Ntfs.h"

namespace
{
    constexpr std::uint16_t kInUse = 0x0001;
    constexpr std::uint16_t kDirectory = 0x0002;

    // Writes a file record the way NTFS lays it out: header, update sequence
    // array, attributes, end marker.
    class RecordWriter
    {
    public:
        explicit RecordWriter(std::uint16_t flags = kInUse, std::size_t size = 1024) : bytes_(size)
        {
            Put<std::uint32_t>(0x00, 0x454C4946);  // "FILE"
            Put<std::uint16_t>(0x04, 0x30);        // update sequence array offset
            Put<std::uint16_t>(0x06, static_cast<std::uint16_t>(size / 512 + 1));
            Put<std::uint16_t>(0x14, 0x48);        // first attribute
            Put<std::uint16_t>(0x16, flags);
            Put<std::uint32_t>(0x1C, static_cast<std::uint32_t>(size));
            next_ = 0x48;
        }

        RecordWriter& Base(std::uint64_t record)
        {
            Put<std::uint64_t>(0x20, record | (std::uint64_t{ 1 } << 48));  // with a sequence number, as on disk
            return *this;
        }

        RecordWriter& StandardInformation(std::uint64_t lastWrite, std::uint32_t attributes)
        {
            std::size_t const value = Resident(0x10, 0x48, {});
            Put<std::uint64_t>(value + 0x08, lastWrite);
            Put<std::uint32_t>(value + 0x20, attributes);
            return *this;
        }

        RecordWriter& Name(std::uint64_t parent, std::wstring_view name, std::uint8_t nameSpace = 1,
                           std::uint32_t attributes = 0, std::uint32_t reparseTag = 0)
        {
            std::size_t const value = Resident(0x30, 0x42 + name.size() * 2, {});
            Put<std::uint64_t>(value + 0x00, parent | (std::uint64_t{ 3 } << 48));
            Put<std::uint32_t>(value + 0x38, attributes);
            Put<std::uint32_t>(value + 0x3C, reparseTag);
            Put<std::uint8_t>(value + 0x40, static_cast<std::uint8_t>(name.size()));
            Put<std::uint8_t>(value + 0x41, nameSpace);
            for (std::size_t i = 0; i < name.size(); ++i)
            {
                Put<std::uint16_t>(value + 0x42 + i * 2, static_cast<std::uint16_t>(name[i]));
            }
            return *this;
        }

        RecordWriter& ResidentData(std::size_t length, std::wstring_view stream = {})
        {
            std::size_t const value = Resident(0x80, length, stream);
            for (std::size_t i = 0; i < length; ++i)
            {
                Put<std::uint8_t>(value + i, static_cast<std::uint8_t>(i * 7 + 1));  // never the check number
            }
            return *this;
        }

        RecordWriter& Data(std::uint64_t allocated, std::uint64_t size, std::uint16_t flags = 0,
                           std::uint64_t totalAllocated = 0, std::wstring_view stream = {}, std::uint64_t lowestVcn = 0)
        {
            NonResident(0x80, allocated, size, flags, totalAllocated, stream, lowestVcn);
            return *this;
        }

        // An attribute list too long for the record, kept on the disk.
        RecordWriter& AttributeListOnDisk(std::uint64_t length)
        {
            NonResident(0x20, 4096, length, 0, 0, {}, 0);
            return *this;
        }

        RecordWriter& ReparsePoint(std::uint32_t tag)
        {
            std::size_t const value = Resident(0xC0, 8, {});
            Put<std::uint32_t>(value, tag);
            return *this;
        }

        RecordWriter& AttributeList(std::vector<std::uint64_t> const& records)
        {
            std::size_t const value = Resident(0x20, records.size() * 0x20, {});
            for (std::size_t i = 0; i < records.size(); ++i)
            {
                std::size_t const entry = value + i * 0x20;
                Put<std::uint32_t>(entry + 0x00, 0x80);
                Put<std::uint16_t>(entry + 0x04, 0x20);
                Put<std::uint64_t>(entry + 0x10, records[i] | (std::uint64_t{ 1 } << 48));
            }
            return *this;
        }

        // The record as the parser reads it: fixups undone, the update
        // sequence array still holding the check number and the original end
        // of each 512-byte block.
        std::vector<std::byte> Clean(std::uint16_t check = 0x0BAD)
        {
            Put<std::uint32_t>(next_, 0xFFFFFFFF);
            Put<std::uint32_t>(0x18, static_cast<std::uint32_t>(next_ + 8));
            Put<std::uint16_t>(0x30, check);
            for (std::size_t i = 1; i <= bytes_.size() / 512; ++i)
            {
                std::uint16_t original = 0;
                std::memcpy(&original, bytes_.data() + i * 512 - 2, 2);
                Put<std::uint16_t>(0x30 + i * 2, original);
            }
            return bytes_;
        }

        // The record as it sits on the disk: the end of each block swapped
        // for the check number.
        std::vector<std::byte> OnDisk(std::uint16_t check = 0x0BAD)
        {
            std::vector<std::byte> record = Clean(check);
            for (std::size_t i = 1; i <= record.size() / 512; ++i)
            {
                std::memcpy(record.data() + i * 512 - 2, &check, sizeof(check));
            }
            return record;
        }

    private:
        template <typename T>
        void Put(std::size_t offset, T value)
        {
            CHECK(offset + sizeof(T) <= bytes_.size());
            std::memcpy(bytes_.data() + offset, &value, sizeof(T));
        }

        std::size_t Attribute(std::uint32_t type, std::size_t length, bool nonResident, std::wstring_view name,
                              std::size_t nameOffset, std::uint16_t flags = 0)
        {
            length = (length + 7) / 8 * 8;
            std::size_t const at = next_;
            Put<std::uint32_t>(at + 0x00, type);
            Put<std::uint32_t>(at + 0x04, static_cast<std::uint32_t>(length));
            Put<std::uint8_t>(at + 0x08, nonResident ? 1 : 0);
            Put<std::uint8_t>(at + 0x09, static_cast<std::uint8_t>(name.size()));
            Put<std::uint16_t>(at + 0x0A, static_cast<std::uint16_t>(nameOffset));
            Put<std::uint16_t>(at + 0x0C, flags);
            for (std::size_t i = 0; i < name.size(); ++i)
            {
                Put<std::uint16_t>(at + nameOffset + i * 2, static_cast<std::uint16_t>(name[i]));
            }
            next_ += length;
            return at;
        }

        // A non-resident attribute: its sizes, and one run of one cluster at cluster 16.
        void NonResident(std::uint32_t type, std::uint64_t allocated, std::uint64_t size, std::uint16_t flags,
                         std::uint64_t totalAllocated, std::wstring_view stream, std::uint64_t lowestVcn)
        {
            bool const counted = (flags & 0x80FF) != 0;
            std::size_t const nameOffset = counted ? 0x48 : 0x40;
            std::size_t const pairsOffset = (nameOffset + stream.size() * 2 + 7) / 8 * 8;
            std::size_t const length = pairsOffset + 8;  // one run and the terminator
            std::size_t const at = Attribute(type, length, true, stream, nameOffset, flags);
            Put<std::uint64_t>(at + 0x10, lowestVcn);
            Put<std::uint16_t>(at + 0x20, static_cast<std::uint16_t>(pairsOffset));
            Put<std::uint64_t>(at + 0x28, allocated);
            Put<std::uint64_t>(at + 0x30, size);
            Put<std::uint64_t>(at + 0x38, size);
            if (counted)
            {
                Put<std::uint64_t>(at + 0x40, totalAllocated);
            }
            Put<std::uint8_t>(at + pairsOffset, 0x11);      // one run: 1 byte of length, 1 of offset
            Put<std::uint8_t>(at + pairsOffset + 1, 0x01);  // 1 cluster
            Put<std::uint8_t>(at + pairsOffset + 2, 0x10);  // at cluster 16
        }

        // A resident attribute; returns where its value starts.
        std::size_t Resident(std::uint32_t type, std::size_t valueLength, std::wstring_view name)
        {
            std::size_t const valueOffset = (0x18 + name.size() * 2 + 7) / 8 * 8;
            std::size_t const at = Attribute(type, valueOffset + valueLength, false, name, 0x18);
            Put<std::uint32_t>(at + 0x10, static_cast<std::uint32_t>(valueLength));
            Put<std::uint16_t>(at + 0x14, static_cast<std::uint16_t>(valueOffset));
            return at + valueOffset;
        }

        std::vector<std::byte> bytes_;
        std::size_t next_ = 0;
    };

    sm::ntfs::FileRecord Parse(std::vector<std::byte> const& bytes, std::uint64_t number)
    {
        sm::ntfs::FileRecord record;
        CHECK(sm::ntfs::ParseRecord(bytes, number, record));
        return record;
    }

    sm::Entry const* Find(sm::Folder const& folder, std::wstring_view name)
    {
        for (sm::Entry const& entry : folder.entries)
        {
            if (folder.Name(entry) == name)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    constexpr std::uint64_t kTime = 133'000'000'000'000'000;
}

TEST_CASE(FixupsUndoTheUpdateSequence)
{
    RecordWriter writer;
    writer.StandardInformation(kTime, FILE_ATTRIBUTE_ARCHIVE).Name(5, L"file.txt").ResidentData(600);
    std::vector<std::byte> const clean = writer.Clean();
    std::vector<std::byte> disk = writer.OnDisk();
    CHECK(disk != clean);
    CHECK(sm::ntfs::ApplyFixups(disk));
    CHECK(disk == clean);

    // A block whose check number does not match was written by another write: the record is torn.
    std::vector<std::byte> torn = writer.OnDisk();
    torn[1022] = std::byte{ 0x42 };
    CHECK(!sm::ntfs::ApplyFixups(torn));

    std::vector<std::byte> notARecord(1024);
    CHECK(!sm::ntfs::ApplyFixups(notARecord));
}

TEST_CASE(RunListsAreDecoded)
{
    // 16 clusters at 256; 5 at 256 - 16 = 240 (a negative step); 8 sparse; end.
    std::array const runs{ std::byte{ 0x21 }, std::byte{ 0x10 }, std::byte{ 0x00 }, std::byte{ 0x01 },
                           std::byte{ 0x11 }, std::byte{ 0x05 }, std::byte{ 0xF0 },
                           std::byte{ 0x01 }, std::byte{ 0x08 },
                           std::byte{ 0x00 } };
    std::vector<sm::ntfs::Extent> extents;
    CHECK(sm::ntfs::DecodeRuns(runs, 100, extents));
    CHECK_EQ(extents.size(), std::size_t{ 3 });
    CHECK_EQ(extents[0].vcn, std::uint64_t{ 100 });
    CHECK_EQ(extents[0].clusters, std::uint64_t{ 16 });
    CHECK_EQ(extents[0].lcn, std::int64_t{ 256 });
    CHECK_EQ(extents[1].vcn, std::uint64_t{ 116 });
    CHECK_EQ(extents[1].lcn, std::int64_t{ 240 });
    CHECK_EQ(extents[2].vcn, std::uint64_t{ 121 });
    CHECK_EQ(extents[2].lcn, std::int64_t{ -1 });

    std::vector<sm::ntfs::Extent> ignored;
    std::array const truncated{ std::byte{ 0x31 }, std::byte{ 0x10 } };
    CHECK(!sm::ntfs::DecodeRuns(truncated, 0, ignored));
    std::array const noLength{ std::byte{ 0x10 }, std::byte{ 0x01 } };
    CHECK(!sm::ntfs::DecodeRuns(noLength, 0, ignored));
    std::array const beforeTheStart{ std::byte{ 0x11 }, std::byte{ 0x01 }, std::byte{ 0xFF } };  // cluster -1
    CHECK(!sm::ntfs::DecodeRuns(beforeTheStart, 0, ignored));
}

TEST_CASE(FileRecordsAreRead)
{
    RecordWriter writer;
    writer.StandardInformation(kTime, FILE_ATTRIBUTE_ARCHIVE | 0x00040000)
        .Name(1234, L"REPORT~1.DOC", 2)  // the 8.3 alias: left out
        .Name(1234, L"Quarterly report.docx")
        .Data(8192, 5000);
    sm::ntfs::FileRecord const file = Parse(writer.Clean(), 77);
    CHECK(file.inUse);
    CHECK(!file.directory);
    CHECK_EQ(file.number, std::uint64_t{ 77 });
    CHECK_EQ(file.base, std::uint64_t{ 0 });
    CHECK_EQ(file.names.size(), std::size_t{ 1 });
    CHECK_EQ(file.names[0].parent, std::uint64_t{ 1234 });
    CHECK(file.Name(file.names[0]) == L"Quarterly report.docx");
    CHECK_EQ(file.lastWriteTime, kTime);
    CHECK_EQ(file.ListedAttributes(), static_cast<std::uint32_t>(FILE_ATTRIBUTE_ARCHIVE));  // as a folder listing shows them
    CHECK_EQ(file.length, std::uint64_t{ 5000 });
    CHECK_EQ(file.SizeOnDisk(), std::uint64_t{ 8192 });
    CHECK(!file.IsAlias());

    // A small file kept in its record counts its length rounded up to 8 bytes, as listings do.
    RecordWriter resident;
    resident.StandardInformation(kTime, 0).Name(5, L"tiny").ResidentData(13);
    sm::ntfs::FileRecord const tiny = Parse(resident.Clean(), 78);
    CHECK_EQ(tiny.length, std::uint64_t{ 13 });
    CHECK_EQ(tiny.SizeOnDisk(), std::uint64_t{ 16 });
    CHECK_EQ(tiny.ListedAttributes(), static_cast<std::uint32_t>(FILE_ATTRIBUTE_NORMAL));

    // Compressed and sparse files take what is really allocated.
    RecordWriter compressed;
    compressed.StandardInformation(kTime, FILE_ATTRIBUTE_COMPRESSED).Name(5, L"log.txt").Data(65536, 60000, 0x0001, 12288);
    CHECK_EQ(Parse(compressed.Clean(), 79).SizeOnDisk(), std::uint64_t{ 12288 });

    // Windows' file compression keeps the data in a stream of its own.
    RecordWriter wof;
    wof.StandardInformation(kTime, FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_REPARSE_POINT)
        .Name(5, L"app.exe")
        .Data(0, 50000, 0x8000, 0)
        .Data(20480, 20000, 0, 0, L"WofCompressedData")
        .ReparsePoint(IO_REPARSE_TAG_WOF);
    sm::ntfs::FileRecord const packed = Parse(wof.Clean(), 80);
    CHECK_EQ(packed.length, std::uint64_t{ 50000 });
    CHECK_EQ(packed.SizeOnDisk(), std::uint64_t{ 20480 });
    CHECK(!packed.IsAlias());
    CHECK_EQ(packed.ListedAttributes(), static_cast<std::uint32_t>(FILE_ATTRIBUTE_ARCHIVE));  // as listings show it

    // A junction stands for a folder elsewhere.
    RecordWriter junction(kInUse | kDirectory);
    junction.StandardInformation(kTime, FILE_ATTRIBUTE_REPARSE_POINT).Name(5, L"Link").ReparsePoint(IO_REPARSE_TAG_MOUNT_POINT);
    sm::ntfs::FileRecord const link = Parse(junction.Clean(), 81);
    CHECK(link.directory);
    CHECK(link.IsAlias());
    CHECK((link.ListedAttributes() & FILE_ATTRIBUTE_DIRECTORY) != 0);

    // A hard-linked file has a name in each folder.
    RecordWriter linked;
    linked.StandardInformation(kTime, 0).Name(30, L"a.bin").Name(31, L"copy of a.bin").Data(4096, 4000);
    CHECK_EQ(Parse(linked.Clean(), 82).names.size(), std::size_t{ 2 });

    // Extension records and attribute lists.
    RecordWriter extension;
    extension.Base(90).Data(1 << 20, 1 << 20);
    sm::ntfs::FileRecord const part = Parse(extension.Clean(), 91);
    CHECK_EQ(part.base, std::uint64_t{ 90 });
    RecordWriter listed;
    listed.StandardInformation(kTime, 0).AttributeList({ 90, 91, 92 }).Name(5, L"big.iso");
    sm::ntfs::FileRecord const whole = Parse(listed.Clean(), 90);
    CHECK(whole.hasAttributeList);
    std::vector<std::uint64_t> const others{ 91, 92 };
    CHECK(whole.extensions == others);
    CHECK_EQ(whole.listLength, std::uint64_t{ 0 });

    // A list too long for the record is on the disk: where, so it can be read from there.
    RecordWriter outside;
    outside.StandardInformation(kTime, 0).AttributeListOnDisk(1500).Name(5, L"fragmented.vhdx");
    sm::ntfs::FileRecord const scattered = Parse(outside.Clean(), 93);
    CHECK(scattered.hasAttributeList);
    CHECK(scattered.extensions.empty());
    CHECK_EQ(scattered.listLength, std::uint64_t{ 1500 });
    CHECK_EQ(scattered.listRuns.size(), std::size_t{ 1 });
    CHECK_EQ(scattered.listRuns[0].lcn, std::int64_t{ 16 });
    CHECK_EQ(scattered.listRuns[0].clusters, std::uint64_t{ 1 });

    sm::ntfs::FileRecord unused;
    CHECK(sm::ntfs::ParseRecord(RecordWriter(0).Clean(), 83, unused));
    CHECK(!unused.inUse);

    // A damaged attribute chain is refused rather than read past the record.
    std::vector<std::byte> damaged = RecordWriter().StandardInformation(kTime, 0).Name(5, L"x").Clean();
    std::uint32_t const tooLong = 4000;
    std::memcpy(damaged.data() + 0x48 + 4, &tooLong, sizeof(tooLong));
    sm::ntfs::FileRecord ignored;
    CHECK(!sm::ntfs::ParseRecord(damaged, 84, ignored));
}

namespace
{
    // A small volume: the root (5) holds "Users" (30, holding "file.txt" and
    // "big.iso"), "a.bin" (41, hard-linked into Users too), a junction, and
    // the file system's own $MFT and $Extend; plus a folder whose parent does
    // not exist.
    std::unique_ptr<sm::Folder> BuildVolume(bool extensionFirst)
    {
        std::vector<std::pair<std::uint64_t, std::vector<std::byte>>> records;
        auto const add = [&records](std::uint64_t number, RecordWriter& writer) { records.emplace_back(number, writer.Clean()); };
        RecordWriter root(kInUse | kDirectory);
        root.StandardInformation(kTime, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM).Name(5, L".");
        RecordWriter mft;
        mft.StandardInformation(kTime, 0).Name(5, L"$MFT").Data(1 << 26, 1 << 26);
        RecordWriter extend(kInUse | kDirectory);
        extend.StandardInformation(kTime, 0).Name(5, L"$Extend");
        RecordWriter journal;
        journal.StandardInformation(kTime, 0).Name(11, L"$UsnJrnl").Data(1 << 25, 1 << 25);
        RecordWriter users(kInUse | kDirectory);
        users.StandardInformation(kTime, 0).Name(5, L"Users");
        RecordWriter file;
        file.StandardInformation(kTime, FILE_ATTRIBUTE_ARCHIVE).Name(30, L"file.txt").Data(4096, 3000);
        RecordWriter linked;
        linked.StandardInformation(kTime, 0).Name(5, L"a.bin").Name(30, L"a.bin").Data(8192, 8000);
        RecordWriter junction(kInUse | kDirectory);
        junction.StandardInformation(kTime, FILE_ATTRIBUTE_REPARSE_POINT).Name(5, L"Link").ReparsePoint(IO_REPARSE_TAG_MOUNT_POINT);
        RecordWriter orphan(kInUse | kDirectory);
        orphan.StandardInformation(kTime, 0).Name(999, L"Orphan");
        RecordWriter orphanFile;
        orphanFile.StandardInformation(kTime, 0).Name(43, L"lost.dat").Data(4096, 4096);
        RecordWriter big;
        big.StandardInformation(kTime, 0).AttributeList({ 44, 45 }).Name(30, L"big.iso");
        RecordWriter bigData;
        bigData.Base(44).Data(1 << 20, 1 << 20);

        add(0, mft);
        add(5, root);
        add(11, extend);
        add(30, users);
        add(40, file);
        add(41, linked);
        add(42, junction);
        add(43, orphan);
        add(46, orphanFile);
        add(60, journal);
        if (extensionFirst)
        {
            add(45, bigData);
            add(44, big);
        }
        else
        {
            add(44, big);
            add(45, bigData);
        }

        sm::ntfs::TreeBuilder builder(1000);
        sm::ntfs::FileRecord record;
        for (auto const& [number, bytes] : records)
        {
            record.Clear();
            CHECK(sm::ntfs::ParseRecord(bytes, number, record));
            builder.Add(record);
        }
        return builder.Finish();
    }
}

TEST_CASE(MasterFileTableBecomesTheTree)
{
    for (bool const extensionFirst : { false, true })
    {
        std::unique_ptr<sm::Folder> const root = BuildVolume(extensionFirst);
        CHECK(root != nullptr);
        CHECK_EQ(root->entries.size(), std::size_t{ 2 });  // Users and a.bin: no $MFT, $Extend, Link or Orphan
        sm::Entry const* const users = Find(*root, L"Users");
        CHECK(users != nullptr && users->IsFolder() && users->child);
        CHECK_EQ(users->record, std::uint32_t{ 30 });
        CHECK(Find(*root, L"a.bin") != nullptr);
        CHECK(Find(*root, L"Link") == nullptr);

        sm::Folder const& inside = *users->child;
        CHECK_EQ(inside.parent, root.get());
        sm::Entry const* const file = Find(inside, L"file.txt");
        CHECK(file != nullptr && file->size == 4096 && file->logicalSize == 3000 && file->record == 40);
        CHECK(Find(inside, L"a.bin") != nullptr);  // the second link
        sm::Entry const* const big = Find(inside, L"big.iso");
        CHECK(big != nullptr && big->size == (1u << 20));  // its data is in its extension record

        // Totals: each link counts, like a folder listing.
        CHECK_EQ(inside.SizeTotal(), std::uint64_t{ 4096 + 8192 + (1 << 20) });
        CHECK_EQ(root->SizeTotal(), std::uint64_t{ 8192 + 4096 + 8192 + (1 << 20) });
        CHECK_EQ(root->fileCount, std::uint64_t{ 4 });
        CHECK_EQ(root->folderCount, std::uint64_t{ 1 });
        CHECK_EQ(root->entries.front().size, inside.SizeTotal());  // largest first
    }
}

TEST_CASE(HardLinksAreCountedOnce)
{
    // "a.bin" (8192 bytes on disk) has a name in the root (record 5) and one in Users (30).
    sm::FolderTree tree(BuildVolume(false), L"C:\\", sm::VolumeInfo{}, false);
    tree.rootRecord = 5;
    sm::CountHardLinksOnce(tree);
    sm::Folder& root = *tree.Root();
    sm::Folder const& users = *Find(root, L"Users")->child;
    sm::Entry const* const counted = Find(root, L"a.bin");
    sm::Entry const* const other = Find(users, L"a.bin");
    CHECK(counted != nullptr && counted->size == 8192 && !counted->countedElsewhere);  // nearer the root
    CHECK(other != nullptr && other->size == 0 && other->countedElsewhere && other->logicalSize == 8000);
    std::uint64_t const total = 8192 + 4096 + (1 << 20);
    CHECK_EQ(users.SizeTotal(), std::uint64_t{ 4096 + (1 << 20) });
    CHECK_EQ(root.SizeTotal(), total);
    CHECK_EQ(root.fileCount, std::uint64_t{ 4 });  // still listed under both names

    sm::CountHardLinksOnce(tree);
    CHECK_EQ(root.SizeTotal(), total);

    // When the name it is counted under goes, a remaining one takes the space, as read from the file.
    for (std::size_t i = 0; i < root.entries.size(); ++i)
    {
        if (root.Name(i) == L"a.bin")
        {
            tree.Remove(root, i);
            break;
        }
    }
    std::size_t asked = 0;
    sm::CountHardLinksOnce(tree, [&asked](sm::Folder const&, std::size_t) {
        ++asked;
        return std::optional<std::uint64_t>(8192);
    });
    CHECK_EQ(asked, std::size_t{ 1 });
    CHECK(Find(users, L"a.bin")->size == 8192 && !Find(users, L"a.bin")->countedElsewhere);
    CHECK_EQ(root.SizeTotal(), total);
}

TEST_CASE(ChangeJournalPatchesTheTree)
{
    std::wstring const rootPath = L"C:\\";
    sm::FolderTree tree(BuildVolume(false), rootPath, sm::VolumeInfo{}, false);
    auto const changed = [](std::uint32_t record, bool directory, std::vector<std::pair<std::uint32_t, std::wstring>> names,
                            std::vector<std::uint32_t> formerFolders, std::uint64_t size = 0) {
        sm::ChangedFile file;
        file.record = record;
        file.exists = !names.empty();
        file.directory = directory;
        file.attributes = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
        file.size = size;
        file.length = size;
        file.names = std::move(names);
        file.formerFolders = std::move(formerFolders);
        return file;
    };

    sm::JournalChanges changes;
    changes.position = sm::JournalPosition{ L"\\\\?\\Volume{test}", 7, 4242 };
    // A file in a folder made in the same batch - listed before the folder itself.
    changes.files.push_back(changed(51, false, { { 50, L"new.txt" } }, { 50 }, 1000));
    // "Users" moves into a new folder "Home", contents and all.
    changes.files.push_back(changed(50, true, { { 5, L"Home" } }, { 5 }));
    changes.files.push_back(changed(30, true, { { 50, L"Users" } }, { 5, 50 }));
    // file.txt is renamed; big.iso grows; a.bin is deleted (both links).
    changes.files.push_back(changed(40, false, { { 30, L"notes.txt" } }, { 30 }, 4096));
    changes.files.push_back(changed(44, false, { { 30, L"big.iso" } }, { 30 }, 2 << 20));
    changes.files.push_back(changed(41, false, {}, { 5, 30 }));
    // Inside the file system's own folders: not part of the tree.
    changes.files.push_back(changed(61, false, { { 11, L"$Secure" } }, { 11 }, 4096));
    sm::ApplyJournalChanges(tree, changes);

    sm::Folder const& root = *tree.Root();
    CHECK_EQ(root.entries.size(), std::size_t{ 1 });
    sm::Entry const* const home = Find(root, L"Home");
    CHECK(home != nullptr && home->child);
    sm::Folder const& homeFolder = *home->child;
    sm::Entry const* const users = Find(homeFolder, L"Users");
    CHECK(users != nullptr && users->child && users->child->parent == &homeFolder);
    CHECK(Find(homeFolder, L"new.txt") != nullptr);
    sm::Folder const& usersFolder = *users->child;
    CHECK(Find(usersFolder, L"file.txt") == nullptr);
    CHECK(Find(usersFolder, L"notes.txt") != nullptr);
    CHECK(Find(usersFolder, L"a.bin") == nullptr);
    CHECK_EQ(Find(usersFolder, L"big.iso")->size, std::uint64_t{ 2 << 20 });

    // Totals are right all the way up.
    CHECK_EQ(usersFolder.SizeTotal(), std::uint64_t{ 4096 + (2 << 20) });
    CHECK_EQ(homeFolder.SizeTotal(), usersFolder.SizeTotal() + 1000);
    CHECK_EQ(root.SizeTotal(), homeFolder.SizeTotal());
    CHECK_EQ(root.fileCount, std::uint64_t{ 3 });
    CHECK_EQ(root.folderCount, std::uint64_t{ 2 });
    CHECK(tree.journal.has_value() && tree.journal->nextUsn == 4242);

    // Applying the same changes again changes nothing.
    sm::ApplyJournalChanges(tree, changes);
    CHECK_EQ(tree.Root()->SizeTotal(), homeFolder.SizeTotal());
    CHECK_EQ(Find(*tree.Root(), L"Home")->child->entries.size(), std::size_t{ 2 });

    // A new file with a name in Home (record 50) and one in Home\Users (record 30):
    // counted under the name nearer the root, though its folder's record is higher.
    std::uint64_t const before = tree.Root()->SizeTotal();
    sm::JournalChanges linked;
    linked.files.push_back(changed(70, false, { { 30, L"shared.bin" }, { 50, L"shared.bin" } }, { 30, 50 }, 65536));
    sm::ApplyJournalChanges(tree, linked);
    sm::Folder const& homeNow = *Find(*tree.Root(), L"Home")->child;
    sm::Folder const& usersNow = *Find(homeNow, L"Users")->child;
    CHECK(Find(homeNow, L"shared.bin")->size == 65536 && !Find(homeNow, L"shared.bin")->countedElsewhere);
    CHECK(Find(usersNow, L"shared.bin")->size == 0 && Find(usersNow, L"shared.bin")->countedElsewhere);
    CHECK_EQ(tree.Root()->SizeTotal(), before + 65536);

    // Deleting "Home" takes everything in it.
    sm::JournalChanges gone;
    gone.files.push_back(changed(50, true, {}, { 5 }));
    sm::ApplyJournalChanges(tree, gone);
    CHECK(tree.Root()->entries.empty());
    CHECK_EQ(tree.Root()->SizeTotal(), std::uint64_t{ 0 });
}
