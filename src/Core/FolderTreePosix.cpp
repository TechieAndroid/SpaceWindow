// SpaceWindow - the scanner's Linux side: listing folders, reading file sizes.
#include "pch.h"
#include "FolderTreePosix.h"

#include "FolderTree.h"
#include "FolderWatch.h"
#include "Posix.h"
#include "ScopeExit.h"

#include <cerrno>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#ifndef STATX_ATTR_MOUNT_ROOT
#define STATX_ATTR_MOUNT_ROOT 0x00002000  // Linux 5.8
#endif

namespace sm::posix
{
    namespace
    {
        bool IsDotOrDotDot(char const* name) noexcept
        {
            return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
        }

        // What a folder's entry is and takes, as the scanner keeps it.
        Listed Describe(struct statx const& info, struct statx const& parent, char const* name) noexcept
        {
            bool const sameDevice =
                info.stx_dev_major == parent.stx_dev_major && info.stx_dev_minor == parent.stx_dev_minor;
            Listed listed;
            listed.allocated = info.stx_blocks * 512;
            if (S_ISLNK(info.stx_mode))
            {
                listed.attributes = FILE_ATTRIBUTE_REPARSE_POINT;
                listed.reparseTag = IO_REPARSE_TAG_SYMLINK;
            }
            else if (S_ISDIR(info.stx_mode))
            {
                listed.attributes = FILE_ATTRIBUTE_DIRECTORY;
                // Another file system mounted here: an alias, like a Windows mount
                // point (what keeps /proc, /sys and /dev out of a scan of /).  A
                // btrfs subvolume has a device of its own but is no mount.
                bool const mounted = (info.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT) != 0
                                         ? (info.stx_attributes & STATX_ATTR_MOUNT_ROOT) != 0
                                         : !sameDevice;
                if (mounted)
                {
                    listed.attributes |= FILE_ATTRIBUTE_REPARSE_POINT;
                    listed.reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
                }
            }
            else if (S_ISREG(info.stx_mode))
            {
                listed.logical = info.stx_size;
                if ((info.stx_attributes & STATX_ATTR_COMPRESSED) != 0)
                {
                    listed.attributes |= FILE_ATTRIBUTE_COMPRESSED;
                }
                else if (listed.allocated < listed.logical)
                {
                    listed.attributes |= FILE_ATTRIBUTE_SPARSE_FILE;
                }
            }
            if ((info.stx_attributes & STATX_ATTR_ENCRYPTED) != 0)
            {
                listed.attributes |= FILE_ATTRIBUTE_ENCRYPTED;
            }
            if (name[0] == '.')
            {
                listed.attributes |= FILE_ATTRIBUTE_HIDDEN;
            }
            if ((info.stx_mode & 0222) == 0)
            {
                listed.attributes |= FILE_ATTRIBUTE_READONLY;
            }
            if (listed.attributes == 0)
            {
                listed.attributes = FILE_ATTRIBUTE_NORMAL;
            }
            listed.lastWriteTime = ToFileTime(info.stx_mtime.tv_sec, info.stx_mtime.tv_nsec);
            return listed;
        }
    }

    bool ListFolder(std::wstring const& path, ListingOptions const& options, std::stop_token const& stop,
                    std::vector<std::byte>& buffer, std::wstring& name, ListingSink& sink)
    {
        int const directory = OpenDirectory(ToNative(path));
        if (directory < 0)
        {
            return false;
        }
        ScopeExit const closer([directory]() noexcept { close(directory); });

        struct statx self{};
        if (statx(directory, "", AT_EMPTY_PATH, STATX_INO, &self) != 0)
        {
            return false;
        }
        if (options.watch != nullptr)
        {
            options.watch->Add(directory, path);  // before listing: nothing that changes meanwhile is missed
        }
        std::uint32_t const device =
            options.inodes != nullptr ? options.inodes->Device(makedev(self.stx_dev_major, self.stx_dev_minor)) : 0;
        // A network file system's client may answer from what it last heard;
        // the server has the sizes as they are (stat's behaviour).  Local file
        // systems know either way, and are not made to flush anything.
        int const sync = options.remote ? AT_STATX_SYNC_AS_STAT : AT_STATX_DONT_SYNC;
        constexpr unsigned kWanted =
            STATX_TYPE | STATX_MODE | STATX_NLINK | STATX_INO | STATX_SIZE | STATX_BLOCKS | STATX_MTIME;

        bool first = true;
        bool described = false;  // some entry could be asked about
        while (!stop.stop_requested())
        {
            ssize_t const bytes = getdents64(directory, buffer.data(), buffer.size());
            if (bytes <= 0)
            {
                return bytes >= 0 || !first;  // a listing that fails at once: unreadable
            }
            first = false;
            // The buffer holds a chain of variable-length records.
            for (std::size_t offset = 0; std::cmp_less(offset, bytes);)
            {
                auto const* const entry = reinterpret_cast<dirent64 const*>(buffer.data() + offset);
                offset += entry->d_reclen;
                if (IsDotOrDotDot(entry->d_name))
                {
                    continue;
                }
                struct statx info{};
                if (statx(directory, entry->d_name, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | sync, kWanted, &info) != 0)
                {
                    // Read but no search permission: the names, and nothing about them.
                    if (errno == EACCES && !described && faccessat(directory, ".", X_OK, AT_EACCESS) != 0)
                    {
                        return false;
                    }
                    continue;  // gone since the listing (or kept from us alone)
                }
                described = true;
                Listed listed = Describe(info, self, entry->d_name);
                if (options.inodes != nullptr && listed.reparseTag == 0)
                {
                    bool const sameDevice =
                        info.stx_dev_major == self.stx_dev_major && info.stx_dev_minor == self.stx_dev_minor;
                    std::uint32_t const at =
                        sameDevice ? device : options.inodes->Device(makedev(info.stx_dev_major, info.stx_dev_minor));
                    listed.record = options.inodes->Intern(at, info.stx_ino);
                }
                FromNative(entry->d_name, name);
                listed.name = name;
                sink.Add(listed);
            }
        }
        return true;
    }

    std::optional<FileSpace> ReadFileSpace(std::wstring const& path)
    {
        std::string const native = ToNative(path);
        std::size_t const slash = native.rfind('/');
        if (slash == std::string::npos || slash + 1 == native.size())
        {
            return std::nullopt;
        }
        // From its folder, so that paths longer than PATH_MAX work too.
        int const folder = OpenDirectory(slash == 0 ? std::string("/") : native.substr(0, slash));
        if (folder < 0)
        {
            return std::nullopt;
        }
        struct statx info{};
        int const result = statx(folder, native.c_str() + slash + 1, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT,
                                 STATX_TYPE | STATX_SIZE | STATX_BLOCKS, &info);
        close(folder);
        if (result != 0 || !S_ISREG(info.stx_mode))
        {
            return std::nullopt;
        }
        return FileSpace{ info.stx_blocks * 512, info.stx_size };
    }

    bool IsFolder(std::wstring const& path)
    {
        struct stat status{};
        return stat(ToNative(path).c_str(), &status) == 0 && S_ISDIR(status.st_mode);
    }

    std::uint32_t RecordOfFolder(std::wstring const& path, InodeTable& inodes)
    {
        struct stat status{};
        if (stat(ToNative(path).c_str(), &status) != 0)
        {
            return 0;
        }
        return inodes.Intern(inodes.Device(status.st_dev), status.st_ino);
    }

    void NumberInInodeOrder(FolderTree& tree, InodeTable& inodes, std::uint32_t rootRecord)
    {
        std::vector<std::uint32_t> const renumbered = inodes.Renumber();
        ForEachFolder(*tree.Root(), [&renumbered](Folder& folder) {
            for (Entry& entry : folder.entries)
            {
                entry.record = entry.record < renumbered.size() ? renumbered[entry.record] : 0;
            }
        });
        tree.rootRecord = rootRecord < renumbered.size() ? renumbered[rootRecord] : 0;
    }
}
