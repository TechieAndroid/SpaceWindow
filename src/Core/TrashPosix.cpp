// SpaceWindow - moving files to the desktop's trash (Linux), by the
// freedesktop.org Trash specification 1.0, which GNOME, KDE and the other
// desktops follow: on the file's bytes, so that a name in any encoding goes
// and can be restored.  Nothing is copied: a file the trash cannot be renamed
// into stays where it is, and the caller says so.
#include "pch.h"
#include "Posix.h"

#include <cerrno>
#include <cstdlib>
#include <ctime>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sm::posix
{
    namespace
    {
        // A path in a trash information file: its bytes, escaped as in URLs.
        std::string Escaped(std::string_view bytes)
        {
            constexpr std::string_view kHex = "0123456789ABCDEF";
            std::string escaped;
            escaped.reserve(bytes.size());
            for (char const c : bytes)
            {
                auto const byte = static_cast<unsigned char>(c);
                bool const plain = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                   (byte >= '0' && byte <= '9') || c == '/' || c == '-' || c == '.' || c == '_' ||
                                   c == '~';
                if (plain)
                {
                    escaped.push_back(c);
                }
                else
                {
                    escaped.push_back('%');
                    escaped.push_back(kHex[byte >> 4]);
                    escaped.push_back(kHex[byte & 0x0F]);
                }
            }
            return escaped;
        }

        std::string DeletionDate()
        {
            std::time_t const now = std::time(nullptr);
            std::tm local{};
            std::array<char, 32> text{};
            if (localtime_r(&now, &local) == nullptr ||
                std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%S", &local) == 0)
            {
                return {};
            }
            return text.data();
        }

        // The user's own trash: $XDG_DATA_HOME/Trash, ~/.local/share/Trash by default.
        std::string HomeTrash()
        {
            if (char const* const data = std::getenv("XDG_DATA_HOME"); data != nullptr && data[0] == '/')
            {
                return std::string(data) + "/Trash";
            }
            if (char const* const home = std::getenv("HOME"); home != nullptr && home[0] == '/')
            {
                return std::string(home) + "/.local/share/Trash";
            }
            return {};
        }

        // A folder of the user's own - made, with those above it, if need be - and not a link.
        bool MakeFolder(std::string const& path)
        {
            for (std::size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1))
            {
                (void)mkdir(path.substr(0, slash).c_str(), 0700);
            }
            (void)mkdir(path.c_str(), 0700);
            struct stat status{};
            return lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) && status.st_uid == geteuid();
        }

        bool MakeTrash(std::string const& trash)
        {
            return MakeFolder(trash) && MakeFolder(trash + "/files") && MakeFolder(trash + "/info");
        }

        // Where the file system holding `path` (on `device`) is mounted: the
        // highest folder above it on the same device.
        std::string TopFolder(std::string const& path, dev_t device)
        {
            std::string top = path;
            while (top.size() > 1)
            {
                std::size_t const slash = top.rfind('/');
                std::string const parent = slash == 0 ? std::string("/") : top.substr(0, slash);
                struct stat status{};
                if (stat(parent.c_str(), &status) != 0 || status.st_dev != device)
                {
                    break;
                }
                top = parent;
            }
            return top;
        }

        // A trash at the top of another file system: the administrator's shared
        // $top/.Trash/$uid where there is one (a real folder, sticky), else $top/.Trash-$uid.
        std::string TopTrash(std::string const& top)
        {
            std::string const base = top == "/" ? std::string() : top;
            std::string const user = std::to_string(geteuid());
            struct stat shared{};
            if (lstat((base + "/.Trash").c_str(), &shared) == 0 && S_ISDIR(shared.st_mode) &&
                (shared.st_mode & S_ISVTX) != 0)
            {
                std::string const mine = base + "/.Trash/" + user;
                if (MakeTrash(mine))
                {
                    return mine;
                }
            }
            return base + "/.Trash-" + user;
        }

        bool WriteAll(int file, std::string_view text) noexcept
        {
            while (!text.empty())
            {
                ssize_t const written = write(file, text.data(), text.size());
                if (written < 0 && errno == EINTR)
                {
                    continue;
                }
                if (written <= 0)
                {
                    return false;
                }
                text.remove_prefix(static_cast<std::size_t>(written));
            }
            return true;
        }
    }

    bool MoveToTrash(std::wstring const& path)
    {
        std::string native = ToNative(path);
        while (native.size() > 1 && native.back() == '/')
        {
            native.pop_back();
        }
        struct stat item{};
        if (native.size() < 2 || native[0] != '/' || lstat(native.c_str(), &item) != 0)
        {
            return false;
        }

        // The user's own trash for a file on its file system; for one elsewhere,
        // the trash at the top of that file system, which records it relative to there.
        std::string trash = HomeTrash();
        std::string top;
        struct stat status{};
        if (trash.empty() || !MakeTrash(trash) || stat(trash.c_str(), &status) != 0 || status.st_dev != item.st_dev)
        {
            top = TopFolder(native, item.st_dev);
            trash = TopTrash(top);
            if (!MakeTrash(trash) || stat(trash.c_str(), &status) != 0 || status.st_dev != item.st_dev)
            {
                return false;
            }
        }
        std::string const recorded = top.empty() ? native : native.substr(top == "/" ? 1 : top.size() + 1);
        std::string const name = native.substr(native.rfind('/') + 1);

        // The information file is made first, and alone (O_EXCL): it holds the
        // name in the trash against anyone else trashing a namesake meanwhile.
        for (int copy = 1; copy < 10'000; ++copy)
        {
            std::string kept = name;
            if (copy > 1)
            {
                kept.append(".").append(std::to_string(copy));
            }
            std::string const info = std::string(trash).append("/info/").append(kept).append(".trashinfo");
            int const file = open(info.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (file < 0)
            {
                if (errno == EEXIST)
                {
                    continue;
                }
                return false;
            }
            std::string const target = std::string(trash).append("/files/").append(kept);
            struct stat taken{};
            if (lstat(target.c_str(), &taken) == 0)
            {
                close(file);  // left behind by someone who did not finish: another name
                (void)unlink(info.c_str());
                continue;
            }
            std::string const text =
                "[Trash Info]\nPath=" + Escaped(recorded) + "\nDeletionDate=" + DeletionDate() + "\n";
            bool const written = WriteAll(file, text);
            bool const closed = close(file) == 0;
            if (!written || !closed || rename(native.c_str(), target.c_str()) != 0)
            {
                int const error = errno;
                (void)unlink(info.c_str());
                errno = error;
                return false;
            }
            return true;
        }
        return false;
    }
}
