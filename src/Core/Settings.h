// SpaceWindow - user settings.
//
// Modernized from CCurrentSettings in SpaceMonger.h/.cpp of SpaceMonger 1.4.0
// (Copyright (c) 1998-2020 Sean Werkema, MIT License).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sm
{
    // Which details the file-info tip shows (same bit values as 1.4.0, so
    // settings migrated from the old registry key keep their meaning).
    enum TipFlags : std::uint32_t
    {
        TipPath = 1,
        TipName = 2,
        TipIcon = 4,
        TipDate = 8,
        TipSize = 16,
        TipAttrib = 32,
        TipAll = TipPath | TipName | TipIcon | TipDate | TipSize | TipAttrib,
    };

    // How the map is drawn.
    enum class ViewMode : int
    {
        Treemap = 0,
        Pie = 1,
        InvertedPie = 2,  // the folder as the outer ring, deeper levels inward (PieLayout.h)
    };

    // The animation in the space the map leaves free (around the pie charts,
    // and behind the welcome screen); see Backdrop.h.
    enum class Backdrop : int
    {
        None = 0,
        Fire = 1,      // the PSX DOOM fire, as in kiedtl/fire and ly
        Matrix = 2,    // falling characters, as in CMatrix and ly
        ColorMix = 3,  // ly's colour-mixing pattern
        Life = 4,      // Conway's Game of Life, as in ly
        Tunnel = 5,    // an endless, curving neon tube
    };
    inline constexpr int kBackdropCount = 6;

    struct WindowPlacement
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        bool maximized = false;

        bool IsValid() const noexcept { return width > 0 && height > 0; }

        friend bool operator==(WindowPlacement const&, WindowPlacement const&) = default;
    };

    struct Settings
    {
        // Ranges.  Density has one value per entry of Lang::densityNames; 1.4.0
        // accepted +3 from the registry, which selected nothing in its combo box.
        static constexpr int kMinDensity = -3;
        static constexpr int kMaxDensity = 2;
        static constexpr int kMinBias = -20;
        static constexpr int kMaxBias = 20;
        static constexpr int kMaxDelay = 99'999;
        static constexpr std::size_t kMaxThemeId = 200;  // theme ids are file names

        int density = 0;             // label density, -3 (fewest) .. 2 (most)
        int fileColor = 0;           // colour scheme of files (Palette.h)
        int folderColor = 0;         // colour scheme of folders
        bool autoRescan = false;     // rescan after deleting
        bool animatedZoom = true;    // animate zoom in/out
        bool disableDelete = false;  // remove the Delete command
        bool rolloverBox = false;    // highlight the boxes under the mouse
        int bias = 0;                // split preference, -20 (horizontal) .. 20 (vertical)

        bool savePosition = false;  // remember the window placement
        WindowPlacement window;

        bool showNameTips = true;
        int nameTipDelay = 125;  // milliseconds
        bool showInfoTips = true;
        std::uint32_t infoTipFlags = TipDate | TipSize | TipIcon;
        int infoTipDelay = 250;  // milliseconds

        std::wstring language;           // Lang::code; empty = follow Windows
        std::wstring theme = L"system";  // Theme::id (Theme.h); kept while its theme file is missing
        ViewMode viewMode = ViewMode::Treemap;
        Backdrop backdrop = Backdrop::None;

        // Clamps every value into its valid range.
        void Validate();

        friend bool operator==(Settings const&, Settings const&) = default;
    };

#ifdef _WIN32
    inline constexpr std::wstring_view kSettingsKey = L"Software\\SpaceWindow";
    inline constexpr std::wstring_view kLegacySettingsKey = L"Software\\65\\SpaceMonger\\1.3";  // SpaceMonger 1.x
#else
    // Elsewhere the settings are a text file of "name = value" lines, with the
    // registry's value names; a relative path is below $XDG_CONFIG_HOME
    // (~/.config).  SpaceMonger 1.x never ran there: there is nothing to import.
    inline constexpr std::wstring_view kSettingsKey = L"SpaceWindow/settings.ini";
    inline constexpr std::wstring_view kLegacySettingsKey;
#endif

    // Loads the settings from HKEY_CURRENT_USER\<key>.  If that key does not
    // exist yet, the settings of SpaceMonger 1.x are imported from
    // HKEY_CURRENT_USER\<legacyKey>.  Missing values keep their defaults.
    // (Elsewhere: from the files `key` and `legacyKey`.)
    Settings LoadSettings(std::wstring_view key = kSettingsKey, std::wstring_view legacyKey = kLegacySettingsKey);

    // Saves the settings to HKEY_CURRENT_USER\<key> (elsewhere: the file `key`).
    // Returns false on failure.
    bool SaveSettings(Settings const& settings, std::wstring_view key = kSettingsKey);
}
