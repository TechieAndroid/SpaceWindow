// SpaceWindow - folder tree model and parallel disk scanner.
//
// Modernized from FolderTree.cpp of SpaceMonger 1.4.0
// (Copyright (c) 1998-2020 Sean Werkema, MIT License).  See FolderTree.h
// for a summary of what changed.
#include "pch.h"
#include "FolderTree.h"

#include "FolderWatch.h"
#include "IoCancellation.h"
#include "Ntfs.h"
#include "ScopeExit.h"
#include "Text.h"
#ifndef _WIN32
#include "FolderTreePosix.h"
#include "Posix.h"
#endif

namespace sm
{
    namespace
    {
        // Each GetFileInformationByHandleEx call fills this buffer with as many
        // directory entries as fit: a few thousand per system call.
        constexpr std::size_t kListingBufferBytes = std::size_t{ 256 } * 1024;

        // How often a worker publishes the folder it is scanning.
        constexpr auto kPathReportInterval = std::chrono::milliseconds(100);

#ifdef _WIN32
        constexpr std::wstring_view kExtendedPrefix = L"\\\\?\\";
        constexpr std::wstring_view kExtendedUncPrefix = L"\\\\?\\UNC\\";

        constexpr std::uint64_t ToUInt64(LARGE_INTEGER value) noexcept
        {
            return static_cast<std::uint64_t>(value.QuadPart);
        }

        constexpr std::uint64_t ToUInt64(FILETIME value) noexcept
        {
            return (std::uint64_t{ value.dwHighDateTime } << 32) | value.dwLowDateTime;
        }
#endif

        // Paths name the same file: as Windows compares them, or byte for byte.
        bool SamePath(std::wstring_view a, std::wstring_view b) noexcept
        {
            return kCaseInsensitivePaths ? EqualsIgnoringCase(a, b) : a == b;
        }

        void Deduct(std::uint64_t& total, std::uint64_t amount) noexcept
        {
            total = total > amount ? total - amount : 0;
        }

        bool IsDotOrDotDot(std::wstring_view name) noexcept
        {
            return name == L"." || name == L"..";
        }

#ifdef _WIN32
        // IsAlias (FolderTree.h) tests what the SDK's IsReparseTagNameSurrogate does, without its C-style cast.
        static_assert(IsReparseTagNameSurrogate(IO_REPARSE_TAG_MOUNT_POINT) &&
                      IsAlias(FILE_ATTRIBUTE_REPARSE_POINT, IO_REPARSE_TAG_MOUNT_POINT) &&
                      IsAlias(FILE_ATTRIBUTE_REPARSE_POINT, IO_REPARSE_TAG_SYMLINK) &&
                      !IsAlias(FILE_ATTRIBUTE_REPARSE_POINT, IO_REPARSE_TAG_CLOUD_6) &&
                      !IsAlias(FILE_ATTRIBUTE_REPARSE_POINT, IO_REPARSE_TAG_WOF) &&
                      !IsAlias(0, IO_REPARSE_TAG_MOUNT_POINT));
#else
        // Symbolic links and mount points are skipped as aliases (Posix scanner below).
        static_assert(IsAlias(FILE_ATTRIBUTE_REPARSE_POINT, IO_REPARSE_TAG_MOUNT_POINT) &&
                      IsAlias(FILE_ATTRIBUTE_REPARSE_POINT, IO_REPARSE_TAG_SYMLINK) &&
                      !IsAlias(FILE_ATTRIBUTE_REPARSE_POINT, IO_REPARSE_TAG_CLOUD_6) &&
                      !IsAlias(0, IO_REPARSE_TAG_MOUNT_POINT));
#endif

        // Windows' own file compression (compact.exe, CompactOS) moves a file's
        // data into a stream of its own and hides that from folder listings:
        // they show the file taking less space than its length, with none of
        // the marks that explain it for NTFS compression, sparse files or files
        // kept in the cloud.  Such a file is asked for its real size.
        constexpr bool HidesItsSize(std::uint32_t attributes, std::uint64_t allocated, std::uint64_t logical) noexcept
        {
            constexpr std::uint32_t kExplained = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_COMPRESSED |
                                                 FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_REPARSE_POINT |
                                                 FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN |
                                                 FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
            return allocated < logical && (attributes & kExplained) == 0;
        }
        static_assert(HidesItsSize(FILE_ATTRIBUTE_ARCHIVE, 0, 50000) &&
                      !HidesItsSize(FILE_ATTRIBUTE_ARCHIVE, 53248, 50000) &&
                      !HidesItsSize(FILE_ATTRIBUTE_SPARSE_FILE, 0, 50000) &&
                      !HidesItsSize(FILE_ATTRIBUTE_NORMAL, 0, 0));

#ifdef _WIN32
        // The space a file's data takes, as the file itself reports it.
        std::optional<std::uint64_t> CompressedSize(std::wstring const& path)
        {
            DWORD high = 0;
            DWORD const low = GetCompressedFileSizeW(path.c_str(), &high);
            if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR)
            {
                return std::nullopt;
            }
            return (std::uint64_t{ high } << 32) | low;
        }

        // A record number as entries keep it, from an NTFS file reference; 0 when it does not fit.
        constexpr std::uint32_t RecordOfReference(std::uint64_t fileReference) noexcept
        {
            return ntfs::Record32(ntfs::RecordOf(fileReference)).value_or(0);
        }

        // One entry of a folder listing, from either kind of listing record.
        struct Listed
        {
            std::wstring_view name;
            std::uint32_t attributes = 0;
            std::uint32_t reparseTag = 0;
            std::uint64_t allocated = 0;
            std::uint64_t logical = 0;
            std::uint64_t lastWriteTime = 0;
            std::uint32_t record = 0;  // with file IDs
            std::size_t next = 0;      // offset of the next record; 0 for the last
        };

        Listed Describe(FILE_FULL_DIR_INFO const& info) noexcept
        {
            // For a reparse point, EaSize holds the reparse tag ([MS-FSCC] 2.4.17).
            return { { info.FileName, info.FileNameLength / sizeof(wchar_t) },
                     info.FileAttributes,
                     info.EaSize,
                     ToUInt64(info.AllocationSize),
                     ToUInt64(info.EndOfFile),
                     ToUInt64(info.LastWriteTime),
                     0,
                     info.NextEntryOffset };
        }

        Listed Describe(FILE_ID_EXTD_DIR_INFO const& info) noexcept
        {
            // On NTFS the 128-bit file ID holds the 64-bit file reference.
            std::uint64_t reference = 0;
            std::memcpy(&reference, info.FileId.Identifier, sizeof(reference));
            return { { info.FileName, info.FileNameLength / sizeof(wchar_t) },
                     info.FileAttributes,
                     info.ReparsePointTag,
                     ToUInt64(info.AllocationSize),
                     ToUInt64(info.EndOfFile),
                     ToUInt64(info.LastWriteTime),
                     RecordOfReference(reference),
                     info.NextEntryOffset };
        }

        // A folder's own record number (0 if unknown).
        std::uint32_t RecordOfFolder(std::wstring const& extendedPath)
        {
            wil::unique_hfile const folder(CreateFileW(extendedPath.c_str(), FILE_READ_ATTRIBUTES,
                                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if (!folder || !GetFileInformationByHandle(folder.get(), &info))
            {
                return 0;
            }
            return RecordOfReference((std::uint64_t{ info.nFileIndexHigh } << 32) | info.nFileIndexLow);
        }

        constexpr bool IsUnsupportedInfoClass(DWORD error) noexcept
        {
            return error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED ||
                   error == ERROR_INVALID_FUNCTION || error == ERROR_INVALID_LEVEL;
        }

        // "\\?\C:\x\" -> "C:\x\", "\\?\UNC\srv\share\" -> "\\srv\share\".
        std::wstring WithoutExtendedPrefix(std::wstring_view path)
        {
            if (path.starts_with(kExtendedUncPrefix))
            {
                return std::wstring(L"\\\\").append(path.substr(kExtendedUncPrefix.size()));
            }
            if (path.starts_with(kExtendedPrefix))
            {
                return std::wstring(path.substr(kExtendedPrefix.size()));
            }
            return std::wstring(path);
        }

        // "C:\PROGRA~1\" -> "C:\Program Files\", for paths that exist; any
        // other path comes back unchanged.
        std::wstring ExpandShortNames(std::wstring path)
        {
            std::wstring const extended = ToExtendedLengthPath(path);
            DWORD const length = GetLongPathNameW(extended.c_str(), nullptr, 0);
            if (length == 0)
            {
                return path;
            }
            std::wstring expanded(length, L'\0');
            DWORD const written = GetLongPathNameW(extended.c_str(), expanded.data(), length);
            if (written == 0 || written >= length)
            {
                return path;
            }
            expanded.resize(written);
            return WithoutExtendedPrefix(expanded);
        }
#else
        // A path as the progress shows it: paths have no other form here.
        std::wstring WithoutExtendedPrefix(std::wstring_view path)
        {
            return std::wstring(path);
        }
#endif

        // What a worker lists a folder into before the folder gets it: kept
        // from folder to folder, so that a folder's entries and names are
        // copied once, at their final size - with millions of entries held,
        // the slack of vectors grown entry by entry would add up.
        struct Listing
        {
            std::vector<Entry> entries;
            std::wstring names;

            void MoveInto(Folder& folder)
            {
                folder.entries.reserve(entries.size());
                std::ranges::move(entries, std::back_inserter(folder.entries));
                folder.names.assign(names);
                entries.clear();
                names.clear();
            }
        };

        // What was found below a folder so far, while it is being scanned.
        struct Found
        {
            std::uint64_t size = 0;
            std::uint64_t logical = 0;
            std::uint64_t files = 0;
            std::uint64_t folders = 0;
        };

        class ParallelScan
        {
        public:
            // `fileIds`: list with file IDs (NTFS), so that hard links can be
            // told apart.  `shallow`: list only the root; its sub-folders stay empty.
            ParallelScan(ScanProgress& progress, std::stop_token stop, IoCancellation& io, std::uint64_t clusterSize,
                         bool fileIds, bool shallow = false) noexcept
                : progress_(progress), stop_(std::move(stop)), io_(io), clusterSize_(clusterSize), fileIds_(fileIds),
                  shallow_(shallow)
            {
            }

            // Scans the tree below `root`; returns the number of unreadable folders.
            std::uint64_t Run(Folder& root, std::wstring rootPath, unsigned threadCount)
            {
                queue_.push_back({ &root, std::move(rootPath) });
                outstanding_ = 1;

                std::stop_callback const wake(stop_, [this]() noexcept {
                    {
                        std::scoped_lock const lock(mutex_);
                        stopping_ = true;
                    }
                    available_.notify_all();
                });

                {
                    std::vector<std::jthread> helpers;
                    helpers.reserve(threadCount > 0 ? threadCount - 1 : 0);
                    for (unsigned i = 1; i < threadCount; ++i)
                    {
                        try
                        {
                            helpers.emplace_back([this] { Work(); });
                        }
                        catch (std::system_error const&)
                        {
                            break;  // could not start another thread: carry on with the ones we have
                        }
                    }
                    Work();
                }  // joins the helpers

                if (error_)
                {
                    std::rethrow_exception(error_);
                }
                return unreadable_.load(std::memory_order_relaxed);
            }

            // A copy of what the scan has found below `root` so far, for a live
            // map: the folders listed - breadth first while `budget` entries
            // last, then as boxes of the size found in them.  Any thread.
            // Each folder is copied whole, while no folder is finalized; the
            // workers wait for one folder's copy at a time, not for all of it.
            std::unique_ptr<Folder> Snapshot(Folder const& root, std::size_t budget)
            {
                if (!root.listed.load(std::memory_order_acquire))
                {
                    return nullptr;
                }
                auto copy = std::make_unique<Folder>();
                std::vector<std::pair<Folder const*, Folder*>> queue{ { &root, copy.get() } };
                std::vector<std::pair<Folder const*, Folder*>> boxes;  // folders beyond the budget
                std::size_t entries = 0;
                for (std::size_t next = 0; next < queue.size(); ++next)
                {
                    auto const [source, target] = queue[next];
                    std::unique_lock const lock(finalizing_);  // the folder is not finalized meanwhile
                    for (Entry const& entry : source->entries)
                    {
                        Folder const* const child = entry.child.get();
                        if (entry.IsSpecial() || (child != nullptr && !child->listed.load(std::memory_order_acquire)))
                        {
                            continue;  // not listed yet: nothing to show
                        }
                        Entry& added = target->entries.emplace_back();
                        added.kind = entry.kind;
                        added.attributes = entry.attributes;
                        added.lastWriteTime = entry.lastWriteTime;
                        added.nameOffset = static_cast<std::uint32_t>(target->names.size());
                        added.nameLength = entry.nameLength;
                        target->names.append(source->Name(entry));
                        ++entries;
                        if (child == nullptr)
                        {
                            added.size = entry.size;
                            added.logicalSize = entry.logicalSize;
                            continue;
                        }
                        added.child = std::make_unique<Folder>();
                        added.child->parent = target;
                        (entries < budget ? queue : boxes).emplace_back(child, added.child.get());
                    }
                }
                for (auto const& [source, box] : boxes)
                {
                    // A box of the size found in it, with nothing inside.
                    Found const found = FoundBelow(*source);
                    box->sizeFiles = found.size;
                    box->logicalTotal = found.logical;
                    box->fileCount = found.files;
                    box->folderCount = found.folders;
                }
                for (auto const& copied : queue | std::views::reverse)
                {
                    UpdateTotals(*copied.second);  // deepest first; boxes keep theirs
                }
                return copy;
            }

        private:
            struct Task
            {
                Folder* folder = nullptr;
                std::wstring path;  // extended-length form, ends with a backslash
            };

            struct Counts
            {
                std::uint64_t files = 0;
                std::uint64_t folders = 0;
                std::uint64_t bytes = 0;
            };

            // What a worker keeps from one folder to the next.
            struct Scratch
            {
                std::vector<std::byte> buffer = std::vector<std::byte>(kListingBufferBytes);  // records, as listed
                Listing listing;          // the folder's entries, until it gets them
                std::vector<Task> found;  // its sub-folders, to be scanned
                Counts counts;
#ifndef _WIN32
                std::wstring name;  // the entry being added
#endif
            };

            void Work() noexcept
            {
                try
                {
                    IoCancellation::Enlistment const enlisted(io_);
                    Scratch scratch;
                    std::chrono::steady_clock::time_point nextReport{};
                    Task task;
                    while (Pop(task))
                    {
                        auto const now = std::chrono::steady_clock::now();
                        if (now >= nextReport)
                        {
                            progress_.SetCurrentPath(WithoutExtendedPrefix(task.path));
                            nextReport = now + kPathReportInterval;
                        }
                        Scan(task, scratch);
                        Retire(scratch.found);
                    }
                }
                catch (...)
                {
                    Fail(std::current_exception());
                }
            }

            bool Pop(Task& task)
            {
                std::unique_lock lock(mutex_);
                available_.wait(lock, [this] { return stopping_ || !queue_.empty() || outstanding_ == 0; });
                if (stopping_ || queue_.empty())
                {
                    return false;
                }
                // LIFO: depth-first order keeps the queue short and the caches warm.
                task = std::move(queue_.back());
                queue_.pop_back();
                return true;
            }

            // Publishes the sub-folders found by a task and retires the task.
            void Retire(std::vector<Task>& found)
            {
                std::size_t const count = found.size();
                bool finished = false;
                {
                    std::scoped_lock const lock(mutex_);
                    for (Task& task : found)
                    {
                        queue_.push_back(std::move(task));
                    }
                    outstanding_ += count;
                    --outstanding_;
                    finished = outstanding_ == 0;
                }
                found.clear();

                if (finished || count > 1)
                {
                    available_.notify_all();
                }
                else if (count == 1)
                {
                    available_.notify_one();
                }
            }

            void Fail(std::exception_ptr const& error) noexcept
            {
                {
                    // std::mutex::lock throws only on misuse (e.g. a recursive lock),
                    // which would be a bug; terminating is the right outcome then.
#pragma warning(suppress : 26447)
                    std::scoped_lock const lock(mutex_);
                    if (!error_)
                    {
                        error_ = error;
                    }
                    stopping_ = true;
                }
                available_.notify_all();
            }

            void Scan(Task const& task, Scratch& scratch)
            {
                scratch.counts = {};
#ifdef _WIN32
                if (!ListWithHandle(task, scratch))
                {
                    ListWithFindFile(task, scratch);
                }
#else
                ListDirectory(task, scratch);
#endif
                // Before the folder counts as listed: a snapshot then reads its entries.
                scratch.listing.MoveInto(*task.folder);
                task.folder->listed.store(true, std::memory_order_release);
                progress_.files.fetch_add(scratch.counts.files, std::memory_order_relaxed);
                progress_.folders.fetch_add(scratch.counts.folders, std::memory_order_relaxed);
                progress_.bytes.fetch_add(scratch.counts.bytes, std::memory_order_relaxed);
                Complete(task.folder);
            }

            // Retires one unit of a folder's pending work.  The thread that
            // retires the last unit finalizes the folder - totals and sorts
            // it - and reports to its parent, and so on up the tree:
            // iteratively, so depth never matters.  A snapshot sees a folder
            // either still pending, or finalized whole.
            void Complete(Folder* folder)
            {
                std::shared_lock const lock(finalizing_);
                while (folder != nullptr && folder->pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                    UpdateTotals(*folder);
                    folder = folder->parent;
                }
            }

#ifdef _WIN32
            // Lists a folder in large batches.  Returns false if the file system
            // does not support FileFullDirectoryInfo, so the caller falls back.
            bool ListWithHandle(Task const& task, Scratch& scratch)
            {
                if (stop_.stop_requested())
                {
                    return true;  // stopping: not worth opening another folder, which may block
                }
                wil::unique_hfile const directory(CreateFileW(
                    task.path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
                if (!directory)
                {
                    MarkUnreadable(*task.folder);
                    return true;
                }

                std::vector<std::byte>& buffer = scratch.buffer;
                bool const ids = fileIds_.load(std::memory_order_relaxed);
                bool restart = true;
                while (!stop_.stop_requested())
                {
                    FILE_INFO_BY_HANDLE_CLASS const infoClass =
                        ids ? (restart ? FileIdExtdDirectoryRestartInfo : FileIdExtdDirectoryInfo)
                            : (restart ? FileFullDirectoryRestartInfo : FileFullDirectoryInfo);
                    if (!GetFileInformationByHandleEx(directory.get(), infoClass, buffer.data(),
                                                      static_cast<DWORD>(buffer.size())))
                    {
                        DWORD const error = GetLastError();
                        if (error != ERROR_NO_MORE_FILES && restart)
                        {
                            if (IsUnsupportedInfoClass(error))
                            {
                                if (ids)
                                {
                                    fileIds_.store(false,
                                                   std::memory_order_relaxed);  // this file system lists without them
                                    return ListWithHandle(task, scratch);
                                }
                                return false;
                            }
                            MarkUnreadable(*task.folder);
                        }
                        break;
                    }
                    restart = false;

                    // The buffer holds a chain of variable-length records.
                    std::size_t offset = 0;
                    for (;;)
                    {
                        std::byte const* const at = buffer.data() + offset;
                        Listed const listed = ids ? Describe(*reinterpret_cast<FILE_ID_EXTD_DIR_INFO const*>(at))
                                                  : Describe(*reinterpret_cast<FILE_FULL_DIR_INFO const*>(at));
                        std::uint64_t allocated = listed.allocated;
                        if (HidesItsSize(listed.attributes, allocated, listed.logical))
                        {
                            allocated = CompressedSize(task.path + std::wstring(listed.name)).value_or(allocated);
                        }
                        Add(task, scratch, listed.name, listed.attributes, listed.reparseTag, allocated, listed.logical,
                            listed.lastWriteTime, listed.record);
                        if (listed.next == 0)
                        {
                            break;
                        }
                        offset += listed.next;
                    }
                }
                return true;
            }

            // Fallback for file systems that only support FindFirstFile.  Those do
            // not report allocation sizes, so compressed and sparse files ask for
            // theirs and everything is rounded up to whole clusters.
            void ListWithFindFile(Task const& task, Scratch& scratch)
            {
                std::wstring const pattern = task.path + L'*';
                WIN32_FIND_DATAW data{};
                wil::unique_hfind const find(FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                                              FindExSearchNameMatch, nullptr,
                                                              FIND_FIRST_EX_LARGE_FETCH));
                if (!find)
                {
                    MarkUnreadable(*task.folder);
                    return;
                }

                do
                {
                    std::wstring_view const name(data.cFileName);
                    std::uint64_t const logical = (std::uint64_t{ data.nFileSizeHigh } << 32) | data.nFileSizeLow;
                    std::uint64_t allocated = logical;
                    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                        (data.dwFileAttributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)) != 0)
                    {
                        allocated = CompressedSize(task.path + std::wstring(name)).value_or(allocated);
                    }
                    Add(task, scratch, name, data.dwFileAttributes, data.dwReserved0,
                        RoundUpToCluster(allocated, clusterSize_), logical, ToUInt64(data.ftLastWriteTime), 0);
                } while (!stop_.stop_requested() && FindNextFileW(find.get(), &data));
            }
#else
        public:
            // How folders are listed (FolderTreePosix.h): with record numbers for
            // the files, handed to a watch first, from a network drive.
            void Use(posix::ListingOptions const& options) noexcept { listing_ = options; }

        private:
            // Takes what posix::ListFolder lists into the task's folder.
            class Sink final : public posix::ListingSink
            {
            public:
                Sink(ParallelScan const& scan, Task const& task, Scratch& scratch) noexcept
                    : scan_(scan), task_(task), scratch_(scratch)
                {
                }

                void Add(posix::Listed const& entry) override
                {
                    scan_.Add(task_, scratch_, entry.name, entry.attributes, entry.reparseTag, entry.allocated,
                              entry.logical, entry.lastWriteTime, entry.record);
                }

            private:
                ParallelScan const& scan_;
                Task const& task_;
                Scratch& scratch_;
            };

            void ListDirectory(Task const& task, Scratch& scratch)
            {
                if (stop_.stop_requested())
                {
                    return;  // stopping: not worth opening another folder, which may block
                }
                Sink sink(*this, task, scratch);
                if (!posix::ListFolder(task.path, listing_, stop_, scratch.buffer, scratch.name, sink))
                {
                    MarkUnreadable(*task.folder);
                }
            }
#endif

            void Add(Task const& task, Scratch& scratch, std::wstring_view name, std::uint32_t attributes,
                     std::uint32_t reparseTag, std::uint64_t allocated, std::uint64_t logical,
                     std::uint64_t lastWriteTime, std::uint32_t record) const
            {
                if (IsDotOrDotDot(name) || IsAlias(attributes, reparseTag) || name.size() > UINT16_MAX)
                {
                    return;
                }

                Listing& listing = scratch.listing;
                Entry entry;
                entry.attributes = attributes;
                entry.lastWriteTime = lastWriteTime;
                entry.record = record;
                entry.nameOffset = static_cast<std::uint32_t>(listing.names.size());
                entry.nameLength = static_cast<std::uint16_t>(name.size());
                listing.names.append(name);

                if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                {
                    entry.kind = EntryKind::File;
                    entry.size = allocated;
                    entry.logicalSize = logical;
                    listing.entries.push_back(std::move(entry));
                    ++scratch.counts.files;
                    scratch.counts.bytes += allocated;
                    return;
                }

                Folder& folder = *task.folder;
                auto child = std::make_unique<Folder>();
                child->parent = &folder;
                Folder* const childFolder = child.get();
                entry.kind = EntryKind::Folder;
                entry.child = std::move(child);
                listing.entries.push_back(std::move(entry));
                ++scratch.counts.folders;
                if (shallow_)
                {
                    return;
                }

                std::wstring path;
                path.reserve(task.path.size() + name.size() + 1);
                path.append(task.path).append(name).push_back(kPathSeparator);
                scratch.found.push_back({ childFolder, std::move(path) });

                // The child cannot finish before Retire() publishes it, so the
                // count is raised in time.
                folder.pending.fetch_add(1, std::memory_order_relaxed);
            }

            void MarkUnreadable(Folder& folder) noexcept
            {
                folder.unreadable = true;
                unreadable_.fetch_add(1, std::memory_order_relaxed);
            }

            // What was found below a folder so far: everything, once it is
            // finalized.  Each folder is looked at while no folder is
            // finalized, so the workers wait only a moment each time.
            Found FoundBelow(Folder const& folder)
            {
                Found found;
                std::vector<Folder const*> pending{ &folder };
                while (!pending.empty())
                {
                    Folder const* const at = pending.back();
                    pending.pop_back();
                    std::unique_lock const lock(finalizing_);
                    if (at->pending.load(std::memory_order_acquire) == 0)
                    {
                        found.size += at->SizeTotal();
                        found.logical += at->logicalTotal;
                        found.files += at->fileCount;
                        found.folders += at->folderCount;
                        continue;
                    }
                    if (!at->listed.load(std::memory_order_acquire))
                    {
                        continue;
                    }
                    for (Entry const& entry : at->entries)
                    {
                        if (entry.child)
                        {
                            ++found.folders;
                            pending.push_back(entry.child.get());
                        }
                        else if (entry.kind == EntryKind::File)
                        {
                            found.size += entry.size;
                            found.logical += entry.logicalSize;
                            ++found.files;
                        }
                    }
                }
                return found;
            }

            ScanProgress& progress_;
            std::stop_token stop_;
            IoCancellation& io_;
            [[maybe_unused]] std::uint64_t const clusterSize_;  // (unused where listings report allocations)
            std::atomic<bool> fileIds_;
            bool const shallow_;
#ifndef _WIN32
            posix::ListingOptions listing_;
#endif
            std::shared_mutex finalizing_;  // shared while folders are finalized, exclusive for a snapshot
            std::atomic<std::uint64_t> unreadable_{ 0 };

            std::mutex mutex_;
            std::condition_variable available_;
            std::vector<Task> queue_;      // guarded by mutex_
            std::size_t outstanding_ = 0;  // queued + running tasks; guarded by mutex_
            bool stopping_ = false;        // guarded by mutex_
            std::exception_ptr error_;     // guarded by mutex_ while workers run
        };

#ifdef _WIN32
        std::optional<bool> QuerySeekPenalty(std::wstring const& volumeRoot)
        {
            wchar_t volumeName[64]{};
            if (!GetVolumeNameForVolumeMountPointW(volumeRoot.c_str(), volumeName,
                                                   static_cast<DWORD>(std::size(volumeName))))
            {
                return std::nullopt;
            }

            // "\\?\Volume{guid}\" names the root directory; without the final
            // backslash it names the volume device, which answers storage queries.
            std::wstring device(volumeName);
            if (!device.empty() && device.back() == L'\\')
            {
                device.pop_back();
            }

            wil::unique_hfile const volume(
                CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
            if (!volume)
            {
                return std::nullopt;
            }

            STORAGE_PROPERTY_QUERY query{};
            query.PropertyId = StorageDeviceSeekPenaltyProperty;
            query.QueryType = PropertyStandardQuery;
            DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor{};
            DWORD returned = 0;
            if (!DeviceIoControl(volume.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &descriptor,
                                 sizeof(descriptor), &returned, nullptr) ||
                returned < sizeof(descriptor))
            {
                return std::nullopt;
            }
            return descriptor.IncursSeekPenalty != FALSE;
        }

        VolumeInfo QueryVolume(std::wstring const& path)
        {
            VolumeInfo volume;

            std::wstring root(path.size() + 1, L'\0');
            if (GetVolumePathNameW(path.c_str(), root.data(), static_cast<DWORD>(root.size())))
            {
                root.resize(std::wcslen(root.c_str()));
                volume.root = std::move(root);
            }

            ULARGE_INTEGER available{};
            ULARGE_INTEGER total{};
            ULARGE_INTEGER free{};
            if (GetDiskFreeSpaceExW(path.c_str(), &available, &total, &free))
            {
                volume.totalBytes = total.QuadPart;
                volume.freeBytes = free.QuadPart;
            }

            if (!volume.root.empty())
            {
                DWORD sectorsPerCluster = 0;
                DWORD bytesPerSector = 0;
                DWORD freeClusters = 0;
                DWORD clusters = 0;
                if (GetDiskFreeSpaceW(volume.root.c_str(), &sectorsPerCluster, &bytesPerSector, &freeClusters,
                                      &clusters))
                {
                    volume.clusterSize = std::uint64_t{ sectorsPerCluster } * bytesPerSector;
                }
                volume.remote = GetDriveTypeW(volume.root.c_str()) == DRIVE_REMOTE;
                if (!volume.remote)
                {
                    volume.seekPenalty = QuerySeekPenalty(volume.root);
                }
                wchar_t fileSystem[MAX_PATH + 1]{};
                if (GetVolumeInformationW(volume.root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fileSystem,
                                          static_cast<DWORD>(std::size(fileSystem))))
                {
                    volume.fileSystem = fileSystem;
                }
            }
            return volume;
        }
#else
        VolumeInfo QueryVolume(std::wstring const& path)
        {
            return posix::QueryVolume(path);
        }
#endif

        unsigned ChooseThreadCount(VolumeInfo const& volume)
        {
            unsigned const hardware = std::max(1u, std::thread::hardware_concurrency());
            if (volume.seekPenalty.value_or(false))
            {
                return 2;  // a spinning disk: more threads only make the heads thrash
            }
            if (volume.remote)
            {
                return std::clamp(hardware * 2, 4u, 16u);  // network latency hides behind parallel requests
            }
            // SSD/NVMe: listing scales with threads, up to a point.  On a
            // 16-thread PC, twelve list a drive as fast as sixteen do, with
            // less processor time, and leave the window room to stay smooth.
            return std::clamp(hardware * 3 / 4, 2u, 16u);
        }

#ifdef _WIN32
        // Local NTFS drives list each entry's file ID - its record - which tells hard links apart.
        bool ListsFileIds(VolumeInfo const& volume)
        {
            return !volume.remote && EqualsIgnoringCase(volume.fileSystem, L"NTFS");
        }
#endif

        // A set of record numbers, one bit each: records are dense.
        class RecordSet
        {
        public:
            // False if the record was in the set already.
            bool Insert(std::uint32_t record)
            {
                std::size_t const word = record / 64;
                if (word >= words_.size())
                {
                    words_.resize(std::max(word + 1, words_.size() * 2));
                }
                std::uint64_t const bit = std::uint64_t{ 1 } << (record % 64);
                bool const inserted = (words_[word] & bit) == 0;
                words_[word] |= bit;
                return inserted;
            }

            bool Contains(std::uint32_t record) const noexcept
            {
                std::size_t const word = record / 64;
                return word < words_.size() && (words_[word] & (std::uint64_t{ 1 } << (record % 64))) != 0;
            }

        private:
            std::vector<std::uint64_t> words_;
        };

        // A name of a file whose space is to be counted once, and where it is.
        struct LinkName
        {
            std::uint32_t record = 0;        // the file's
            std::size_t depth = 0;           // of the folder holding the name, below the root
            std::uint32_t folderRecord = 0;  // that folder's own record
            Folder* folder = nullptr;
            std::uint32_t index = 0;
        };

        // The names of the files `wanted` picks, in the order they are counted
        // in: by file, then nearest the root, then in the folder with the
        // lowest record, then the lowest name.
        template <typename Wanted>
        std::vector<LinkName> NamesOf(FolderTree& tree, Wanted const& wanted)
        {
            struct Walked
            {
                Folder* folder = nullptr;
                std::size_t depth = 0;
                std::uint32_t record = 0;
            };
            std::vector<LinkName> names;
            std::vector<Walked> walk{ { tree.Root(), 0, tree.rootRecord } };
            while (!walk.empty())
            {
                Walked const at = walk.back();
                walk.pop_back();
                for (std::size_t i = 0; i < at.folder->entries.size(); ++i)
                {
                    Entry const& entry = at.folder->entries[i];
                    if (entry.child)
                    {
                        walk.push_back({ entry.child.get(), at.depth + 1, entry.record });
                    }
                    else if (entry.kind == EntryKind::File && entry.record != 0 && wanted(entry))
                    {
                        names.push_back(
                            { entry.record, at.depth, at.record, at.folder, static_cast<std::uint32_t>(i) });
                    }
                }
            }
#ifdef _WIN32
            auto const folderBefore = [](std::uint32_t a, std::uint32_t b) { return a < b; };
#else
            // A scan numbers the files in the order of their inodes; a reload
            // numbers those new to the tree as it meets them.  The folders' own
            // inodes decide, as the numbers of a fresh scan would.
            std::vector<InodeTable::Key> keys;
            auto const folderBefore = [&tree, &keys](std::uint32_t a, std::uint32_t b) {
                if (keys.empty() && tree.inodes)
                {
                    keys = tree.inodes->Keys();
                }
                return a < keys.size() && b < keys.size() ? keys[a] < keys[b] : a < b;
            };
#endif
            std::ranges::sort(names, [&folderBefore](LinkName const& a, LinkName const& b) {
                if (a.record != b.record)
                {
                    return a.record < b.record;
                }
                if (a.depth != b.depth)
                {
                    return a.depth < b.depth;
                }
                if (a.folderRecord != b.folderRecord)
                {
                    return folderBefore(a.folderRecord, b.folderRecord);
                }
                return a.folder->Name(a.index) < b.folder->Name(b.index);
            });
            return names;
        }

        // Counts each file of `names` under its first name; its others take
        // no space.  The folders whose entries change are added to `changed`.
        // True if some file has several names.
        bool CountOnce(std::span<LinkName const> names, SizeLookup const& lookup, std::vector<Folder*>& changed)
        {
            auto const set = [&changed](LinkName const& name, std::uint64_t size, bool elsewhere) {
                Entry& entry = name.folder->entries[name.index];
                if (entry.size != size || entry.countedElsewhere != elsewhere)
                {
                    entry.size = size;
                    entry.countedElsewhere = elsewhere;
                    changed.push_back(name.folder);
                }
            };
            bool linked = false;
            for (std::size_t first = 0; first < names.size();)
            {
                std::size_t last = first + 1;
                while (last < names.size() && names[last].record == names[first].record)
                {
                    ++last;
                }
                std::span<LinkName const> const file = names.subspan(first, last - first);
                first = last;
                linked = linked || file.size() > 1;

                // Its size, from a name that holds it - or from the file, when all have given it up.
                std::optional<std::uint64_t> size;
                for (LinkName const& name : file)
                {
                    if (Entry const& entry = name.folder->entries[name.index]; !entry.countedElsewhere)
                    {
                        size = entry.size;
                        break;
                    }
                }
                if (!size && lookup)
                {
                    size = lookup(*file.front().folder, file.front().index);
                }
                if (size)
                {
                    set(file.front(), *size, false);
                    for (LinkName const& other : file.subspan(1))
                    {
                        set(other, 0, true);
                    }
                }
            }
            return linked;
        }
    }

    Folder::~Folder()
    {
        // Free the sub-tree iteratively.  Each detached folder is destroyed with
        // no children left, so destruction never recurses more than one level.
        try
        {
            std::vector<std::unique_ptr<Folder>> doomed;
            auto const detachChildren = [&doomed](Folder& folder) {
                for (Entry& entry : folder.entries)
                {
                    if (entry.child)
                    {
                        doomed.push_back(std::move(entry.child));
                    }
                }
            };
            detachChildren(*this);
            while (!doomed.empty())
            {
                std::unique_ptr<Folder> const folder = std::move(doomed.back());
                doomed.pop_back();
                detachChildren(*folder);
            }
        }
        catch (std::bad_alloc const&)  // NOLINT(bugprone-empty-catch): deliberate fallback, see below
        {
            // Out of memory for the work list: whatever is left is freed by the
            // ordinary (recursive) member destructors.
        }
    }

    std::optional<std::size_t> Folder::LevelsBelow(Folder const* ancestor) const noexcept
    {
        constexpr std::size_t kMaxDepth = std::size_t{ 1 } << 20;  // guards against damaged links
        std::size_t levels = 0;
        for (Folder const* folder = this; folder != nullptr && levels < kMaxDepth; folder = folder->parent, ++levels)
        {
            if (folder == ancestor)
            {
                return levels;
            }
        }
        return std::nullopt;
    }

    void UpdateTotals(Folder& folder)
    {
        std::uint64_t fileBytes = 0;
        std::uint64_t subfolderBytes = 0;
        std::uint64_t logicalBytes = 0;
        std::uint64_t files = 0;
        std::uint64_t folders = 0;
        for (Entry& entry : folder.entries)
        {
            if (entry.child)
            {
                Folder const& child = *entry.child;
                entry.size = child.SizeTotal();
                entry.logicalSize = child.logicalTotal;
                subfolderBytes += entry.size;
                files += child.fileCount;
                folders += child.folderCount + 1;
            }
            else if (entry.kind == EntryKind::File)
            {
                fileBytes += entry.size;
                ++files;
            }
            if (!entry.IsSpecial())
            {
                logicalBytes += entry.logicalSize;
            }
        }
        folder.sizeFiles = fileBytes;
        folder.sizeSubfolders = subfolderBytes;
        folder.logicalTotal = logicalBytes;
        folder.fileCount = files;
        folder.folderCount = folders;
        SortEntries(folder);
    }

    void SortEntries(Folder& folder)
    {
        // Stable, like 1.4.0's radix sort, so equal sizes keep their listing order.
        std::ranges::stable_sort(folder.entries, std::ranges::greater{}, &Entry::size);
        for (std::size_t i = 0; i < folder.entries.size(); ++i)
        {
            if (Folder* const child = folder.entries[i].child.get())
            {
                child->indexInParent = static_cast<std::uint32_t>(i);
            }
        }
    }

    void UpdateTotalsUpward(Folder& root, std::span<Folder* const> changed)
    {
        constexpr std::size_t kMaxDepth = std::size_t{ 1 } << 20;  // guards against damaged links
        std::vector<std::pair<std::size_t, Folder*>> update;       // depth, folder
        std::unordered_set<Folder*> queued;
        std::vector<Folder*> chain;
        for (Folder* const folder : changed)  // NOLINT(misc-const-correctness): updated below, through `chain`
        {
            if (queued.contains(folder))
            {
                continue;  // and so is everything above it
            }
            chain.assign(1, folder);
            while (chain.back() != &root && chain.back()->parent != nullptr && chain.size() < kMaxDepth)
            {
                chain.push_back(chain.back()->parent);
            }
            if (chain.back() != &root)
            {
                continue;  // no longer in the tree
            }
            for (std::size_t i = 0; i < chain.size(); ++i)
            {
                if (queued.insert(chain[i]).second)
                {
                    update.emplace_back(chain.size() - 1 - i, chain[i]);
                }
            }
        }
        std::ranges::sort(update, std::ranges::greater{}, &std::pair<std::size_t, Folder*>::first);
        for (auto const& [depth, folder] : update)
        {
            UpdateTotals(*folder);
        }
    }

    void CountHardLinksOnce(FolderTree& tree, SizeLookup const& lookup)
    {
        Folder* const root = tree.Root();
        if (root == nullptr)
        {
            return;
        }

        // Files listed under more than one name.
        RecordSet seen;
        RecordSet linked;
        bool anyLinked = false;
        bool anyMarked = false;  // names counted elsewhere before: their file may have lost its other names
        ForEachFolder(std::as_const(*root), [&](Folder const& folder) {
            for (Entry const& entry : folder.entries)
            {
                if (entry.kind == EntryKind::File && entry.record != 0)
                {
                    if (!seen.Insert(entry.record))
                    {
                        anyLinked = true;
                        (void)linked.Insert(entry.record);
                    }
                    anyMarked = anyMarked || entry.countedElsewhere;
                }
            }
        });
        tree.hardLinks = anyLinked;
        if (!anyLinked && !anyMarked)
        {
            return;
        }

        // Their names, each file counted under its first.
        std::vector<LinkName> const names = NamesOf(
            tree, [&linked](Entry const& entry) { return linked.Contains(entry.record) || entry.countedElsewhere; });
        std::vector<Folder*> changed;
        (void)CountOnce(names, lookup, changed);
        UpdateTotalsUpward(*root, changed);
        tree.UpdateSpecialEntries();
    }

    void CountHardLinksOnce(FolderTree& tree, std::span<std::uint32_t const> records, SizeLookup const& lookup)
    {
        Folder* const root = tree.Root();
        if (root == nullptr || records.empty())
        {
            return;
        }
        RecordSet wanted;
        for (std::uint32_t const record : records)
        {
            (void)wanted.Insert(record);
        }
        std::vector<LinkName> const names =
            NamesOf(tree, [&wanted](Entry const& entry) { return wanted.Contains(entry.record); });
        std::vector<Folder*> changed;
        if (CountOnce(names, lookup, changed))
        {
            tree.hardLinks = true;
        }
        UpdateTotalsUpward(*root, changed);
        tree.UpdateSpecialEntries();
    }

    std::vector<std::uint32_t> FileRecords(Folder const& folder)
    {
        std::vector<std::uint32_t> records;
        ForEachFolder(folder, [&records](Folder const& at) {
            for (Entry const& entry : at.entries)
            {
                if (entry.kind == EntryKind::File && entry.record != 0)
                {
                    records.push_back(entry.record);
                }
            }
        });
        std::ranges::sort(records);
        records.erase(std::ranges::unique(records).begin(), records.end());
        return records;
    }

    namespace
    {
        // A file's size on disk and length, read from the file - a folder
        // listing shows a file kept open and written to as it was when last
        // closed.
        struct FileSizes
        {
            std::uint64_t size = 0;
            std::uint64_t length = 0;
        };

        std::optional<FileSizes> ReadFileSizes(std::wstring const& path, std::uint32_t attributes)
        {
#ifndef _WIN32
            (void)attributes;  // statx reports what compressed and sparse files really take
            std::optional<posix::FileSpace> const space = posix::ReadFileSpace(path);
            if (!space)
            {
                return std::nullopt;
            }
            return FileSizes{ space->allocated, space->length };
#else
            std::wstring const extended = ToExtendedLengthPath(path);
            wil::unique_hfile const file(CreateFileW(extended.c_str(), FILE_READ_ATTRIBUTES,
                                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
            FILE_STANDARD_INFO info{};
            if (!file || !GetFileInformationByHandleEx(file.get(), FileStandardInfo, &info, sizeof(info)) ||
                info.Directory)
            {
                return std::nullopt;
            }
            FileSizes sizes{ ToUInt64(info.AllocationSize), ToUInt64(info.EndOfFile) };
            if ((attributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)) != 0)
            {
                sizes.size = CompressedSize(extended).value_or(sizes.size);
            }
            return sizes;
#endif
        }

        std::int64_t CountUnreadable(Folder const& root)
        {
            std::int64_t count = 0;
            ForEachFolder(root, [&count](Folder const& folder) { count += folder.unreadable ? 1 : 0; });
            return count;
        }

        // A tree's count of unreadable folders, moved on by `change`.
        void AddUnreadable(FolderTree& tree, std::int64_t change) noexcept
        {
            std::uint64_t const count = tree.unreadableFolders;
            std::uint64_t const magnitude =
                change < 0 ? 0 - static_cast<std::uint64_t>(change) : static_cast<std::uint64_t>(change);
            tree.unreadableFolders = change >= 0 ? count + magnitude : (count > magnitude ? count - magnitude : 0);
        }

        // Scans the folder at `extendedPath` of `tree` into a new folder object; nothing if stopped.
        std::unique_ptr<Folder> ScanInto(std::wstring const& extendedPath, FolderTree const& tree,
                                         ScanProgress& progress, std::stop_token const& stop, IoCancellation& io,
                                         bool shallow)
        {
            VolumeInfo const& volume = tree.Volume();
            auto folder = std::make_unique<Folder>();
#ifdef _WIN32
            ParallelScan scan(progress, stop, io, volume.clusterSize, ListsFileIds(volume), shallow);
#else
            ParallelScan scan(progress, stop, io, volume.clusterSize, false, shallow);
            // No watch: it adds the folders made since the scan itself.
            scan.Use({ tree.inodes.get(), nullptr, volume.remote });
#endif
            (void)scan.Run(*folder, extendedPath, shallow ? 1 : ChooseThreadCount(volume));
            if (stop.stop_requested())
            {
                return nullptr;
            }
            return folder;
        }

        // After part of a tree changed: totals up to the root, the space of
        // the files `records` (whose names came or went, so a hard-linked one
        // may now be counted under another name), and the free space.
        void Settle(FolderTree& tree, std::span<Folder* const> changed, std::vector<std::uint32_t>& records)
        {
            UpdateTotalsUpward(*tree.Root(), changed);
            if (tree.rootRecord != 0 && !records.empty())
            {
                std::ranges::sort(records);
                records.erase(std::ranges::unique(records).begin(), records.end());
                CountHardLinksOnce(tree, records, [&tree](Folder const& folder, std::size_t index) {
                    return ReadSizeOnDisk(tree.PathOf(folder, index), folder.entries[index].attributes);
                });
            }
            tree.RefreshFreeSpace();
        }

        // The records of the files among `entries` (not below them).
        void AddFileRecords(std::span<Entry const> entries, std::vector<std::uint32_t>& records)
        {
            for (Entry const& entry : entries)
            {
                if (entry.kind == EntryKind::File && entry.record != 0)
                {
                    records.push_back(entry.record);
                }
            }
        }

        Folder* ChildFolder(Folder const& folder, std::wstring_view name, bool ignoreCase) noexcept
        {
            for (Entry const& entry : folder.entries)
            {
                if (!entry.child)
                {
                    continue;
                }
                std::wstring_view const listed = folder.Name(entry);
                if (listed == name || (ignoreCase && EqualsIgnoringCase(listed, name)))
                {
                    return entry.child.get();
                }
            }
            return nullptr;
        }

        // The folder of the tree a path below its root leads to, as far as the
        // tree has it: the folder itself (`whole`), or - for a path through a
        // folder that is new, gone, or written with a short (8.3) name the
        // tree does not know - its nearest ancestor that is in the tree.
        Folder* Resolve(FolderTree const& tree, std::wstring_view relative, bool& whole)
        {
            Folder* folder = tree.Root();
            std::wstring path = tree.RootPath();
            whole = true;
            while (!relative.empty())
            {
                std::size_t const slash = relative.find(kPathSeparator);
                std::wstring name(relative.substr(0, slash));  // NOLINT(misc-const-correctness): Windows expands it
                relative = slash == std::wstring_view::npos ? std::wstring_view() : relative.substr(slash + 1);
                Folder* next = ChildFolder(*folder, name, kCaseInsensitivePaths);
#ifdef _WIN32
                if (next == nullptr)
                {
                    // A short name, as programs using %TEMP% report changes for example: its long form.
                    std::wstring const extended = ToExtendedLengthPath(path + name);
                    DWORD const needed = GetLongPathNameW(extended.c_str(), nullptr, 0);
                    std::wstring expanded(needed, L'\0');
                    DWORD const length = needed != 0 ? GetLongPathNameW(extended.c_str(), expanded.data(), needed) : 0;
                    if (length != 0 && length < needed)
                    {
                        expanded.resize(length);
                        name = expanded.substr(expanded.rfind(L'\\') + 1);
                        next = ChildFolder(*folder, name, true);
                    }
                }
#endif
                if (next == nullptr)
                {
                    whole = false;
                    break;
                }
                folder = next;
                path.append(name).push_back(kPathSeparator);
            }
            return folder;
        }

        Entry const* FileNamed(Folder const& folder, std::wstring_view name) noexcept
        {
            auto const found = std::ranges::find_if(folder.entries, [&folder, name](Entry const& entry) {
                return entry.kind == EntryKind::File && folder.Name(entry) == name;
            });
            return found != folder.entries.end() ? &*found : nullptr;
        }
    }

    std::optional<std::uint64_t> ReadSizeOnDisk(std::wstring const& path, std::uint32_t attributes)
    {
        std::optional<FileSizes> const sizes = ReadFileSizes(path, attributes);
        return sizes ? std::optional<std::uint64_t>(sizes->size) : std::nullopt;
    }

    std::unique_ptr<Folder> ScanAgain(FolderTree const& tree, Folder const& folder, ScanProgress& progress,
                                      std::stop_token const& stop, bool shallow)
    {
        IoCancellation io(stop);
        IoCancellation::Enlistment const enlisted(io);
        return ScanInto(ToExtendedLengthPath(tree.PathOf(folder)), tree, progress, stop, io, shallow);
    }

    void ReplaceContents(FolderTree& tree, Folder& folder, std::unique_ptr<Folder> fresh)
    {
        tree.journalIndex.reset();  // the folders it held go

        // The files whose names go and come, and the folders that could not
        // be listed, before and after.
        std::vector<std::uint32_t> records = FileRecords(folder);
        std::vector<std::uint32_t> const added = FileRecords(*fresh);
        records.insert(records.end(), added.begin(), added.end());
        AddUnreadable(tree, CountUnreadable(*fresh) - CountUnreadable(folder));

        // What it held goes once nothing points into it any more.
        std::vector<Entry> const old = std::exchange(folder.entries, std::move(fresh->entries));
        folder.names = std::move(fresh->names);
        folder.unreadable = fresh->unreadable;
        for (Entry const& entry : folder.entries)
        {
            if (entry.child)
            {
                entry.child->parent = &folder;
            }
        }
        std::array<Folder*, 1> const changed{ &folder };
        Settle(tree, changed, records);
    }

    std::optional<FolderReload> PrepareReload(FolderTree const& tree, std::span<std::wstring const> folders,
                                              std::span<std::wstring const> files, ScanProgress& progress,
                                              std::stop_token const& stop)
    {
        IoCancellation io(stop);
        IoCancellation::Enlistment const enlisted(io);
        FolderReload reload;

        // The folders to list again, each once.
        std::vector<Folder*> targets;
        std::unordered_set<Folder*> chosen;
        auto const choose = [&targets, &chosen](Folder* folder) {
            if (chosen.insert(folder).second)
            {
                targets.push_back(folder);
            }
        };
        for (std::wstring const& relative : folders)
        {
            bool whole = false;
            choose(Resolve(tree, relative, whole));
        }

        // A folder listed somewhere new may have moved there rather than be
        // new: is it one of the tree's?  The tree's folders by record, sorted,
        // are only gathered for the first such folder.
        std::optional<std::vector<std::uint32_t>> records;
        auto const isFolderOfTheTree = [&tree, &records](std::uint32_t record) {
            if (record == 0 || tree.rootRecord == 0)
            {
                return false;
            }
            if (!records)
            {
                records.emplace();
                ForEachFolder(std::as_const(*tree.Root()), [&records](Folder const& folder) {
                    for (Entry const& entry : folder.entries)
                    {
                        if (entry.child && entry.record != 0)
                        {
                            records->push_back(entry.record);
                        }
                    }
                });
                std::ranges::sort(*records);
            }
            return std::ranges::binary_search(*records, record);
        };

        std::unordered_map<Folder const*, Folder const*> listed;  // a folder of the tree, its new listing
        for (std::size_t i = 0; i < targets.size(); ++i)          // NOLINT(modernize-loop-convert): grows as it goes
        {
            Folder* const target = targets[i];
            std::wstring const path = ToExtendedLengthPath(tree.PathOf(*target));
#ifdef _WIN32
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                DWORD const error = GetLastError();
                if ((error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) && target->parent != nullptr)
                {
                    choose(target->parent);  // gone: its folder, listed again, drops it
                    continue;
                }
            }
#else
            if (posix::IsGone(path) && target->parent != nullptr)
            {
                choose(target->parent);  // gone: its folder, listed again, drops it
                continue;
            }
#endif
            std::unique_ptr<Folder> now = ScanInto(path, tree, progress, stop, io, true);
            if (!now)
            {
                return std::nullopt;
            }
            // Folders new to the tree are scanned whole; the others keep what the tree has.
            for (Entry& entry : now->entries)
            {
                if (!entry.child)
                {
                    continue;
                }
                std::wstring_view const name = now->Name(entry);
                if (ChildFolder(*target, name, false) != nullptr || isFolderOfTheTree(entry.record))
                {
                    continue;
                }
                std::unique_ptr<Folder> scanned =
                    ScanInto(path + std::wstring(name) + kPathSeparator, tree, progress, stop, io, false);
                if (!scanned)
                {
                    return std::nullopt;
                }
                entry.child = std::move(scanned);
            }
            listed.emplace(target, now.get());
            reload.listings.push_back({ target, std::move(now) });
        }

        // Files changed, or still growing at the last reload: read from the file.
        constexpr std::size_t kMaxFilesRead = 5'000;
        std::vector<std::wstring> candidates(files.begin(), files.end());
        candidates.insert(candidates.end(), tree.growingFiles.begin(), tree.growingFiles.end());
        std::ranges::sort(candidates);
        candidates.erase(std::ranges::unique(candidates).begin(), candidates.end());
        std::size_t read = 0;
        for (std::wstring const& relative : candidates)
        {
            if (stop.stop_requested())
            {
                return std::nullopt;
            }
            if (read == kMaxFilesRead)
            {
                break;
            }
            std::size_t const slash = relative.rfind(kPathSeparator);
            std::wstring_view const inFolder =
                slash == std::wstring::npos ? std::wstring_view() : std::wstring_view(relative).substr(0, slash);
            std::wstring_view const name = slash == std::wstring::npos ? std::wstring_view(relative)
                                                                       : std::wstring_view(relative).substr(slash + 1);
            bool whole = false;
            Folder* const folder = Resolve(tree, inFolder, whole);
            if (!whole)
            {
                continue;
            }
            auto const fresh = listed.find(folder);
            Entry const* const entry = FileNamed(fresh != listed.end() ? *fresh->second : *folder, name);
            if (entry == nullptr || entry->countedElsewhere)
            {
                continue;
            }
            ++read;
            std::optional<FileSizes> const sizes =
                ReadFileSizes(tree.PathOf(*folder) + std::wstring(name), entry->attributes);
            if (sizes && (sizes->size != entry->size || sizes->length != entry->logicalSize))
            {
                reload.grown.push_back({ folder, std::wstring(name), sizes->size, sizes->length });
                reload.growing.push_back(relative);
            }
        }
        return reload;
    }

    void ApplyReload(FolderTree& tree, FolderReload reload)
    {
        Folder* const root = tree.Root();
        tree.journalIndex.reset();  // folders come and go

        // The sub-folders of every folder listed again, taken out: they go
        // back in by name - or, moved, by record.
        struct Detached
        {
            std::unique_ptr<Folder> folder;
        };
        std::vector<Detached> detached;
        std::map<std::pair<Folder const*, std::wstring>, std::size_t> byName;
        std::unordered_map<std::uint32_t, std::size_t> byRecord;
        for (FolderReload::Listing const& listing : reload.listings)
        {
            Folder& folder = *listing.folder;
            for (Entry& entry : folder.entries)
            {
                if (entry.child)
                {
                    entry.child->parent = nullptr;
                    byName.emplace(std::make_pair(&folder, std::wstring(folder.Name(entry))), detached.size());
                    if (entry.record != 0)
                    {
                        byRecord.emplace(entry.record, detached.size());
                    }
                    detached.push_back({ std::move(entry.child) });
                }
            }
        }

        // Every folder of the tree by record, for one that moved in from a
        // folder not listed again: gathered only then - from the tree, from
        // what was taken out of it, and from what the listing being put in
        // place has taken back so far, as one moved may come from deep inside
        // another that moved too.  Only folders that were listed: a folder
        // a listing put in place but could not find stands empty for the
        // real one, and is no place to take a folder from.
        std::optional<std::unordered_map<std::uint32_t, Folder*>> folderByRecord;
        std::vector<std::pair<std::uint32_t, Folder*>> keptHere;  // by the listing being put in place
        auto const folderOf = [&](std::uint32_t record) -> Folder* {
            if (record == 0 || tree.rootRecord == 0)
            {
                return nullptr;
            }
            if (!folderByRecord)
            {
                folderByRecord.emplace();
                auto const gather = [&folderByRecord](std::uint32_t own, Folder& top) {
                    if (own != 0)
                    {
                        folderByRecord->emplace(own, &top);
                    }
                    ForEachFolder(top, [&folderByRecord](Folder& folder) {
                        for (Entry const& entry : folder.entries)
                        {
                            if (entry.child && entry.record != 0 && entry.child->listed.load(std::memory_order_relaxed))
                            {
                                folderByRecord->emplace(entry.record, entry.child.get());
                            }
                        }
                    });
                };
                gather(0, *root);
                for (auto const& [own, at] : byRecord)
                {
                    if (Folder* const folder = detached[at].folder.get(); folder != nullptr)
                    {
                        gather(own, *folder);
                    }
                }
                for (auto const& [own, folder] : keptHere)
                {
                    gather(own, *folder);
                }
            }
            auto const found = folderByRecord->find(record);
            return found != folderByRecord->end() ? found->second : nullptr;
        };

        // What changes with it: the files whose names came, went or moved
        // (their space may now be counted under another name), and the
        // number of folders that could not be listed.
        std::vector<std::uint32_t> records;
        std::int64_t unreadable = 0;
        auto const allFilesIn = [&records](Folder const& folder) {
            std::vector<std::uint32_t> const inside = FileRecords(folder);
            records.insert(records.end(), inside.begin(), inside.end());
        };

        // Each folder as listed now.
        std::vector<Folder*> touched;
        for (FolderReload::Listing const& listing : reload.listings)
        {
            Folder& folder = *listing.folder;
            Folder& now = *listing.now;
            AddFileRecords(folder.entries, records);
            AddFileRecords(now.entries, records);
            unreadable += (now.unreadable ? 1 : 0) - (folder.unreadable ? 1 : 0);
            keptHere.clear();
            for (Entry& entry : now.entries)
            {
                if (!entry.child)
                {
                    continue;  // a file
                }
                if (entry.child->listed.load(std::memory_order_relaxed))
                {
                    // A folder new to the tree, scanned (where the others were only named).
                    allFilesIn(*entry.child);
                    unreadable += CountUnreadable(*entry.child);
                    continue;
                }
                std::unique_ptr<Folder>* kept = nullptr;
                bool moved = false;
                if (auto const found = byName.find({ &folder, std::wstring(now.Name(entry)) });
                    found != byName.end() && detached[found->second].folder)
                {
                    kept = &detached[found->second].folder;
                }
                else if (auto const from = byRecord.find(entry.record);
                         entry.record != 0 && from != byRecord.end() && detached[from->second].folder)
                {
                    kept = &detached[from->second].folder;
                    moved = true;
                }
                if (kept != nullptr)
                {
                    if (!folder.IsWithin(kept->get()))
                    {
                        entry.child = std::move(*kept);
                        keptHere.emplace_back(entry.record, entry.child.get());
                        if (moved)
                        {
                            allFilesIn(*entry.child);
                        }
                    }
                    continue;
                }
                // Moved here from a folder not listed again: take it from there.
                Folder const* const elsewhere = folderOf(entry.record);
                if (elsewhere == nullptr || elsewhere->parent == nullptr || folder.IsWithin(elsewhere))
                {
                    continue;  // what it holds stays unknown until the next full scan
                }
                Folder& from = *elsewhere->parent;
                auto const at = std::ranges::find_if(
                    from.entries, [elsewhere](Entry const& e) { return e.child.get() == elsewhere; });
                if (at != from.entries.end())
                {
                    entry.child = std::move(at->child);
                    from.entries.erase(at);
                    touched.push_back(&from);
                    keptHere.emplace_back(entry.record, entry.child.get());
                    allFilesIn(*entry.child);
                }
            }
            folder.entries = std::move(now.entries);
            folder.names = std::move(now.names);
            folder.unreadable = now.unreadable;
            for (Entry const& entry : folder.entries)
            {
                if (entry.child)
                {
                    entry.child->parent = &folder;
                }
            }
            touched.push_back(&folder);
        }

        // Files whose listing lags behind them.
        for (FolderReload::Grown const& grown : reload.grown)
        {
            if (!grown.folder->IsWithin(root))
            {
                continue;
            }
            for (Entry& entry : grown.folder->entries)
            {
                if (entry.kind == EntryKind::File && grown.folder->Name(entry) == grown.name)
                {
                    entry.size = grown.size;
                    entry.logicalSize = grown.length;
                    touched.push_back(grown.folder);
                    break;
                }
            }
        }
        tree.growingFiles = std::move(reload.growing);

        // What is left was deleted.
        for (Detached const& gone : detached)
        {
            if (gone.folder)
            {
                allFilesIn(*gone.folder);
                unreadable -= CountUnreadable(*gone.folder);
            }
        }
        detached.clear();
        AddUnreadable(tree, unreadable);
        Settle(tree, touched, records);
    }

    std::wstring ScanProgress::CurrentPath() const
    {
        std::scoped_lock const lock(mutex_);
        return currentPath_;
    }

    void ScanProgress::SetCurrentPath(std::wstring_view path)
    {
        std::scoped_lock const lock(mutex_);
        currentPath_.assign(path);
    }

    std::unique_ptr<FolderTree> ScanProgress::Snapshot(std::size_t budget) const
    {
        std::scoped_lock const lock(snapshotMutex_);
        return snapshot_ ? snapshot_(budget) : nullptr;
    }

    void ScanProgress::SetSnapshotSource(SnapshotSource source)
    {
        std::scoped_lock const lock(snapshotMutex_);  // waits for a snapshot being taken
        snapshot_ = std::move(source);
    }

    FolderTree::FolderTree(std::unique_ptr<Folder> root, std::wstring rootPath, VolumeInfo volume,
                           bool addSpecialEntries)
        : root_(std::move(root)), rootPath_(std::move(rootPath)), volume_(std::move(volume)),
          wholeVolume_(addSpecialEntries)
    {
        UpdateSpecialEntries();
    }

    std::wstring FolderTree::PathOf(Folder const& folder) const
    {
        std::vector<std::wstring_view> names;
        for (Folder const* f = &folder; f->parent != nullptr; f = f->parent)
        {
            names.push_back(f->parent->Name(f->indexInParent));
        }

        std::wstring path = rootPath_;
        for (std::wstring_view const name : names | std::views::reverse)
        {
            path.append(name);
            path.push_back(kPathSeparator);
        }
        return path;
    }

    std::wstring FolderTree::PathOf(Folder const& folder, std::size_t index) const
    {
        std::wstring path = PathOf(folder);
        if (index < folder.entries.size() && !folder.entries[index].IsSpecial())
        {
            path.append(folder.Name(index));
        }
        return path;
    }

    void FolderTree::Remove(Folder& folder, std::size_t index)
    {
        if (index >= folder.entries.size() || folder.entries[index].IsSpecial())
        {
            return;
        }

        journalIndex.reset();  // the folders and files of the entry go
        Entry const& entry = folder.entries[index];
        std::uint64_t const size = entry.size;
        std::uint64_t const logical = entry.logicalSize;
        std::uint64_t const files = entry.child ? entry.child->fileCount : 1;
        std::uint64_t const folders = entry.child ? entry.child->folderCount + 1 : 0;

        Deduct(entry.IsFolder() ? folder.sizeSubfolders : folder.sizeFiles, size);
        Deduct(folder.logicalTotal, logical);
        Deduct(folder.fileCount, files);
        Deduct(folder.folderCount, folders);
        folder.entries.erase(folder.entries.begin() + static_cast<std::ptrdiff_t>(index));
        for (std::size_t i = index; i < folder.entries.size(); ++i)
        {
            if (Folder* const child = folder.entries[i].child.get())
            {
                child->indexInParent = static_cast<std::uint32_t>(i);
            }
        }

        for (Folder const* f = &folder; f->parent != nullptr; f = f->parent)
        {
            Folder& parent = *f->parent;
            Entry& own = parent.entries[f->indexInParent];
            Deduct(own.size, size);
            Deduct(own.logicalSize, logical);
            Deduct(parent.sizeSubfolders, size);
            Deduct(parent.logicalTotal, logical);
            Deduct(parent.fileCount, files);
            Deduct(parent.folderCount, folders);
            SortEntries(parent);  // `parent` shrank; it may belong further down its own list now
        }
    }

    std::vector<std::wstring> FolderTree::UnreadableFolders() const
    {
        std::vector<std::wstring> paths;
        if (!root_)
        {
            return paths;
        }
        ForEachFolder(std::as_const(*root_), [this, &paths](Folder const& folder) {
            if (folder.unreadable)
            {
                paths.push_back(PathOf(folder));
            }
        });
        std::ranges::sort(paths, LessIgnoringCase);
        return paths;
    }

    void FolderTree::RefreshFreeSpace()
    {
#ifndef _WIN32
        (void)posix::QueryDiskSpace(rootPath_, volume_.totalBytes, volume_.freeBytes);
#else
        ULARGE_INTEGER available{};
        ULARGE_INTEGER total{};
        ULARGE_INTEGER free{};
        if (GetDiskFreeSpaceExW(rootPath_.c_str(), &available, &total, &free))
        {
            volume_.totalBytes = total.QuadPart;
            volume_.freeBytes = free.QuadPart;
        }
#endif
        UpdateSpecialEntries();
    }

    void FolderTree::UpdateSpecialEntries()
    {
        if (!wholeVolume_ || !root_)
        {
            return;
        }

        std::uint64_t const scanned = root_->SizeTotal();
        std::uint64_t const used = volume_.UsedBytes();
        auto const set = [this](EntryKind kind, std::uint64_t size) {
            for (Entry& entry : root_->entries)
            {
                if (entry.kind == kind)
                {
                    entry.size = entry.logicalSize = size;
                    return;
                }
            }
            Entry entry;
            entry.kind = kind;
            entry.size = entry.logicalSize = size;
            entry.nameOffset = static_cast<std::uint32_t>(root_->names.size());
            root_->entries.push_back(std::move(entry));
        };
        set(EntryKind::FreeSpace, volume_.freeBytes);
        set(EntryKind::Unscanned, used > scanned ? used - scanned : 0);
        SortEntries(*root_);
    }

    ScanResult ScanFolder(std::wstring_view path, ScanProgress& progress, std::stop_token const& stop,
                          ScanOptions const& options)
    {
        auto const started = std::chrono::steady_clock::now();

        // Stopping also breaks off I/O stuck on a network share that stopped
        // answering: here (the checks below touch the path too) and in every
        // scanning thread.
        IoCancellation io(stop);
        IoCancellation::Enlistment const enlisted(io);

        ScanResult result;
        // A check broken off by Cancel fails too: that is no reason to say the path is not there.
        auto const notFound = [&stop]() {
            return ScanResult{ stop.stop_requested() ? ScanStatus::Cancelled : ScanStatus::NotFound, nullptr };
        };
        std::wstring const displayPath = NormalizeScanPath(path);
        if (displayPath.empty())
        {
            return notFound();
        }
        std::wstring const extendedPath = ToExtendedLengthPath(displayPath);
#ifdef _WIN32
        DWORD const attributes = GetFileAttributesW(extendedPath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
#else
        if (!posix::IsFolder(extendedPath))
#endif
        {
            return notFound();
        }

        VolumeInfo volume = QueryVolume(displayPath);
        bool const wholeVolume = !volume.root.empty() && SamePath(volume.root, displayPath);
        progress.expectedBytes.store(wholeVolume ? volume.UsedBytes() : 0, std::memory_order_relaxed);

#ifdef _WIN32
        // A whole NTFS drive, with administrator rights: read its master file table.
        if (wholeVolume && options.masterFileTable)
        {
            if (std::optional<ScanResult> fast =
                    ntfs::ScanVolume(displayPath, volume, progress, stop, options.addFreeSpace))
            {
                if (fast->tree)
                {
                    fast->tree->elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started);
                }
                return std::move(*fast);
            }
            // Not possible here: list the folders instead, counting from zero again.
            progress.files.store(0, std::memory_order_relaxed);
            progress.folders.store(0, std::memory_order_relaxed);
            progress.bytes.store(0, std::memory_order_relaxed);
        }
#endif

        unsigned const threads = options.threads != 0 ? options.threads : ChooseThreadCount(volume);
#ifdef _WIN32
        bool const fileIds = ListsFileIds(volume);
#else
        // Every file gets a record number, so that hard links are told apart on any file system.
        auto inodes = std::make_shared<InodeTable>();
        bool const fileIds = false;
#endif
        // Watched from before the listing starts, so that nothing changing meanwhile is missed.
        std::shared_ptr<FolderWatch> watch = options.watch ? FolderWatch::Start(displayPath, volume.remote) : nullptr;
        auto root = std::make_unique<Folder>();
        ParallelScan scan(progress, stop, io, volume.clusterSize, fileIds);
#ifndef _WIN32
        if (watch && options.watchLimit != 0)
        {
            watch->Limit(options.watchLimit);
        }
        scan.Use({ inodes.get(), watch.get(), volume.remote });
#endif
        // A live map meanwhile: what the scan has found so far.
        bool const specialEntries = wholeVolume && options.addFreeSpace;
        progress.SetSnapshotSource([&scan, &found = *root, &displayPath, volume, specialEntries](std::size_t budget) {
            std::unique_ptr<Folder> copy = scan.Snapshot(found, budget);
            return copy ? std::make_unique<FolderTree>(std::move(copy), displayPath, volume, specialEntries) : nullptr;
        });
        ScopeExit stopSnapshots([&progress]() noexcept { progress.SetSnapshotSource({}); });
        std::uint64_t const unreadable = scan.Run(*root, extendedPath, threads);
        stopSnapshots.reset();  // before the tree changes hands
        if (stop.stop_requested())
        {
            result.status = ScanStatus::Cancelled;
            return result;
        }

        auto tree = std::make_unique<FolderTree>(std::move(root), displayPath, std::move(volume),
                                                 wholeVolume && options.addFreeSpace);
        tree->threads = threads;
        tree->unreadableFolders = unreadable;
        tree->watch = std::move(watch);
#ifndef _WIN32
        if (tree->watch && tree->watch->OutOfWatches())
        {
            // More folders than the system lets a program watch: the next reloads
            // scan everything again, without trying to watch it all once more.
            tree->watch.reset();
            tree->tooBigToWatch = true;
        }
#endif
#ifdef _WIN32
        if (fileIds)
        {
            tree->rootRecord = RecordOfFolder(extendedPath);
            CountHardLinksOnce(*tree);
        }
#else
        posix::NumberInInodeOrder(*tree, *inodes, posix::RecordOfFolder(extendedPath, *inodes));
        tree->inodes = std::move(inodes);
        CountHardLinksOnce(*tree);
#endif
        tree->elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        result.status = ScanStatus::Completed;
        result.tree = std::move(tree);
        return result;
    }

    std::wstring NormalizeScanPath(std::wstring_view input)
    {
        auto const isTrimmed = [](wchar_t c) noexcept { return c == L' ' || c == L'\t' || c == L'"'; };
        while (!input.empty() && isTrimmed(input.front()))
        {
            input.remove_prefix(1);
        }
        while (!input.empty() && isTrimmed(input.back()))
        {
            input.remove_suffix(1);
        }
        if (input.empty())
        {
            return {};
        }

#ifndef _WIN32
        return posix::NormalizePath(input);
#else
        std::wstring path = WithoutExtendedPrefix(input);

        // "C:" on its own means the root of C:, not C:'s current directory.
        if (path.size() == 2 && path[1] == L':')
        {
            path.push_back(L'\\');
        }

        DWORD const length = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (length == 0)
        {
            return {};
        }
        std::wstring full(length, L'\0');
        DWORD const written = GetFullPathNameW(path.c_str(), length, full.data(), nullptr);
        if (written == 0 || written >= length)
        {
            return {};
        }
        full.resize(written);

        // Short (8.3) names, as in %TEMP% for long user names, would otherwise
        // show up in the title bar and make one folder look like two.  Only
        // generated short names contain '~', so other paths skip the lookup.
        if (full.find(L'~') != std::wstring::npos)
        {
            full = ExpandShortNames(std::move(full));
        }
        if (full.back() != L'\\')
        {
            full.push_back(L'\\');
        }
        return full;
#endif
    }

    std::wstring ToExtendedLengthPath(std::wstring_view path)
    {
#ifndef _WIN32
        return std::wstring(path);  // one form of path, unbounded in length (OpenDirectory)
#else
        if (path.starts_with(kExtendedPrefix) || path.starts_with(L"\\\\.\\"))
        {
            return std::wstring(path);
        }
        if (path.starts_with(L"\\\\"))
        {
            return std::wstring(kExtendedUncPrefix).append(path.substr(2));
        }
        return std::wstring(kExtendedPrefix).append(path);
#endif
    }

    std::uint64_t RoundUpToCluster(std::uint64_t bytes, std::uint64_t clusterSize) noexcept
    {
        if (clusterSize == 0 || bytes % clusterSize == 0)
        {
            return bytes;
        }
        std::uint64_t const rounded = bytes - bytes % clusterSize;
        return rounded > UINT64_MAX - clusterSize ? bytes : rounded + clusterSize;
    }

    bool TryEnableBackupPrivilege() noexcept
    {
#ifndef _WIN32
        return false;  // nothing to switch on: the superuser reads every folder anyway
#else
        wil::unique_handle token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, token.put()))
        {
            return false;
        }

        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (!LookupPrivilegeValueW(nullptr, SE_BACKUP_NAME, &privileges.Privileges[0].Luid) ||
            !AdjustTokenPrivileges(token.get(), FALSE, &privileges, sizeof(privileges), nullptr, nullptr))
        {
            return false;
        }
        // AdjustTokenPrivileges "succeeds" with ERROR_NOT_ALL_ASSIGNED when the
        // user does not hold the privilege at all.
        return GetLastError() == ERROR_SUCCESS;
#endif
    }
}
