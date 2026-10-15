// Tests for colour themes: the built-in ones, theme files, and the colours the map is drawn with.
#include "pch.h"
#include "TestHarness.h"

#include "../../src/Core/Theme.h"

namespace
{
    constexpr sm::Rgb Hex(std::uint32_t rgb) noexcept
    {
        return { static_cast<std::uint8_t>(rgb >> 16), static_cast<std::uint8_t>(rgb >> 8), static_cast<std::uint8_t>(rgb) };
    }

    bool SameBox(sm::BoxColors const& a, sm::BoxColors const& b) noexcept
    {
        return a.face == b.face && a.highlight == b.highlight && a.shadow == b.shadow;
    }

    sm::Theme ParseOk(std::wstring_view text)
    {
        std::wstring error;
        std::optional<sm::Theme> theme = sm::ParseTheme(text, L"test", error);
        if (!theme)
        {
            test::Fail("ParseTheme failed: " + test::ToUtf8(error), std::source_location::current());
        }
        return std::move(*theme);
    }

    // The error ParseTheme reports for text it must reject.
    std::wstring ErrorOf(std::wstring_view text)
    {
        std::wstring error;
        CHECK(!sm::ParseTheme(text, L"test", error).has_value());
        CHECK(!error.empty());
        return error;
    }

    bool Contains(std::wstring_view text, std::wstring_view part) noexcept
    {
        return text.find(part) != std::wstring_view::npos;
    }

    // UTF-16 LE with a byte-order mark, as Notepad saves "UTF-16 LE".
    std::string Utf16File(std::wstring_view text)
    {
        std::string bytes = "\xFF\xFE";
        for (wchar_t const c : text)
        {
            bytes.push_back(static_cast<char>(c & 0xFF));
            bytes.push_back(static_cast<char>((c >> 8) & 0xFF));
        }
        return bytes;
    }

    // A scratch folder in %TEMP%, deleted afterwards.
    class TempFolder
    {
    public:
        TempFolder()
        {
            wchar_t temp[MAX_PATH + 1]{};
            CHECK(GetTempPathW(static_cast<DWORD>(std::size(temp)), temp) != 0);
            GUID guid{};
            (void)CoCreateGuid(&guid);
            wchar_t id[40]{};
            (void)StringFromGUID2(guid, id, static_cast<int>(std::size(id)));
            path_ = std::wstring(temp) + L"SpaceWindowThemes-" + id;
            CHECK(CreateDirectoryW(path_.c_str(), nullptr));
        }
        TempFolder(TempFolder const&) = delete;
        TempFolder& operator=(TempFolder const&) = delete;
        ~TempFolder()
        {
            try
            {
                WIN32_FIND_DATAW data{};
                wil::unique_hfind const find(FindFirstFileW((path_ + L"\\*").c_str(), &data));
                if (find)
                {
                    do
                    {
                        std::wstring path = path_;
                        path.append(L"\\").append(data.cFileName);
                        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                        {
                            (void)RemoveDirectoryW(path.c_str());  // fails harmlessly for "." and ".."
                        }
                        else
                        {
                            (void)DeleteFileW(path.c_str());
                        }
                    } while (FindNextFileW(find.get(), &data));
                }
                (void)RemoveDirectoryW(path_.c_str());
            }
            catch (...)  // NOLINT(bugprone-empty-catch): best-effort cleanup; a destructor must not throw
            {
            }
        }

        std::wstring const& Path() const noexcept { return path_; }

        void Write(std::wstring const& name, std::string_view bytes) const
        {
            wil::unique_hfile const file(CreateFileW((path_ + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr,
                                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            CHECK(static_cast<bool>(file));
            DWORD written = 0;
            CHECK(WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr));
            CHECK_EQ(static_cast<std::size_t>(written), bytes.size());
        }

        void MakeFolder(std::wstring const& name) const
        {
            CHECK(CreateDirectoryW((path_ + L"\\" + name).c_str(), nullptr));
        }

    private:
        std::wstring path_;
    };
}

TEST_CASE(BuiltInThemesAreTheClassicOnesDraculaAndAlucard)
{
    std::vector<std::wstring> ids;
    for (sm::Theme const& theme : sm::BuiltInThemes())
    {
        ids.push_back(theme.id);
    }
    std::vector<std::wstring> const expected{ L"system", L"light", L"dark", L"dracula", L"alucard" };
    CHECK(ids == expected);

    std::span<sm::Theme const> const themes = sm::BuiltInThemes();
    CHECK(themes[0].classic && themes[1].classic && themes[2].classic);
    CHECK(!themes[3].classic && !themes[4].classic);
    CHECK(themes[0].base == sm::ThemeBase::System);
    CHECK(themes[1].base == sm::ThemeBase::Light);
    CHECK(themes[2].base == sm::ThemeBase::Dark);
    CHECK(themes[3].base == sm::ThemeBase::Dark);
    CHECK(themes[4].base == sm::ThemeBase::Light);
    CHECK_EQ(themes[3].name, std::wstring(L"Dracula"));
    CHECK_EQ(themes[4].name, std::wstring(L"Alucard"));
}

TEST_CASE(DraculaUsesTheDraculaPalette)
{
    // https://draculatheme.com/contribute: the palette of the dark theme.
    std::array const palette{ Hex(0x282A36), Hex(0x44475A), Hex(0xF8F8F2), Hex(0x6272A4), Hex(0x8BE9FD), Hex(0x50FA7B),
                              Hex(0xFFB86C), Hex(0xFF79C6), Hex(0xBD93F9), Hex(0xFF5555), Hex(0xF1FA8C) };
    auto const inPalette = [&palette](sm::Rgb color) {
        return std::ranges::any_of(palette, [color](sm::Rgb entry) { return entry == color; });
    };

    sm::Theme const& dracula = sm::FindTheme(sm::BuiltInThemes(), L"dracula");
    CHECK(dracula.window.background == Hex(0x282A36));
    CHECK(dracula.window.text == Hex(0xF8F8F2));
    CHECK(dracula.window.accent == Hex(0xBD93F9));
    std::array const rainbow{ Hex(0xFF5555), Hex(0xFFB86C), Hex(0xF1FA8C), Hex(0x50FA7B),
                              Hex(0x8BE9FD), Hex(0xBD93F9), Hex(0xF8F8F2), Hex(0xFF79C6) };
    CHECK(dracula.map.rainbow == rainbow);

    // Everything but the secondary text (which Comment is too faint for) is from the palette.
    sm::WindowColors const& w = dracula.window;
    for (sm::Rgb const color : { w.background, w.surface, w.text, w.accent, w.accentText, w.border })
    {
        CHECK(inPalette(color));
    }
    sm::MapColors const& m = dracula.map;
    for (sm::Rgb const color : m.fixed)
    {
        CHECK(inPalette(color));
    }
    for (sm::Rgb const color : { m.windows, m.outline, m.selection, m.darkText, m.lightText, m.freeSpace, m.grouped })
    {
        CHECK(inPalette(color));
    }
}

TEST_CASE(AlucardUsesTheAlucardPalette)
{
    sm::Theme const& alucard = sm::FindTheme(sm::BuiltInThemes(), L"alucard");
    CHECK(alucard.window.background == Hex(0xFFFBEB));
    CHECK(alucard.window.text == Hex(0x1F1F1F));
    CHECK(alucard.window.secondaryText == Hex(0x6C664B));
    CHECK(alucard.window.accent == Hex(0x644AC9));
    std::array const rainbow{ Hex(0xCB3A2A), Hex(0xA34D14), Hex(0x846E15), Hex(0x14710A),
                              Hex(0x036A96), Hex(0x644AC9), Hex(0xCFCFDE), Hex(0xA3144D) };
    CHECK(alucard.map.rainbow == rainbow);
}

TEST_CASE(BuiltInThemesAreLegible)
{
    for (std::wstring_view const id : { L"dracula", L"alucard" })
    {
        sm::Theme const& theme = sm::FindTheme(sm::BuiltInThemes(), id);
        sm::WindowColors const& w = theme.window;
        CHECK(sm::Contrast(w.text, w.background) >= 7.0);
        CHECK(sm::Contrast(w.text, w.surface) >= 7.0);
        CHECK(sm::Contrast(w.secondaryText, w.background) >= 4.5);
        CHECK(sm::Contrast(w.secondaryText, w.surface) >= 4.5);
        CHECK(sm::Contrast(w.accentText, w.accent) >= 4.5);
        CHECK(sm::Contrast(w.accent, w.background) >= 3.0);

        // Labels on every box colour.  4.4 rather than 4.5: Dracula's Comment
        // stays 3:1 against the window, which leaves 4.4:1 for light text on it.
        sm::MapColors const& m = theme.map;
        std::vector<sm::Rgb> faces(m.rainbow.begin(), m.rainbow.end());
        faces.insert(faces.end(), m.fixed.begin(), m.fixed.end());
        faces.insert(faces.end(), { m.windows, m.selection, m.freeSpace, m.grouped });
        for (sm::Rgb const face : faces)
        {
            CHECK(sm::Contrast(sm::TextColor(m, face), face) >= 4.4);
        }
    }
}

TEST_CASE(ThemeFileNeedsOnlyThreeColours)
{
    sm::Theme const theme = ParseOk(L"[window]\nbackground = #102030\ntext = #E0E0E0\naccent = #FF8800\n");
    CHECK_EQ(theme.id, std::wstring(L"test"));
    CHECK_EQ(theme.name, std::wstring(L"test"));  // the file's name
    CHECK(!theme.classic);
    CHECK(theme.base == sm::ThemeBase::Dark);     // dark background, light text

    sm::Rgb const background = Hex(0x102030);
    sm::Rgb const text = Hex(0xE0E0E0);
    CHECK(theme.window.accentText == background);  // more contrast on orange than the light text
    CHECK(sm::Contrast(theme.window.secondaryText, background) < sm::Contrast(text, background));
    for (std::size_t i = 0; i < theme.map.rainbow.size(); ++i)
    {
        CHECK(theme.map.rainbow.at(i) == sm::kRainbow.at(i));  // SpaceMonger's colours
    }
    for (std::size_t i = 0; i < theme.map.fixed.size(); ++i)
    {
        CHECK(theme.map.fixed.at(i) == sm::kFixed.at(i));
    }
    CHECK(theme.map.outline == background);
    CHECK(theme.map.selection == background);
    CHECK(theme.map.darkText == background);
    CHECK(theme.map.lightText == text);
    CHECK(theme.map.freeSpace == theme.map.windows);

    // The same colours the other way round make a light theme.
    sm::Theme const light = ParseOk(L"[window]\nbackground = #E0E0E0\ntext = #102030\naccent = #0050A0\n");
    CHECK(light.base == sm::ThemeBase::Light);
    CHECK(light.map.selection == background);
    CHECK(light.window.accentText == text);
}

TEST_CASE(ThemeFileSyntax)
{
    sm::Theme const theme = ParseOk(
        L"﻿# A comment line\r\n"
        L"NAME = Night Owl ; a comment after a value\r\n"
        L"Base = LIGHT\r\n"
        L"\r\n"
        L"[ Window ]\r\n"
        L"  background = #0a0B0c\r\n"
        L"text=#FFF\r\n"
        L"accent = #f80   ; #RGB is #RRGGBB with each digit doubled\r\n"
        L"[map]\r\n"
        L"rainbow = #000000,#111111, #222222 #333333\t#444444 , #555555 #666666 #777777\r\n"
        L"Light-Gray = #ABCDEF\r\n"
        L"grouped = #123456");
    CHECK_EQ(theme.name, std::wstring(L"Night Owl"));
    CHECK(theme.base == sm::ThemeBase::Light);  // as written, though the colours are dark
    CHECK(theme.window.background == Hex(0x0A0B0C));
    CHECK(theme.window.text == Hex(0xFFFFFF));
    CHECK(theme.window.accent == Hex(0xFF8800));
    CHECK(theme.map.rainbow[3] == Hex(0x333333));
    CHECK(theme.map.rainbow[7] == Hex(0x777777));
    CHECK(theme.map.fixed[1] == Hex(0xABCDEF));
    CHECK(theme.map.fixed[0] == sm::kFixed[0]);
    CHECK(theme.map.grouped == Hex(0x123456));
}

TEST_CASE(ThemeFileErrorsSayWhatAndWhere)
{
    std::wstring const color = ErrorOf(L"[window]\nbackground = #12345\ntext = #FFFFFF\naccent = #FF0000\n");
    CHECK(Contains(color, L"line 2"));
    CHECK(Contains(color, L"#12345"));

    CHECK(Contains(ErrorOf(L"; comment\n[colours]\n"), L"line 2: unknown section [colours]"));
    CHECK(Contains(ErrorOf(L"[window]\nforeground = #FFFFFF\n"), L"line 2: unknown setting 'foreground' in [window]"));
    CHECK(Contains(ErrorOf(L"[window]\nrainbow = #FFFFFF\n"), L"line 2: unknown setting 'rainbow' in [window]"));
    CHECK(Contains(ErrorOf(L"colour = #FFFFFF\n"), L"line 1: unknown setting 'colour'"));
    CHECK(Contains(ErrorOf(L"[map]\nrainbow = #FFF #000\n"), L"line 2: the rainbow has exactly 8 colours"));
    CHECK(Contains(ErrorOf(L"[map]\nrainbow = #000 #111 #222 #333 #444 #555 #666 #777 #888\n"), L"exactly 8"));
    CHECK(Contains(ErrorOf(L"[map]\nrainbow = #000 #111 #222 #333 red #555 #666 #777\n"), L"'red' is not a colour"));
    CHECK(Contains(ErrorOf(L"name = x\nbase = dim\n"), L"line 2"));
    CHECK(Contains(ErrorOf(L"[window\n"), L"line 1"));
    CHECK(Contains(ErrorOf(L"[window]\ntext\n"), L"line 2: expected a setting"));
    CHECK(Contains(ErrorOf(L"[window]\ntext =\n"), L"line 2: 'text' has no value"));
    CHECK(Contains(ErrorOf(std::wstring(L"name = ") + std::wstring(65, L'n')), L"longer than 64"));
    CHECK(Contains(ErrorOf(L"[window]\nbackground = #000000\ntext = #FFFFFF\n"), L"needs at least"));
    CHECK(Contains(ErrorOf(L""), L"needs at least"));
}

TEST_CASE(FindThemeIgnoresCaseAndFallsBackToSystem)
{
    std::span<sm::Theme const> const themes = sm::BuiltInThemes();
    CHECK_EQ(sm::FindTheme(themes, L"DRACULA").id, std::wstring(L"dracula"));
    CHECK_EQ(sm::FindTheme(themes, L"Dark").id, std::wstring(L"dark"));
    CHECK_EQ(sm::FindTheme(themes, L"no such theme").id, std::wstring(L"system"));
    CHECK_EQ(sm::FindTheme({}, L"dracula").id, std::wstring(L"system"));
}

TEST_CASE(ThemeFilesAreLoadedFromFolders)
{
    TempFolder const first;
    first.Write(L"Midnight.ini", "\xEF\xBB\xBFname = Midnight\n[window]\nbackground = #000020\ntext = #E0E0FF\naccent = #8080FF\n");
    first.Write(L"paper.ini", Utf16File(L"name = Paper — été\n[window]\nbackground = #FFFFF0\n"
                                        L"text = #202020\naccent = #A04000\n"));
    first.Write(L"broken.ini", "[window]\nbackground = nope\n");
    first.Write(L"dracula.ini", "[window]\nbackground = #000000\ntext = #FFFFFF\naccent = #FF0000\n");
    first.Write(L"notes.txt", "not a theme");
    first.MakeFolder(L"folder.ini");

    TempFolder const second;
    second.Write(L"MIDNIGHT.INI", "[window]\nbackground = #000000\ntext = #FFFFFF\naccent = #FF0000\n");
    second.Write(L"extra.ini", "[window]\nbackground = #000000\ntext = #FFFFFF\naccent = #FF0000\n");

    std::vector<std::wstring> const folders{ first.Path(), first.Path() + L"\\missing", second.Path() };
    std::vector<std::wstring> errors;
    std::vector<sm::Theme> const themes = sm::LoadThemes(folders, errors);

    std::vector<std::wstring> ids;
    ids.reserve(themes.size());
    for (sm::Theme const& theme : themes)
    {
        ids.push_back(theme.id);
    }
    std::vector<std::wstring> const expected{ L"system", L"light", L"dark", L"dracula", L"alucard",
                                              L"Midnight", L"paper", L"extra" };
    CHECK(ids == expected);
    CHECK_EQ(sm::FindTheme(themes, L"midnight").name, std::wstring(L"Midnight"));
    CHECK_EQ(sm::FindTheme(themes, L"paper").name, std::wstring(L"Paper — été"));
    CHECK(sm::FindTheme(themes, L"dracula").window.background == Hex(0x282A36));  // the built-in one

    // One line per file that was not used: the broken one and the two whose names are taken.
    CHECK_EQ(errors.size(), std::size_t{ 3 });
    CHECK(std::ranges::any_of(errors, [](std::wstring const& e) { return Contains(e, L"broken.ini: line 2"); }));
    CHECK(std::ranges::any_of(errors, [](std::wstring const& e) { return Contains(e, L"dracula.ini: not used"); }));
    CHECK(std::ranges::any_of(errors, [](std::wstring const& e) { return Contains(e, L"MIDNIGHT.INI: not used"); }));
}

TEST_CASE(ClassicThemesKeepSpaceMongersColours)
{
    sm::Theme const& system = sm::FindTheme(sm::BuiltInThemes(), L"system");
    for (bool const dark : { false, true })
    {
        for (int scheme = -1; scheme <= sm::kSchemeCount; ++scheme)
        {
            for (int depth = 0; depth < 10; ++depth)
            {
                CHECK(SameBox(sm::ThemeBoxColors(system, scheme, depth, dark), sm::SchemeColors(scheme, depth, dark)));
            }
        }
        CHECK(SameBox(sm::FreeSpaceColors(system, dark), sm::NeutralColors(dark)));

        sm::MapColors const map = sm::ThemeMapColors(system, dark);
        CHECK(map.outline == Hex(0x000000));
        CHECK(map.selection == Hex(0x000000));
        CHECK(map.freeSpace == sm::NeutralColors(dark).face);
        // Black or white labels, as before themes existed: white on dark faces.
        CHECK(sm::TextColor(map, map.selection) == Hex(0xFFFFFF));
        for (sm::Rgb const face : sm::kRainbow)
        {
            CHECK(sm::TextColor(map, face) == (sm::Luminance(face) < 0.179 ? Hex(0xFFFFFF) : Hex(0x000000)));
        }
    }
}

TEST_CASE(OtherThemesShadeTheirOwnColours)
{
    sm::Theme const& dracula = sm::FindTheme(sm::BuiltInThemes(), L"dracula");
    sm::BoxColors const box = sm::ThemeBoxColors(dracula, sm::kSchemeRainbow, 9, true);
    CHECK(box.face == dracula.map.rainbow[1]);  // the rainbow repeats every 8 levels
    CHECK(sm::Luminance(box.highlight) > sm::Luminance(box.face));
    CHECK(sm::Luminance(box.shadow) < sm::Luminance(box.face));
    CHECK(sm::ThemeBoxColors(dracula, sm::kSchemeWindows, 0, true).face == dracula.map.windows);
    CHECK(sm::ThemeBoxColors(dracula, 5, 0, true).face == dracula.map.fixed[3]);         // "Red"
    CHECK(sm::ThemeBoxColors(dracula, 12, 2, true).face == dracula.map.rainbow[2]);     // out of range: Rainbow
    CHECK(SameBox(sm::FreeSpaceColors(dracula, true), { dracula.map.freeSpace, dracula.map.freeSpace, dracula.map.freeSpace }));
    CHECK(sm::ThemeMapColors(dracula, false).outline == dracula.map.outline);  // the window mode does not matter
}

TEST_CASE(ColourHelpers)
{
    CHECK(sm::Mix(Hex(0x000000), Hex(0xFFFFFF), 0.5) == Hex(0x808080));
    CHECK(sm::Mix(Hex(0x204060), Hex(0xFFFFFF), 0.0) == Hex(0x204060));
    CHECK(sm::Mix(Hex(0x204060), Hex(0xFFFFFF), 2.0) == Hex(0xFFFFFF));
    CHECK(std::abs(sm::Contrast(Hex(0x000000), Hex(0xFFFFFF)) - 21.0) < 1e-9);
    CHECK(sm::Contrast(Hex(0x336699), Hex(0xFFCC00)) == sm::Contrast(Hex(0xFFCC00), Hex(0x336699)));
    CHECK(sm::Contrast(Hex(0x808080), Hex(0x808080)) == 1.0);

    // Dracula's Purple on Current Line is 3.8:1; a step towards Foreground makes it readable text.
    sm::Rgb const purple = Hex(0xBD93F9);
    sm::Rgb const line = Hex(0x44475A);
    CHECK(sm::Contrast(purple, line) < 4.5);
    sm::Rgb const legible = sm::Legible(purple, Hex(0xF8F8F2), line, 4.5);
    CHECK(sm::Contrast(legible, line) >= 4.5);
    CHECK(!(legible == Hex(0xF8F8F2)));
    CHECK(sm::Legible(purple, Hex(0xF8F8F2), Hex(0x282A36), 4.5) == purple);  // already enough
}
