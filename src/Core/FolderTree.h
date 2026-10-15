// SpaceWindow - folder tree model and parallel disk scanner.
//
// Modernized from FolderTree.h/.cpp of SpaceMonger 1.4.0
// (Copyright (c) 1998-2020 Sean Werkema, MIT License).
//
// What changed, and why:
//  * Scanning runs on a pool of worker threads instead of a recursive
//    function that pumped the message loop.  The recursion kept a 17 KB
//    reparse-point buffer on the stack per level, so a folder tree about 58
//    levels deep overflowed the stack; the pump made the UI re-entrant.
//  * Folders are enumerated in large batches with
//    GetFileInformationByHandleEx(FileFullDirectoryInfo), which reports each
//    file's on-disk allocation.  Compressed, sparse and cloud-placeholder
//    files are therefore measured correctly (1.4.0's README lists sparse
//    files as a known problem), and reparse points no longer need a handle
//    and an FSCTL per entry to be classified.
//  * 1.4.0 meant to round sizes up to the cluster size but rounded down (its
//    power-of-two test could never succeed), so every file smaller than a
//    cluster counted as zero bytes.  The fallback path now rounds up.
//  * Names are UTF-16 and paths use the \\?\ form, so long paths and
//    non-ANSI names work.
//  * Sorting fixes child->indexInParent (the radix sort stored the source
//    index instead of the destination index).
//  * Trees are destroyed iteratively: deep trees cannot overflow the stack.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace sm
{
    struct Folder;
    class FolderWatch;

    enum class EntryKind : std::uint8_t
    {
        File,
        Folder,
        FreeSpace,  // pseudo-entry in the root of a drive: the drive's free space
        Unscanned,  // pseudo-entry in the root of a drive: used space no scanned file accounts for
    };

    struct Entry
    {
        std::uint64_t size = 0;         // bytes allocated on disk (a folder: everything below it)
        std::uint64_t logicalSize = 0;  // file length (a folder: everything below it)
        std::uint64_t lastWriteTime = 0;  // FILETIME, UTC
        std::uint32_t attributes = 0;   // FILE_ATTRIBUTE_*
        std::uint32_t nameOffset = 0;   // offset of the name in Folder::names
        std::uint16_t nameLength = 0;   // length of the name in UTF-16 code units
        EntryKind kind = EntryKind::File;
        bool countedElsewhere = false;  // a hard link whose file's space is counted under another of its names
        std::uint32_t record = 0;       // NTFS master file table record (0: unknown)
        std::unique_ptr<Folder> child;  // contents of a sub-folder

        bool IsFolder() const noexcept { return kind == EntryKind::Folder; }
        bool IsSpecial() const noexcept { return kind == EntryKind::FreeSpace || kind == EntryKind::Unscanned; }
    };
    // `countedElsewhere` and `record` fit in what was padding: millions of entries are held at once.
    static_assert(sizeof(void*) != 8 || sizeof(Entry) == 48);

    struct Folder
    {
        // Folders are linked to their parents by address, so they never move.
        Folder() = default;
        ~Folder();
        Folder(Folder const&) = delete;
        Folder& operator=(Folder const&) = delete;
        Folder(Folder&&) = delete;
        Folder& operator=(Folder&&) = delete;

        Folder* parent = nullptr;
        std::uint32_t indexInParent = 0;   // index of this folder's entry in parent->entries
        std::vector<Entry> entries;        // largest first, once the folder is finalized
        std::wstring names;                // the names of all entries, back to back

        std::uint64_t sizeFiles = 0;       // on-disk bytes of the files directly in this folder
        std::uint64_t sizeSubfolders = 0;  // on-disk bytes of all sub-folders, recursively
        std::uint64_t logicalTotal = 0;    // logical bytes of everything below this folder
        std::uint64_t fileCount = 0;       // files below this folder, recursively
        std::uint64_t folderCount = 0;     // sub-folders below this folder, recursively
        bool unreadable = false;           // the folder could not be listed (e.g. access denied)

        // Scanner bookkeeping: one for the folder's own listing plus one per
        // sub-folder still being scanned.  The last one to finish finalizes.
        std::atomic<std::uint32_t> pending{ 1 };
        std::atomic<bool> listed{ false };  // its own listing is done: its entries stay as they are (live maps)

        std::uint64_t SizeTotal() const noexcept { return sizeFiles + sizeSubfolders; }

        std::wstring_view Name(Entry const& entry) const noexcept
        {
            return { names.data() + entry.nameOffset, entry.nameLength };
        }
        std::wstring_view Name(std::size_t index) const noexcept { return Name(entries[index]); }
    };

    // Re-sorts a folder's entries, largest first, and re-links the children.
    void SortEntries(Folder& folder);

    // Recomputes a folder's totals from its entries (the totals of its
    // sub-folders must be up to date) and sorts it.
    void UpdateTotals(Folder& folder);

    // New totals for folders whose entries changed and for every folder above
    // them, deepest first.  Folders no longer below `root` are left alone.
    void UpdateTotalsUpward(Folder& root, std::span<Folder* const> changed);

    struct VolumeInfo
    {
        std::wstring root;                 // mount point, e.g. L"C:\\" (empty if unknown)
        std::uint64_t totalBytes = 0;
        std::uint64_t freeBytes = 0;
        std::uint64_t clusterSize = 0;
        std::wstring fileSystem;           // L"NTFS", L"ReFS", ... (empty if unknown)
        bool remote = false;
        std::optional<bool> seekPenalty;   // true for spinning disks, unknown for network/virtual volumes

        std::uint64_t UsedBytes() const noexcept { return totalBytes > freeBytes ? totalBytes - freeBytes : 0; }
    };

    class FolderTree;

    // Shared between the scanner threads and the UI thread.
    class ScanProgress
    {
    public:
        std::atomic<std::uint64_t> files{ 0 };
        std::atomic<std::uint64_t> folders{ 0 };
        std::atomic<std::uint64_t> bytes{ 0 };
        std::atomic<std::uint64_t> expectedBytes{ 0 };  // used bytes of the drive when scanning a whole drive

        std::wstring CurrentPath() const;
        void SetCurrentPath(std::wstring_view path);

        // While folders are listed: what was found so far, as a tree to draw
        // a live map from (up to `budget` entries; folders beyond show as
        // boxes of their size).  Nothing at other times.  Any thread.
        std::unique_ptr<FolderTree> Snapshot(std::size_t budget) const;

        // For the scanner, while it lists folders.
        using SnapshotSource = std::function<std::unique_ptr<FolderTree>(std::size_t budget)>;
        void SetSnapshotSource(SnapshotSource source);

    private:
        mutable std::mutex mutex_;
        std::wstring currentPath_;
        mutable std::mutex snapshotMutex_;  // held while a snapshot is taken, so its source outlives it
        SnapshotSource snapshot_;
    };

    struct ScanOptions
    {
        unsigned threads = 0;       // 0: choose from the kind of storage
        bool addFreeSpace = true;   // add the free/unscanned pseudo-entries when scanning a whole drive
        bool masterFileTable = true;  // read a whole NTFS drive's master file table, when allowed (Ntfs.h)
        bool watch = false;           // when listing folders: watch them for changes, from the start (FolderWatch.h)
    };

    enum class ScanMethod : std::uint8_t
    {
        Folders,          // every folder listed
        MasterFileTable,  // the NTFS master file table read directly
    };

    // Where a volume's NTFS change journal stood when a tree was last up to
    // date, so that what changed since can be applied to it (ChangeJournal.h).
    struct JournalPosition
    {
        std::wstring device;          // the volume, as \\?\Volume{...}
        std::uint64_t journalId = 0;  // a journal deleted and created again gets a new id
        std::int64_t nextUsn = 0;     // the first change not applied yet
        // Files changed and not closed since: the journal names a file once
        // until it is closed, so these are read again on every rescan.
        std::vector<std::uint32_t> openFiles;
    };

    class FolderTree
    {
    public:
        // `addSpecialEntries`: the tree covers a whole drive, so it gets the
        // free-space and unscanned-space pseudo-entries.
        FolderTree(std::unique_ptr<Folder> root, std::wstring rootPath, VolumeInfo volume, bool addSpecialEntries);

        Folder* Root() const noexcept { return root_.get(); }
        std::wstring const& RootPath() const noexcept { return rootPath_; }  // ends with a backslash
        VolumeInfo const& Volume() const noexcept { return volume_; }
        bool IsWholeVolume() const noexcept { return wholeVolume_; }

        // Statistics of the scan that produced the tree.
        std::chrono::milliseconds elapsed{};
        unsigned threads = 0;
        std::uint64_t unreadableFolders = 0;
        ScanMethod method = ScanMethod::Folders;
        std::uint32_t rootRecord = 0;  // the root folder's own NTFS record, when entries have records

        // Set for a tree read from an NTFS master file table: rescans then
        // apply the volume's change journal instead of scanning again.
        std::optional<JournalPosition> journal;

        // Set for a tree listed folder by folder (when asked for): which
        // folders changed since, so rescans list only those again.
        std::shared_ptr<FolderWatch> watch;

        // Files whose listing lagged behind them at the last reload - kept
        // open and written to, folder listings show them as they were when
        // last closed: their sizes are read from them again on the next.
        std::vector<std::wstring> growingFiles;  // relative to the root

        // Paths of the folders that could not be listed, sorted.
        std::vector<std::wstring> UnreadableFolders() const;

        // Full paths, built by walking up the tree (1.4.0 used recursion).
        std::wstring PathOf(Folder const& folder) const;                       // ends with a backslash
        std::wstring PathOf(Folder const& folder, std::size_t index) const;

        // Removes an entry that was deleted from the disk and updates the totals
        // of every folder above it.
        void Remove(Folder& folder, std::size_t index);

        // Re-reads the drive's free space and updates the pseudo-entries.
        void RefreshFreeSpace();

        // Adds (or updates) the free-space and unscanned-space pseudo-entries.
        void UpdateSpecialEntries();

    private:
        std::unique_ptr<Folder> root_;
        std::wstring rootPath_;
        VolumeInfo volume_;
        bool wholeVolume_ = false;
    };

    enum class ScanStatus
    {
        Completed,
        Cancelled,
        NotFound,
    };

    struct ScanResult
    {
        ScanStatus status = ScanStatus::NotFound;
        std::unique_ptr<FolderTree> tree;
    };

    // Scans a drive or folder.  Blocks until the scan completes or `stop` is
    // requested, so call it from a background thread.  `progress` may be
    // polled from any thread while the scan runs.
    ScanResult ScanFolder(std::wstring_view path, ScanProgress& progress, std::stop_token const& stop,
                          ScanOptions const& options = {});

    // Hard links: a file with several names is listed under each, but its
    // space is counted once, so that totals add up to what the drive holds -
    // under its name nearest the root (C:\Windows\System32\x.dll rather than
    // its copy in the component store, C:\Windows\WinSxS\...\x.dll), then in
    // the folder with the lowest record number, then the lowest name.  Its
    // other names take no space and are marked `countedElsewhere`.  Needs
    // records (NTFS drives).  Run again after names came or went; `lookup`,
    // when given, reads the size of a file whose counted name is gone and
    // whose remaining names have none.
    using SizeLookup = std::function<std::optional<std::uint64_t>(Folder const& folder, std::size_t index)>;
    void CountHardLinksOnce(FolderTree& tree, SizeLookup const& lookup = {});

    // The space a file takes on disk, from the file itself.
    std::optional<std::uint64_t> ReadSizeOnDisk(std::wstring const& path, std::uint32_t attributes);

    // Scanning part of a tree again.  The functions that read the disk block:
    // call them from a background thread and leave the tree alone meanwhile;
    // those that change the tree run on the thread that owns it.  Both bring
    // totals, hard links, the unreadable count and the free space up to date.

    // What `folder` of `tree` holds now, scanned again: everything below it,
    // or (`shallow`) only what it holds directly, its sub-folders empty.
    // Nothing if stopped.
    std::unique_ptr<Folder> ScanAgain(FolderTree const& tree, Folder const& folder, ScanProgress& progress,
                                      std::stop_token const& stop, bool shallow = false);

    // Puts what ScanAgain read in place of what `folder` held.
    void ReplaceContents(FolderTree& tree, Folder& folder, std::unique_ptr<Folder> fresh);

    // The folders a FolderWatch saw change, listed again.
    struct FolderReload
    {
        struct Listing
        {
            Folder* folder = nullptr;     // a folder of the tree...
            std::unique_ptr<Folder> now;  // ...as it is now: files, and sub-folders - scanned if new, else empty
        };
        struct Grown
        {
            Folder* folder = nullptr;  // a file whose listing lags behind it
            std::wstring name;
            std::uint64_t size = 0;    // as it is, read from the file
            std::uint64_t length = 0;
        };
        std::vector<Listing> listings;  // a sub-folder new to the tree is scanned whole: `listed` tells it
        std::vector<Grown> grown;
        std::vector<std::wstring> growing;          // FolderTree::growingFiles to be
    };

    // Lists again the folders that changed and scans the folders new in them;
    // reads the sizes of `files` that changed (and of FolderTree::growingFiles)
    // from the files.  Paths are relative to the root, as FolderWatch gives
    // them.  Nothing if stopped.
    std::optional<FolderReload> PrepareReload(FolderTree const& tree, std::span<std::wstring const> folders,
                                              std::span<std::wstring const> files, ScanProgress& progress,
                                              std::stop_token const& stop);

    // Puts a reload in place.
    void ApplyReload(FolderTree& tree, FolderReload reload);

    // Turns user input ("C:", "c:\\users\\..", "\\\\?\\C:\\x", "C:\\PROGRA~1")
    // into an absolute path with long names and a trailing backslash; empty if
    // it cannot be resolved.
    std::wstring NormalizeScanPath(std::wstring_view path);

    // L"C:\\x\\" -> L"\\\\?\\C:\\x\\", L"\\\\srv\\share\\" -> L"\\\\?\\UNC\\srv\\share\\".
    std::wstring ToExtendedLengthPath(std::wstring_view path);

    // Rounds up to a multiple of the cluster size (0 = leave unchanged).
    std::uint64_t RoundUpToCluster(std::uint64_t bytes, std::uint64_t clusterSize) noexcept;

    // Lets an elevated process list folders its user cannot normally read,
    // which is what a disk-usage tool needs when run as administrator.
    bool TryEnableBackupPrivilege() noexcept;
}
