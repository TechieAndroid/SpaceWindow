// SpaceWindow - box colour schemes.
//
// Colour selection follows MinimalDrawDisplayFolder() in FolderView.cpp of
// SpaceMonger 1.4.0 (Copyright (c) 1998-2020 Sean Werkema, MIT License).
#include "pch.h"
#include "Palette.h"

namespace sm
{
    namespace
    {
        constexpr Rgb FromColorRef(COLORREF color) noexcept
        {
            return { static_cast<std::uint8_t>(color & 0xFF), static_cast<std::uint8_t>((color >> 8) & 0xFF),
                     static_cast<std::uint8_t>((color >> 16) & 0xFF) };
        }

        BoxColors SystemColors() noexcept
        {
            return { FromColorRef(GetSysColor(COLOR_3DFACE)), FromColorRef(GetSysColor(COLOR_3DHILIGHT)),
                     FromColorRef(GetSysColor(COLOR_3DSHADOW)) };
        }

        constexpr BoxColors kDarkNeutral{ { 0x3A, 0x3A, 0x3A }, { 0x52, 0x52, 0x52 }, { 0x24, 0x24, 0x24 } };
        constexpr Rgb kWhite{ 0xFF, 0xFF, 0xFF };

        double Linear(std::uint8_t channel) noexcept
        {
            double const c = channel / 255.0;
            return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        }
    }

    BoxColors SchemeColors(int scheme, int depth, bool darkTheme) noexcept
    {
        if (scheme == kSchemeWindows)
        {
            return darkTheme ? kDarkNeutral : SystemColors();
        }
        if (scheme > kSchemeWindows && scheme < kSchemeCount)
        {
            std::size_t const i = static_cast<std::size_t>(scheme) - 2;
            return { kFixed[i], kFixed[i + 10], kFixed[i + 20] };
        }

        // Rainbow.  Negative depths cannot occur for real boxes, but stay in range anyway.
        auto const i = static_cast<std::size_t>(static_cast<unsigned>(depth) & 7u);
        return { kRainbow[i], kRainbow[i + 8], kRainbow[i + 16] };
    }

    BoxColors NeutralColors(bool darkTheme) noexcept
    {
        if (darkTheme)
        {
            return kDarkNeutral;
        }
        Rgb const face = FromColorRef(GetSysColor(COLOR_3DFACE));
        return { face, face, face };
    }

    BoxColors ApplyRollover(BoxColors colors, bool highlighted) noexcept
    {
        if (highlighted)
        {
            return { colors.highlight, kWhite, colors.face };
        }
        return { colors.shadow, colors.face, colors.shadow };
    }

    double Luminance(Rgb color) noexcept
    {
        return 0.2126 * Linear(color.r) + 0.7152 * Linear(color.g) + 0.0722 * Linear(color.b);
    }

    double Contrast(Rgb a, Rgb b) noexcept
    {
        double const x = Luminance(a);
        double const y = Luminance(b);
        return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
    }
}
