// Checks the master file table scan against the folder scanner, file by file.
//
//   CoreTests.exe --compare C:\Windows
//
// Lists the folder, reads its whole drive through the master file table (run
// as administrator, on NTFS), and compares the two trees below the folder.
// Files that change between the two scans differ too, so expect a few.
#include "pch.h"
#include "Compare.h"
#include "TestHarness.h"

namespace test
{
    namespace
    {
        class Comparison
        {
        public:
            explicit Comparison(std::vector<Difference>& out) noexcept : out_(out) {}

            void Folders(sm::Folder const& listed, sm::Folder const& table, std::wstring const& path)
            {
                std::unordered_map<std::wstring_view, sm::Entry const*> fromTable;
                for (sm::Entry const& entry : table.entries)
                {
                    if (!entry.IsSpecial())
                    {
                        fromTable.emplace(table.Name(entry), &entry);
                    }
                }
                for (sm::Entry const& entry : listed.entries)
                {
                    if (entry.IsSpecial())
                    {
                        continue;
                    }
                    std::wstring_view const name = listed.Name(entry);
                    std::wstring const where = path + std::wstring(name);
                    auto const found = fromTable.find(name);
                    if (found == fromTable.end())
                    {
                        Note("only listed", where, entry.size, 0);
                        continue;
                    }
                    sm::Entry const& other = *found->second;
                    fromTable.erase(found);
                    if (entry.IsFolder() != other.IsFolder())
                    {
                        Note("file in one, folder in the other", where, 0, 0);
                    }
                    else if (entry.IsFolder())
                    {
                        if (entry.child && other.child)
                        {
                            Folders(*entry.child, *other.child, where + L"\\");
                        }
                    }
                    else
                    {
                        ++files_;
                        if (entry.size != other.size)
                        {
                            bool const compressed = (entry.attributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE |
                                                                         FILE_ATTRIBUTE_REPARSE_POINT)) != 0;
                            Note(compressed ? "size on disk (compressed, sparse or reparse point)" : "size on disk", where,
                                 entry.size, other.size);
                        }
                        if (entry.logicalSize != other.logicalSize)
                        {
                            Note("length", where, entry.logicalSize, other.logicalSize);
                        }
                        if (entry.countedElsewhere != other.countedElsewhere)
                        {
                            Note("hard link counted under this name in one, under another in the other", where,
                                 entry.size, other.size);
                        }
                        if (entry.attributes != other.attributes)
                        {
                            char bits[48]{};
                            (void)std::snprintf(bits, sizeof(bits), "attributes (bits 0x%08X differ)",
                                                static_cast<unsigned>(entry.attributes ^ other.attributes));
                            Note(bits, where, entry.attributes, other.attributes);
                        }
                        if (entry.lastWriteTime != other.lastWriteTime)
                        {
                            Note("date", where, entry.lastWriteTime, other.lastWriteTime);
                        }
                    }
                }
                for (auto const& [name, entry] : fromTable)
                {
                    Note("only in the table", path + std::wstring(name), 0, entry->size);
                }
            }

            std::uint64_t Files() const noexcept { return files_; }

        private:
            void Note(std::string what, std::wstring path, std::uint64_t listed, std::uint64_t table)
            {
                out_.push_back({ std::move(what), std::move(path), listed, table });
            }

            std::vector<Difference>& out_;
            std::uint64_t files_ = 0;
        };
    }

    std::vector<Difference> CompareTrees(sm::Folder const& listed, sm::Folder const& table, std::wstring const& path,
                                         std::uint64_t* filesCompared)
    {
        std::vector<Difference> differences;
        Comparison comparison(differences);
        comparison.Folders(listed, table, path);
        if (filesCompared != nullptr)
        {
            *filesCompared = comparison.Files();
        }
        return differences;
    }

    sm::Folder const* FindFolder(sm::Folder const& root, std::wstring_view relative)
    {
        sm::Folder const* within = &root;
        while (!relative.empty() && within != nullptr)
        {
            std::size_t const slash = relative.find(L'\\');
            std::wstring_view const name = relative.substr(0, slash);
            relative = slash == std::wstring_view::npos ? std::wstring_view() : relative.substr(slash + 1);
            if (name.empty())
            {
                continue;
            }
            sm::Folder const* next = nullptr;
            for (sm::Entry const& entry : within->entries)
            {
                std::wstring_view const candidate = within->Name(entry);
                if (entry.child && CompareStringOrdinal(candidate.data(), static_cast<int>(candidate.size()), name.data(),
                                                        static_cast<int>(name.size()), TRUE) == CSTR_EQUAL)
                {
                    next = entry.child.get();
                }
            }
            within = next;
        }
        return within;
    }
}

namespace
{
    void PrintTree(char const* name, sm::FolderTree const& tree, sm::Folder const& folder)
    {
        std::printf("  %-20s %10llu files %9llu folders %16llu bytes on disk  (scan: %lld ms)\n", name,
                    static_cast<unsigned long long>(folder.fileCount), static_cast<unsigned long long>(folder.folderCount),
                    static_cast<unsigned long long>(folder.SizeTotal()), static_cast<long long>(tree.elapsed.count()));
    }

    // How many of each kind of difference, with a few examples of each.
    void PrintDifferences(std::vector<test::Difference> const& differences, std::uint64_t files)
    {
        std::printf("  %llu files compared\n", static_cast<unsigned long long>(files));
        std::map<std::string, std::vector<test::Difference const*>> byKind;
        for (test::Difference const& difference : differences)
        {
            byKind[difference.what].push_back(&difference);
        }
        for (auto const& [what, found] : byKind)
        {
            std::printf("\n  %s: %llu\n", what.c_str(), static_cast<unsigned long long>(found.size()));
            for (test::Difference const* const example : found | std::views::take(8))
            {
                std::printf("    %s  (listed %llu / table %llu)\n", test::ToUtf8(example->path).c_str(),
                            static_cast<unsigned long long>(example->listed),
                            static_cast<unsigned long long>(example->table));
            }
        }
        if (differences.empty())
        {
            std::printf("  no differences\n");
        }
    }
}

int RunCompare(std::wstring_view path)
{
    std::wstring const folder = sm::NormalizeScanPath(path);
    wchar_t volume[MAX_PATH + 1]{};
    if (folder.empty() || !GetVolumePathNameW(folder.c_str(), volume, static_cast<DWORD>(std::size(volume))))
    {
        std::printf("Cannot resolve the path.\n");
        return 2;
    }

    // As the app does: let the folder scanner list what only backup programs may.
    (void)sm::TryEnableBackupPrivilege();
    sm::ScanProgress listing;
    sm::ScanOptions options;
    options.masterFileTable = false;
    sm::ScanResult const listed = sm::ScanFolder(folder, listing, std::stop_token{}, options);
    sm::ScanProgress reading;
    sm::ScanResult const table = sm::ScanFolder(volume, reading, std::stop_token{});
    if (!listed.tree || !table.tree || table.tree->method != sm::ScanMethod::MasterFileTable)
    {
        std::printf("Reading the master file table needs an NTFS drive and administrator rights.\n");
        return 2;
    }

    sm::Folder const* const within = test::FindFolder(*table.tree->Root(), std::wstring_view(folder).substr(std::wcslen(volume)));
    if (within == nullptr)
    {
        std::printf("The folder is not in the master file table's tree.\n");
        return 1;
    }

    std::printf("Comparing %s\n\n", test::ToUtf8(folder).c_str());
    PrintTree("folder scanner", *listed.tree, *listed.tree->Root());
    PrintTree("master file table", *table.tree, *within);
    std::printf("\n");
    std::uint64_t files = 0;
    std::vector<test::Difference> const differences = test::CompareTrees(*listed.tree->Root(), *within, folder, &files);
    PrintDifferences(differences, files);
    return 0;
}
