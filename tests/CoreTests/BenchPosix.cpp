// Scanner benchmark, on Linux.
//
//   CoreTests --bench /usr             a classic single-threaded walk against the scanner
//   CoreTests --bench /usr 4 8 12 16   ... with the scanner on that many threads each
//
// As Bench.cpp on Windows: each scanner runs three times, and the fastest run
// counts, with the processor time it took (all threads).  The yardstick is
// the walk du and SpaceMonger 1.4.0 make: one thread, readdir and lstat for
// every entry, recursively.  Then a reload of the watched tree is timed.
#include "pch.h"
#include "TestHarness.h"

#include "../../src/Core/FolderTree.h"
#include "../../src/Core/FolderWatch.h"
#include "../../src/Core/Format.h"
#include "../../src/Core/Lang.h"

#include <sys/resource.h>

namespace
{
    struct Timing
    {
        double ms = 0.0;
        double cpuMs = 0.0;
    };

    double ProcessorMs()
    {
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        auto const ms = [](timeval const& time) { return time.tv_sec * 1000.0 + time.tv_usec / 1000.0; };
        return ms(usage.ru_utime) + ms(usage.ru_stime);
    }

    template <typename Prepare, typename Run>
    Timing BestOfThree(Prepare const& prepare, Run const& run)
    {
        Timing best{ std::numeric_limits<double>::infinity(), 0.0 };
        for (int i = 0; i < 3; ++i)
        {
            prepare();
            double const cpu = ProcessorMs();
            auto const started = std::chrono::steady_clock::now();
            run();
            double const ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            if (ms < best.ms)
            {
                best = { ms, ProcessorMs() - cpu };
            }
        }
        return best;
    }

    struct Totals
    {
        std::uint64_t files = 0;
        std::uint64_t folders = 0;
    };

    // One thread, readdir and lstat for every entry, recursively: the walk du makes.
    void ClassicScan(std::string const& path, dev_t device, Totals& totals)
    {
        DIR* const listing = opendir(path.c_str());
        if (listing == nullptr)
        {
            return;
        }
        while (dirent const* const entry = readdir(listing))
        {
            std::string_view const name(entry->d_name);
            if (name == "." || name == "..")
            {
                continue;
            }
            std::string const child = path + "/" + entry->d_name;
            struct stat status{};
            if (lstat(child.c_str(), &status) != 0)
            {
                continue;
            }
            if (S_ISDIR(status.st_mode))
            {
                if (status.st_dev == device)
                {
                    ++totals.folders;
                    ClassicScan(child, device, totals);
                }
            }
            else if (!S_ISLNK(status.st_mode))
            {
                ++totals.files;
            }
        }
        closedir(listing);
    }
}

int RunBenchmark(std::wstring_view path, std::span<unsigned const> threadCounts)
{
    std::wstring const root = sm::NormalizeScanPath(path);
    struct stat status{};
    if (root.empty() || stat(sm::posix::ToNative(root).c_str(), &status) != 0)
    {
        std::printf("Cannot resolve the path.\n");
        return 2;
    }
    std::printf("Benchmarking %s (fastest of three runs, warm cache)\n\n", test::ToUtf8(root).c_str());

    // Warm the file system cache so every contender sees the same conditions.
    {
        sm::ScanProgress progress;
        if (sm::ScanFolder(root, progress, std::stop_token{}).status != sm::ScanStatus::Completed)
        {
            std::printf("The folder cannot be scanned.\n");
            return 2;
        }
    }

    auto const row = [](char const* name, std::uint64_t files, std::uint64_t folders, Timing const& timing,
                        double baseMs) {
        std::printf("  %-34s %10llu %10llu %12.1f %12.1f %8.2fx\n", name, static_cast<unsigned long long>(files),
                    static_cast<unsigned long long>(folders), timing.ms, timing.cpuMs, baseMs / timing.ms);
    };
    std::printf("  %-34s %10s %10s %12s %12s %9s\n", "scanner", "files", "folders", "time (ms)", "CPU (ms)",
                "speed-up");

    Totals classic;
    std::string native = sm::posix::ToNative(root);
    if (native.size() > 1)
    {
        native.pop_back();  // without its final '/'
    }
    Timing const classicTiming =
        BestOfThree([&classic] { classic = {}; }, [&] { ClassicScan(native, status.st_dev, classic); });
    double const baseMs = classicTiming.ms;
    row("readdir + lstat (1 thread)", classic.files, classic.folders, classicTiming, baseMs);

    auto const modern = [&root](unsigned threads, bool watch, std::unique_ptr<sm::FolderTree>& tree) {
        return BestOfThree([&tree] { tree.reset(); },
                           [&] {
                               sm::ScanProgress progress;
                               sm::ScanOptions options;
                               options.threads = threads;
                               options.watch = watch;
                               tree = sm::ScanFolder(root, progress, std::stop_token{}, options).tree;
                           });
    };
    std::vector<unsigned> counts{ 1 };
    counts.insert(counts.end(), threadCounts.begin(), threadCounts.end());
    for (unsigned const threads : counts)
    {
        std::unique_ptr<sm::FolderTree> tree;
        Timing const timing = modern(threads, false, tree);
        std::string const label = "scanner, " + std::to_string(tree->threads) + (threads == 1 ? " thread" : " threads");
        row(label.c_str(), tree->Root()->fileCount, tree->Root()->folderCount, timing, baseMs);
    }
    std::unique_ptr<sm::FolderTree> parallel;
    Timing const timing = modern(0, false, parallel);
    std::string const label = "scanner, " + std::to_string(parallel->threads) + " threads (default)";
    row(label.c_str(), parallel->Root()->fileCount, parallel->Root()->folderCount, timing, baseMs);
    std::unique_ptr<sm::FolderTree> watched;
    Timing const watchedTiming = modern(0, true, watched);
    row("  ... watched for reloads", watched->Root()->fileCount, watched->Root()->folderCount, watchedTiming, baseMs);

    sm::Lang const& lang = sm::FindLanguage(L"us");
    sm::Folder const& top = *parallel->Root();
    std::printf(
        "\n  On disk: %s (%llu bytes) in %s files and %s folders; length %llu bytes; unreadable folders: %llu\n",
        test::ToUtf8(sm::FormatSize(top.SizeTotal(), lang)).c_str(), static_cast<unsigned long long>(top.SizeTotal()),
        test::ToUtf8(sm::FormatCount(top.fileCount, lang)).c_str(),
        test::ToUtf8(sm::FormatCount(top.folderCount, lang)).c_str(), static_cast<unsigned long long>(top.logicalTotal),
        static_cast<unsigned long long>(parallel->unreadableFolders));
    std::printf("  Volume %s (%s): %s, %s free\n", test::ToUtf8(parallel->Volume().root).c_str(),
                test::ToUtf8(parallel->Volume().fileSystem).c_str(),
                test::ToUtf8(sm::FormatSize(parallel->Volume().totalBytes, lang)).c_str(),
                test::ToUtf8(sm::FormatSize(parallel->Volume().freeBytes, lang)).c_str());

    // A reload: the watch saw nothing change, so the top folder alone is listed again.
    if (watched->watch)
    {
        sm::FolderWatch::Changes changes = watched->watch->Take();
        std::printf("\n  Reload (watch %s):\n", changes.complete ? "kept track" : "lost track: a full scan is due");
        if (changes.complete)
        {
            std::vector<std::wstring> const folders{ L"" };
            sm::ScanProgress progress;
            std::optional<sm::FolderReload> reload;
            auto const started = std::chrono::steady_clock::now();
            reload = sm::PrepareReload(*watched, folders, changes.files, progress, std::stop_token{});
            double const prepared =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            auto const applying = std::chrono::steady_clock::now();
            if (!reload)
            {
                return 1;
            }
            sm::ApplyReload(*watched, std::move(*reload));
            double const applied =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - applying).count();
            std::printf("    %-30s %10.1f ms\n", "top folder listed again", prepared);
            std::printf("    %-30s %10.1f ms\n", "put in place", applied);
        }
    }
    else
    {
        std::printf("\n  (no reload: the tree could not be watched)\n");
    }
    return 0;
}
