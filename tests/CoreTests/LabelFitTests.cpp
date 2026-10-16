// Tests for fitting the map's labels into boxes and slices.
#include "pch.h"
#include "TestHarness.h"

#include "../../src/Core/LabelFit.h"

namespace
{
    // Letters 10 pixels wide, each one UTF-16 code unit.
    std::vector<sm::Cluster> Letters(std::wstring_view text)
    {
        return std::vector<sm::Cluster>(text.size(), sm::Cluster{ 10.0f, 1 });
    }

    // Text 7 pixels a character.
    double Measure(std::wstring_view text)
    {
        return 7.0 * static_cast<double>(text.size());
    }

    // Names shortened as the renderer does, with letters 5 pixels wide and dots 10.
    std::wstring Shorten(std::wstring_view name, double width)
    {
        std::vector<sm::Cluster> const letters(name.size(), sm::Cluster{ 5.0f, 1 });
        return sm::FitName(name, letters, static_cast<float>(width), 10.0f);
    }

    constexpr double kPi = std::numbers::pi;
    constexpr sm::PiePoint kCenter{ 200.0, 200.0 };
}

TEST_CASE(FolderNamesAreShortenedToFiveLettersAtLeast)
{
    CHECK_EQ(sm::FitName(L"Documents", Letters(L"Documents"), 90.0f, 15.0f), std::wstring(L"Documents"));  // fits whole
    CHECK_EQ(sm::FitName(L"Documents", Letters(L"Documents"), 80.0f, 15.0f), std::wstring(L"Docume..."));
    CHECK_EQ(sm::FitName(L"Documents", Letters(L"Documents"), 65.0f, 15.0f), std::wstring(L"Docum..."));
    CHECK(sm::FitName(L"Documents", Letters(L"Documents"), 64.0f, 15.0f).empty());  // four letters: no name at all
    CHECK(sm::FitName(L"", {}, 100.0f, 15.0f).empty());
    // No space before the dots.
    CHECK_EQ(sm::FitName(L"Program Files", Letters(L"Program Files"), 100.0f, 15.0f), std::wstring(L"Program..."));

    // A letter is whole or not at all: an emoji is two code units, one letter.
    std::wstring const name = L"ab\U0001F600cdefg";
    std::vector<sm::Cluster> clusters{ { 10.0f, 1 }, { 10.0f, 1 }, { 20.0f, 2 }, { 10.0f, 1 },
                                       { 10.0f, 1 }, { 10.0f, 1 }, { 10.0f, 1 }, { 10.0f, 1 } };
    CHECK_EQ(sm::FitName(name, clusters, 70.0f, 10.0f), std::wstring(L"ab\U0001F600cd..."));
    clusters[2].width = 41.0f;  // too wide to go before the dots: the name would stop at two letters
    CHECK(sm::FitName(name, clusters, 70.0f, 10.0f).empty());
}

TEST_CASE(RingSegmentsKeepTheirLabelsInside)
{
    sm::RingSegment const disc{ kCenter, 0.0, 50.0 };
    CHECK(sm::Inside(disc, 200.0, 200.0, 0.0));
    CHECK(sm::Inside(disc, 249.0, 200.0, 0.0));
    CHECK(!sm::Inside(disc, 251.0, 200.0, 0.0));
    CHECK(!sm::Inside(disc, 246.0, 200.0, 5.0));  // not 5 pixels from the edge

    // A block 20 high at the centre of a disc of radius 50: about 2 * sqrt(50² - 10²) wide.
    double const room = sm::Room(disc, kCenter, 20.0, 0.0);
    CHECK(room > 97.0 && room <= 98.0);

    // A ring has no room in its hole, and none for a block taller than it is thick.
    sm::RingSegment const ring{ kCenter, 40.0, 100.0 };
    CHECK_EQ(sm::Room(ring, kCenter, 10.0, 0.0), 0.0);
    sm::PiePoint const onRing{ kCenter.x, kCenter.y - 70.0 };
    CHECK(sm::Room(ring, onRing, 20.0, 0.0) > 0.0);
    CHECK_EQ(sm::Room(ring, onRing, 70.0, 0.0), 0.0);

    // A quarter of the ring (twelve to three o'clock) has none on the other side.
    sm::RingSegment const quarter{ kCenter, 40.0, 100.0, 0.0, kPi / 2.0 };
    double const r = 70.0 / std::numbers::sqrt2;
    CHECK(sm::Room(quarter, { kCenter.x + r, kCenter.y - r }, 14.0, 2.0) > 0.0);
    CHECK_EQ(sm::Room(quarter, { kCenter.x - r, kCenter.y - r }, 14.0, 2.0), 0.0);
}

TEST_CASE(SliceLabelsTakeTheRoomThereIs)
{
    constexpr double kLine = 14.0;
    // Plenty of room: name and size on two lines, centred on the anchor.
    sm::RingSegment const disc{ kCenter, 0.0, 100.0 };
    std::optional<sm::FittedLabel> label =
        sm::FitLabel(disc, kCenter, 2.0, kLine, L"Photos", L"1.2 GB", true, false, Measure, Shorten);
    CHECK(label.has_value() && label->text == L"Photos\n1.2 GB" && !label->trimmed);
    CHECK_EQ(label->bottom - label->top, 2.0 * kLine);
    CHECK_EQ((label->left + label->right) / 2.0, kCenter.x);

    // A ring too thin for two lines: the name alone - or, side by side, both.
    sm::RingSegment const thin{ kCenter, 80.0, 100.0 };
    sm::PiePoint const top{ kCenter.x, kCenter.y - 90.0 };
    label = sm::FitLabel(thin, top, 0.0, kLine, L"ab", L"cd", false, false, Measure, Shorten);
    CHECK(label.has_value() && label->text == L"ab");
    auto const narrow = [](std::wstring_view text) { return 5.0 * static_cast<double>(text.size()); };
    label = sm::FitLabel(thin, top, 0.0, kLine, L"a", L"b", false, true, narrow, Shorten);
    CHECK(label.has_value() && label->text == L"a  ·  b");

    // A name longer than the room: shortened if it may be, else left out.
    std::wstring const longName = L"A rather long folder name";
    label = sm::FitLabel(thin, top, 0.0, kLine, longName, L"9 KB", true, false, Measure, Shorten);
    CHECK(label.has_value() && label->trimmed && label->text.ends_with(L"..."));
    CHECK(!sm::FitLabel(thin, top, 0.0, kLine, longName, L"9 KB", false, false, Measure, Shorten).has_value());
}
