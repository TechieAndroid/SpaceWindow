// SpaceWindow - bringing a scanned tree up to date from the NTFS change journal.
#include "pch.h"
#include "ChangeJournal.h"

#include "Fields.h"
#include "Ntfs.h"

namespace sm
{
    namespace
    {
        using fields::At;
        using ntfs::Record32;

        // With more changed files than this, scanning the volume again is about
        // as quick - and the changes are applied on the window's thread, where
        // a large update holds the window up for a moment.
        constexpr std::size_t kMaxChangedFiles = 50'000;
        constexpr std::size_t kReadBufferBytes = std::size_t{ 256 } * 1024;

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

        bool IsLarge(Entry const& entry) noexcept
        {
            return entry.kind == EntryKind::File && entry.record != 0 &&
                   (entry.size >= kLargeFile || entry.logicalSize >= kLargeFile);
        }

        // A tree's folders by record number (JournalIndex::folders).  A plain
        // array: record numbers are dense, and a drive has hundreds of
        // thousands of folders.
        class FolderIndex
        {
        public:
            explicit FolderIndex(std::vector<Folder*>& folders) noexcept : folders_(folders) {}

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
            std::vector<Folder*>& folders_;
        };

        // The tree's journal index; built first, with a walk of the whole
        // tree, if it has none (it was not read from a master file table, or
        // changed since in a way that dropped it).
        JournalIndex& IndexOf(FolderTree& tree)
        {
            if (!tree.journalIndex)
            {
                JournalIndex index;
                FolderIndex folders(index.folders);
                Folder* const root = tree.Root();
                folders.Set(tree.rootRecord != 0 ? tree.rootRecord : static_cast<std::uint32_t>(ntfs::kRootRecord),
                            root);
                ForEachFolder(*root, [&](Folder const& folder) {
                    for (Entry const& entry : folder.entries)
                    {
                        if (entry.child)
                        {
                            if (entry.record != 0)
                            {
                                folders.Set(entry.record, entry.child.get());
                            }
                        }
                        else if (IsLarge(entry))
                        {
                            index.largeFiles.push_back(entry.record);
                        }
                    }
                });
                std::ranges::sort(index.largeFiles);
                index.largeFiles.erase(std::ranges::unique(index.largeFiles).begin(), index.largeFiles.end());
                tree.journalIndex = std::move(index);
            }
            return *tree.journalIndex;
        }

        // Folders taken out of a tree, by record, until they go back in; and
        // any second copy of one (only a damaged tree would have that), kept
        // until the end because the folder index may still point to it.
        struct Loose
        {
            std::unordered_map<std::uint32_t, std::unique_ptr<Folder>> folders;
            std::vector<std::pair<std::uint32_t, std::unique_ptr<Folder>>> extra;
        };

        // Takes the entries of the files `records` (sorted) out of `folder`,
        // in one pass however many there are.  A folder taken out keeps its
        // contents, to be put back where it is now.
        bool TakeOut(Folder& folder, std::span<std::uint32_t const> records, Loose& loose)
        {
            auto const listed = [records](Entry const& entry) {
                return entry.record != 0 && !entry.IsSpecial() && std::ranges::binary_search(records, entry.record);
            };
            bool found = false;
            for (Entry& entry : folder.entries)
            {
                if (!listed(entry))
                {
                    continue;
                }
                found = true;
                if (entry.child)
                {
                    entry.child->parent = nullptr;
                    std::unique_ptr<Folder>& slot = loose.folders[entry.record];
                    if (slot)
                    {
                        loose.extra.emplace_back(entry.record, std::move(entry.child));
                    }
                    else
                    {
                        slot = std::move(entry.child);
                    }
                }
            }
            if (found)
            {
                std::erase_if(folder.entries, listed);
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

        // The name a file's space is counted under, as CountHardLinksOnce
        // chooses it: the one nearest the root, then in the folder with the
        // lowest record, then the lowest.  Nothing if none of its folders is in the tree.
        std::optional<std::size_t> CountedName(ChangedFile const& file, FolderIndex const& folders, Folder const* root)
        {
            std::optional<std::size_t> counted;
            std::size_t countedDepth = 0;
            for (std::size_t i = 0; i < file.names.size(); ++i)
            {
                Folder const* const folder = folders.Find(file.names[i].first);
                std::optional<std::size_t> const depth = folder != nullptr ? folder->LevelsBelow(root) : std::nullopt;
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

        // A folder about to be freed, and everything in it, out of the index.
        void Forget(FolderIndex& folders, std::uint32_t record, Folder& gone)
        {
            if (folders.Find(record) == &gone)
            {
                folders.Set(record, nullptr);
            }
            ForEachFolder(gone, [&folders](Folder const& folder) {
                for (Entry const& entry : folder.entries)
                {
                    if (entry.child && entry.record != 0 && folders.Find(entry.record) == entry.child.get())
                    {
                        folders.Set(entry.record, nullptr);
                    }
                }
            });
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
            !DeviceIoControl(volume->Handle(), FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &journal, sizeof(journal),
                             &returned, nullptr) ||
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
            read.StartUsn =
                std::max(journal.LowestValidUsn, (from.nextUsn - lookbackBytes) / kJournalPage * kJournalPage);
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
                    (At<USN>(view, offset + kEntryUsn) >= from.nextUsn ||
                     At<std::uint64_t>(view, offset + kEntryTime) >= recentSince))
                {
                    std::optional<std::uint32_t> const file =
                        Record32(ntfs::RecordOf(At<std::uint64_t>(view, offset + kEntryFile)));
                    std::optional<std::uint32_t> const folder =
                        Record32(ntfs::RecordOf(At<std::uint64_t>(view, offset + kEntryParent)));
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

    std::vector<std::uint32_t> LargeFiles(FolderTree& tree)
    {
        if (tree.Root() == nullptr)
        {
            return {};
        }
        return IndexOf(tree).largeFiles;
    }

    void ApplyJournalChanges(FolderTree& tree, JournalChanges const& changes)
    {
        Folder* const root = tree.Root();
        if (root == nullptr)
        {
            return;
        }
        JournalIndex& index = IndexOf(tree);
        FolderIndex folders(index.folders);
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

        // Take every changed file out of the folders it may be listed in:
        // each of those folders gone through once, for all its files - in
        // record order, so the tree comes out the same every time.
        std::map<std::uint32_t, std::vector<std::uint32_t>> outOf;  // folder record, file records
        for (ChangedFile const* const file : files)
        {
            auto const from = [&](std::uint32_t record) {
                if (folders.Find(record) != nullptr)
                {
                    outOf[record].push_back(file->record);
                }
            };
            for (std::uint32_t const folder : file->formerFolders)
            {
                from(folder);
            }
            for (auto const& [folder, name] : file->names)
            {
                from(folder);
            }
        }
        for (auto& [record, records] : outOf)
        {
            Folder* const folder = folders.Find(record);  // NOLINT(misc-const-correctness): its entries go
            std::ranges::sort(records);
            records.erase(std::ranges::unique(records).begin(), records.end());
            if (TakeOut(*folder, records, loose))
            {
                touched.push_back(folder);
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
                    std::array<std::uint32_t, 1> const record{ file->record };
                    if (Folder* const parent = found->parent; parent != nullptr && TakeOut(*parent, record, loose))
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
            if (parent == nullptr || !folder || parent->IsWithin(folder.get()))
            {
                continue;
            }
            AddEntry(*parent, *file, name, std::move(folder));
            touched.push_back(parent);
        }

        // Files: one entry per name.  A hard-linked file is listed in each of
        // its folders but counted once (CountedName).  The large ones are
        // noted for the next reload.
        std::vector<std::uint32_t> changedFiles;
        std::vector<std::uint32_t> largeNow;
        for (ChangedFile const* const file : files)
        {
            changedFiles.push_back(file->record);
            if (!file->exists || file->directory || file->alias)
            {
                continue;
            }
            std::optional<std::size_t> const counted = CountedName(*file, folders, root);
            std::size_t listed = 0;
            for (std::size_t i = 0; i < file->names.size(); ++i)
            {
                auto const& [folder, name] = file->names[i];
                if (Folder* const found = folders.Find(folder); found != nullptr)
                {
                    AddEntry(*found, *file, name, nullptr, i == counted);
                    touched.push_back(found);
                    ++listed;
                }
            }
            tree.hardLinks = tree.hardLinks || listed > 1;
            if (listed != 0 && (file->size >= kLargeFile || file->length >= kLargeFile))
            {
                largeNow.push_back(file->record);
            }
        }
        std::ranges::sort(changedFiles);
        std::ranges::sort(largeNow);
        std::vector<std::uint32_t> large;
        large.reserve(index.largeFiles.size() + largeNow.size());
        std::ranges::set_difference(index.largeFiles, changedFiles, std::back_inserter(large));
        index.largeFiles.clear();
        std::ranges::set_union(large, largeNow, std::back_inserter(index.largeFiles));

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

        // What is left was deleted, or moved off the tree.
        for (auto& [record, folder] : loose.folders)
        {
            if (folder)
            {
                Forget(folders, record, *folder);
            }
        }
        for (auto& [record, folder] : loose.extra)
        {
            Forget(folders, record, *folder);
        }
        loose = {};
        tree.journal = changes.position;
        tree.RefreshFreeSpace();
    }
}
