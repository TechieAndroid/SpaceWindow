// SpaceWindow core tests.
//
//   CoreTests.exe                    run every test but those that read a drive directly
//   CoreTests.exe <filter>           run the tests whose name contains <filter>
//   CoreTests.exe --volume [filter]  ... including those that read a drive directly (as administrator)
//   CoreTests.exe --bench <path>     benchmark the scanner on a real folder or drive
//   CoreTests.exe --compare <path>   compare the master file table scan with the folder scanner
#include "pch.h"
#include "TestHarness.h"

int RunBenchmark(std::wstring_view path);
int RunCompare(std::wstring_view path);

namespace test
{
    std::vector<TestCase>& Registry()
    {
        static std::vector<TestCase> tests;
        return tests;
    }

    std::string ToUtf8(std::wstring_view text)
    {
        if (text.empty())
        {
            return {};
        }
        int const length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                               nullptr, nullptr);
        std::string result(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr,
                            nullptr);
        return result;
    }

    void Fail(std::string const& message, std::source_location location)
    {
        throw Failure{ std::string(location.file_name()) + "(" + std::to_string(location.line()) + "): " + message };
    }
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);

    if (argc >= 3 && std::wstring_view(argv[1]) == L"--bench")
    {
        return RunBenchmark(argv[2]);
    }
    if (argc >= 3 && std::wstring_view(argv[1]) == L"--compare")
    {
        return RunCompare(argv[2]);
    }
    int first = 1;
    bool volume = false;
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--volume")
    {
        volume = true;
        first = 2;
    }
    std::string const filter = argc > first ? test::ToUtf8(argv[first]) : std::string();

    int passed = 0;
    int failed = 0;
    int skipped = 0;
    auto const started = std::chrono::steady_clock::now();
    for (test::TestCase const& testCase : test::Registry())
    {
        if (!filter.empty() && std::string_view(testCase.name).find(filter) == std::string_view::npos)
        {
            continue;
        }
        if (testCase.readsVolume && !volume)
        {
            ++skipped;
            continue;
        }
        try
        {
            testCase.body();
            ++passed;
            std::printf("  [pass] %s\n", testCase.name);
        }
        catch (test::Failure const& failure)
        {
            ++failed;
            std::printf("  [FAIL] %s\n      %s\n", testCase.name, failure.message.c_str());
        }
        catch (std::exception const& error)
        {
            ++failed;
            std::printf("  [FAIL] %s\n      unexpected exception: %s\n", testCase.name, error.what());
        }
    }
    auto const elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    std::printf("\n%d passed, %d failed (%lld ms)\n", passed, failed, static_cast<long long>(elapsed.count()));
    if (skipped != 0)
    {
        std::printf("%d skipped: they read a drive directly (run with --volume, as administrator)\n", skipped);
    }
    return failed == 0 && passed > 0 ? 0 : 1;
}
