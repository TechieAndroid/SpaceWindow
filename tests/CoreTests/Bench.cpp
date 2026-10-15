// Scanner benchmarks.
//
//   CoreTests.exe --bench C:\Windows     a folder: the 1.4.0 algorithm against the modern scanner
//   CoreTests.exe --bench C:\            a whole drive: the folder scanner against the master file table
#include "pch.h"
#include "TestHarness.h"

#include "../../src/Core/FolderTree.h"
#include "../../src/Core/Format.h"
#include "../../src/Core/Lang.h"

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

    template <typename Body>
    double BestOfThree(Body&& body)
    {
        double best = 1e300;
        for (int run = 0; run < 3; ++run)
        {
            auto const started = std::chrono::steady_clock::now();
            body();
            std::chrono::duration<double, std::milli> const elapsed = std::chrono::steady_clock::now() - started;
            best = std::min(best, elapsed.count());
        }
        return best;
    }
}

int RunBenchmark(std::wstring_view path)
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
                            CompareStringOrdinal(volumeRoot, -1, root.c_str(), -1, TRUE) == CSTR_EQUAL;
    std::printf("Benchmarking %s (best of three runs, warm cache)\n\n", test::ToUtf8(root).c_str());

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
        return BestOfThree([&] {
            sm::ScanProgress progress;
            sm::ScanOptions options;
            options.threads = threads;
            options.masterFileTable = masterFileTable;
            tree = sm::ScanFolder(root, progress, std::stop_token{}, options).tree;
        });
    };
    auto const row = [](char const* name, std::uint64_t files, std::uint64_t folders, double ms, double baseMs) {
        std::printf("  %-34s %10llu %10llu %12.1f %8.2fx\n", name, static_cast<unsigned long long>(files),
                    static_cast<unsigned long long>(folders), ms, baseMs / ms);
    };
    std::printf("  %-34s %10s %10s %12s %9s\n", "scanner", "files", "folders", "time (ms)", "speed-up");

    double baseMs = 0.0;
    if (!wholeDrive)
    {
        Totals legacy;
        baseMs = BestOfThree([&] {
            legacy = {};
            std::wstring scanPath = sm::ToExtendedLengthPath(root);
            LegacyScan(scanPath, legacy);
        });
        row("1.4.0 algorithm (1 thread)", legacy.files, legacy.folders, baseMs, baseMs);
        std::unique_ptr<sm::FolderTree> single;
        double const singleMs = modern(1, false, single);
        row("modern, 1 thread", single->Root()->fileCount, single->Root()->folderCount, singleMs, baseMs);
    }
    std::unique_ptr<sm::FolderTree> parallel;
    double const parallelMs = modern(0, false, parallel);
    if (baseMs == 0.0)
    {
        baseMs = parallelMs;
    }
    std::string const label = "modern, " + std::to_string(parallel->threads) + " threads";
    row(label.c_str(), parallel->Root()->fileCount, parallel->Root()->folderCount, parallelMs, baseMs);

    std::unique_ptr<sm::FolderTree> table;
    double const tableMs = wholeDrive ? modern(0, true, table) : 0.0;
    if (table && table->method == sm::ScanMethod::MasterFileTable)
    {
        row("master file table", table->Root()->fileCount, table->Root()->folderCount, tableMs, baseMs);
    }
    else
    {
        std::printf("  (master file table: a whole NTFS drive, run as administrator)\n");
    }

    sm::Lang const& lang = sm::FindLanguage(L"us");
    std::printf("\n  On disk: %s in %s files; unreadable folders: %llu\n",
                test::ToUtf8(sm::FormatSize(parallel->Root()->SizeTotal(), lang)).c_str(),
                test::ToUtf8(sm::FormatCount(parallel->Root()->fileCount, lang)).c_str(),
                static_cast<unsigned long long>(parallel->unreadableFolders));
    return 0;
}
