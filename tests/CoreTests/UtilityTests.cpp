// Tests for the small helpers: text comparison and joining, and parallel loops.
#include "pch.h"
#include "TestHarness.h"

#include "../../src/Core/Parallel.h"
#include "../../src/Core/Text.h"

TEST_CASE(TextComparesAsFileNamesDo)
{
    CHECK(sm::EqualsIgnoringCase(L"NTFS", L"ntfs"));
    CHECK(sm::EqualsIgnoringCase(L"Ärger", L"äRGER"));
    CHECK(!sm::EqualsIgnoringCase(L"a", L"ab"));
    CHECK(sm::EqualsIgnoringCase(L"", L""));
    CHECK(!sm::EqualsIgnoringCase(L"", L"a"));
    CHECK(sm::LessIgnoringCase(L"apple", L"Banana"));
    CHECK(!sm::LessIgnoringCase(L"b", L"A"));
    CHECK(!sm::LessIgnoringCase(L"Same", L"same"));
    CHECK(sm::LessIgnoringCase(L"", L"a") && !sm::LessIgnoringCase(L"a", L""));
    std::wstring const middle = L"b";
    CHECK_EQ(sm::Join(L"a", middle, std::wstring_view(L"c"), L""), std::wstring(L"abc"));
}

TEST_CASE(ParallelLoopsRunEveryStepOnce)
{
    sm::ParallelFor(0, [](std::size_t) { CHECK(false); });

    std::vector<std::atomic<int>> runs(10'000);
    sm::ParallelFor(runs.size(), [&runs](std::size_t i) { runs[i].fetch_add(1); });
    CHECK(std::ranges::all_of(runs, [](std::atomic<int> const& count) { return count.load() == 1; }));
    CHECK(sm::ParallelThreads() >= 1);

    // An exception in a step comes out of the loop.
    bool thrown = false;
    try
    {
        sm::ParallelFor(100, [](std::size_t i) {
            if (i == 37)
            {
                throw std::runtime_error("step 37");
            }
        });
    }
    catch (std::runtime_error const& error)
    {
        thrown = std::string_view(error.what()) == "step 37";
    }
    CHECK(thrown);
}
