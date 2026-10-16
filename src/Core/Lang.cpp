// SpaceWindow - user-interface string tables.
//
// Modernized from lang_en.cpp and lang_fr.cpp of SpaceMonger 1.4.0
// (Copyright (c) 1998-2020 Sean Werkema, MIT License).
//
// Changes from 1.4.0:
//  * "Kb/Mb/Gb" are now "KB/MB/GB" (Kb is a kilobit); TB and PB were added
//    because 1.4.0 printed a 4 TB drive as "4096.0 Gb".
//  * The French table was corrected.  Several commands were mistranslated
//    ("Vrombir" - to roar - meant Zoom In, "Démarrer" - Start - meant Zoom
//    Out), the warranty text was left in English, and "Properties" was never
//    translated.  French now uses Windows' standard terminology.
//  * Access keys no longer collide ("Zoom &Out" and "&Open Drive" both used O).
#include "pch.h"
#include "Lang.h"

namespace sm
{
    namespace
    {
        constexpr Lang kEnglishUs{
            .code = L"us",
            .name = L"English (US)",
            .locale = L"en-US",

            .aboutApp = L"About SpaceWindow",
            .freeware = L"SpaceWindow is free software, released under the MIT License.",
            .warranty = L"You may distribute this program freely.  Because this program is free, the author "
                        L"cannot and will not assume responsibility for this program's behavior.  There are no "
                        L"warranties of any kind, expressed or implied, on this product.",
            .credits = L"Based on SpaceMonger 1.4.0 by Sean Werkema.  Modernized for Windows 11 with WinUI 3 "
                       L"and the Windows App SDK.",

            .ok = L"OK",
            .cancel = L"Cancel",

            .bytes = L"bytes",
            .kb = L"KB",
            .mb = L"MB",
            .gb = L"GB",
            .tb = L"TB",
            .pb = L"PB",
            .percent = L"%",

            .zoomIn = L"Zoom &In",
            .zoomOut = L"Zoom &Out",
            .zoomFull = L"Zoom &Full",
            .run = L"&Run / Open",
            .del = L"&Delete",
            .openDrive = L"Op&en Drive or Folder…",
            .rescanDrive = L"Re&scan",
            .showFreeSpace = L"Show Free Spa&ce",
            .pieChart = L"Pie Ch&art",
            .invertedPie = L"In&verted Pie Chart",
            .properties = L"&Properties…",
            .showInExplorer = L"Show in File E&xplorer",
            .copyPath = L"Copy Pat&h",
            .fullRescan = L"F&ull Rescan",
            .rescanFolder = L"Resca&n “{0}”",

            .tbOpen = L"Open",
            .tbReload = L"Reload",
            .tbZoomFull = L"Zoom Full",
            .tbZoomIn = L"Zoom In",
            .tbZoomOut = L"Zoom Out",
            .tbFreeSpace = L"Free Space",
            .tbPieChart = L"Pie Chart",
            .tbInvertedPie = L"Inverted Pie",
            .tbRun = L"Run or Open",
            .tbDelete = L"Delete",
            .tbSetup = L"Setup",
            .tbAbout = L"About",

            .freeSpace = L"Free Space",
            .unscanned = L"Unscanned",
            .total = L"Total",
            .free = L"Free",
            .filesTotal = L"Files Total:",
            .foldersTotal = L"Folders Total:",
            .filesFound = L"Files Found:",
            .foldersFound = L"Folders Found:",
            .onDisk = L"{0} on disk",
            .unreadableFolders = L"Unreadable folders: {0}",
            .smallerItems = L"{0} smaller items",
            .hardLink = L"Hard link: the file's space is counted under another of its names",
            .unreadableFolder = L"This folder could not be read: what it holds is missing",
            .unreadableTitle = L"Folders that could not be read",
            .unreadableText = L"SpaceWindow could not list these folders (access was denied, or they were in use), "
                              L"so what they hold is missing from the map.  Run SpaceWindow as administrator to "
                              L"read most of them.",
            .moreFolders = L"… and {0} more",
            .copyList = L"Copy List",
            .close = L"Close",

            .decimalPoint = L'.',
            .thousandsSeparator = L',',

            .densityNames = { L"Too Few Files", L"Very Few Files", L"Normal", L"Lots of Files",
                              L"Very Many Files", L"Too Many Files" },
            .colorNames = { L"Rainbow", L"Windows Colors", L"White", L"Light Gray", L"Dark Gray", L"Red",
                            L"Orange", L"Yellow", L"Green", L"Aqua", L"Blue", L"Violet" },
            .monthNames = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                            L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" },
            .attributeNames = { L"Archive", L"Compressed", L"Folder", L"Encrypted", L"Hidden", L"Offline",
                                L"Read-only", L"Reparse point", L"Sparse", L"System", L"Temporary" },
            .themeNames = { L"Use system setting", L"Light", L"Dark" },
            .backdropNames = { L"None", L"DOOM Fire", L"Matrix", L"Color Mix", L"Game of Life", L"Y2K Tunnel" },

            .selectDrive = L"Select a Drive or Folder",
            .browseFolder = L"Browse for a folder…",
            .scanning = L"Scanning…",
            .scanFailed = L"SpaceWindow couldn't open “{0}”.",
            .confirmDeleteTitle = L"Move to Recycle Bin?",
            .confirmDelete = L"Are you sure you want to move “{0}” ({1}) to the Recycle Bin?",
            .deleteFailed = L"Windows failed to delete “{0}”.",
            .emptyTitle = L"See where your disk space goes",
            .emptyText = L"Open a drive or folder to draw a map of everything stored on it.  "
                         L"The bigger the box, the more space it uses.",

            .settings = L"SpaceWindow Settings",
            .layout = L"File Layout",
            .density = L"Density",
            .bias = L"Bias",
            .horz = L"Horz",
            .equal = L"Equal",
            .vert = L"Vert",
            .displayColors = L"Display Colors",
            .files = L"Files",
            .folders = L"Folders",
            .toolTips = L"Tooltips",
            .showNameTips = L"Show file-name tips",
            .showInfoTips = L"Show file-info tips",
            .delay = L"Delay (ms)",
            .fullPath = L"Full Path",
            .fileName = L"Filename",
            .icon = L"Icon",
            .dateTime = L"Date / Time",
            .fileSize = L"File Size",
            .attrib = L"Attributes",
            .miscOptions = L"Miscellaneous Options",
            .autoRescan = L"Auto Rescan on Delete",
            .disableDelete = L"Disable “Delete” Command",
            .animatedZoom = L"Animated Zoom In / Zoom Out",
            .savePos = L"Remember Window Position",
            .showRolloverBox = L"Show Rollover Boxes",
            .appearance = L"Appearance",
            .theme = L"Theme",
            .themesFolder = L"Open themes folder",
            .themeErrors = L"Some theme files could not be used:",
            .language = L"Language",
            .backgroundAnimation = L"Background animation",
            .backgroundAnimationHint = L"Plays around the pie charts and behind the welcome screen",
            .keyboardShortcuts = L"Keyboard Shortcuts",
            .keyNames = { .ctrl = L"Ctrl", .shift = L"Shift", .alt = L"Alt", .enter = L"Enter", .backspace = L"Backspace",
                          .home = L"Home", .escape = L"Esc", .del = L"Del", .menu = L"Menu" },
            .shortcutHelp = {
                .selectNearest = L"Select the nearest box or slice in that direction",
                .activate = L"Zoom into the selected folder, or run or open the selected file",
                .zoomOut = L"Up one level (Zoom Out)",
                .goDown = L"Down one level: towards the selection, or else back the way you came up",
                .goBack = L"Back to the previous view",
                .goForward = L"Forward again, to the view you went back from",
                .zoomFull = L"All the way out (Zoom Full)",
                .clearSelection = L"Clear the selection",
                .open = L"Open a drive or folder",
                .reload = L"Reload: read again what changed",
                .fullRescan = L"Full Rescan: scan everything again",
                .pieChart = L"Switch between the treemap and the pie chart",
                .invertedPie = L"Switch between the treemap and the inverted pie chart",
                .del = L"Move the selection to the Recycle Bin",
                .properties = L"Properties of the selection",
                .copyPath = L"Copy the path of the selection",
                .contextMenu = L"Context menu of the selection",
                .about = L"About SpaceWindow",
            },
        };

        constexpr Lang MakeEnglishUk()
        {
            Lang lang = kEnglishUs;
            lang.code = L"uk";
            lang.name = L"English (UK/Aust.)";
            lang.locale = L"en-GB";
            // clang-format off: laid out by hand
            lang.colorNames = { L"Rainbow", L"Windows Colours", L"White", L"Light Grey", L"Dark Grey", L"Red",
                                L"Orange", L"Yellow", L"Green", L"Aqua", L"Blue", L"Violet" };
            // clang-format on
            lang.displayColors = L"Display Colours";
            lang.backdropNames = { L"None", L"DOOM Fire", L"Matrix", L"Colour Mix", L"Game of Life", L"Y2K Tunnel" };
            lang.warranty = L"You may distribute this program freely.  Because this program is free, the author "
                            L"cannot and will not assume responsibility for this program's behaviour.  There are "
                            L"no warranties of any kind, expressed or implied, on this product.";
            return lang;
        }

        constexpr Lang kEnglishUk = MakeEnglishUk();

        constexpr Lang kFrench{
            .code = L"fr",
            .name = L"Français (France)",
            .locale = L"fr-FR",

            .aboutApp = L"À propos de SpaceWindow",
            .freeware = L"SpaceWindow est un logiciel libre distribué sous licence MIT.",
            .warranty = L"Vous pouvez distribuer ce programme librement.  Ce programme étant gratuit, l'auteur "
                        L"ne peut et ne veut assumer aucune responsabilité quant à son comportement.  Ce produit "
                        L"n'est assorti d'aucune garantie, expresse ou implicite.",
            .credits = L"D'après SpaceMonger 1.4.0 de Sean Werkema.  Modernisé pour Windows 11 avec WinUI 3 et "
                       L"le SDK d'application Windows.",

            .ok = L"OK",
            .cancel = L"Annuler",

            .bytes = L"octets",
            .kb = L"Ko",
            .mb = L"Mo",
            .gb = L"Go",
            .tb = L"To",
            .pb = L"Po",
            .percent = L" %",

            .zoomIn = L"&Zoom avant",
            .zoomOut = L"Zoom a&rrière",
            .zoomFull = L"&Vue complète",
            .run = L"&Exécuter / ouvrir",
            .del = L"&Supprimer",
            .openDrive = L"&Ouvrir un lecteur ou un dossier…",
            .rescanDrive = L"Ac&tualiser",
            .showFreeSpace = L"Afficher l'espace &libre",
            .pieChart = L"&Graphique en secteurs",
            .invertedPie = L"Secteurs &inversés",
            .properties = L"&Propriétés…",
            .showInExplorer = L"Afficher dans l'Explorateur de &fichiers",
            .copyPath = L"&Copier le chemin d'accès",
            .fullRescan = L"Analyse co&mplète",
            .rescanFolder = L"Actualiser &uniquement « {0} »",

            .tbOpen = L"Ouvrir",
            .tbReload = L"Actualiser",
            .tbZoomFull = L"Vue complète",
            .tbZoomIn = L"Zoom avant",
            .tbZoomOut = L"Zoom arrière",
            .tbFreeSpace = L"Espace libre",
            .tbPieChart = L"Secteurs",
            .tbInvertedPie = L"Secteurs inversés",
            .tbRun = L"Exécuter",
            .tbDelete = L"Supprimer",
            .tbSetup = L"Paramètres",
            .tbAbout = L"À propos",

            .freeSpace = L"Espace libre",
            .unscanned = L"Non analysé",
            .total = L"au total",
            .free = L"libres",
            .filesTotal = L"Fichiers au total :",
            .foldersTotal = L"Dossiers au total :",
            .filesFound = L"Fichiers trouvés :",
            .foldersFound = L"Dossiers trouvés :",
            .onDisk = L"{0} sur le disque",
            .unreadableFolders = L"Dossiers illisibles : {0}",
            .smallerItems = L"{0} éléments plus petits",
            .hardLink = L"Lien physique : l'espace du fichier est compté sous un autre de ses noms",
            .unreadableFolder = L"Ce dossier n'a pas pu être lu : son contenu manque",
            .unreadableTitle = L"Dossiers qui n'ont pas pu être lus",
            .unreadableText = L"SpaceWindow n'a pas pu lister ces dossiers (accès refusé, ou dossiers en cours "
                              L"d'utilisation) : leur contenu manque sur la carte.  Lancez SpaceWindow en tant "
                              L"qu'administrateur pour lire la plupart d'entre eux.",
            .moreFolders = L"… et {0} de plus",
            .copyList = L"Copier la liste",
            .close = L"Fermer",

            .decimalPoint = L',',
            .thousandsSeparator = L' ',

            .densityNames = { L"Trop peu de fichiers", L"Très peu de fichiers", L"Normal",
                              L"Beaucoup de fichiers", L"Énormément de fichiers", L"Trop de fichiers" },
            .colorNames = { L"Arc-en-ciel", L"Couleurs de Windows", L"Blanc", L"Gris clair", L"Gris foncé",
                            L"Rouge", L"Orange", L"Jaune", L"Vert", L"Cyan", L"Bleu", L"Violet" },
            .monthNames = { L"janv.", L"févr.", L"mars", L"avr.", L"mai", L"juin",
                            L"juil.", L"août", L"sept.", L"oct.", L"nov.", L"déc." },
            .attributeNames = { L"Archive", L"Compressé", L"Dossier", L"Chiffré", L"Caché", L"Hors connexion",
                                L"Lecture seule", L"Point d'analyse", L"Éparse", L"Système", L"Temporaire" },
            .themeNames = { L"Utiliser le paramètre système", L"Clair", L"Sombre" },
            .backdropNames = { L"Aucune", L"Feu de DOOM", L"Matrice", L"Mélange de couleurs", L"Jeu de la vie",
                               L"Tunnel Y2K" },

            .selectDrive = L"Sélectionner un lecteur ou un dossier",
            .browseFolder = L"Parcourir les dossiers…",
            .scanning = L"Analyse en cours…",
            .scanFailed = L"SpaceWindow n'a pas pu ouvrir « {0} ».",
            .confirmDeleteTitle = L"Déplacer vers la Corbeille ?",
            .confirmDelete = L"Voulez-vous vraiment déplacer « {0} » ({1}) vers la Corbeille ?",
            .deleteFailed = L"Windows n'a pas pu supprimer « {0} ».",
            .emptyTitle = L"Découvrez où va votre espace disque",
            .emptyText = L"Ouvrez un lecteur ou un dossier pour dessiner la carte de tout ce qu'il contient.  "
                         L"Plus la boîte est grande, plus l'élément occupe d'espace.",

            .settings = L"Paramètres de SpaceWindow",
            .layout = L"Disposition des fichiers",
            .density = L"Densité",
            .bias = L"Orientation",
            .horz = L"Horiz.",
            .equal = L"Égale",
            .vert = L"Vert.",
            .displayColors = L"Couleurs d'affichage",
            .files = L"Fichiers",
            .folders = L"Dossiers",
            .toolTips = L"Info-bulles",
            .showNameTips = L"Afficher les info-bulles de nom",
            .showInfoTips = L"Afficher les info-bulles détaillées",
            .delay = L"Délai (ms)",
            .fullPath = L"Chemin complet",
            .fileName = L"Nom du fichier",
            .icon = L"Icône",
            .dateTime = L"Date / heure",
            .fileSize = L"Taille",
            .attrib = L"Attributs",
            .miscOptions = L"Autres options",
            .autoRescan = L"Réanalyser après une suppression",
            .disableDelete = L"Désactiver la commande « Supprimer »",
            .animatedZoom = L"Animer les zooms avant et arrière",
            .savePos = L"Mémoriser la position de la fenêtre",
            .showRolloverBox = L"Afficher les cadres de survol",
            .appearance = L"Apparence",
            .theme = L"Thème",
            .themesFolder = L"Ouvrir le dossier des thèmes",
            .themeErrors = L"Certains fichiers de thème n'ont pas pu être utilisés :",
            .language = L"Langue",
            .backgroundAnimation = L"Animation d'arrière-plan",
            .backgroundAnimationHint = L"Autour des graphiques en secteurs et derrière l'écran d'accueil",
            .keyboardShortcuts = L"Raccourcis clavier",
            .keyNames = { .ctrl = L"Ctrl", .shift = L"Maj", .alt = L"Alt", .enter = L"Entrée", .backspace = L"Retour arrière",
                          .home = L"Origine", .escape = L"Échap", .del = L"Suppr", .menu = L"Menu" },
            .shortcutHelp = {
                .selectNearest = L"Sélectionner la boîte ou le secteur le plus proche dans cette direction",
                .activate = L"Zoom avant sur le dossier sélectionné, ou exécuter ou ouvrir le fichier sélectionné",
                .zoomOut = L"Remonter d'un niveau (zoom arrière)",
                .goDown = L"Descendre d'un niveau : vers la sélection, sinon par où vous êtes remonté",
                .goBack = L"Revenir à la vue précédente",
                .goForward = L"Avancer à nouveau, vers la vue d'où vous êtes revenu",
                .zoomFull = L"Vue complète",
                .clearSelection = L"Annuler la sélection",
                .open = L"Ouvrir un lecteur ou un dossier",
                .reload = L"Actualiser : relire ce qui a changé",
                .fullRescan = L"Analyse complète : tout relire",
                .pieChart = L"Basculer entre le compartimentage et le graphique en secteurs",
                .invertedPie = L"Basculer entre le compartimentage et les secteurs inversés",
                .del = L"Déplacer la sélection vers la Corbeille",
                .properties = L"Propriétés de la sélection",
                .copyPath = L"Copier le chemin d'accès de la sélection",
                .contextMenu = L"Menu contextuel de la sélection",
                .about = L"À propos de SpaceWindow",
            },
        };

        constexpr std::array<Lang, 3> kLanguages{ kEnglishUs, kEnglishUk, kFrench };

        constexpr bool StartsWithNoCase(std::wstring_view text, std::wstring_view prefix) noexcept
        {
            if (text.size() < prefix.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < prefix.size(); ++i)
            {
                wchar_t a = text[i];
                wchar_t b = prefix[i];
                if (a >= L'A' && a <= L'Z')
                {
                    a = static_cast<wchar_t>(a - L'A' + L'a');
                }
                if (b >= L'A' && b <= L'Z')
                {
                    b = static_cast<wchar_t>(b - L'A' + L'a');
                }
                if (a != b)
                {
                    return false;
                }
            }
            return true;
        }
    }

    std::span<Lang const> Languages() noexcept
    {
        return kLanguages;
    }

    Lang const& FindLanguage(std::wstring_view code) noexcept
    {
        for (Lang const& lang : kLanguages)
        {
            if (lang.code == code)
            {
                return lang;
            }
        }
        return kLanguages.front();
    }

    std::wstring_view DefaultLanguageCode()
    {
        ULONG count = 0;
        ULONG length = 0;
        if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &length) || length == 0)
        {
            return kEnglishUs.code;
        }

        std::wstring buffer(length, L'\0');
        if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &length))
        {
            return kEnglishUs.code;
        }

        // The buffer is a multi-string ("fr-FR\0en-US\0\0"); the first entry is the preference.
        std::wstring_view const languages{ buffer };
        std::wstring_view const preferred = languages.substr(0, languages.find(L'\0'));
        if (StartsWithNoCase(preferred, L"fr"))
        {
            return kFrench.code;
        }
        for (std::wstring_view const commonwealth : { L"en-GB", L"en-AU", L"en-NZ", L"en-IE", L"en-ZA", L"en-IN" })
        {
            if (StartsWithNoCase(preferred, commonwealth))
            {
                return kEnglishUk.code;
            }
        }
        return kEnglishUs.code;
    }

    Caption ParseCaption(std::wstring_view caption)
    {
        Caption result;
        result.text.reserve(caption.size());
        for (std::size_t i = 0; i < caption.size(); ++i)
        {
            wchar_t const c = caption[i];
            if (c != L'&')
            {
                result.text.push_back(c);
                continue;
            }
            if (i + 1 >= caption.size())
            {
                break;  // a trailing '&' marks nothing
            }
            wchar_t const next = caption[++i];
            result.text.push_back(next);
            if (next != L'&' && result.accessKey.empty())
            {
                result.accessKey.assign(1, next);
            }
        }
        return result;
    }

    std::wstring FormatText(std::wstring_view pattern, std::initializer_list<std::wstring_view> args)
    {
        std::wstring result;
        result.reserve(pattern.size() + 32);
        std::size_t i = 0;
        while (i < pattern.size())
        {
            if (pattern[i] == L'{')
            {
                std::size_t j = i + 1;
                std::size_t index = 0;
                while (j < pattern.size() && pattern[j] >= L'0' && pattern[j] <= L'9' && j - i <= 3)
                {
                    index = index * 10 + static_cast<std::size_t>(pattern[j] - L'0');
                    ++j;
                }
                if (j > i + 1 && j < pattern.size() && pattern[j] == L'}' && index < args.size())
                {
                    result.append(*(args.begin() + index));
                    i = j + 1;
                    continue;
                }
            }
            result.push_back(pattern[i]);
            ++i;
        }
        return result;
    }
}
