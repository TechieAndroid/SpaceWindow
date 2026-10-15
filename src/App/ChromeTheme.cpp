// SpaceWindow - colours of the window's controls.
#include "pch.h"
#include "ChromeTheme.h"

#include "ChromeThemeKeys.h"

namespace sm::ui
{
    namespace
    {
        namespace mux = winrt::Microsoft::UI::Xaml;
        namespace muxm = winrt::Microsoft::UI::Xaml::Media;
        using winrt::Windows::UI::Color;

        // What a brush is used for, which picks its colour in a theme.
        enum class Role : std::uint8_t
        {
            Text,
            SecondaryText,
            TertiaryText,
            DisabledText,
            Glyph,               // check marks in menus
            Accent,              // default buttons, toggles, check boxes, progress bars, sliders
            Link,                // accent-coloured text
            OnAccent,            // text on the accent colour
            OnAccentSecondary,
            Background,
            Surface,             // dialogs, menus, drop-down lists, tooltips
            CommandArea,         // the button row of a dialog
            SurfaceSecondary,
            SurfaceQuarternary,
            Raised,              // slider thumbs
            Input,               // a text box being typed in
            Border,              // the edges of dialogs, menus and tooltips
            Divider,             // separators
        };
        constexpr std::size_t kRoleCount = static_cast<std::size_t>(Role::Divider) + 1;

        struct BrushSpec
        {
            wchar_t const* key;
            Role role;
            double opacity = 1.0;  // the brush's own opacity in generic.xaml
        };

        // The shared brushes the controls are coloured with (from the theme
        // dictionaries of WinUI's generic.xaml).  The rest are translucent
        // overlays - hover and pressed fills, control borders - that suit any
        // background as they are.  tools/make_theme_keys.py reads this table.
        constexpr std::array kBrushes{
            BrushSpec{ L"TextFillColorPrimaryBrush", Role::Text },
            BrushSpec{ L"TextFillColorSecondaryBrush", Role::SecondaryText },
            BrushSpec{ L"TextFillColorTertiaryBrush", Role::TertiaryText },
            BrushSpec{ L"TextFillColorDisabledBrush", Role::DisabledText },
            BrushSpec{ L"SystemControlForegroundBaseHighBrush", Role::Text },
            BrushSpec{ L"SystemControlForegroundBaseMediumHighBrush", Role::Glyph },
            BrushSpec{ L"AccentFillColorDefaultBrush", Role::Accent },
            BrushSpec{ L"AccentFillColorSecondaryBrush", Role::Accent, 0.9 },
            BrushSpec{ L"AccentFillColorTertiaryBrush", Role::Accent, 0.8 },
            BrushSpec{ L"AccentTextFillColorPrimaryBrush", Role::Link },
            BrushSpec{ L"AccentTextFillColorSecondaryBrush", Role::Link },
            BrushSpec{ L"AccentTextFillColorTertiaryBrush", Role::Link },
            BrushSpec{ L"TextOnAccentFillColorPrimaryBrush", Role::OnAccent },
            BrushSpec{ L"TextOnAccentFillColorSecondaryBrush", Role::OnAccentSecondary },
            BrushSpec{ L"SolidBackgroundFillColorBaseBrush", Role::CommandArea },
            BrushSpec{ L"SolidBackgroundFillColorBaseAltBrush", Role::Background },
            BrushSpec{ L"SolidBackgroundFillColorSecondaryBrush", Role::SurfaceSecondary },
            BrushSpec{ L"SolidBackgroundFillColorTertiaryBrush", Role::Surface },
            BrushSpec{ L"SolidBackgroundFillColorQuarternaryBrush", Role::SurfaceQuarternary },
            BrushSpec{ L"LayerFillColorAltBrush", Role::Surface },                         // dialogs
            BrushSpec{ L"SystemControlBackgroundChromeMediumLowBrush", Role::Surface },    // tooltips
            BrushSpec{ L"DesktopAcrylicTransparentBrush", Role::Surface },                 // menus
            BrushSpec{ L"AcrylicInAppFillColorDefaultBrush", Role::Surface },              // drop-down lists, flyouts
            BrushSpec{ L"AcrylicBackgroundFillColorDefaultBrush", Role::Surface },
            BrushSpec{ L"ControlSolidFillColorDefaultBrush", Role::Raised },
            BrushSpec{ L"ControlFillColorInputActiveBrush", Role::Input },
            BrushSpec{ L"SurfaceStrokeColorFlyoutBrush", Role::Border },
            BrushSpec{ L"SurfaceStrokeColorDefaultBrush", Role::Border },
            BrushSpec{ L"CardStrokeColorDefaultBrush", Role::Border },
            BrushSpec{ L"DividerStrokeColorDefaultBrush", Role::Divider },
        };

        Color Opaque(sm::Rgb color) noexcept
        {
            return { 0xFF, color.r, color.g, color.b };
        }

        std::array<Color, kRoleCount> RoleColors(sm::Theme const& theme)
        {
            sm::WindowColors const& w = theme.window;
            bool const dark = theme.base == sm::ThemeBase::Dark;
            constexpr sm::Rgb kWhite{ 0xFF, 0xFF, 0xFF };
            // Accent-coloured text must be readable on the window and on dialogs.
            sm::Rgb const link = sm::Legible(sm::Legible(w.accent, w.text, w.surface, 4.5), w.text, w.background, 4.5);

            std::array<Color, kRoleCount> colors{};
            auto const set = [&colors](Role role, Color color) { colors.at(static_cast<std::size_t>(role)) = color; };
            set(Role::Text, Opaque(w.text));
            set(Role::SecondaryText, Opaque(w.secondaryText));
            set(Role::TertiaryText, Opaque(sm::Mix(w.secondaryText, w.background, 0.3)));
            set(Role::DisabledText, Opaque(sm::Mix(w.secondaryText, w.background, 0.5)));
            set(Role::Glyph, Opaque(sm::Mix(w.text, w.surface, 0.2)));
            set(Role::Accent, Opaque(w.accent));
            set(Role::Link, Opaque(link));
            set(Role::OnAccent, Opaque(w.accentText));
            set(Role::OnAccentSecondary, Opaque(sm::Mix(w.accentText, w.accent, 0.3)));
            set(Role::Background, Opaque(w.background));
            set(Role::Surface, Opaque(w.surface));
            set(Role::CommandArea, Opaque(sm::Mix(w.surface, w.background, 0.5)));
            set(Role::SurfaceSecondary, Opaque(sm::Mix(w.surface, w.background, 0.25)));
            set(Role::SurfaceQuarternary, Opaque(sm::Mix(w.surface, w.text, 0.04)));
            set(Role::Raised, Opaque(dark ? sm::Mix(w.surface, w.text, 0.1) : sm::Mix(w.surface, kWhite, 0.6)));
            set(Role::Input, Opaque(dark ? w.background : sm::Mix(w.surface, kWhite, 0.6)));
            set(Role::Border, Opaque(w.border));
            set(Role::Divider, Color{ 0x80, w.border.r, w.border.g, w.border.b });
            return colors;
        }
    }

    bool ChromeTheme::Apply(sm::Theme const& theme)
    {
        mux::ResourceDictionary const resources = mux::Application::Current().Resources();
        bool const changed = !names_.empty() || !theme.classic;
        for (winrt::hstring const& name : names_)
        {
            if (auto const key = winrt::box_value(name); resources.HasKey(key))
            {
                resources.Remove(key);
            }
        }
        names_.clear();
        if (theme.classic)
        {
            return changed;
        }

        bool const dark = theme.base == sm::ThemeBase::Dark;
        std::array<Color, kRoleCount> const colors = RoleColors(theme);
        auto const add = [&](std::wstring_view name, muxm::SolidColorBrush const& brush) {
            winrt::hstring key(name);
            resources.Insert(winrt::box_value(key), brush);
            names_.push_back(std::move(key));
        };
        for (BrushSpec const& spec : kBrushes)
        {
            // A new brush every time rather than a recoloured one: WinUI locks
            // the brush it draws plain text with.
            muxm::SolidColorBrush const brush(colors.at(static_cast<std::size_t>(spec.role)));
            brush.Opacity(spec.opacity);
            add(spec.key, brush);
            for (BrushAliases const& aliases : kBrushAliases)
            {
                if (aliases.brush != spec.key || !(dark ? aliases.dark : aliases.light))
                {
                    continue;
                }
                std::wstring_view rest = aliases.aliases;
                while (!rest.empty())
                {
                    std::size_t const space = rest.find(L' ');
                    add(rest.substr(0, space), brush);
                    rest = space == std::wstring_view::npos ? std::wstring_view() : rest.substr(space + 1);
                }
            }
        }
        return true;
    }
}
