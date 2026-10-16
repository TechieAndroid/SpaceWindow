// SpaceWindow - box colour schemes.
//
// The palettes are copied verbatim from BoxColors[] and FixedColors[] in
// FolderView.cpp of SpaceMonger 1.4.0 (Copyright (c) 1998-2020 Sean Werkema,
// MIT License); only the 256-colour GDI palette plumbing was dropped.
#pragma once

#include <array>
#include <cstdint>

namespace sm
{
    struct Rgb
    {
        std::uint8_t r = 0;
        std::uint8_t g = 0;
        std::uint8_t b = 0;

        friend constexpr bool operator==(Rgb, Rgb) = default;
    };

    // A box is drawn with a 3-D bevel: highlight on the top/left edges,
    // shadow on the bottom/right edges, and the face colour inside.
    struct BoxColors
    {
        Rgb face;
        Rgb highlight;
        Rgb shadow;
    };

    // Colour schemes, in the order of Lang::colorNames.
    inline constexpr int kSchemeRainbow = 0;
    inline constexpr int kSchemeWindows = 1;
    inline constexpr int kSchemeCount = 12;

    // "Rainbow": the colour cycles with the nesting depth.
    // [0..7] faces, [8..15] highlights, [16..23] shadows.
    // clang-format off: laid out by hand
    inline constexpr std::array<Rgb, 24> kRainbow{ {
        { 0xFF, 0x7F, 0x7F }, { 0xFF, 0xBF, 0x7F }, { 0xFF, 0xFF, 0x00 }, { 0x7F, 0xFF, 0x7F },
        { 0x7F, 0xFF, 0xFF }, { 0xBF, 0xBF, 0xFF }, { 0xBF, 0xBF, 0xBF }, { 0xFF, 0x7F, 0xFF },

        { 0xFF, 0xBF, 0xBF }, { 0xFF, 0xDF, 0xBF }, { 0xFF, 0xFF, 0xBF }, { 0xBF, 0xFF, 0xBF },
        { 0xDF, 0xFF, 0xFF }, { 0xDF, 0xDF, 0xFF }, { 0xDF, 0xDF, 0xDF }, { 0xFF, 0xBF, 0xFF },

        { 0xBF, 0x7F, 0x7F }, { 0xBF, 0x9F, 0x5F }, { 0xBF, 0xBF, 0x3F }, { 0x7F, 0xBF, 0x7F },
        { 0x7F, 0xBF, 0xBF }, { 0x9F, 0x9F, 0xFF }, { 0x9F, 0x9F, 0x9F }, { 0xBF, 0x7F, 0xBF },
    } };
    // clang-format on

    // Fixed schemes "White" through "Violet" (scheme numbers 2..11).
    // [0..9] faces, [10..19] highlights, [20..29] shadows.
    // clang-format off: laid out by hand
    inline constexpr std::array<Rgb, 30> kFixed{ {
        { 0xFF, 0xFF, 0xFF }, { 0xBF, 0xBF, 0xBF }, { 0x7F, 0x7F, 0x7F },
        { 0xFF, 0x7F, 0x7F }, { 0xFF, 0xBF, 0x7F }, { 0xFF, 0xFF, 0x00 }, { 0x7F, 0xFF, 0x7F },
        { 0x7F, 0xFF, 0xFF }, { 0xBF, 0xBF, 0xFF }, { 0xFF, 0x7F, 0xFF },

        { 0xFF, 0xFF, 0xFF }, { 0xFF, 0xFF, 0xFF }, { 0xBF, 0xBF, 0xBF },
        { 0xFF, 0x9F, 0x9F }, { 0xFF, 0xDF, 0xBF }, { 0xFF, 0xFF, 0xBF }, { 0xBF, 0xFF, 0xBF },
        { 0xDF, 0xFF, 0xFF }, { 0xDF, 0xDF, 0xFF }, { 0xFF, 0xBF, 0xFF },

        { 0xBF, 0xBF, 0xBF }, { 0x7F, 0x7F, 0x7F }, { 0x3F, 0x3F, 0x3F },
        { 0xBF, 0x7F, 0x7F }, { 0xBF, 0x9F, 0x9F }, { 0xBF, 0xBF, 0x3F }, { 0x7F, 0xBF, 0x7F },
        { 0x7F, 0xBF, 0xBF }, { 0x9F, 0x9F, 0xFF }, { 0xBF, 0x7F, 0xBF },
    } };
    // clang-format on

    // Colours of a box in `scheme` at nesting `depth`.  Out-of-range schemes
    // fall back to Rainbow (1.4.0 indexed past the end of FixedColors[] when
    // the registry held scheme 12).  "Windows Colors" follows the system's 3-D
    // colours in the light theme (so high-contrast themes are honoured) and
    // uses neutral greys in the dark theme.
    BoxColors SchemeColors(int scheme, int depth, bool darkTheme) noexcept;

    // Colours of the free-space and unscanned-space blocks.
    BoxColors NeutralColors(bool darkTheme) noexcept;

    // The "rollover box" look of 1.4.0: when enabled, ordinary boxes are drawn
    // one shade darker and the boxes under the mouse one shade brighter.
    BoxColors ApplyRollover(BoxColors colors, bool highlighted) noexcept;

    // Relative luminance in [0, 1]; used to pick black or white text.
    double Luminance(Rgb color) noexcept;

    // The WCAG contrast ratio of two colours, from 1 (the same) to 21 (black on white).
    double Contrast(Rgb a, Rgb b) noexcept;
}
