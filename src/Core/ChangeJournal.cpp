// SpaceWindow - bringing a scanned tree up to date from the NTFS change journal.
#include "pch.h"
#include "ChangeJournal.h"

#include "Ntfs.h"

namespace sm
{
    namespace
    {
        // With more changed files than this, scanning the volume again is about as quick.
        constexpr std::size_t kMaxChangedFiles = 200'000;
        constexpr std::size_t kReadBufferBytes = std::size_t{ 256 } * 1024;
        constexpr std::size_t kMaxDepth = std::size_t{ 1 } << 20;  // guards walks up a tree against damaged links

        // USN_RECORD_V2, the journal entry format read here.
        constexpr std::size_t kEntryLength = 0x00;
        constexpr std::size_t kEntryMajorVersion = 0x04;
        constexpr std::size_t kEntryFile = 0x08;
        constexpr std::size_t kEntryParent = 0x10;
        constexpr std::size_t kEntryUsn = 0x18;
        constexpr std::size_t kEntryTime = 0x20;
        constexpr std::size_t kEntryReason = 0x28;
        constexpr std::size_t kEntryMinimum = 0x3C;

        // A journal page: entries never straddle one, so each starts with an entry.
        constexpr std::int64_t kJournalPage = 4096;

        template <typename T>
        T At(std::span<std::byte const> bytes, std::size_t offset) noexcept
        {
            T value{};
            if (offset <= bytes.size() && bytes.size() - offset >= sizeof(T))
            {
                std::memcpy(&value, bytes.data() + offset, sizeof(T));
            }
            return value;
        }

        // A record number as a tree stores it; nothing if it does not fit.
        std::optional<std::uint32_t> Record32(std::uint64_t record) noexcept
        {
            if (record > UINT32_MAX)
            {
                return std::nullopt;
            }
            return static_cast<std::uint32_t>(record);
        }

        // Every folder of a tree read from an MFT, by record number.  A plain
        // array: record numbers are dense, and a drive has hundreds of
        // thousands of folders to index on every rescan.
        class FolderIndex
        {
        public:
            explicit FolderIndex(Folder& root)
            {
                Set(static_cast<std::uint32_t>(ntfs::kRootRecord), &root);
                std::vector<Folder*> pending{ &root };
                while (!pending.empty())
                {
                    Folder const* const folder = pending.back();
                    pending.pop_back();
                    for (Entry const& entry : folder->entries)
                    {
                        if (entry.child)
                        {
                            if (entry.record != 0)
                            {
                                Set(entry.record, entry.child.get());
                            }
                            pending.push_back(entry.child.get());
                        }
                    }
                }
            }

            Folder* Find(std::uint32_t record) const noexcept
            {
                return record < folders_.size() ? folders_[record] : nullptr;
            }

            void Set(std::uint32_t record, Folder* folder)
            {
                if (record >= folders_.size())
                {
                    folders_.resize(std::max(std::size_t{ record } + 1, folders_.size() * 2));
                }
                folders_[record] = folder;
            }

        private:
            std::vector<Folder*> folders_;
        };

        // Is `folder` the folder `ancestor`, or inside it?
        bool IsWithin(Folder const* folder, Folder const* ancestor) noexcept
        {
            for (std::size_t steps = 0; folder != nullptr && steps < kMaxDepth; ++steps, folder = folder->parent)
            {
                if (folder == ancestor)
                {
                    return true;
                }
            }
            return false;
        }

        // Folders taken out of a tree, by record, until they go back in; and
        // any second copy of one (only a damaged tree would have that), kept
        // until the end because the folder map may still point to it.
        struct Loose
        {
            std::unordered_map<std::uint32_t, std::unique_ptr<Folder>> folders;
            std::vector<std::unique_ptr<Folder>> extra;
        };

        // Takes the entries of file `record` out of `folder`.  A folder taken
        // out keeps its contents, to be put back where it is now.
        bool TakeOut(Folder& folder, std::uint32_t record, Loose& loose)
        {
            bool found = false;
            for (Entry& entry : folder.entries)
            {
                if (entry.record == record && !entry.IsSpecial())
                {
                    found = true;
                    if (entry.child)
                    {
                        entry.child->parent = nullptr;
                        std::unique_ptr<Folder>& slot = loose.folders[record];
                        if (slot)
                        {
                            loose.extra.push_back(std::move(entry.child));
                        }
                        else
                        {
                            slot = std::move(entry.child);
                        }
                    }
                }
            }
            if (found)
            {
                std::erase_if(folder.entries, [record](Entry const& entry) { return entry.record == record && !entry.IsSpecial(); });
            }
            return found;
        }

        // `counted`: the name under which a file's space is counted (all but
        // one of a hard-linked file's names take none).
        void AddEntry(Folder& folder, ChangedFile const& file, std::wstring_view name, std::unique_ptr<Folder> child,
                      bool counted = true)
        {
            Entry entry;
            entry.kind = child ? EntryKind::Folder : EntryKind::File;
            entry.record = file.record;
            entry.attributes = file.attributes;
            entry.lastWriteTime = file.lastWriteTime;
            if (!child)
            {
                entry.size = counted ? file.size : 0;
                entry.countedElsewhere = !counted;
                entry.logicalSize = file.length;
            }
            std::wstring_view const text = name.substr(0, UINT16_MAX);
            entry.nameOffset = static_cast<std::uint32_t>(folder.names.size());
            entry.nameLength = static_cast<std::uint16_t>(text.size());
            folder.names.append(text);
            if (child)
            {
                child->parent = &folder;
                entry.child = std::move(child);
            }
            folder.entries.push_back(std::move(entry));
        }

        // How deep a folder is below `root` (the root itself: 0); nothing if it is not below it.
        std::optional<std::size_t> Depth(Folder const* folder, Folder const* root) noexcept
        {
            for (std::size_t depth = 0; folder != nullptr && depth < kMaxDepth; ++depth, folder = folder->parent)
            {
                if (folder == root)
                {
                    return depth;
                }
            }
            return std::nullopt;
        }

        // The name a file's space is counted under, as CountHardLinksOnce
        // chooses it: the one nearest the root, then in the folder with the
        // lowest record, then the lowest.  Nothing if none of its folders is in the tree.
        std::optional<std::size_t> CountedName(ChangedFile const& file, FolderIndex const& folders, Folder const* root)
        {
            std::optional<std::size_t> counted;
            std::size_t countedDepth = 0;
            for (std::size_t i = 0; i < file.names.size(); ++i)
            {
                std::optional<std::size_t> const depth = Depth(folders.Find(file.names[i].first), root);
                if (depth && (!counted || *depth < countedDepth ||
                              (*depth == countedDepth && file.names[i] < file.names[*counted])))
                {
                    counted = i;
                    countedDepth = *depth;
                }
            }
            return counted;
        }

        // A file read again only in case it changed unseen: is it listed just as it is?
        bool IsListedAsIs(ChangedFile const& file, FolderIndex const& folders, Folder const* root)
        {
            if (!file.exists || file.directory || file.alias || file.names.empty())
            {
                return false;
            }
            std::optional<std::size_t> const counted = CountedName(file, folders, root);
            for (std::size_t i = 0; i < file.names.size(); ++i)
            {
                auto const& [folderRecord, name] = file.names[i];
                Folder const* const folder = folders.Find(folderRecord);
                if (folder == nullptr)
                {
                    continue;  // not a folder of the tree, so not listed there
                }
                auto const listed = std::ranges::find_if(folder->entries, [&](Entry const& entry) {
                    return entry.record == file.record && entry.kind == EntryKind::File && folder->Name(entry) == name;
                });
                bool const isCounted = i == counted;
                if (listed == folder->entries.end() || listed->size != (isCounted ? file.size : 0) ||
                    listed->countedElsewhere == isCounted || listed->logicalSize != file.length ||
                    listed->attributes != file.attributes || listed->lastWriteTime != file.lastWriteTime)
                {
                    return false;
                }
            }
            return true;
        }

        // Drops the text of removed entries' names once it is most of a folder's.
        void CompactNames(Folder& folder)
        {
            std::size_t live = 0;
            for (Entry const& entry : folder.entries)
            {
                live += entry.nameLength;
            }
            if (folder.names.size() <= 2 * live + 256)
            {
                return;
            }
            std::wstring names;
            names.reserve(live);
            for (Entry& entry : folder.entries)
            {
                std::wstring_view const name = folder.Name(entry);
                entry.nameOffset = static_cast<std::uint32_t>(names.size());
                names.append(name);
            }
            folder.names = std::move(names);
        }
    }

    std::optional<JournalPosition> CurrentJournalPosition(ntfs::Volume const& volume)
    {
        USN_JOURNAL_DATA_V0 journal{};
        DWORD returned = 0;
        if (!DeviceIoControl(volume.Handle(), FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &journal, sizeof(journal), &returned,
                             nullptr))
        {
            return std::nullopt;  // no journal on this volume (one is never created here: that would write to it)
        }
        return JournalPosition{ volume.Device(), journal.UsnJournalID, journal.NextUsn };
    }

    JournalRead ReadJournalChanges(JournalPosition const& from, std::stop_token const& stop, std::int64_t lookbackBytes,
                                   std::uint64_t recentSince, std::span<std::uint32_t const> recheck)
    {
        JournalRead result;
        std::optional<ntfs::Volume> const volume = ntfs::Volume::OpenDevice(from.device);
        USN_JOURNAL_DATA_V0 journal{};
        DWORD returned = 0;
        if (!volume ||
            !DeviceIoControl(volume->Handle(), FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &journal, sizeof(journal), &returned,
                             nullptr) ||
            journal.UsnJournalID != from.journalId || from.nextUsn < journal.LowestValidUsn)
        {
            return result;  // a new journal, or one that has dropped entries since `from`
        }

        // The files named in the journal since `from`, with the folders they
        // were in and why they were last named.
        struct Named
        {
            std::vector<std::uint32_t> folders;
            std::uint32_t reason = 0;  // USN_REASON_*
            bool recheck = false;      // not named: read again in case it changed unseen
        };
        std::unordered_map<std::uint32_t, Named> changed;
        READ_USN_JOURNAL_DATA_V0 read{};
        read.StartUsn = from.nextUsn;
        if (lookbackBytes > 0)
        {
            read.StartUsn = std::max(journal.LowestValidUsn, (from.nextUsn - lookbackBytes) / kJournalPage * kJournalPage);
        }
        read.ReasonMask = 0xFFFFFFFF;
        read.UsnJournalID = journal.UsnJournalID;
        std::vector<std::byte> buffer(kReadBufferBytes);
        while (read.StartUsn < journal.NextUsn)
        {
            if (stop.stop_requested())
            {
                result.status = JournalReadStatus::Cancelled;
                return result;
            }
            DWORD bytes = 0;
            if (!DeviceIoControl(volume->Handle(), FSCTL_READ_USN_JOURNAL, &read, sizeof(read), buffer.data(),
                                 static_cast<DWORD>(buffer.size()), &bytes, nullptr) ||
                bytes < sizeof(USN))
            {
                return result;
            }
            std::span<std::byte const> const view(buffer.data(), bytes);
            auto const next = At<USN>(view, 0);
            for (std::size_t offset = sizeof(USN); bytes - offset >= kEntryMinimum;)
            {
                auto const length = At<std::uint32_t>(view, offset + kEntryLength);
                if (length < kEntryMinimum || length > bytes - offset)
                {
                    break;
                }
                if (At<std::uint16_t>(view, offset + kEntryMajorVersion) == 2 &&
                    (At<USN>(view, offset + kEntryUsn) >= from.nextUsn || At<std::uint64_t>(view, offset + kEntryTime) >= recentSince))
                {
                    std::optional<std::uint32_t> const file = Record32(ntfs::RecordOf(At<std::uint64_t>(view, offset + kEntryFile)));
                    std::optional<std::uint32_t> const folder = Record32(ntfs::RecordOf(At<std::uint64_t>(view, offset + kEntryParent)));
                    if (!file || !folder)
                    {
                        return result;  // record numbers beyond what a tree stores
                    }
                    Named& named = changed[*file];
                    if (std::ranges::find(named.folders, *folder) == named.folders.end())
                    {
                        named.folders.push_back(*folder);
                    }
                    named.reason = At<std::uint32_t>(view, offset + kEntryReason);
                    if (changed.size() > kMaxChangedFiles)
                    {
                        return result;
                    }
                }
                offset += length;
            }
            if (next <= read.StartUsn)
            {
                break;
            }
            read.StartUsn = next;
        }

        // Files changed but not closed yet: the journal will not name them
        // again until they are.  Those still open since `from`, and the
        // files asked for, are read again whatever the journal says.
        std::vector<std::uint32_t> open;
        for (auto const& [number, named] : changed)
        {
            if ((named.reason & USN_REASON_CLOSE) == 0)
            {
                open.push_back(number);
            }
        }
        auto const readAgain = [&changed](std::uint32_t number) {
            auto const [at, added] = changed.try_emplace(number);
            if (added)
            {
                at->second.recheck = true;
            }
            return added;
        };
        for (std::uint32_t const number : from.openFiles)
        {
            if (readAgain(number))
            {
                open.push_back(number);  // not named since `from`: not closed yet
            }
        }
        for (std::uint32_t const number : recheck)
        {
            (void)readAgain(number);
        }
        if (changed.size() > kMaxChangedFiles)
        {
            return result;
        }
        std::ranges::sort(open);

        // Each of them as it is now, in record order (neighbours share pages of the MFT).
        std::vector<std::pair<std::uint32_t, Named>> files(std::make_move_iterator(changed.begin()),
                                                           std::make_move_iterator(changed.end()));
        std::ranges::sort(files, {}, &std::pair<std::uint32_t, Named>::first);
        result.changes.files.reserve(files.size());
        ntfs::FileRecord record;
        for (auto& [number, named] : files)
        {
            if (stop.stop_requested())
            {
                result.status = JournalReadStatus::Cancelled;
                return result;
            }
            if (!volume->ReadFile(number, record))
            {
                return result;
            }
            ChangedFile file;
            file.record = number;
            file.formerFolders = std::move(named.folders);
            file.recheck = named.recheck;
            file.exists = record.inUse && record.base == 0 && number >= ntfs::kFirstUserRecord;
            if (file.exists)
            {
                file.directory = record.directory;
                file.alias = record.IsAlias();
                file.attributes = record.ListedAttributes();
                file.lastWriteTime = record.lastWriteTime;
                if (!record.directory)
                {
                    file.size = record.SizeOnDisk();
                    file.length = record.length;
                }
                for (ntfs::FileName const& name : record.names)
                {
                    std::optional<std::uint32_t> const folder = Record32(name.parent);
                    if (!folder)
                    {
                        return result;
                    }
                    if (*folder == number)
                    {
                        continue;
                    }
                    file.names.emplace_back(*folder, std::wstring(record.Name(name)));
                    if (record.directory)
                    {
                        break;  // a folder has a single name
                    }
                }
            }
            result.changes.files.push_back(std::move(file));
        }
        result.changes.position = JournalPosition{ from.device, journal.UsnJournalID, read.StartUsn, std::move(open) };
        result.status = JournalReadStatus::Ready;
        return result;
    }

    std::vector<std::uint32_t> LargeFiles(Folder const& root, std::uint64_t threshold)
    {
        std::vector<std::uint32_t> large;
        std::vector<Folder const*> pending{ &root };
        while (!pending.empty())
        {
            Folder const* const folder = pending.back();
            pending.pop_back();
            for (Entry const& entry : folder->entries)
            {
                if (entry.child)
                {
                    pending.push_back(entry.child.get());
                }
                else if (entry.kind == EntryKind::File && entry.record != 0 &&
                         (entry.size >= threshold || entry.logicalSize >= threshold))
                {
                    large.push_back(entry.record);
                }
            }
        }
        std::ranges::sort(large);
        large.erase(std::ranges::unique(large).begin(), large.end());
        return large;
    }

    void ApplyJournalChanges(FolderTree& tree, JournalChanges const& changes)
    {
        Folder* const root = tree.Root();
        if (root == nullptr)
        {
            return;
        }
        FolderIndex folders(*root);
        Loose loose;
        std::vector<Folder*> touched;  // folders whose entries changed (some more than once)

        // What really changed: files read again only in case they did are mostly as listed.
        std::vector<ChangedFile const*> files;
        files.reserve(changes.files.size());
        for (ChangedFile const& file : changes.files)
        {
            if (!file.recheck || !IsListedAsIs(file, folders, root))
            {
                files.push_back(&file);
            }
        }

        // Take every changed file out of the folders it may be listed in.
        for (ChangedFile const* const file : files)
        {
            auto const takeOut = [&](std::uint32_t record) {
                if (Folder* const found = folders.Find(record); found != nullptr && TakeOut(*found, file->record, loose))
                {
                    touched.push_back(found);
                }
            };
            for (std::uint32_t const folder : file->formerFolders)
            {
                takeOut(folder);
            }
            for (auto const& [folder, name] : file->names)
            {
                takeOut(folder);
            }
        }

        // The folders among them that still exist: as taken out, with their
        // contents, or new and empty.
        std::vector<ChangedFile const*> existing;
        for (ChangedFile const* const file : files)
        {
            if (!file->exists || !file->directory || file->alias || file->names.empty())
            {
                continue;
            }
            std::unique_ptr<Folder>& folder = loose.folders[file->record];
            if (!folder)
            {
                // Not found where the journal said: if the tree has it elsewhere, take it from there.
                if (Folder const* const found = folders.Find(file->record); found != nullptr && found != root)
                {
                    if (Folder* const parent = found->parent; parent != nullptr && TakeOut(*parent, file->record, loose))
                    {
                        touched.push_back(parent);
                    }
                }
            }
            if (!folder)
            {
                folder = std::make_unique<Folder>();
            }
            folders.Set(file->record, folder.get());
            touched.push_back(folder.get());
            existing.push_back(file);
        }
        // Each in its folder now - unless that would put a folder inside
        // itself, which only reading records while folders move could suggest.
        for (ChangedFile const* const file : existing)
        {
            auto const& [parentRecord, name] = file->names.front();
            Folder* const parent = folders.Find(parentRecord);
            std::unique_ptr<Folder>& folder = loose.folders[file->record];
            if (parent == nullptr || !folder || IsWithin(parent, folder.get()))
            {
                continue;
            }
            AddEntry(*parent, *file, name, std::move(folder));
            touched.push_back(parent);
        }

        // Files: one entry per name.  A hard-linked file is listed in each of
        // its folders but counted once (CountedName).
        for (ChangedFile const* const file : files)
        {
            if (!file->exists || file->directory || file->alias)
            {
                continue;
            }
            std::optional<std::size_t> const counted = CountedName(*file, folders, root);
            for (std::size_t i = 0; i < file->names.size(); ++i)
            {
                auto const& [folder, name] = file->names[i];
                if (Folder* const found = folders.Find(folder); found != nullptr)
                {
                    AddEntry(*found, *file, name, nullptr, i == counted);
                    touched.push_back(found);
                }
            }
        }

        // New totals for every changed folder still in the tree and the folders above it.
        std::unordered_set<Folder*> compacted;
        for (Folder* const folder : touched)
        {
            if (compacted.insert(folder).second)
            {
                CompactNames(*folder);
            }
        }
        UpdateTotalsUpward(*root, touched);

        loose = {};  // what is left was deleted, or moved off the tree
        tree.journal = changes.position;
        tree.RefreshFreeSpace();
    }
}
