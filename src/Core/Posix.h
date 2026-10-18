// SpaceWindow - what the core library needs from Linux and other POSIX systems.
//
// New in the Linux port.  The rest of the library keeps Windows' vocabulary
// (Compat.h): UTF-16-sized std::wstring names, FILETIME dates, file
// attributes, dense record numbers for hard links.  This is where POSIX's
// vocabulary - UTF-8 byte strings, struct statx, (device, inode) pairs,
// /proc/self/mountinfo - is turned into it.
#pragma once

#ifndef _WIN32

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sm
{
    struct VolumeInfo;

    // Hard links are told apart by record numbers (Entry::record), which the
    // tree keeps dense and 32-bit, as NTFS's are: they index arrays and bit
    // sets.  Inode numbers are 64-bit and sparse (XFS's run into the
    // billions), so each tree numbers the files it meets itself: every file
    // and folder gets the next number, kept for as long as the tree lives so
    // that rescans of part of it give the same files the same numbers.
    // Thread-safe: the scanner's workers number their files at the same time.
    class InodeTable
    {
    public:
        // A small number for a device, for Intern: a tree spans few of them.
        std::uint32_t Device(std::uint64_t device);

        // The record number of a file; 0 if there are more than 2^32 - 1 files.
        std::uint32_t Intern(std::uint32_t device, std::uint64_t inode);

        // Numbers the files again in the order of their (device, inode), so
        // that a tree comes out the same however many threads scanned it.
        // Returns each old number's new one (0 stays 0).  Call it while no
        // other thread uses the table, and renumber the tree's entries with it.
        std::vector<std::uint32_t> Renumber();

        // Every record's file, by record number (0: none), in the order
        // Renumber puts them.  Call it while no other thread adds to the table.
        struct Key
        {
            std::uint64_t device = 0;
            std::uint64_t inode = 0;

            friend auto operator<=>(Key const&, Key const&) = default;
        };
        std::vector<Key> Keys() const;

        std::size_t Size() const noexcept;

    private:
        struct Slot
        {
            std::uint64_t inode = 0;
            std::uint32_t device = 0;
            std::uint32_t id = 0;  // 0: empty
        };
        struct Shard
        {
            mutable std::mutex mutex;
            std::vector<Slot> slots;  // open addressing, a power of two long
            std::size_t used = 0;
        };
        static constexpr std::size_t kShards = 64;
        static void Insert(std::vector<Slot>& slots, Slot const& slot, std::uint64_t hash) noexcept;

        std::array<Shard, kShards> shards_;
        std::mutex devicesMutex_;
        std::vector<std::uint64_t> devices_;
        std::mutex nextMutex_;
        std::uint64_t next_ = 1;
    };
}

namespace sm::posix
{
    // File names are bytes, usually UTF-8.  Bytes that are not UTF-8 become
    // U+DC80..U+DCFF (Python's "surrogateescape"), and go back to the same
    // bytes: every name round-trips, whatever its encoding.
    std::wstring FromNative(std::string_view bytes);
    void FromNative(std::string_view bytes, std::wstring& text);  // into `text`, keeping its memory
    std::string ToNative(std::wstring_view text);

    // UTF-8 text, or nothing if it is not valid UTF-8.
    std::optional<std::wstring> DecodeUtf8(std::string_view bytes);

    // UTF-16 code units (surrogate pairs joined), as wide text.
    std::wstring FromUtf16(std::u16string_view units);

    // Seconds and nanoseconds since 1970 as a FILETIME (100 ns since 1601).
    constexpr std::uint64_t kUnixEpochFileTime = 116'444'736'000'000'000ull;
    constexpr std::uint64_t ToFileTime(std::int64_t seconds, std::uint32_t nanoseconds) noexcept
    {
        std::int64_t const ticks = seconds * 10'000'000 + static_cast<std::int64_t>(nanoseconds / 100);
        return ticks < -static_cast<std::int64_t>(kUnixEpochFileTime)
                   ? 0
                   : static_cast<std::uint64_t>(ticks + static_cast<std::int64_t>(kUnixEpochFileTime));
    }

    // Seconds since 1970 of a FILETIME (rounded down).
    constexpr std::int64_t ToUnixTime(std::uint64_t fileTime) noexcept
    {
        std::int64_t const ticks = static_cast<std::int64_t>(fileTime) - static_cast<std::int64_t>(kUnixEpochFileTime);
        return ticks >= 0 ? ticks / 10'000'000 : -((-ticks + 9'999'999) / 10'000'000);
    }

    // Opens a folder for listing, also when its path is longer than the
    // system takes in one piece (PATH_MAX).  -1 on failure, with errno set.
    int OpenDirectory(std::string const& path) noexcept;

    // An absolute path without "." and ".." and with symbolic links resolved
    // as far as the path exists, ending with '/'; empty if there is no such path.
    std::wstring NormalizePath(std::wstring_view path);

    // A mounted file system, as /proc/self/mountinfo lists it: names as the
    // system has them, bytes (escapes undone).
    struct MountEntry
    {
        unsigned major = 0;  // the device's numbers
        unsigned minor = 0;
        std::string point;   // where it is mounted
        std::string type;    // "ext4", "nfs4", ...
        std::string source;  // "/dev/nvme0n1p2", "server:/share", ...
    };
    std::vector<MountEntry> ParseMountInfo(std::string_view text);
    std::vector<MountEntry> MountedFileSystems();

    // The volume (mounted file system) holding `path`: its mount point,
    // file system, sizes and kind of storage.
    VolumeInfo QueryVolume(std::wstring const& path);

    // The size and free space of the file system holding `path`.  Free space
    // counts the blocks kept for the administrator too, so that they do not
    // show as space used by unscanned files.
    bool QueryDiskSpace(std::wstring const& path, std::uint64_t& total, std::uint64_t& free) noexcept;

    // Whether nothing is at `path` any more (rather than, say, being unreadable).
    bool IsGone(std::wstring const& path) noexcept;

    // Moves a file or folder to the desktop's trash (TrashPosix.cpp), whatever
    // the encoding of its name.  False if it could not be moved there: it is
    // never copied from one file system to another.
    bool MoveToTrash(std::wstring const& path);
}

#endif
