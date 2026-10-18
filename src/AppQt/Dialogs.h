// SpaceWindow (Qt) - the dialogs: drive or folder, Settings, About, and the
// folders that could not be read.  The Qt counterparts of the dialogs of
// src/App/MainWindowDialogs.cpp (SpaceMonger 1.4.0's DriveDialog, SetupDlg
// and AboutDialog).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Lang.h"
#include "Settings.h"
#include "Theme.h"

class QWidget;

namespace sm::qt
{
    // A drive or folder to scan, or nothing if cancelled.  `current`: the
    // drive or folder shown now, selected in the list.
    std::optional<std::wstring> PickDriveOrFolder(QWidget* parent, Lang const& lang, std::wstring const& current);

    // What Settings asks of the window while it is open.
    struct SettingsHooks
    {
        // Shows a pick being tried - a theme, a language, an animation - as the
        // window would look with it (and with the original again on Cancel).
        std::function<void(Settings const&)> preview;
        // The themes, read again from their files (the program came back to
        // the front: a theme file may have been edited); the errors go in the argument.
        std::function<std::vector<Theme>(std::vector<std::wstring>&)> reloadThemes;
    };

    // Edits the settings; false if cancelled.  `themes` are the themes to
    // pick from; `themeErrors` the theme files that could not be used.
    bool EditSettings(QWidget* parent, Lang const& lang, Settings& settings, std::vector<Theme> themes,
                      std::vector<std::wstring> themeErrors, SettingsHooks const& hooks = {});

    void ShowAbout(QWidget* parent, Lang const& lang);

    // The folders a scan could not list.
    void ShowUnreadable(QWidget* parent, Lang const& lang, std::vector<std::wstring> const& paths);
}
