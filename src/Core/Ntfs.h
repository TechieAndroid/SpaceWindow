// SpaceWindow - reading NTFS volumes directly.
//
// New in SpaceWindow.  Most of a scan's time goes into listing folders one by
// one.  NTFS keeps every file of a drive in one table, the master file table
// (MFT), and reading that table from start to end yields the same tree several
// times faster - the way WizTree and Everything work.  Opening a volume for
// direct reading takes administrator rights, and only NTFS has an MFT, so the
// folder-by-folder scanner remains for everything else.
//
// The scan builds exactly the tree the folder scanner would: the same sizes
// (the space each file's data takes on disk), hard links counted once per
// link, junctions and symbolic links skipped, and the file system's own files
// ($MFT, $LogFile, ...) left to the "unscanned" block.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <wil/resource.h>

#include "FolderTree.h"

namespace sm::ntfs
{
    // A file's identity on a volume: its record number in the MFT (48 bits)
    // and a sequence number that changes each time the record is reused.
    constexpr std::uint64_t RecordOf(std::uint64_t fileReference) noexcept
    {
        return fileReference & 0x0000'FFFF'FFFF'FFFFull;
    }

    // A record number as a tree keeps it (Entry::record); nothing if it does
    // not fit (no NTFS volume has 4 billion files, but a damaged one may say so).
    constexpr std::optional<std::uint32_t> Record32(std::uint64_t record) noexcept
    {
        if (record > UINT32_MAX)
        {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(record);
    }

    inline constexpr std::uint64_t kRootRecord = 5;        // the root folder
    inline constexpr std::uint64_t kFirstUserRecord = 24;  // records below belong to the file system itself

    // One name of a file: a hard-linked file has one per link.  Short (8.3)
    // names are left out.
    struct FileName
    {
        std::uint64_t parent = 0;  // record number of the folder holding the name
        std::uint32_t offset = 0;  // in FileRecord::nameText
        std::uint16_t length = 0;
    };

    // A run of clusters of a non-resident attribute.
    struct Extent
    {
        std::uint64_t vcn = 0;  // first cluster within the attribute
        std::uint64_t clusters = 0;
        std::int64_t lcn = -1;  // first cluster on the volume; -1 for a sparse run (no clusters)
    };

    // What a file's record - and its extension records, once merged - say about it.
    struct FileRecord
    {
        std::uint64_t number = 0;
        std::uint64_t base = 0;  // an extension record: the record number of its base record
        bool inUse = false;
        bool directory = false;
        bool hasAttributeList = false;  // some attributes live in extension records
        bool hasStandardInformation = false;
        bool hasData = false;          // the sizes of the unnamed data stream were found
        std::uint32_t attributes = 0;  // FILE_ATTRIBUTE_*, without FILE_ATTRIBUTE_DIRECTORY
        std::uint32_t reparseTag = 0;
        std::uint64_t lastWriteTime = 0;  // FILETIME
        std::uint64_t allocated = 0;      // bytes on disk of the unnamed data stream
        std::uint64_t length = 0;         // its length in bytes
        std::uint64_t wofAllocated = 0;   // bytes on disk of "WofCompressedData" (Windows' compressed files)
        std::vector<FileName> names;
        std::wstring nameText;
        std::vector<std::uint64_t> extensions;  // records the attribute list points to
        std::uint64_t listLength = 0;           // an attribute list too long for the record: its length
        std::vector<Extent> listRuns;           // and where it is on the disk

        std::wstring_view Name(FileName const& name) const noexcept
        {
            return { nameText.data() + name.offset, name.length };
        }

        // Empties the record, keeping its memory for the next one.
        void Clear() noexcept;

        // Adds what an extension record of this file holds.
        void Merge(FileRecord const& extension);

        // Bytes the file takes on disk, as the folder scanner counts them.
        std::uint64_t SizeOnDisk() const noexcept;

        // FILE_ATTRIBUTE_* as a folder listing shows them.
        std::uint32_t ListedAttributes() const noexcept;

        // A junction, symbolic link or mount point: an alias for data elsewhere, not counted.
        bool IsAlias() const noexcept;
    };

    // Undoes the "update sequence" protection of a record read straight from
    // the disk: the last two bytes of each 512-byte block were swapped for a
    // check number when the record was written.  False if the record is not a
    // file record or was torn by an interrupted write.
    bool ApplyFixups(std::span<std::byte> record) noexcept;

    // Reads a record whose fixups are undone.  `number` is its record number.
    // False if it is not a file record.
    bool ParseRecord(std::span<std::byte const> record, std::uint64_t number, FileRecord& out);

    // Decodes an attribute's "mapping pairs" - its list of runs - starting at
    // cluster `lowestVcn` of the attribute.  False if the list is malformed.
    bool DecodeRuns(std::span<std::byte const> mappingPairs, std::uint64_t lowestVcn, std::vector<Extent>& out);

    // An NTFS volume opened for direct reading.  Nothing is ever written to it.
    class Volume
    {
    public:
        // Opens the volume mounted at `root` (such as L"C:\\"), or the volume
        // `device` (L"\\\\?\\Volume{...}").  Nothing if it is not NTFS, or the
        // process is not running as administrator.
        static std::optional<Volume> Open(std::wstring const& root);
        static std::optional<Volume> OpenDevice(std::wstring const& device);

        std::wstring const& Device() const noexcept { return device_; }
        HANDLE Handle() const noexcept { return handle_.get(); }
        std::uint32_t RecordSize() const noexcept { return recordSize_; }
        std::uint64_t RecordCount() const noexcept { return recordCount_; }

        // Reads records [first, first + count) of the MFT from the disk, as
        // stored (fixups not undone).  `buffer` holds count * RecordSize() bytes.
        bool ReadRecords(std::uint64_t first, std::size_t count, std::span<std::byte> buffer) const;

        enum class ReadStatus
        {
            Done,
            Failed,
            Cancelled,
        };

        // Reads the whole MFT from the disk, in order, a few megabytes at a
        // time with several reads in flight, and hands each block to `consume`
        // (its first record number and its records, as stored) while the next
        // ones are being read.
        ReadStatus ReadAll(std::stop_token const& stop,
                           std::function<void(std::uint64_t, std::span<std::byte>)> const& consume) const;

        enum class RecordRead
        {
            Ok,
            NotInUse,
            Failed,
        };

        // Reads a record through the file system, so it is up to date, with
        // its fixups undone.
        RecordRead ReadCurrentRecord(std::uint64_t number, std::vector<std::byte>& record) const;

        // Reads a file's record and its extension records, through the file
        // system.  `out.inUse` is false if the file no longer exists.  False
        // if the record cannot be read, or the file's names and sizes cannot
        // all be found (its attribute list moved and not yet on the disk).
        bool ReadFile(std::uint64_t number, FileRecord& out) const;

    private:
        Volume() = default;
        bool ReadClusters(std::uint64_t lcn, std::span<std::byte> buffer) const;
        bool ReadAttributeList(FileRecord& file) const;
        bool LoadMftLayout();

        wil::unique_hfile handle_;
        std::wstring device_;
        std::uint32_t bytesPerCluster_ = 0;
        std::uint32_t recordSize_ = 0;
        std::uint64_t recordCount_ = 0;
        std::uint64_t mftStartLcn_ = 0;
        std::vector<Extent> mft_;  // where the MFT is on the volume
    };

    // Builds the folder tree from file records given in any order.
    class TreeBuilder
    {
    public:
        explicit TreeBuilder(std::uint64_t recordCount, ScanProgress* progress = nullptr);
        TreeBuilder(TreeBuilder const&) = delete;
        TreeBuilder& operator=(TreeBuilder const&) = delete;
        ~TreeBuilder();

        void Add(FileRecord const& record);

        // The tree below `rootRecord`, totalled and sorted, each file listed
        // under several names counted under one of them, as CountHardLinksOnce
        // would.  Spread over the processors (Parallel.h).
        std::unique_ptr<Folder> Finish(std::uint64_t rootRecord = kRootRecord);

        // What Finish found out on the way, for the tree: its folders by
        // record and its large files (FolderTree::journalIndex), and whether
        // a file is listed under several names (FolderTree::hardLinks).
        JournalIndex TakeIndex() noexcept { return std::move(index_); }
        bool HardLinks() const noexcept { return hardLinks_; }

    private:
        struct Directory
        {
            std::uint64_t record = 0;
            std::uint64_t parent = 0;
            std::uint64_t lastWriteTime = 0;
            std::uint32_t attributes = 0;
            std::uint32_t nameOffset = 0;  // in directoryNames_
            std::uint16_t nameLength = 0;
            bool placed = false;  // hung in its parent (a damaged volume can name a folder twice)
        };

        // A name of a file listed under several: where its entry went.
        struct Link
        {
            std::uint32_t file = 0;    // the file's record
            std::uint32_t folder = 0;  // the record of the folder that lists it
            std::uint32_t index = 0;   // of its entry there, until Finish sorts the folder
        };

        void Emit(FileRecord const& record);
        Folder& FolderOf(std::uint64_t record);
        void CountLinksOnce(std::span<std::uint32_t const> depths);

        ScanProgress* progress_;
        std::vector<std::unique_ptr<Folder>> folders_;  // by record number, until attached to the tree
        std::vector<Directory> directories_;
        std::wstring directoryNames_;  // the names of directories_, back to back
        std::vector<Link> links_;
        std::unordered_map<std::uint64_t, FileRecord> deferred_;  // files with extension records, by base record
        JournalIndex index_;
        bool hardLinks_ = false;
    };

    // Scans a whole NTFS volume through its MFT.  `root` is its mount point.
    // Nothing if the volume cannot be read directly (not NTFS, not running
    // as administrator, a read error); the caller then lists folders instead.
    std::optional<ScanResult> ScanVolume(std::wstring const& root, VolumeInfo volume, ScanProgress& progress,
                                         std::stop_token const& stop, bool addSpecialEntries);
}
