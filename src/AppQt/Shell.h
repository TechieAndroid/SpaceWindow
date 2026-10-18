// SpaceWindow (Qt) - what the desktop does for the app: open files, show them
// in the file manager, move them to the trash, list the drives.
//
// The Linux counterpart of src/App/ShellOps.h, on freedesktop.org standards:
// xdg-open (QDesktopServices), the org.freedesktop.FileManager1 D-Bus
// interface that Dolphin, Nautilus, Nemo and Thunar answer, and the trash
// (QFile::moveToTrash).  Qt's strings are Unicode, file names are bytes: a
// name that is not UTF-8 goes to xdg-open, the file manager and the trash
// as its bytes instead (posix::MoveToTrash for the trash).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <QIcon>

class QUrl;
class QWidget;

namespace sm::qt::shell
{
    // Runs a program or opens a document with the application the desktop picks.
    bool Open(std::wstring const& path);

    // Shows the item selected in the file manager (or opens its folder).  The
    // file manager is asked without waiting for it.
    void ShowInFileManager(std::wstring const& path);

    // The file manager's Properties of the item, where it offers them.
    void ShowProperties(std::wstring const& path);

    // Moves an item to the trash.
    bool Trash(std::wstring const& path);

    // The path of a dropped or pasted file: URL, its bytes kept whole.
    std::optional<std::wstring> LocalPath(QUrl const& url);

    // The folder for a path: itself if it is one, else the folder it is in.
    std::wstring FolderOf(std::wstring const& path);

    // The icon of a file or folder, from the desktop's icon theme.
    QIcon IconOf(std::wstring const& path);

    struct Drive
    {
        std::wstring root;  // the mount point, ending with '/'
        std::wstring name;  // its label, or the mount point
        std::wstring device;
        std::uint64_t totalBytes = 0;
        std::uint64_t freeBytes = 0;
        bool remote = false;
    };

    // The mounted file systems worth scanning: disks and network shares, not
    // the system's own (proc, sysfs, tmpfs ...), nor snaps' images.
    std::vector<Drive> Drives();

    // Where theme files are read from: $XDG_CONFIG_HOME/SpaceWindow/Themes, then
    // Themes next to the program (in an AppImage, next to the AppImage).
    std::vector<std::wstring> ThemeFolders();

    // Opens a folder in the file manager, making it first if need be.
    bool OpenFolder(std::wstring const& path);
}
