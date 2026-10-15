// SpaceWindow - colour themes.
//
// A theme colours the whole app: the window and its controls, and the map.
// The classic themes - "Use system setting", "Light" and "Dark" - keep the
// Windows look (Mica, the Windows accent colour) and SpaceMonger's own box
// colours.  Every other theme sets its colours itself: the built-in Dracula
// and Alucard themes (https://github.com/dracula/dracula-theme), and themes
// read from theme files, whose format is described in README.md.
#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Palette.h"

namespace sm
{
    // The style of Windows controls a theme starts from.
    enum class ThemeBase
    {
        System,  // follow Windows' light or dark mode (the classic "Use system setting")
        Light,
        Dark,
    };

    // The window and its controls.
    struct WindowColors
    {
        Rgb background;     // the window
        Rgb surface;        // dialogs, menus and tips
        Rgb text;
        Rgb secondaryText;  // captions and hints
        Rgb accent;         // default buttons, toggles, check boxes, links, the zoom outline
        Rgb accentText;     // text on the accent colour
        Rgb border;         // dividers and the edges of dialogs, menus and tips
    };

    // The map.
    struct MapColors
    {
        std::array<Rgb, 8> rainbow{};  // the "Rainbow" scheme: one colour per nesting depth
        std::array<Rgb, 10> fixed{};   // the schemes "White" .. "Violet", in Lang::colorNames order
        Rgb windows;                   // the "Windows Colors" scheme
        Rgb outline;                   // the lines between the boxes of the treemap
        Rgb selection;                 // the selected box or slice
        Rgb darkText;                  // labels on light boxes
        Rgb lightText;                 // labels on dark boxes
        Rgb freeSpace;                 // free and unscanned space, and the pie chart's hub
        Rgb grouped;                   // the pie chart's "smaller items" slices, and its hairlines
    };

    struct Theme
    {
        std::wstring id;       // "system", "light", "dark", "dracula", "alucard", or a theme file's name
        std::wstring name;     // shown in Settings (the classic themes' names come from the language tables)
        ThemeBase base = ThemeBase::System;
        bool classic = false;  // the Windows look and SpaceMonger's colours; `window` and `map` are unused
        WindowColors window;
        MapColors map;
    };

    // The themes that come with SpaceWindow: the three classic ones, Dracula and Alucard.
    std::span<Theme const> BuiltInThemes();

    // Reads a theme file's text.  `id` is the file's name without ".ini".
    // On failure returns nothing and describes the first problem in `error`,
    // with its line number.
    std::optional<Theme> ParseTheme(std::wstring_view text, std::wstring_view id, std::wstring& error);

    // The built-in themes followed by the valid *.ini theme files in
    // `folders`, folder by folder in name order.  A file named like a
    // built-in or an earlier theme is skipped.  One line per file that could
    // not be used is added to `errors`.
    std::vector<Theme> LoadThemes(std::span<std::wstring const> folders, std::vector<std::wstring>& errors);

    // The theme with this id (ignoring case), or the "system" theme.
    Theme const& FindTheme(std::span<Theme const> themes, std::wstring_view id);

    // The colours of a box in a colour scheme at a nesting depth.  For the
    // classic themes these are SpaceMonger's (see SchemeColors); other themes'
    // boxes get their bevel shades worked out from the face colour.
    // `darkWindow`: the window is in dark mode.
    BoxColors ThemeBoxColors(Theme const& theme, int scheme, int depth, bool darkWindow) noexcept;

    // The colours of the free-space and unscanned-space boxes.
    BoxColors FreeSpaceColors(Theme const& theme, bool darkWindow) noexcept;

    // The map colours to draw with; a classic theme's depend on whether the
    // window is in dark mode.
    MapColors ThemeMapColors(Theme const& theme, bool darkWindow) noexcept;

    // The colour for a label on `face`: the map's light or dark text colour,
    // whichever contrasts more.
    Rgb TextColor(MapColors const& map, Rgb face) noexcept;

    // A face colour with the lighter and darker shades of its 3-D bevel.
    BoxColors Shades(Rgb face) noexcept;

    // `a` moved towards `b` by `amount` (0 .. 1).
    Rgb Mix(Rgb a, Rgb b, double amount) noexcept;

    // `color` moved towards `toward` just far enough for a contrast of at
    // least `ratio` with `background` (all the way, if even that is not enough).
    Rgb Legible(Rgb color, Rgb toward, Rgb background, double ratio) noexcept;
}
