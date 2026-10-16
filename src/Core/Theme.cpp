// SpaceWindow - colour themes.
#include "pch.h"
#include "Theme.h"

#include "Text.h"

namespace sm
{
    namespace
    {
        constexpr Rgb kBlack{ 0, 0, 0 };
        constexpr Rgb kWhite{ 0xFF, 0xFF, 0xFF };
        constexpr std::size_t kMaxFileBytes = std::size_t{ 64 } * 1024;
        constexpr std::size_t kMaxNameLength = 64;

        // The built-in themes, written as theme files (so they double as
        // examples of the format).  The colours are the Dracula Theme's,
        // https://github.com/dracula/dracula-theme (MIT License, Copyright (c)
        // 2023 Dracula Theme; see THIRD-PARTY-NOTICES.md); the comments name
        // them.  The few colours that are not in the palettes are mixed from them.
        constexpr std::wstring_view kDracula = LR"(
name = Dracula
base = dark

[window]
background = #282A36        ; Background
surface = #44475A           ; Current Line
text = #F8F8F2              ; Foreground
secondary-text = #BCC2D3    ; Foreground, 40% of the way to Comment (Comment is too faint for small text)
accent = #BD93F9            ; Purple
accent-text = #282A36       ; Background
border = #6272A4            ; Comment

[map]
; Red, Orange, Yellow, Green, Cyan, Purple, Foreground and Pink stand in for
; SpaceMonger's red, orange, yellow, green, aqua, blue, grey and violet.
rainbow = #FF5555 #FFB86C #F1FA8C #50FA7B #8BE9FD #BD93F9 #F8F8F2 #FF79C6
white = #F8F8F2             ; Foreground
light-gray = #6272A4        ; Comment
dark-gray = #44475A         ; Current Line
red = #FF5555
orange = #FFB86C
yellow = #F1FA8C
green = #50FA7B
aqua = #8BE9FD              ; Cyan
blue = #BD93F9              ; Purple
violet = #FF79C6            ; Pink
windows = #44475A           ; Current Line
outline = #282A36           ; Background
selection = #282A36         ; Background
dark-text = #282A36         ; Background
light-text = #F8F8F2        ; Foreground
free-space = #44475A        ; Current Line
grouped = #6272A4           ; Comment
)";

        constexpr std::wstring_view kAlucard = LR"(
name = Alucard
base = light

[window]
background = #FFFBEB        ; Background
surface = #FFFDF5           ; Background, halfway to white
text = #1F1F1F              ; Foreground
secondary-text = #6C664B    ; Comment
accent = #644AC9            ; Purple
accent-text = #FFFBEB       ; Background
border = #CFCFDE            ; Selection

[map]
; Red, Orange, Yellow, Green, Cyan, Purple, Selection and Pink.
rainbow = #CB3A2A #A34D14 #846E15 #14710A #036A96 #644AC9 #CFCFDE #A3144D
white = #FFFFFF
light-gray = #CFCFDE        ; Selection
dark-gray = #6C664B         ; Comment
red = #CB3A2A
orange = #A34D14
yellow = #846E15
green = #14710A
aqua = #036A96              ; Cyan
blue = #644AC9              ; Purple
violet = #A3144D            ; Pink
windows = #CFCFDE           ; Selection
outline = #FFFBEB           ; Background
selection = #1F1F1F         ; Foreground
dark-text = #1F1F1F         ; Foreground
light-text = #FFFBEB        ; Background
free-space = #FFFBEB        ; Background
grouped = #8F8D85           ; halfway between Background and Foreground
)";

        // What a theme file sets.  Whatever it leaves out is worked out from the rest.
        struct ThemeFile
        {
            std::optional<std::wstring> name;
            std::optional<ThemeBase> base;
            std::optional<Rgb> background;
            std::optional<Rgb> surface;
            std::optional<Rgb> text;
            std::optional<Rgb> secondaryText;
            std::optional<Rgb> accent;
            std::optional<Rgb> accentText;
            std::optional<Rgb> border;
            std::optional<std::array<Rgb, 8>> rainbow;
            std::array<std::optional<Rgb>, 10> fixed;
            std::optional<Rgb> windows;
            std::optional<Rgb> outline;
            std::optional<Rgb> selection;
            std::optional<Rgb> darkText;
            std::optional<Rgb> lightText;
            std::optional<Rgb> freeSpace;
            std::optional<Rgb> grouped;
        };

        struct ColorSetting
        {
            std::wstring_view key;
            std::optional<Rgb> ThemeFile::* field;
        };

        constexpr std::array<ColorSetting, 7> kWindowSettings{ {
            { L"background", &ThemeFile::background },
            { L"surface", &ThemeFile::surface },
            { L"text", &ThemeFile::text },
            { L"secondary-text", &ThemeFile::secondaryText },
            { L"accent", &ThemeFile::accent },
            { L"accent-text", &ThemeFile::accentText },
            { L"border", &ThemeFile::border },
        } };

        constexpr std::array<ColorSetting, 7> kMapSettings{ {
            { L"windows", &ThemeFile::windows },
            { L"outline", &ThemeFile::outline },
            { L"selection", &ThemeFile::selection },
            { L"dark-text", &ThemeFile::darkText },
            { L"light-text", &ThemeFile::lightText },
            { L"free-space", &ThemeFile::freeSpace },
            { L"grouped", &ThemeFile::grouped },
        } };

        // The single-colour schemes, in Lang::colorNames order.
        constexpr std::array<std::wstring_view, 10> kFixedKeys{
            L"white", L"light-gray", L"dark-gray", L"red", L"orange", L"yellow", L"green", L"aqua", L"blue", L"violet",
        };

        std::wstring_view Trim(std::wstring_view text) noexcept
        {
            constexpr std::wstring_view kSpace = L" \t\r﻿";
            std::size_t const first = text.find_first_not_of(kSpace);
            if (first == std::wstring_view::npos)
            {
                return {};
            }
            return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
        }

        std::wstring Lower(std::wstring_view text)
        {
            std::wstring result(text);
            for (wchar_t& c : result)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return result;
        }

        int HexDigit(wchar_t c) noexcept
        {
            if (c >= L'0' && c <= L'9')
            {
                return c - L'0';
            }
            if (c >= L'a' && c <= L'f')
            {
                return c - L'a' + 10;
            }
            if (c >= L'A' && c <= L'F')
            {
                return c - L'A' + 10;
            }
            return -1;
        }

        // "#RRGGBB" or "#RGB", in either case.
        std::optional<Rgb> ParseColor(std::wstring_view text) noexcept
        {
            if ((text.size() != 7 && text.size() != 4) || text.front() != L'#')
            {
                return std::nullopt;
            }
            std::array<int, 6> digits{};
            for (std::size_t i = 1; i < text.size(); ++i)
            {
                digits[i - 1] = HexDigit(text[i]);
                if (digits[i - 1] < 0)
                {
                    return std::nullopt;
                }
            }
            bool const shortForm = text.size() == 4;
            auto const channel = [&digits, shortForm](std::size_t i) {
                return static_cast<std::uint8_t>(shortForm ? digits[i] * 17 : digits[2 * i] * 16 + digits[2 * i + 1]);
            };
            return Rgb{ channel(0), channel(1), channel(2) };
        }

        std::wstring NotAColor(std::wstring_view value)
        {
            return Join(L"'", value, L"' is not a colour (write colours as #RRGGBB)");
        }

        Theme ClassicTheme(wchar_t const* id, ThemeBase base)
        {
            Theme theme;
            theme.id = id;
            theme.base = base;
            theme.classic = true;
            return theme;
        }

        // A theme file's text: UTF-8, with or without a byte-order mark, or UTF-16 with one.
        std::optional<std::wstring> ReadThemeFile(std::wstring const& path, std::wstring& error)
        {
            wil::unique_hfile const file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            LARGE_INTEGER size{};
            if (!file || !GetFileSizeEx(file.get(), &size))
            {
                error = L"cannot be read";
                return std::nullopt;
            }
            if (std::cmp_greater(size.QuadPart, kMaxFileBytes))
            {
                error = L"is too big for a theme file (the limit is 64 KB)";
                return std::nullopt;
            }
            std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
            DWORD read = 0;
            if (!bytes.empty() &&
                (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
                 read != bytes.size()))
            {
                error = L"cannot be read";
                return std::nullopt;
            }

            auto const byte = [&bytes](std::size_t i) { return static_cast<unsigned char>(bytes[i]); };
            if (bytes.size() >= 2 && byte(0) == 0xFF && byte(1) == 0xFE)
            {
                std::wstring text;
                text.reserve(bytes.size() / 2);
                for (std::size_t i = 2; i + 1 < bytes.size(); i += 2)
                {
                    text.push_back(static_cast<wchar_t>(byte(i) | (byte(i + 1) << 8)));
                }
                return text;
            }
            std::string_view utf8(bytes);
            if (utf8.starts_with("\xEF\xBB\xBF"))
            {
                utf8.remove_prefix(3);
            }
            if (utf8.empty())
            {
                return std::wstring();
            }
            int const length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                                   static_cast<int>(utf8.size()), nullptr, 0);
            if (length <= 0)
            {
                error = L"is not UTF-8 or UTF-16 text";
                return std::nullopt;
            }
            std::wstring text(static_cast<std::size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), text.data(),
                                length);
            return text;
        }

        // The theme files in a folder, in name order.
        std::vector<std::wstring> ThemeFiles(std::wstring const& folder)
        {
            std::vector<std::wstring> names;
            WIN32_FIND_DATAW data{};
            wil::unique_hfind const find(FindFirstFileExW((folder + L"\\*.ini").c_str(), FindExInfoBasic, &data,
                                                          FindExSearchNameMatch, nullptr, 0));
            if (!find)
            {
                return names;  // no such folder, or no theme files in it
            }
            do
            {
                // "*.ini" also matches longer extensions through 8.3 names.
                std::wstring_view const name(data.cFileName);
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && name.size() > 4 &&
                    EqualsIgnoringCase(name.substr(name.size() - 4), L".ini"))
                {
                    names.emplace_back(name);
                }
            } while (FindNextFileW(find.get(), &data));
            std::ranges::sort(names, LessIgnoringCase);
            return names;
        }
    }

    Rgb Mix(Rgb a, Rgb b, double amount) noexcept
    {
        double const t = std::clamp(amount, 0.0, 1.0);
        auto const channel = [t](std::uint8_t x, std::uint8_t y) {
            return static_cast<std::uint8_t>(std::lround(x + (y - x) * t));
        };
        return { channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b) };
    }

    BoxColors Shades(Rgb face) noexcept
    {
        return { face, Mix(face, kWhite, 0.35), Mix(face, kBlack, 0.3) };
    }

    Rgb Legible(Rgb color, Rgb toward, Rgb background, double ratio) noexcept
    {
        constexpr int kSteps = 20;
        for (int step = 0; step < kSteps; ++step)
        {
            Rgb const candidate = Mix(color, toward, static_cast<double>(step) / kSteps);
            if (Contrast(candidate, background) >= ratio)
            {
                return candidate;
            }
        }
        return toward;
    }

    std::optional<Theme> ParseTheme(std::wstring_view text, std::wstring_view id, std::wstring& error)
    {
        ThemeFile file;
        std::wstring section;
        int lineNumber = 0;
        auto const fail = [&error, &lineNumber](std::wstring_view message) {
            std::wstring const number = std::to_wstring(lineNumber);
            error = Join(L"line ", number, L": ", message);
            return std::optional<Theme>();
        };

        std::wstring_view rest = text;
        while (!rest.empty())
        {
            std::size_t const end = rest.find(L'\n');
            std::wstring_view line = Trim(rest.substr(0, end));
            rest = end == std::wstring_view::npos ? std::wstring_view() : rest.substr(end + 1);
            ++lineNumber;
            if (line.starts_with(L'#'))
            {
                continue;  // a comment line
            }
            line = Trim(line.substr(0, line.find(L';')));  // ';' starts a comment anywhere
            if (line.empty())
            {
                continue;
            }

            if (line.front() == L'[')
            {
                if (line.back() != L']')
                {
                    return fail(L"a section name must end with ']'");
                }
                section = Lower(Trim(line.substr(1, line.size() - 2)));
                if (section != L"window" && section != L"map")
                {
                    return fail(Join(L"unknown section [", section, L"] (the sections are [window] and [map])"));
                }
                continue;
            }

            std::size_t const equals = line.find(L'=');
            if (equals == std::wstring_view::npos)
            {
                return fail(L"expected a setting, such as 'text = #F8F8F2'");
            }
            std::wstring const key = Lower(Trim(line.substr(0, equals)));
            std::wstring_view const value = Trim(line.substr(equals + 1));
            if (value.empty())
            {
                return fail(Join(L"'", key, L"' has no value"));
            }

            if (section.empty())
            {
                if (key == L"name")
                {
                    if (value.size() > kMaxNameLength)
                    {
                        return fail(L"the name is longer than 64 characters");
                    }
                    file.name = std::wstring(value);
                }
                else if (key == L"base")
                {
                    std::wstring const base = Lower(value);
                    if (base != L"light" && base != L"dark")
                    {
                        return fail(L"the base is 'light' or 'dark'");
                    }
                    file.base = base == L"dark" ? ThemeBase::Dark : ThemeBase::Light;
                }
                else
                {
                    return fail(Join(L"unknown setting '", key, L"' (before the sections come 'name' and 'base')"));
                }
                continue;
            }

            std::span<ColorSetting const> const settings = section == L"window"
                                                               ? std::span<ColorSetting const>(kWindowSettings)
                                                               : std::span<ColorSetting const>(kMapSettings);
            auto const fixed = std::ranges::find(kFixedKeys, std::wstring_view(key));
            if (auto const setting = std::ranges::find(settings, std::wstring_view(key), &ColorSetting::key);
                setting != settings.end())
            {
                std::optional<Rgb> const color = ParseColor(value);
                if (!color)
                {
                    return fail(NotAColor(value));
                }
                file.*(setting->field) = color;
            }
            else if (section == L"map" && fixed != kFixedKeys.end())
            {
                std::optional<Rgb> const color = ParseColor(value);
                if (!color)
                {
                    return fail(NotAColor(value));
                }
                file.fixed.at(static_cast<std::size_t>(fixed - kFixedKeys.begin())) = color;
            }
            else if (section == L"map" && key == L"rainbow")
            {
                std::array<Rgb, 8> colors{};
                std::size_t count = 0;
                std::wstring_view list = value;
                while (!list.empty())
                {
                    std::size_t const stop = list.find_first_of(L" \t,");
                    std::wstring_view const item = list.substr(0, stop);
                    list = stop == std::wstring_view::npos ? std::wstring_view() : list.substr(stop + 1);
                    if (item.empty())
                    {
                        continue;
                    }
                    std::optional<Rgb> const color = ParseColor(item);
                    if (!color)
                    {
                        return fail(NotAColor(item));
                    }
                    if (count == colors.size())
                    {
                        return fail(L"the rainbow has exactly 8 colours");
                    }
                    colors.at(count++) = *color;
                }
                if (count != colors.size())
                {
                    return fail(L"the rainbow has exactly 8 colours");
                }
                file.rainbow = colors;
            }
            else
            {
                return fail(Join(L"unknown setting '", key, L"' in [", section, L"]"));
            }
        }

        if (!file.background || !file.text || !file.accent)
        {
            error = L"[window] needs at least a background, a text and an accent colour";
            return std::nullopt;
        }

        // Everything else defaults to something that goes with those three.
        Rgb const background = *file.background;
        Rgb const foreground = *file.text;
        Rgb const accent = *file.accent;
        bool const backgroundIsDarker = Luminance(background) <= Luminance(foreground);
        Rgb const darker = backgroundIsDarker ? background : foreground;
        Rgb const lighter = backgroundIsDarker ? foreground : background;
        bool const dark = file.base ? *file.base == ThemeBase::Dark : backgroundIsDarker;

        Theme theme;
        theme.id = id;
        theme.name = file.name.value_or(std::wstring(id));
        theme.base = dark ? ThemeBase::Dark : ThemeBase::Light;

        WindowColors& window = theme.window;
        window.background = background;
        window.text = foreground;
        window.accent = accent;
        window.surface = file.surface.value_or(dark ? Mix(background, foreground, 0.08) : Mix(background, kWhite, 0.5));
        window.secondaryText = file.secondaryText.value_or(Mix(foreground, background, 0.3));
        window.accentText =
            file.accentText.value_or(Contrast(accent, lighter) > Contrast(accent, darker) ? lighter : darker);
        window.border = file.border.value_or(Mix(background, foreground, 0.2));

        MapColors& map = theme.map;
        for (std::size_t i = 0; i < map.rainbow.size(); ++i)
        {
            map.rainbow.at(i) = file.rainbow ? file.rainbow->at(i) : kRainbow.at(i);
        }
        for (std::size_t i = 0; i < map.fixed.size(); ++i)
        {
            map.fixed.at(i) = file.fixed.at(i).value_or(kFixed.at(i));
        }
        map.windows = file.windows.value_or(Mix(background, foreground, 0.15));
        map.outline = file.outline.value_or(background);
        map.selection = file.selection.value_or(darker);
        map.darkText = file.darkText.value_or(darker);
        map.lightText = file.lightText.value_or(lighter);
        map.freeSpace = file.freeSpace.value_or(map.windows);
        map.grouped = file.grouped.value_or(Mix(background, foreground, 0.45));
        return theme;
    }

    std::span<Theme const> BuiltInThemes()
    {
        static std::vector<Theme> const themes = [] {
            std::vector<Theme> list;
            list.push_back(ClassicTheme(L"system", ThemeBase::System));
            list.push_back(ClassicTheme(L"light", ThemeBase::Light));
            list.push_back(ClassicTheme(L"dark", ThemeBase::Dark));
            for (auto const& [id, text] : { std::pair{ L"dracula", kDracula }, std::pair{ L"alucard", kAlucard } })
            {
                std::wstring error;
                if (std::optional<Theme> theme = ParseTheme(text, id, error))
                {
                    list.push_back(std::move(*theme));
                }
            }
            return list;
        }();
        return themes;
    }

    std::vector<Theme> LoadThemes(std::span<std::wstring const> folders, std::vector<std::wstring>& errors)
    {
        std::span<Theme const> const builtIn = BuiltInThemes();
        std::vector<Theme> themes(builtIn.begin(), builtIn.end());
        for (std::wstring const& folder : folders)
        {
            for (std::wstring const& name : ThemeFiles(folder))
            {
                std::wstring_view const id = std::wstring_view(name).substr(0, name.size() - 4);  // without ".ini"
                std::wstring const path = Join(folder, L"\\", name);
                if (std::ranges::any_of(themes, [id](Theme const& theme) { return EqualsIgnoringCase(theme.id, id); }))
                {
                    errors.push_back(Join(path, L": not used, there is already a theme called \"", id, L"\""));
                    continue;
                }
                std::wstring error;
                std::optional<std::wstring> const text = ReadThemeFile(path, error);
                std::optional<Theme> theme = text ? ParseTheme(*text, id, error) : std::nullopt;
                if (!theme)
                {
                    errors.push_back(Join(path, L": ", error));
                    continue;
                }
                themes.push_back(std::move(*theme));
            }
        }
        return themes;
    }

    Theme const& FindTheme(std::span<Theme const> themes, std::wstring_view id)
    {
        for (Theme const& theme : themes)
        {
            if (EqualsIgnoringCase(theme.id, id))
            {
                return theme;
            }
        }
        return BuiltInThemes().front();
    }

    BoxColors ThemeBoxColors(Theme const& theme, int scheme, int depth, bool darkWindow) noexcept
    {
        if (theme.classic)
        {
            return SchemeColors(scheme, depth, darkWindow);
        }
        if (scheme == kSchemeWindows)
        {
            return Shades(theme.map.windows);
        }
        if (scheme > kSchemeWindows && scheme < kSchemeCount)
        {
            return Shades(theme.map.fixed[static_cast<std::size_t>(scheme) - 2]);
        }
        return Shades(theme.map.rainbow[static_cast<std::size_t>(static_cast<unsigned>(depth) & 7u)]);
    }

    BoxColors FreeSpaceColors(Theme const& theme, bool darkWindow) noexcept
    {
        if (theme.classic)
        {
            return NeutralColors(darkWindow);
        }
        // Flat: free space is not a box of anything.
        Rgb const face = theme.map.freeSpace;
        return { face, face, face };
    }

    MapColors ThemeMapColors(Theme const& theme, bool darkWindow) noexcept
    {
        if (!theme.classic)
        {
            return theme.map;
        }
        // SpaceMonger's look: black outlines and selection, black or white labels.
        MapColors map;
        for (std::size_t i = 0; i < map.rainbow.size(); ++i)
        {
            map.rainbow[i] = kRainbow[i];
        }
        for (std::size_t i = 0; i < map.fixed.size(); ++i)
        {
            map.fixed[i] = kFixed[i];
        }
        map.windows = SchemeColors(kSchemeWindows, 0, darkWindow).face;
        map.outline = kBlack;
        map.selection = kBlack;
        map.darkText = kBlack;
        map.lightText = kWhite;
        map.freeSpace = NeutralColors(darkWindow).face;
        // About 3:1 against the window, in either mode.
        map.grouped = darkWindow ? Rgb{ 0x70, 0x70, 0x70 } : Rgb{ 0x8A, 0x8A, 0x8A };
        return map;
    }

    Rgb TextColor(MapColors const& map, Rgb face) noexcept
    {
        return Contrast(face, map.lightText) > Contrast(face, map.darkText) ? map.lightText : map.darkText;
    }
}
