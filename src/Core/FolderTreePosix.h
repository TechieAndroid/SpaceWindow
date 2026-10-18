// SpaceWindow - the scanner's Linux side (FolderTreePosix.cpp): what
// FolderTree.cpp asks of the file system where the Windows build calls Win32.
#pragma once
#ifndef _WIN32

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace sm
{
    class FolderTree;
    class FolderWatch;
    class InodeTable;
}

namespace sm::posix
{
    // An entry of a folder, as listed.
    struct Listed
    {
        std::wstring_view name;
        std::uint32_t attributes = 0;     // FILE_ATTRIBUTE_* (Compat.h)
        std::uint32_t reparseTag = 0;     // IO_REPARSE_TAG_SYMLINK or _MOUNT_POINT: an alias
        std::uint64_t allocated = 0;      // the space it takes
        std::uint64_t logical = 0;        // its length
        std::uint64_t lastWriteTime = 0;  // FILETIME
        std::uint32_t record = 0;         // its file's number, when listed with an InodeTable
    };

    // Takes a folder's entries as they are listed.
    class ListingSink
    {
    public:
        virtual ~ListingSink() = default;
        virtual void Add(Listed const& entry) = 0;
    };

    struct ListingOptions
    {
        InodeTable* inodes = nullptr;  // numbers the entries' files (not the aliases')
        FolderWatch* watch = nullptr;  // is handed each folder before it is listed
        bool remote = false;           // a network drive: sizes from the server, not the client's cache
    };

    // Lists the folder at `path` (of any length) in large batches
    // (getdents64), asking each entry what it is and the space it takes
    // (statx), following no links: symbolic links and the folders other
    // file systems are mounted on come as aliases.  `buffer` and `name` are
    // working space, kept from folder to folder.  False if the folder cannot
    // be read - nor what it holds: one with read but no search permission
    // lists its names, but nothing about them.
    bool ListFolder(std::wstring const& path, ListingOptions const& options, std::stop_token const& stop,
                    std::vector<std::byte>& buffer, std::wstring& name, ListingSink& sink);

    // What a file takes and how long it is, asked of the file itself.
    struct FileSpace
    {
        std::uint64_t allocated = 0;
        std::uint64_t length = 0;
    };
    std::optional<FileSpace> ReadFileSpace(std::wstring const& path);

    // True if `path` names a folder (or a link to one).
    bool IsFolder(std::wstring const& path);

    // The record number of the folder at `path` (0 if unknown).
    std::uint32_t RecordOfFolder(std::wstring const& path, InodeTable& inodes);

    // After a scan: numbers the tree's files again in the order of their
    // inodes rather than of the threads that met them, so that a tree comes
    // out the same every time (hard links are counted by the lowest).
    void NumberInInodeOrder(FolderTree& tree, InodeTable& inodes, std::uint32_t rootRecord);
}
#endif
