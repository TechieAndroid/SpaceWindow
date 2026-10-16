// SpaceWindow - user-interface string tables.
//
// Modernized from lang.h, lang_en.cpp and lang_fr.cpp of SpaceMonger 1.4.0
// (Copyright (c) 1998-2020 Sean Werkema, MIT License).
//
// The tables are now UTF-16 and are initialized with C++20 designated
// initializers.  The originals were positional, so a single missing string
// silently shifted every following entry.  The strings the WinUI front end
// needs (themes, the folder picker, confirmation prompts, ...) were added.
#pragma once

#include <array>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

namespace sm
{
    // The names of the keys that have one, as printed on keyboards in the
    // language.  Letters, digits, function keys and arrows are written as
    // they are (Commands.h).
    struct KeyNames
    {
        std::wstring_view ctrl;
        std::wstring_view shift;
        std::wstring_view alt;
        std::wstring_view enter;
        std::wstring_view backspace;
        std::wstring_view home;
        std::wstring_view escape;
        std::wstring_view del;
        std::wstring_view menu;
    };

    // What each command with keys does, as Settings lists it next to its
    // keys (Commands.h: ShortcutAction).
    struct ShortcutHelp
    {
        std::wstring_view selectNearest;
        std::wstring_view activate;
        std::wstring_view zoomOut;
        std::wstring_view goDown;
        std::wstring_view goBack;
        std::wstring_view goForward;
        std::wstring_view zoomFull;
        std::wstring_view clearSelection;
        std::wstring_view open;
        std::wstring_view reload;
        std::wstring_view fullRescan;
        std::wstring_view pieChart;
        std::wstring_view invertedPie;
        std::wstring_view del;
        std::wstring_view properties;
        std::wstring_view copyPath;
        std::wstring_view contextMenu;
        std::wstring_view about;
    };

    struct Lang
    {
        std::wstring_view code;    // two-letter code persisted in the settings ("us", "uk", "fr")
        std::wstring_view name;    // name shown in the language list
        std::wstring_view locale;  // locale name for the Windows globalization APIs

        // About box.
        std::wstring_view aboutApp;
        std::wstring_view freeware;
        std::wstring_view warranty;
        std::wstring_view credits;

        std::wstring_view ok;
        std::wstring_view cancel;

        // Units.
        std::wstring_view bytes;
        std::wstring_view kb;
        std::wstring_view mb;
        std::wstring_view gb;
        std::wstring_view tb;
        std::wstring_view pb;
        std::wstring_view percent;

        // Menu commands.  An '&' marks the access key, exactly as in the
        // original Win32 menus; see ParseCaption().
        std::wstring_view zoomIn;
        std::wstring_view zoomOut;
        std::wstring_view zoomFull;
        std::wstring_view run;
        std::wstring_view del;
        std::wstring_view openDrive;
        std::wstring_view rescanDrive;
        std::wstring_view showFreeSpace;
        std::wstring_view pieChart;
        std::wstring_view invertedPie;
        std::wstring_view properties;
        std::wstring_view showInExplorer;
        std::wstring_view copyPath;
        std::wstring_view fullRescan;    // scan everything again, even where only some files changed
        std::wstring_view rescanFolder;  // "{0}" = folder name, its '&'s doubled

        // Toolbar captions (the original toolbar bitmaps had these painted in).
        std::wstring_view tbOpen;
        std::wstring_view tbReload;
        std::wstring_view tbZoomFull;
        std::wstring_view tbZoomIn;
        std::wstring_view tbZoomOut;
        std::wstring_view tbFreeSpace;
        std::wstring_view tbPieChart;
        std::wstring_view tbInvertedPie;
        std::wstring_view tbRun;
        std::wstring_view tbDelete;
        std::wstring_view tbSetup;
        std::wstring_view tbAbout;

        // Map and status text.
        std::wstring_view freeSpace;
        std::wstring_view unscanned;
        std::wstring_view total;
        std::wstring_view free;
        std::wstring_view filesTotal;
        std::wstring_view foldersTotal;
        std::wstring_view filesFound;
        std::wstring_view foldersFound;
        std::wstring_view onDisk;             // "{0} on disk"
        std::wstring_view unreadableFolders;  // "Unreadable folders: {0}"
        std::wstring_view smallerItems;       // "{0} smaller items": a pie slice grouping small entries
        std::wstring_view hardLink;           // info tip of a hard link whose space is counted under another name
        std::wstring_view unreadableFolder;   // info tip of a folder that could not be listed
        std::wstring_view unreadableTitle;    // the list of folders that could not be listed
        std::wstring_view unreadableText;
        std::wstring_view moreFolders;  // "{0}" = how many more the list leaves out
        std::wstring_view copyList;
        std::wstring_view close;

        wchar_t decimalPoint;
        wchar_t thousandsSeparator;

        std::array<std::wstring_view, 6> densityNames;
        std::array<std::wstring_view, 12> colorNames;
        std::array<std::wstring_view, 12> monthNames;
        std::array<std::wstring_view, 11> attributeNames;
        std::array<std::wstring_view, 3> themeNames;
        std::array<std::wstring_view, 6> backdropNames;  // by sm::Backdrop

        // Drive selection, scanning and deletion.
        std::wstring_view selectDrive;
        std::wstring_view browseFolder;
        std::wstring_view scanning;
        std::wstring_view scanFailed;  // "{0}" = path
        std::wstring_view confirmDeleteTitle;
        std::wstring_view confirmDelete;  // "{0}" = name, "{1}" = size
        std::wstring_view deleteFailed;   // "{0}" = name
        std::wstring_view emptyTitle;
        std::wstring_view emptyText;

        // Settings dialog.
        std::wstring_view settings;
        std::wstring_view layout;
        std::wstring_view density;
        std::wstring_view bias;
        std::wstring_view horz;
        std::wstring_view equal;
        std::wstring_view vert;
        std::wstring_view displayColors;
        std::wstring_view files;
        std::wstring_view folders;
        std::wstring_view toolTips;
        std::wstring_view showNameTips;
        std::wstring_view showInfoTips;
        std::wstring_view delay;
        std::wstring_view fullPath;
        std::wstring_view fileName;
        std::wstring_view icon;
        std::wstring_view dateTime;
        std::wstring_view fileSize;
        std::wstring_view attrib;
        std::wstring_view miscOptions;
        std::wstring_view autoRescan;
        std::wstring_view disableDelete;
        std::wstring_view animatedZoom;
        std::wstring_view savePos;
        std::wstring_view showRolloverBox;
        std::wstring_view appearance;
        std::wstring_view theme;
        std::wstring_view themesFolder;  // opens the folder that theme files go in
        std::wstring_view themeErrors;   // heads the list of theme files that could not be used
        std::wstring_view language;
        std::wstring_view backgroundAnimation;
        std::wstring_view backgroundAnimationHint;
        std::wstring_view keyboardShortcuts;
        KeyNames keyNames;
        ShortcutHelp shortcutHelp;
    };

    // All available languages.  The first entry is the fallback.
    std::span<Lang const> Languages() noexcept;

    // Finds a language by its two-letter code; unknown codes give US English,
    // which is what SpaceMonger 1.4 did with an unrecognized registry value.
    Lang const& FindLanguage(std::wstring_view code) noexcept;

    // Picks the language that best matches the user's Windows display language.
    std::wstring_view DefaultLanguageCode();

    // Splits an original-style caption such as L"Zoom &In" into its display
    // text (L"Zoom In") and access key (L"I").  "&&" is a literal ampersand.
    struct Caption
    {
        std::wstring text;
        std::wstring accessKey;
    };
    Caption ParseCaption(std::wstring_view caption);

    // Substitutes {0}, {1}, ... in a translated pattern.  Unlike the printf
    // formats used by the original, a mistranslated pattern can never read
    // arguments that were not supplied: unknown placeholders are left as is.
    std::wstring FormatText(std::wstring_view pattern, std::initializer_list<std::wstring_view> args);
}
