// SpaceWindow - what the core library needs from Linux and other POSIX systems.
#include "pch.h"
#include "Posix.h"

#include "FolderTree.h"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace sm
{
    static_assert(sizeof(wchar_t) == 4, "POSIX wide strings hold whole code points");

    namespace
    {
        std::uint64_t Mix(std::uint64_t x) noexcept
        {
            // splitmix64's finalizer: inode numbers come in runs, so spread them.
            x ^= x >> 30;
            x *= 0xBF58476D1CE4E5B9ull;
            x ^= x >> 27;
            x *= 0x94D049BB133111EBull;
            x ^= x >> 31;
            return x;
        }

        std::uint64_t HashOf(std::uint32_t device, std::uint64_t inode) noexcept
        {
            return Mix(inode ^ (std::uint64_t{ device } << 48));
        }
    }

    void InodeTable::Insert(std::vector<Slot>& slots, Slot const& slot, std::uint64_t hash) noexcept
    {
        std::size_t const mask = slots.size() - 1;
        for (std::size_t i = static_cast<std::size_t>(hash) & mask;; i = (i + 1) & mask)
        {
            if (slots[i].id == 0)
            {
                slots[i] = slot;
                return;
            }
        }
    }

    std::uint32_t InodeTable::Device(std::uint64_t device)
    {
        std::scoped_lock const lock(devicesMutex_);
        for (std::size_t i = 0; i < devices_.size(); ++i)
        {
            if (devices_[i] == device)
            {
                return static_cast<std::uint32_t>(i);
            }
        }
        devices_.push_back(device);
        return static_cast<std::uint32_t>(devices_.size() - 1);
    }

    std::uint32_t InodeTable::Intern(std::uint32_t device, std::uint64_t inode)
    {
        std::uint64_t const hash = HashOf(device, inode);
        Shard& shard = shards_[hash >> 58];  // the top bits pick the shard, the low ones the slot
        std::scoped_lock const lock(shard.mutex);
        if (!shard.slots.empty())
        {
            std::size_t const mask = shard.slots.size() - 1;
            for (std::size_t i = static_cast<std::size_t>(hash) & mask; shard.slots[i].id != 0; i = (i + 1) & mask)
            {
                if (shard.slots[i].inode == inode && shard.slots[i].device == device)
                {
                    return shard.slots[i].id;
                }
            }
        }
        std::uint32_t id = 0;
        {
            std::scoped_lock const numbering(nextMutex_);
            if (next_ > UINT32_MAX)
            {
                return 0;
            }
            id = static_cast<std::uint32_t>(next_++);
        }
        if ((shard.used + 1) * 4 > shard.slots.size() * 3)
        {
            std::vector<Slot> grown(std::max<std::size_t>(64, shard.slots.size() * 2));
            for (Slot const& slot : shard.slots)
            {
                if (slot.id != 0)
                {
                    Insert(grown, slot, HashOf(slot.device, slot.inode));
                }
            }
            shard.slots.swap(grown);
        }
        Insert(shard.slots, Slot{ inode, device, id }, hash);
        ++shard.used;
        return id;
    }

    std::vector<std::uint32_t> InodeTable::Renumber()
    {
        struct Key
        {
            std::uint64_t device;
            std::uint64_t inode;
            std::uint32_t id;
        };
        std::vector<Key> keys;
        keys.reserve(Size());
        for (Shard const& shard : shards_)
        {
            for (Slot const& slot : shard.slots)
            {
                if (slot.id != 0)
                {
                    keys.push_back({ devices_[slot.device], slot.inode, slot.id });
                }
            }
        }
        std::ranges::sort(keys, [](Key const& a, Key const& b) {
            return a.device != b.device ? a.device < b.device : a.inode < b.inode;
        });
        std::vector<std::uint32_t> renumbered(static_cast<std::size_t>(next_), 0);
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            renumbered[keys[i].id] = static_cast<std::uint32_t>(i + 1);
        }
        for (Shard& shard : shards_)
        {
            for (Slot& slot : shard.slots)
            {
                if (slot.id != 0)
                {
                    slot.id = renumbered[slot.id];
                }
            }
        }
        next_ = keys.size() + 1;
        return renumbered;
    }

    std::vector<InodeTable::Key> InodeTable::Keys() const
    {
        std::vector<Key> keys(static_cast<std::size_t>(next_));
        for (Shard const& shard : shards_)
        {
            for (Slot const& slot : shard.slots)
            {
                if (slot.id != 0 && slot.id < keys.size())
                {
                    keys[slot.id] = { devices_[slot.device], slot.inode };
                }
            }
        }
        return keys;
    }

    std::size_t InodeTable::Size() const noexcept
    {
        std::size_t size = 0;
        for (Shard const& shard : shards_)
        {
            std::scoped_lock const lock(shard.mutex);
            size += shard.used;
        }
        return size;
    }
}

namespace sm::posix
{
    namespace
    {
        constexpr char32_t kReplacement = 0xFFFD;

        // Decodes one UTF-8 sequence at `at`; nothing if it is not valid there.
        std::optional<char32_t> DecodeOne(std::string_view bytes, std::size_t& at) noexcept
        {
            auto const byte = [&bytes](std::size_t i) { return static_cast<unsigned char>(bytes[i]); };
            unsigned char const lead = byte(at);
            if (lead < 0x80)
            {
                ++at;
                return lead;
            }
            std::size_t length = 0;
            char32_t minimum = 0;
            char32_t code = 0;
            if (lead >= 0xC2 && lead <= 0xDF)
            {
                length = 2;
                minimum = 0x80;
                code = lead & 0x1Fu;
            }
            else if (lead >= 0xE0 && lead <= 0xEF)
            {
                length = 3;
                minimum = 0x800;
                code = lead & 0x0Fu;
            }
            else if (lead >= 0xF0 && lead <= 0xF4)
            {
                length = 4;
                minimum = 0x10000;
                code = lead & 0x07u;
            }
            else
            {
                return std::nullopt;
            }
            if (bytes.size() - at < length)
            {
                return std::nullopt;
            }
            for (std::size_t i = 1; i < length; ++i)
            {
                unsigned char const next = byte(at + i);
                if ((next & 0xC0u) != 0x80u)
                {
                    return std::nullopt;
                }
                code = (code << 6) | (next & 0x3Fu);
            }
            if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            {
                return std::nullopt;
            }
            at += length;
            return code;
        }

        void Encode(std::string& out, char32_t code)
        {
            if (code < 0x80)
            {
                out.push_back(static_cast<char>(code));
            }
            else if (code < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            else if (code < 0x10000)
            {
                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (code >> 18)));
                out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
        }

        // A mount point as /proc/self/mountinfo writes it: space, tab, newline
        // and backslash in octal escapes.
        std::string Unescape(std::string_view field)
        {
            std::string result;
            result.reserve(field.size());
            for (std::size_t i = 0; i < field.size(); ++i)
            {
                if (field[i] == '\\' && field.size() - i >= 4)
                {
                    int value = 0;
                    bool octal = true;
                    for (std::size_t j = 1; j <= 3; ++j)
                    {
                        char const c = field[i + j];
                        octal = octal && c >= '0' && c <= '7';
                        value = value * 8 + (c - '0');
                    }
                    if (octal)
                    {
                        result.push_back(static_cast<char>(value));
                        i += 3;
                        continue;
                    }
                }
                result.push_back(field[i]);
            }
            return result;
        }

        // Is `prefix` the path `path` or a folder above it?
        bool IsWithinPath(std::string_view path, std::string_view prefix) noexcept
        {
            if (prefix == "/")
            {
                return true;
            }
            return path.starts_with(prefix) && (path.size() == prefix.size() || path[prefix.size()] == '/');
        }

        bool IsRemote(std::string_view type) noexcept
        {
            constexpr std::string_view kRemote[] = {
                "nfs",    "nfs4",       "cifs",        "smb3",       "smbfs",
                "9p",     "afs",        "ceph",        "glusterfs",  "davfs",
                "ncpfs",  "fuse.sshfs", "fuse.rclone", "fuse.s3fs",  "fuse.gvfsd-fuse",
                "lustre", "gpfs",       "beegfs",      "fuse.davfs2"
            };
            return std::ranges::find(kRemote, type) != std::end(kRemote);
        }

        std::optional<bool> ReadRotational(unsigned major, unsigned minor)
        {
            // A whole disk has its queue; a partition's is the disk's, one folder up.
            std::string const device = "/sys/dev/block/" + std::to_string(major) + ":" + std::to_string(minor);
            for (char const* const queue : { "/queue/rotational", "/../queue/rotational" })
            {
                std::ifstream file(device + queue);
                int value = -1;
                if (file >> value && (value == 0 || value == 1))
                {
                    return value == 1;
                }
            }
            return std::nullopt;
        }
    }

    std::wstring FromNative(std::string_view bytes)
    {
        std::wstring text;
        FromNative(bytes, text);
        return text;
    }

    void FromNative(std::string_view bytes, std::wstring& text)
    {
        text.clear();
        text.reserve(bytes.size());
        for (std::size_t at = 0; at < bytes.size();)
        {
            if (std::optional<char32_t> const code = DecodeOne(bytes, at))
            {
                text.push_back(static_cast<wchar_t>(*code));
            }
            else
            {
                text.push_back(static_cast<wchar_t>(0xDC00 + static_cast<unsigned char>(bytes[at])));
                ++at;
            }
        }
    }

    std::string ToNative(std::wstring_view text)
    {
        std::string bytes;
        bytes.reserve(text.size());
        for (wchar_t const c : text)
        {
            auto const code = static_cast<char32_t>(c);
            if (code >= 0xDC80 && code <= 0xDCFF)
            {
                bytes.push_back(static_cast<char>(code - 0xDC00));  // an escaped byte
            }
            else if ((code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF)
            {
                Encode(bytes, kReplacement);
            }
            else
            {
                Encode(bytes, code);
            }
        }
        return bytes;
    }

    std::optional<std::wstring> DecodeUtf8(std::string_view bytes)
    {
        std::wstring text;
        text.reserve(bytes.size());
        for (std::size_t at = 0; at < bytes.size();)
        {
            std::optional<char32_t> const code = DecodeOne(bytes, at);
            if (!code)
            {
                return std::nullopt;
            }
            text.push_back(static_cast<wchar_t>(*code));
        }
        return text;
    }

    std::wstring FromUtf16(std::u16string_view units)
    {
        std::wstring text;
        text.reserve(units.size());
        for (std::size_t i = 0; i < units.size(); ++i)
        {
            char32_t code = units[i];
            if (code >= 0xD800 && code <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 &&
                units[i + 1] <= 0xDFFF)
            {
                code = 0x10000 + ((code - 0xD800) << 10) + (units[i + 1] - 0xDC00);
                ++i;
            }
            else if (code >= 0xD800 && code <= 0xDFFF)
            {
                code = kReplacement;  // half a pair
            }
            text.push_back(static_cast<wchar_t>(code));
        }
        return text;
    }

    int OpenDirectory(std::string const& path) noexcept
    {
        constexpr int kFlags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOCTTY;
        int const fd = open(path.c_str(), kFlags);
        if (fd >= 0 || errno != ENAMETOOLONG)
        {
            return fd;
        }
        // Too long for one call: one folder at a time, each relative to the one before.
        int at = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
        std::size_t begin = 0;
        while (at >= 0)
        {
            while (begin < path.size() && path[begin] == '/')
            {
                ++begin;
            }
            if (begin >= path.size())
            {
                break;
            }
            std::size_t const end = std::min(path.find('/', begin), path.size());
            std::string const name = path.substr(begin, end - begin);
            begin = end;
            bool const last = path.find_first_not_of('/', end) == std::string::npos;
            int const next = openat(at, name.c_str(), last ? kFlags : (O_PATH | O_DIRECTORY | O_CLOEXEC));
            int const error = errno;
            close(at);
            errno = error;
            at = next;
            if (last)
            {
                return at;
            }
        }
        return at;
    }

    std::wstring NormalizePath(std::wstring_view path)
    {
        namespace fs = std::filesystem;
        std::error_code error;
        fs::path native(ToNative(path));
        if (native.is_relative())
        {
            native = fs::current_path(error) / native;
            if (error)
            {
                return {};
            }
        }
        fs::path resolved = fs::weakly_canonical(native, error);
        if (error)
        {
            resolved = native.lexically_normal();
        }
        std::wstring result = FromNative(resolved.string());
        if (result.empty() || result.front() != L'/')
        {
            return {};
        }
        while (result.size() > 1 && result.back() == L'/')
        {
            result.pop_back();
        }
        if (result.back() != L'/')
        {
            result.push_back(L'/');
        }
        return result;
    }

    std::vector<MountEntry> ParseMountInfo(std::string_view text)
    {
        std::vector<MountEntry> mounts;
        while (!text.empty())
        {
            std::size_t const end = text.find('\n');
            std::string_view const line = text.substr(0, end);
            text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);

            // id parent major:minor root mount-point options [optional...] - type source super-options
            std::vector<std::string_view> fields;
            std::string_view rest = line;
            while (!rest.empty())
            {
                std::size_t const space = rest.find(' ');
                fields.push_back(rest.substr(0, space));
                rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
            }
            auto const dash = std::ranges::find(fields, std::string_view("-"));
            if (fields.size() < 5 || dash == fields.end() || dash + 1 == fields.end())
            {
                continue;
            }
            // "major:minor", in decimal.
            MountEntry mount;
            unsigned* number = &mount.major;
            bool parsed = !fields[2].empty();
            for (char const c : fields[2])
            {
                if (c == ':' && number == &mount.major)
                {
                    number = &mount.minor;
                }
                else if (c >= '0' && c <= '9' && *number < 100'000'000)
                {
                    *number = *number * 10 + static_cast<unsigned>(c - '0');
                }
                else
                {
                    parsed = false;
                }
            }
            if (!parsed || number != &mount.minor)
            {
                continue;
            }
            mount.point = Unescape(fields[4]);
            mount.type = std::string(*(dash + 1));
            mount.source = dash + 2 != fields.end() ? Unescape(*(dash + 2)) : std::string();
            mounts.push_back(std::move(mount));
        }
        return mounts;
    }

    std::vector<MountEntry> MountedFileSystems()
    {
        std::ifstream file("/proc/self/mountinfo", std::ios::binary);
        std::string const text((std::istreambuf_iterator<char>(file)), {});
        return ParseMountInfo(text);
    }

    VolumeInfo QueryVolume(std::wstring const& path)
    {
        VolumeInfo volume;
        std::string const native = ToNative(path);
        struct stat status{};
        if (stat(native.c_str(), &status) != 0)
        {
            return volume;
        }

        // The mount point holding the path: the longest one above it, of the
        // same device if any is (a bind mount or a subvolume can say otherwise).
        std::string bestPoint;
        std::string bestType;
        unsigned bestMajor = 0;
        unsigned bestMinor = 0;
        bool bestSameDevice = false;
        for (MountEntry const& mount : MountedFileSystems())
        {
            if (!IsWithinPath(native, mount.point))
            {
                continue;
            }
            bool const sameDevice = makedev(mount.major, mount.minor) == status.st_dev;
            if ((sameDevice && !bestSameDevice) ||
                (sameDevice == bestSameDevice && mount.point.size() >= bestPoint.size()))
            {
                bestPoint = mount.point;
                bestType = mount.type;
                bestMajor = mount.major;
                bestMinor = mount.minor;
                bestSameDevice = sameDevice;
            }
        }
        if (!bestPoint.empty())
        {
            volume.root = FromNative(bestPoint);
            if (volume.root.back() != L'/')
            {
                volume.root.push_back(L'/');
            }
            volume.fileSystem = FromNative(bestType);
            volume.remote = IsRemote(bestType);
            if (!volume.remote)
            {
                volume.seekPenalty = ReadRotational(bestMajor, bestMinor);
            }
        }

        struct statvfs space{};
        if (statvfs(native.c_str(), &space) == 0)
        {
            volume.totalBytes = std::uint64_t{ space.f_blocks } * space.f_frsize;
            volume.freeBytes = std::uint64_t{ space.f_bfree } * space.f_frsize;
            volume.clusterSize = space.f_frsize;
        }
        return volume;
    }

    bool QueryDiskSpace(std::wstring const& path, std::uint64_t& total, std::uint64_t& free) noexcept
    {
        try
        {
            struct statvfs space{};
            if (statvfs(ToNative(path).c_str(), &space) != 0)
            {
                return false;
            }
            total = std::uint64_t{ space.f_blocks } * space.f_frsize;
            free = std::uint64_t{ space.f_bfree } * space.f_frsize;
            return true;
        }
        catch (...)  // out of memory for the path
        {
            return false;
        }
    }

    bool IsGone(std::wstring const& path) noexcept
    {
        try
        {
            struct stat status{};
            return lstat(ToNative(path).c_str(), &status) != 0 && (errno == ENOENT || errno == ENOTDIR);
        }
        catch (...)
        {
            return false;
        }
    }
}
