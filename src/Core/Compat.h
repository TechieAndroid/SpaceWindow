// SpaceWindow - the Windows constants the core library speaks, on other systems.
//
// The tree keeps Windows' file attributes (FILE_ATTRIBUTE_*), reparse tags and
// FILETIME dates whatever the system: the NTFS parser reads them from disk,
// the language tables name them, and the scanners of other systems make them
// up from what they find (Posix.cpp).  The window's commands keep Windows'
// virtual-key codes (Commands.h).  On Windows <windows.h> declares all of
// these; elsewhere they come from here, with the same names and values.
#pragma once

#ifndef _WIN32

#include <cstdint>

// Macros, as <windows.h> has them: code that uses them compiles unchanged.
// NOLINTBEGIN(modernize-macro-to-enum)

// File attributes.
#define FILE_ATTRIBUTE_READONLY 0x00000001u
#define FILE_ATTRIBUTE_HIDDEN 0x00000002u
#define FILE_ATTRIBUTE_SYSTEM 0x00000004u
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020u
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100u
#define FILE_ATTRIBUTE_SPARSE_FILE 0x00000200u
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400u
#define FILE_ATTRIBUTE_COMPRESSED 0x00000800u
#define FILE_ATTRIBUTE_OFFLINE 0x00001000u
#define FILE_ATTRIBUTE_ENCRYPTED 0x00004000u
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000u
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000u

// Reparse tags.  A symbolic link and a mount point are "name surrogates"
// (FolderTree.h: IsAlias), which the scanners skip.
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003u
#define IO_REPARSE_TAG_SYMLINK 0xA000000Cu
#define IO_REPARSE_TAG_WOF 0x80000017u
#define IO_REPARSE_TAG_CLOUD_6 0x9000601Au

// Virtual-key codes (WinRT's VirtualKey has the same values).
#define VK_BACK 0x08
#define VK_RETURN 0x0D
#define VK_ESCAPE 0x1B
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_DELETE 0x2E
#define VK_APPS 0x5D
#define VK_F1 0x70
#define VK_F5 0x74
#define VK_F24 0x87

// NOLINTEND(modernize-macro-to-enum)

#endif
