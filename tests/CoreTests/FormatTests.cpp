// Tests for formatting, string tables and colour schemes.
#include "pch.h"
#include "TestHarness.h"

#include "../../src/Core/Commands.h"
#include "../../src/Core/Format.h"
#include "../../src/Core/Lang.h"
#include "../../src/Core/Palette.h"

namespace
{
    sm::Lang const& Us()
    {
        return sm::FindLanguage(L"us");
    }
    sm::Lang const& Fr()
    {
        return sm::FindLanguage(L"fr");
    }

    constexpr std::uint64_t KiB = 1024;
    constexpr std::uint64_t MiB = KiB * 1024;
    constexpr std::uint64_t GiB = MiB * 1024;
    constexpr std::uint64_t TiB = GiB * 1024;
    constexpr std::uint64_t PiB = TiB * 1024;
}

TEST_CASE(FormatCountGroupsThousands)
{
    CHECK_EQ(sm::FormatCount(0, Us()), std::wstring(L"0"));
    CHECK_EQ(sm::FormatCount(999, Us()), std::wstring(L"999"));
    CHECK_EQ(sm::FormatCount(1000, Us()), std::wstring(L"1,000"));
    CHECK_EQ(sm::FormatCount(1234567, Us()), std::wstring(L"1,234,567"));
    CHECK_EQ(sm::FormatCount(UINT64_MAX, Us()), std::wstring(L"18,446,744,073,709,551,615"));
    CHECK_EQ(sm::FormatCount(1234567, Fr()), std::wstring(L"1 234 567"));
}

TEST_CASE(FormatBytesPrintsZero)
{
    // 1.4.0's PrintFileSize() printed nothing at all for an empty file.
    CHECK_EQ(sm::FormatBytes(0, Us()), std::wstring(L"0 bytes"));
    CHECK_EQ(sm::FormatBytes(4096, Us()), std::wstring(L"4,096 bytes"));
    CHECK_EQ(sm::FormatBytes(4096, Fr()), std::wstring(L"4 096 octets"));
}

TEST_CASE(FormatSizeUsesBinaryUnits)
{
    CHECK_EQ(sm::FormatSize(0, Us()), std::wstring(L"0 bytes"));
    CHECK_EQ(sm::FormatSize(1023, Us()), std::wstring(L"1,023 bytes"));
    CHECK_EQ(sm::FormatSize(1024, Us()), std::wstring(L"1.0 KB"));
    CHECK_EQ(sm::FormatSize(1536, Us()), std::wstring(L"1.5 KB"));
    CHECK_EQ(sm::FormatSize(MiB - 1, Us()), std::wstring(L"1,023.9 KB"));  // truncated, never rounded up
    CHECK_EQ(sm::FormatSize(5 * MiB + MiB / 2, Us()), std::wstring(L"5.5 MB"));
    CHECK_EQ(sm::FormatSize(3 * GiB, Us()), std::wstring(L"3.0 GB"));
    CHECK_EQ(sm::FormatSize(5 * GiB / 2, Fr()), std::wstring(L"2,5 Go"));
}

TEST_CASE(FormatSizeHandlesTerabytes)
{
    // 1.4.0 stopped at gigabytes: a 4 TB drive read "4096.0 Gb".
    CHECK_EQ(sm::FormatSize(4 * TiB, Us()), std::wstring(L"4.0 TB"));
    CHECK_EQ(sm::FormatSize(3 * PiB, Us()), std::wstring(L"3.0 PB"));
    CHECK_EQ(sm::FormatSize(UINT64_MAX, Us()), std::wstring(L"16,383.9 PB"));
}

TEST_CASE(FormatPercentTruncatesAndAvoidsOverflow)
{
    CHECK_EQ(sm::FormatPercent(1, 3, Us()), std::wstring(L"33.3%"));
    CHECK_EQ(sm::FormatPercent(2, 3, Us()), std::wstring(L"66.6%"));
    CHECK_EQ(sm::FormatPercent(5, 5, Us()), std::wstring(L"100.0%"));
    CHECK_EQ(sm::FormatPercent(7, 0, Us()), std::wstring(L"0.0%"));
    CHECK_EQ(sm::FormatPercent(1, 8, Fr()), std::wstring(L"12,5 %"));
    // part * 1000 overflows 64 bits here; 1.4.0 would have printed garbage.
    CHECK_EQ(sm::FormatPercent(std::uint64_t{ 1 } << 62, std::uint64_t{ 1 } << 63, Us()), std::wstring(L"50.0%"));
    CHECK_EQ(sm::PerMille(UINT64_MAX - 1, UINT64_MAX), 999u);
}

TEST_CASE(FormatFileTimeUsesLocalTime)
{
    SYSTEMTIME universal{};
    universal.wYear = 1999;
    universal.wMonth = 3;
    universal.wDay = 5;
    universal.wHour = 14;
    universal.wMinute = 3;
    universal.wSecond = 22;
    FILETIME fileTime{};
    CHECK(SystemTimeToFileTime(&universal, &fileTime));
    std::uint64_t const value = (std::uint64_t{ fileTime.dwHighDateTime } << 32) | fileTime.dwLowDateTime;

    SYSTEMTIME local{};
    CHECK(SystemTimeToTzSpecificLocalTimeEx(nullptr, &universal, &local));
    std::wstring const month(Us().monthNames[local.wMonth - 1u]);
    wchar_t expected[64];
    CHECK(swprintf_s(expected, L"%02u %ls %04u   %u:%02u:%02u", static_cast<unsigned>(local.wDay), month.c_str(),
                     static_cast<unsigned>(local.wYear), static_cast<unsigned>(local.wHour),
                     static_cast<unsigned>(local.wMinute), static_cast<unsigned>(local.wSecond)) > 0);
    CHECK_EQ(sm::FormatFileTime(value, Us()), std::wstring(expected));
}

TEST_CASE(FormatAttributesListsNames)
{
    CHECK_EQ(sm::FormatAttributes(0, Us()), std::wstring());
    CHECK_EQ(sm::FormatAttributes(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM, Us()), std::wstring(L"Hidden System"));
    CHECK_EQ(sm::FormatAttributes(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE, Fr()),
             std::wstring(L"Archive Lecture seule"));
}

TEST_CASE(FormatTextSubstitutesOnlyKnownPlaceholders)
{
    CHECK_EQ(sm::FormatText(L"Move {0} ({1})?", { L"a.txt", L"1 KB" }), std::wstring(L"Move a.txt (1 KB)?"));
    CHECK_EQ(sm::FormatText(L"{1}{0}{1}", { L"x", L"y" }), std::wstring(L"yxy"));
    // A translation that refers to a missing argument cannot read garbage.
    CHECK_EQ(sm::FormatText(L"{0} {2} {x} {", { L"a" }), std::wstring(L"a {2} {x} {"));
}

TEST_CASE(ParseCaptionExtractsAccessKey)
{
    sm::Caption const zoom = sm::ParseCaption(L"Zoom &In");
    CHECK_EQ(zoom.text, std::wstring(L"Zoom In"));
    CHECK_EQ(zoom.accessKey, std::wstring(L"I"));

    sm::Caption const ampersand = sm::ParseCaption(L"Copy && Paste");
    CHECK_EQ(ampersand.text, std::wstring(L"Copy & Paste"));
    CHECK_EQ(ampersand.accessKey, std::wstring());

    CHECK_EQ(sm::ParseCaption(L"Trailing&").text, std::wstring(L"Trailing"));
}

TEST_CASE(LanguagesAreComplete)
{
    // A positional initializer that skipped a string shifted every later one;
    // with designated initializers a forgotten string is simply empty.
    using Field = std::wstring_view sm::Lang::*;
    // clang-format off: laid out by hand
    static constexpr Field kFields[] = {
        &sm::Lang::code, &sm::Lang::name, &sm::Lang::locale, &sm::Lang::aboutApp, &sm::Lang::freeware,
        &sm::Lang::warranty, &sm::Lang::credits, &sm::Lang::ok, &sm::Lang::cancel, &sm::Lang::bytes, &sm::Lang::kb,
        &sm::Lang::mb, &sm::Lang::gb, &sm::Lang::tb, &sm::Lang::pb, &sm::Lang::percent, &sm::Lang::zoomIn,
        &sm::Lang::zoomOut, &sm::Lang::zoomFull, &sm::Lang::run, &sm::Lang::del, &sm::Lang::openDrive,
        &sm::Lang::rescanDrive, &sm::Lang::showFreeSpace, &sm::Lang::pieChart, &sm::Lang::properties,
        &sm::Lang::showInExplorer, &sm::Lang::copyPath, &sm::Lang::tbOpen, &sm::Lang::tbReload, &sm::Lang::tbZoomFull,
        &sm::Lang::tbZoomIn, &sm::Lang::tbZoomOut, &sm::Lang::tbFreeSpace, &sm::Lang::tbPieChart, &sm::Lang::tbRun,
        &sm::Lang::tbDelete, &sm::Lang::tbSetup, &sm::Lang::tbAbout, &sm::Lang::freeSpace, &sm::Lang::unscanned,
        &sm::Lang::total, &sm::Lang::free, &sm::Lang::filesTotal, &sm::Lang::foldersTotal, &sm::Lang::filesFound,
        &sm::Lang::foldersFound, &sm::Lang::onDisk, &sm::Lang::unreadableFolders, &sm::Lang::smallerItems,
        &sm::Lang::fullRescan, &sm::Lang::rescanFolder, &sm::Lang::hardLink, &sm::Lang::unreadableFolder,
        &sm::Lang::unreadableTitle, &sm::Lang::unreadableText, &sm::Lang::moreFolders, &sm::Lang::copyList,
        &sm::Lang::close, &sm::Lang::invertedPie, &sm::Lang::tbInvertedPie, &sm::Lang::backgroundAnimation,
        &sm::Lang::backgroundAnimationHint, &sm::Lang::keyboardShortcuts, &sm::Lang::selectDrive,
        &sm::Lang::browseFolder,
        &sm::Lang::scanning, &sm::Lang::scanFailed, &sm::Lang::confirmDeleteTitle, &sm::Lang::confirmDelete,
        &sm::Lang::deleteFailed, &sm::Lang::emptyTitle, &sm::Lang::emptyText, &sm::Lang::settings,
        &sm::Lang::layout, &sm::Lang::density, &sm::Lang::bias, &sm::Lang::horz, &sm::Lang::equal, &sm::Lang::vert,
        &sm::Lang::displayColors, &sm::Lang::files, &sm::Lang::folders, &sm::Lang::toolTips,
        &sm::Lang::showNameTips, &sm::Lang::showInfoTips, &sm::Lang::delay, &sm::Lang::fullPath,
        &sm::Lang::fileName, &sm::Lang::icon, &sm::Lang::dateTime, &sm::Lang::fileSize, &sm::Lang::attrib,
        &sm::Lang::miscOptions, &sm::Lang::autoRescan, &sm::Lang::disableDelete, &sm::Lang::animatedZoom,
        &sm::Lang::savePos, &sm::Lang::showRolloverBox, &sm::Lang::appearance, &sm::Lang::theme,
        &sm::Lang::themesFolder, &sm::Lang::themeErrors, &sm::Lang::language,
    };
    // clang-format on

    std::set<std::wstring_view> codes;
    for (sm::Lang const& lang : sm::Languages())
    {
        CHECK(codes.insert(lang.code).second);
        CHECK_EQ(lang.code.size(), std::size_t{ 2 });
        for (Field const field : kFields)
        {
            CHECK(!(lang.*field).empty());
        }
        for (auto const& list :
             { std::span<std::wstring_view const>(lang.densityNames),
               std::span<std::wstring_view const>(lang.colorNames), std::span<std::wstring_view const>(lang.monthNames),
               std::span<std::wstring_view const>(lang.attributeNames),
               std::span<std::wstring_view const>(lang.themeNames),
               std::span<std::wstring_view const>(lang.backdropNames) })
        {
            for (std::wstring_view const name : list)
            {
                CHECK(!name.empty());
            }
        }
        // Every key has a name for its key cap, and every command with keys says what it does.
        sm::KeyNames const& keys = lang.keyNames;
        for (std::wstring_view const key : { keys.ctrl, keys.shift, keys.alt, keys.enter, keys.backspace, keys.home,
                                             keys.escape, keys.del, keys.menu })
        {
            CHECK(!key.empty() && key.front() != L' ' && key.back() != L' ' &&
                  key.find(L'+') == std::wstring_view::npos);
        }
        for (sm::Command const command : sm::CommandsWithKeys())
        {
            CHECK(!sm::ShortcutAction(command, lang).empty());
        }
        CHECK(lang.decimalPoint != lang.thousandsSeparator);
        CHECK(sm::FormatText(lang.confirmDelete, { L"\x1", L"\x2" }).find(L'\x1') != std::wstring::npos);
        CHECK(sm::FormatText(lang.confirmDelete, { L"\x1", L"\x2" }).find(L'\x2') != std::wstring::npos);
        CHECK(sm::FormatText(lang.smallerItems, { L"\x1" }).find(L'\x1') != std::wstring::npos);
    }
    CHECK_EQ(&sm::FindLanguage(L"zz"), &sm::Languages().front());
}

TEST_CASE(ContextMenuAccessKeysAreUnique)
{
    // 1.4.0 used O for both "Zoom &Out" and "&Open Drive...".
    for (sm::Lang const& lang : sm::Languages())
    {
        std::set<wchar_t> keys;
        for (std::wstring_view const caption :
             { lang.zoomIn, lang.zoomOut, lang.zoomFull, lang.run, lang.del, lang.openDrive, lang.rescanDrive,
               lang.fullRescan, lang.rescanFolder, lang.showFreeSpace, lang.pieChart, lang.invertedPie, lang.properties,
               lang.showInExplorer, lang.copyPath })
        {
            std::wstring const key = sm::ParseCaption(caption).accessKey;
            CHECK_EQ(key.size(), std::size_t{ 1 });
            CHECK(keys.insert(static_cast<wchar_t>(towupper(key.front()))).second);
        }
    }
}

TEST_CASE(SchemeColorsMatchTheOriginalPalette)
{
    sm::BoxColors const rainbow0 = sm::SchemeColors(sm::kSchemeRainbow, 0, false);
    CHECK(rainbow0.face == sm::kRainbow[0]);
    CHECK(rainbow0.highlight == sm::kRainbow[8]);
    CHECK(rainbow0.shadow == sm::kRainbow[16]);

    // The rainbow repeats every eight levels.
    sm::BoxColors const rainbow11 = sm::SchemeColors(sm::kSchemeRainbow, 11, false);
    CHECK(rainbow11.face == sm::kRainbow[3]);

    // "White" is the first fixed scheme, "Violet" the last.
    CHECK(sm::SchemeColors(2, 0, false).face == sm::kFixed[0]);
    sm::BoxColors const violet = sm::SchemeColors(sm::kSchemeCount - 1, 0, false);
    CHECK(violet.face == sm::kFixed[9]);
    CHECK(violet.shadow == sm::kFixed[29]);

    // 1.4.0 read past the end of FixedColors[] for scheme 12.
    CHECK(sm::SchemeColors(sm::kSchemeCount, 0, false).face == sm::kRainbow[0]);
    CHECK(sm::SchemeColors(-5, 2, false).face == sm::kRainbow[2]);
}

TEST_CASE(RolloverShadesLikeTheOriginal)
{
    sm::BoxColors const base{ { 10, 10, 10 }, { 20, 20, 20 }, { 5, 5, 5 } };
    sm::BoxColors const normal = sm::ApplyRollover(base, false);
    CHECK(normal.face == base.shadow);
    CHECK(normal.highlight == base.face);
    CHECK(normal.shadow == base.shadow);

    sm::BoxColors const lit = sm::ApplyRollover(base, true);
    CHECK(lit.face == base.highlight);
    CHECK(lit.highlight == (sm::Rgb{ 255, 255, 255 }));
    CHECK(lit.shadow == base.face);

    CHECK(sm::Luminance({ 0, 0, 0 }) < 0.01);
    CHECK(sm::Luminance({ 255, 255, 255 }) > 0.99);
}
