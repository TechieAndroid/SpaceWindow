// Scanner benchmarks.
//
//   CoreTests.exe --bench C:\Windows         a folder: the 1.4.0 algorithm against the modern scanner
//   CoreTests.exe --bench C:\                a whole drive: the folder scanner against the master file table
//   CoreTests.exe --bench C:\ 4 8 12 16      ... with the folder scanner on that many threads each
//
// Each scanner runs three times; the fastest run counts, with the processor
// time it took (all threads).  Then reloads are timed on the trees scanned:
// listing the top folder again, and, for a drive read from its master file
// table, bringing the tree up to date from the change journal.
#include "pch.h"
#include "TestHarness.h"

#include "../../src/Core/ChangeJournal.h"
#include "../../src/Core/FolderTree.h"
#include "../../src/Core/Format.h"
#include "../../src/Core/Lang.h"
#include "../../src/Core/Ntfs.h"
#include "../../src/Core/Text.h"

namespace
{
    struct Totals
    {
        std::uint64_t files = 0;
        std::uint64_t folders = 0;
    };

    // CFolder::LoadFolder() of 1.4.0: recursive FindFirstFile/FindNextFile on a
    // single thread, opening every reparse point to read its tag.  Converted to
    // Unicode and \\?\ paths so it can walk the same trees.  Two changes keep the
    // comparison fair: the reparse buffer is on the heap (1.4.0's 17 KB stack
    // buffer per level overflows on deep trees), and the probe passes
    // FILE_FLAG_BACKUP_SEMANTICS - without it CreateFile cannot open a folder,
    // so 1.4.0 never recognized a junction and walked straight into it.
    void LegacyScan(std::wstring& path, Totals& totals)
    {
        std::size_t const length = path.size();
        path.push_back(L'*');
        WIN32_FIND_DATAW data{};
        wil::unique_hfind const find(FindFirstFileW(path.c_str(), &data));
        path.resize(length);
        if (!find)
        {
            return;
        }
        do
        {
            std::wstring_view const name(data.cFileName);
            if (name == L"." || name == L"..")
            {
                continue;
            }
            path.append(name);
            bool alias = false;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                wil::unique_hfile const file(CreateFileW(path.c_str(), 0, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                                         FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                                         nullptr));
                if (file)
                {
                    std::vector<BYTE> buffer(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
                    DWORD returned = 0;
                    if (DeviceIoControl(file.get(), FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer.data(),
                                        static_cast<DWORD>(buffer.size()), &returned, nullptr) &&
                        returned >= sizeof(DWORD))
                    {
                        DWORD tag = 0;
                        std::memcpy(&tag, buffer.data(), sizeof(tag));
                        alias = IsReparseTagNameSurrogate(tag);
                    }
                }
            }
            if (!alias)
            {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    ++totals.folders;
                    path.push_back(L'\\');
                    LegacyScan(path, totals);
                }
                else
                {
                    ++totals.files;
                }
            }
            path.resize(length);
        } while (FindNextFileW(find.get(), &data));
    }

    // Processor time used by this process so far, all threads, in milliseconds.
    double ProcessorMs() noexcept
    {
        FILETIME created{};
        FILETIME exited{};
        FILETIME kernel{};
        FILETIME user{};
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        {
            return 0.0;
        }
        auto const ticks = [](FILETIME const& time) {
            return static_cast<double>((std::uint64_t{ time.dwHighDateTime } << 32) | time.dwLowDateTime);
        };
        return (ticks(kernel) + ticks(user)) / 10'000.0;
    }

    struct Timing
    {
        double ms = 1e300;   // the fastest run
        double cpuMs = 0.0;  // the processor time of that run
    };

    // `before` runs ahead of each run, untimed.
    template <typename Before, typename Body>
    Timing BestOfThree(Before&& before, Body&& body)
    {
        Timing best;
        for (int run = 0; run < 3; ++run)
        {
            before();
            double const cpuBefore = ProcessorMs();
            auto const started = std::chrono::steady_clock::now();
            body();
            std::chrono::duration<double, std::milli> const elapsed = std::chrono::steady_clock::now() - started;
            double const cpu = ProcessorMs() - cpuBefore;
            if (elapsed.count() < best.ms)
            {
                best = { elapsed.count(), cpu };
            }
        }
        return best;
    }

    template <typename Body>
    Timing BestOfThree(Body&& body)
    {
        return BestOfThree([] {}, std::forward<Body>(body));
    }

    double Since(std::chrono::steady_clock::time_point started) noexcept
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }

    // Listing the top folder again, as Reload does after a change there.
    void BenchFolderReload(sm::FolderTree& tree)
    {
        double prepare = 1e300;
        double apply = 1e300;
        for (int run = 0; run < 3; ++run)
        {
            std::array<std::wstring const, 1> const folders{ std::wstring() };
            sm::ScanProgress progress;
            auto started = std::chrono::steady_clock::now();
            std::optional<sm::FolderReload> reload = sm::PrepareReload(tree, folders, {}, progress, std::stop_token{});
            prepare = std::min(prepare, Since(started));
            if (!reload)
            {
                std::printf("  (the reload was stopped)\n");
                return;
            }
            started = std::chrono::steady_clock::now();
            sm::ApplyReload(tree, std::move(*reload));
            apply = std::min(apply, Since(started));
        }
        std::printf("  Reload of the listed tree, the top folder listed again (fastest of three):\n");
        std::printf("    %-30s %10.1f ms\n", "list again", prepare);
        std::printf("    %-30s %10.1f ms\n", "apply", apply);
    }

    // Bringing the master file table's tree up to date from the change journal, as Reload does.
    void BenchJournalReload(sm::FolderTree& tree)
    {
        if (!tree.journal)
        {
            std::printf("  (the drive keeps no change journal)\n");
            return;
        }
        double large = 1e300;
        double read = 1e300;
        double apply = 1e300;
        std::size_t changed = 0;
        for (int run = 0; run < 3; ++run)
        {
            auto started = std::chrono::steady_clock::now();
            std::vector<std::uint32_t> const files = sm::LargeFiles(tree);
            large = std::min(large, Since(started));
            started = std::chrono::steady_clock::now();
            sm::JournalRead const changes = sm::ReadJournalChanges(*tree.journal, std::stop_token{}, 0, 0, files);
            read = std::min(read, Since(started));
            if (changes.status != sm::JournalReadStatus::Ready)
            {
                std::printf("  (the journal cannot tell what changed)\n");
                return;
            }
            changed = changes.changes.files.size();
            started = std::chrono::steady_clock::now();
            sm::ApplyJournalChanges(tree, changes.changes);
            apply = std::min(apply, Since(started));
        }
        std::printf("  Reload from the change journal (fastest of three; %zu files read again the last time):\n",
                    changed);
        std::printf("    %-30s %10.1f ms\n", "large files", large);
        std::printf("    %-30s %10.1f ms\n", "journal and records read", read);
        std::printf("    %-30s %10.1f ms\n", "apply", apply);
    }
}

int RunBenchmark(std::wstring_view path, std::span<unsigned const> threadCounts)
{
    std::wstring const root = sm::NormalizeScanPath(path);
    if (root.empty())
    {
        std::printf("Cannot resolve the path.\n");
        return 2;
    }
    // A whole drive would take the 1.4.0 algorithm many minutes, three times
    // over: there, the folder scanner is the yardstick for the master file table.
    wchar_t volumeRoot[MAX_PATH + 1]{};
    bool const wholeDrive = GetVolumePathNameW(root.c_str(), volumeRoot, static_cast<DWORD>(std::size(volumeRoot))) &&
                            sm::EqualsIgnoringCase(volumeRoot, root);
    std::printf("Benchmarking %s (fastest of three runs, warm cache)\n\n", test::ToUtf8(root).c_str());

    // Warm the file system cache so every contender sees the same conditions.
    {
        sm::ScanProgress progress;
        sm::ScanOptions options;
        options.masterFileTable = false;
        if (sm::ScanFolder(root, progress, std::stop_token{}, options).status != sm::ScanStatus::Completed)
        {
            std::printf("The folder cannot be scanned.\n");
            return 2;
        }
    }

    auto const modern = [&root](unsigned threads, bool masterFileTable, std::unique_ptr<sm::FolderTree>& tree) {
        // The last run's tree is freed before the next run starts, untimed.
        return BestOfThree([&tree] { tree.reset(); },
                           [&] {
                               sm::ScanProgress progress;
                               sm::ScanOptions options;
                               options.threads = threads;
                               options.masterFileTable = masterFileTable;
                               tree = sm::ScanFolder(root, progress, std::stop_token{}, options).tree;
                           });
    };
    auto const row = [](char const* name, std::uint64_t files, std::uint64_t folders, Timing const& timing,
                        double baseMs) {
        std::printf("  %-34s %10llu %10llu %12.1f %12.1f %8.2fx\n", name, static_cast<unsigned long long>(files),
                    static_cast<unsigned long long>(folders), timing.ms, timing.cpuMs, baseMs / timing.ms);
    };
    std::printf("  %-34s %10s %10s %12s %12s %9s\n", "scanner", "files", "folders", "time (ms)", "CPU (ms)",
                "speed-up");

    double baseMs = 0.0;
    if (!wholeDrive)
    {
        Totals legacy;
        Timing const timing = BestOfThree([&] {
            legacy = {};
            std::wstring scanPath = sm::ToExtendedLengthPath(root);
            LegacyScan(scanPath, legacy);
        });
        baseMs = timing.ms;
        row("1.4.0 algorithm (1 thread)", legacy.files, legacy.folders, timing, baseMs);
        std::unique_ptr<sm::FolderTree> single;
        Timing const singleTiming = modern(1, false, single);
        row("modern, 1 thread", single->Root()->fileCount, single->Root()->folderCount, singleTiming, baseMs);
    }
    std::unique_ptr<sm::FolderTree> parallel;
    for (unsigned const threads : threadCounts)
    {
        std::unique_ptr<sm::FolderTree> tree;
        Timing const timing = modern(threads, false, tree);
        baseMs = baseMs == 0.0 ? timing.ms : baseMs;
        std::string const label = "modern, " + std::to_string(tree->threads) + " threads";
        row(label.c_str(), tree->Root()->fileCount, tree->Root()->folderCount, timing, baseMs);
    }
    {
        Timing const timing = modern(0, false, parallel);
        baseMs = baseMs == 0.0 ? timing.ms : baseMs;
        std::string const label = "modern, " + std::to_string(parallel->threads) + " threads (default)";
        row(label.c_str(), parallel->Root()->fileCount, parallel->Root()->folderCount, timing, baseMs);
    }

    std::unique_ptr<sm::FolderTree> table;
    Timing const tableTiming = wholeDrive ? modern(0, true, table) : Timing{};
    if (table && table->method == sm::ScanMethod::MasterFileTable)
    {
        row("master file table", table->Root()->fileCount, table->Root()->folderCount, tableTiming, baseMs);
        // What the disk takes to deliver the table, with nothing done with it.
        if (std::optional<sm::ntfs::Volume> const volume = sm::ntfs::Volume::Open(root))
        {
            std::uint64_t records = 0;
            Timing const readTiming = BestOfThree([&] {
                records = 0;
                (void)volume->ReadAll(std::stop_token{}, [&records, size = volume->RecordSize()](
                                                             std::uint64_t, std::span<std::byte> block) {
                    records += block.size() / size;
                });
            });
            row("  ... its reads alone", records, 0, readTiming, baseMs);
        }
    }
    else
    {
        std::printf("  (master file table: a whole NTFS drive, run as administrator)\n");
    }

    sm::Lang const& lang = sm::FindLanguage(L"us");
    std::printf("\n  On disk: %s in %s files; unreadable folders: %llu\n\n",
                test::ToUtf8(sm::FormatSize(parallel->Root()->SizeTotal(), lang)).c_str(),
                test::ToUtf8(sm::FormatCount(parallel->Root()->fileCount, lang)).c_str(),
                static_cast<unsigned long long>(parallel->unreadableFolders));

    BenchFolderReload(*parallel);
    if (table && table->method == sm::ScanMethod::MasterFileTable)
    {
        BenchJournalReload(*table);
    }
    return 0;
}
