// SpaceWindow (Qt) - text and colours between the core library and Qt.
#include "QtText.h"

namespace sm::qt
{
    namespace
    {
        constexpr Wording kEnglish{
            .showInFileManager = L"Show in File Mana&ger",
            .confirmDeleteTitle = L"Move to Trash?",
            .confirmDelete = L"Are you sure you want to move “{0}” ({1}) to the Trash?",
            .deleteFailed = L"“{0}” could not be moved to the Trash.",
            .deleteHelp = L"Move the selection to the Trash",
            .unreadableText = L"SpaceWindow could not list these folders (permission was denied, or they went away "
                              L"during the scan), so what they hold is missing from the map.",
            .credits = L"Based on SpaceMonger 1.4.0 by Sean Werkema.  The Linux version is drawn with Qt.",
            .windowsColors = L"System Colors",
            .mapName = L"Disk usage map",
        };

        constexpr Wording MakeEnglishUk()
        {
            Wording wording = kEnglish;
            wording.windowsColors = L"System Colours";
            return wording;
        }
        constexpr Wording kEnglishUk = MakeEnglishUk();

        constexpr Wording kFrench{
            .showInFileManager = L"Afficher dans le &gestionnaire de fichiers",
            .confirmDeleteTitle = L"Mettre à la corbeille ?",
            .confirmDelete = L"Voulez-vous vraiment mettre « {0} » ({1}) à la corbeille ?",
            .deleteFailed = L"« {0} » n'a pas pu être mis à la corbeille.",
            .deleteHelp = L"Mettre la sélection à la corbeille",
            .unreadableText = L"SpaceWindow n'a pas pu lister ces dossiers (accès refusé, ou dossiers disparus "
                              L"pendant l'analyse) : leur contenu manque sur la carte.",
            .credits = L"D'après SpaceMonger 1.4.0 de Sean Werkema.  La version Linux est dessinée avec Qt.",
            .windowsColors = L"Couleurs du système",
            .mapName = L"Carte de l'espace disque",
        };
    }

    Wording const& WordingFor(Lang const& lang) noexcept
    {
        if (lang.code == L"fr")
        {
            return kFrench;
        }
        return lang.code == L"uk" ? kEnglishUk : kEnglish;
    }
}
