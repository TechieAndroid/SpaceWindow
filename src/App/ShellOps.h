// SpaceWindow - Windows Shell operations.
//
// Replaces OnFileRun(), OnFileDelete() and OnFileProperties() in
// SpaceMonger.cpp and CDriveInfo in DriveDialog.cpp of SpaceMonger 1.4.0
// (Copyright (c) 1998-2020 Sean Werkema, MIT License).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sm::shell
{
    // Runs a program or opens a document with its default verb.
    bool Open(HWND owner, std::wstring const& path);

    // Shows the Windows "Properties" dialog for a file or folder.
    bool ShowProperties(HWND owner, std::wstring const& path);

    // Opens File Explorer with the item selected.
    bool ShowInExplorer(std::wstring const& path);

    enum class DeleteResult
    {
        Deleted,
        Cancelled,
        Failed,
    };

    // Moves an item to the Recycle Bin with IFileOperation.  1.4.0 copied the
    // path into a MAX_PATH stack buffer with strcpy (an overflow for long
    // paths) and passed FOF_NOCONFIRMATION without asking first; the caller
    // now confirms, and Windows still warns before destroying anything that
    // cannot be recycled.
    DeleteResult Recycle(HWND owner, std::wstring const& path);

    struct DriveInfo
    {
        std::wstring root;          // "C:\"
        std::wstring displayName;   // "Local Disk (C:)"
        UINT type = DRIVE_UNKNOWN;
        bool ready = false;         // false: no media in a removable drive, disconnected share, ...
        std::uint64_t totalBytes = 0;
        std::uint64_t freeBytes = 0;
    };

    // Lists the drives.  Slow drives (network, removable) can take a while to
    // answer, so call this off the UI thread.
    std::vector<DriveInfo> EnumerateDrives();

    // A file's or drive's shell icon, as premultiplied 32-bit BGRA pixels.
    struct IconPixels
    {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> bgra;
    };
    IconPixels GetIcon(std::wstring const& path, int size);

    // The classic folder picker (used if the Windows App SDK picker is unavailable).
    std::optional<std::wstring> PickFolder(HWND owner);

    // Where theme files are read from: the user's own folder,
    // %APPDATA%\SpaceWindow\Themes, then Themes next to SpaceWindow.exe (for a
    // copy carried around on a USB stick).
    std::vector<std::wstring> ThemeFolders();

    // Opens a folder in File Explorer, creating it first if need be.
    bool OpenFolder(std::wstring const& path);
}
