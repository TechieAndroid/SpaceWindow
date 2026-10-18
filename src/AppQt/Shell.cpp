// SpaceWindow (Qt) - what the desktop does for the app.
#include "Shell.h"

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <spawn.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusUnixFileDescriptor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QUrl>

#include "FolderTreePosix.h"
#include "Posix.h"
#include "QtText.h"

namespace sm::qt::shell
{
    namespace
    {
        constexpr int kFileManagerTimeout = 5000;  // ms: time for a file manager to start up and answer
        constexpr int kPortalTimeout = 10'000;

        // A path as Qt's file classes take it: only for names that are UTF-8
        // (Qt would decode the others, and lose what does not decode).
        QString FileName(std::wstring const& path)
        {
            return QFile::decodeName(QByteArray::fromStdString(posix::ToNative(path)));
        }

        bool IsUtf8(std::wstring const& path)
        {
            return posix::DecodeUtf8(posix::ToNative(path)).has_value();
        }

        // A file: URI of the path's bytes, each byte that needs it escaped
        // (RFC 3986): a name that is not UTF-8 survives it, and the file
        // managers' GLib and KIO turn it back into the same bytes.
        QString UriOf(std::wstring const& path)
        {
            QByteArray const bytes = QByteArray::fromStdString(posix::ToNative(path));
            return QStringLiteral("file://") + QString::fromLatin1(bytes.toPercentEncoding("/"));
        }

        // xdg-open with the path's bytes as its argument; waited for on a thread
        // of its own, so that it leaves no zombie.
        bool XdgOpen(std::wstring const& path)
        {
            std::string program = "xdg-open";
            std::string argument = posix::ToNative(path);
            std::array<char*, 3> arguments{ program.data(), argument.data(), nullptr };
            pid_t child = 0;
            if (posix_spawnp(&child, program.c_str(), nullptr, nullptr, arguments.data(), environ) != 0)
            {
                return false;
            }
            std::thread([child] {
                int status = 0;
                while (waitpid(child, &status, 0) < 0 && errno == EINTR)
                {
                }
            }).detach();
            return true;
        }

        // org.freedesktop.FileManager1 (ShowItems or ShowItemProperties), without
        // waiting for the answer: a file manager that takes its time to start
        // does not hold the window.  `otherwise` runs if none answers.
        void AskFileManager(QString const& method, std::wstring const& path, std::function<void()> otherwise)
        {
            QDBusMessage message = QDBusMessage::createMethodCall(
                QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("/org/freedesktop/FileManager1"),
                QStringLiteral("org.freedesktop.FileManager1"), method);
            message << QStringList{ UriOf(path) } << QString();
            auto* const watcher = new QDBusPendingCallWatcher(
                QDBusConnection::sessionBus().asyncCall(message, kFileManagerTimeout), QCoreApplication::instance());
            QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher,
                             [otherwise = std::move(otherwise)](QDBusPendingCallWatcher* call) {
                                 if (call->isError())
                                 {
                                     otherwise();
                                 }
                                 call->deleteLater();
                             });
        }

        // In a Flatpak, the trash is the desktop's, reached through its portal:
        // the sandbox's own would be one nobody looks in.  The portal answers at
        // once; the call waits for it, as moving a file to the trash does.
        bool InFlatpak()
        {
            static bool const inside = QFile::exists(QStringLiteral("/.flatpak-info"));
            return inside;
        }

        bool TrashThroughPortal(std::wstring const& path)
        {
            std::string native = posix::ToNative(path);
            while (native.size() > 1 && native.back() == '/')
            {
                native.pop_back();
            }
            int const file = open(native.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
            if (file < 0)
            {
                return false;
            }
            QDBusMessage message = QDBusMessage::createMethodCall(
                QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
                QStringLiteral("org.freedesktop.portal.Trash"), QStringLiteral("TrashFile"));
            message << QVariant::fromValue(QDBusUnixFileDescriptor(file));  // a copy of it
            close(file);
            QDBusMessage const reply = QDBusConnection::sessionBus().call(message, QDBus::Block, kPortalTimeout);
            return reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty() &&
                   reply.arguments().front().toUInt() == 1;
        }

        std::wstring ParentFolder(std::wstring path)
        {
            while (path.size() > 1 && path.back() == L'/')
            {
                path.pop_back();
            }
            std::size_t const slash = path.rfind(L'/');
            return slash == std::wstring::npos ? path : path.substr(0, slash + 1);
        }

        bool IsSystemFileSystem(std::string_view type)
        {
            static constexpr std::array<std::string_view, 26> kSystem{
                "proc",       "sysfs",  "tmpfs",     "devtmpfs",    "devpts",         "cgroup",     "cgroup2",
                "securityfs", "pstore", "efivarfs",  "bpf",         "tracefs",        "debugfs",    "configfs",
                "fusectl",    "mqueue", "hugetlbfs", "autofs",      "binfmt_misc",    "rpc_pipefs", "squashfs",
                "overlay",    "nsfs",   "ramfs",     "fuse.portal", "fuse.gvfsd-fuse"
            };
            return std::ranges::find(kSystem, type) != kSystem.end();
        }

        bool IsRemote(std::string_view type)
        {
            return type.starts_with("nfs") || type == "cifs" || type == "smb3" || type == "smbfs" ||
                   type == "fuse.sshfs" || type == "fuse.rclone" || type == "9p" || type == "ceph" ||
                   type == "glusterfs" || type == "davfs" || type == "fuse.davfs2";
        }

        // Volume labels, by the device that holds them: /dev/disk/by-label names
        // its links after them, with "\xNN" for the bytes a name cannot hold.
        std::map<std::string, std::string> Labels()
        {
            std::map<std::string, std::string> labels;
            std::error_code error;
            for (auto const& link : std::filesystem::directory_iterator("/dev/disk/by-label", error))
            {
                std::filesystem::path const device = std::filesystem::canonical(link.path(), error);
                if (error)
                {
                    error.clear();
                    continue;
                }
                std::string const escaped = link.path().filename().string();
                std::string label;
                for (std::size_t i = 0; i < escaped.size(); ++i)
                {
                    if (escaped.compare(i, 2, "\\x") == 0 && i + 3 < escaped.size() &&
                        std::isxdigit(static_cast<unsigned char>(escaped[i + 2])) != 0 &&
                        std::isxdigit(static_cast<unsigned char>(escaped[i + 3])) != 0)
                    {
                        label.push_back(static_cast<char>(std::stoi(escaped.substr(i + 2, 2), nullptr, 16)));
                        i += 3;
                    }
                    else
                    {
                        label.push_back(escaped[i]);
                    }
                }
                labels[device.string()] = label;
            }
            return labels;
        }
    }

    bool Open(std::wstring const& path)
    {
        // Qt's way where it can take the name: it knows the desktop's own launchers (and portals).
        return IsUtf8(path) ? QDesktopServices::openUrl(QUrl::fromLocalFile(FileName(path))) : XdgOpen(path);
    }

    void ShowInFileManager(std::wstring const& path)
    {
        AskFileManager(QStringLiteral("ShowItems"), path, [path] { (void)Open(ParentFolder(path)); });
    }

    void ShowProperties(std::wstring const& path)
    {
        AskFileManager(QStringLiteral("ShowItemProperties"), path, [path] { ShowInFileManager(path); });
    }

    bool Trash(std::wstring const& path)
    {
        if (InFlatpak())
        {
            return TrashThroughPortal(path);
        }
        if (!IsUtf8(path))
        {
            return posix::MoveToTrash(path);
        }
        QString name = FileName(path);
        while (name.size() > 1 && name.endsWith(QLatin1Char('/')))
        {
            name.chop(1);
        }
        return QFile::moveToTrash(name);
    }

    std::optional<std::wstring> LocalPath(QUrl const& url)
    {
        if (!url.isLocalFile())
        {
            return std::nullopt;
        }
        // From the encoded form, whose escapes are the name's bytes.
        QByteArray const bytes = QByteArray::fromPercentEncoding(url.path(QUrl::FullyEncoded).toLatin1());
        if (bytes.isEmpty())
        {
            return std::nullopt;
        }
        return posix::FromNative(bytes.toStdString());
    }

    std::wstring FolderOf(std::wstring const& path)
    {
        return posix::IsFolder(path) ? path : ParentFolder(path);
    }

    QIcon IconOf(std::wstring const& path)
    {
        static QFileIconProvider const provider;
        if (IsUtf8(path))
        {
            return provider.icon(QFileInfo(FileName(path)));
        }
        // A name Qt cannot reach the file by: the icon of its kind, from the
        // name's extension (nothing is read).
        if (posix::IsFolder(path))
        {
            return provider.icon(QAbstractFileIconProvider::Folder);
        }
        static QMimeDatabase const types;
        QMimeType const type = types.mimeTypeForFile(FileName(path), QMimeDatabase::MatchExtension);
        QIcon const icon = QIcon::fromTheme(type.iconName(), QIcon::fromTheme(type.genericIconName()));
        return icon.isNull() ? provider.icon(QAbstractFileIconProvider::File) : icon;
    }

    // From /proc/self/mountinfo, as the system names the mount points: a name
    // that is not UTF-8 is listed too, and reached (Qt's own list decodes them).
    std::vector<Drive> Drives()
    {
        std::map<std::string, std::string> const labels = Labels();
        std::map<std::string, Drive> byRoot;  // a folder mounted over is listed once, as its last mount
        for (posix::MountEntry const& mount : posix::MountedFileSystems())
        {
            struct statvfs space{};
            if (IsSystemFileSystem(mount.type) || mount.source.starts_with("/dev/loop") ||
                mount.point.starts_with("/snap/") || mount.point.starts_with("/boot/efi") ||
                statvfs(mount.point.c_str(), &space) != 0 || space.f_blocks == 0)
            {
                continue;
            }
            Drive drive;
            drive.root = posix::FromNative(mount.point);
            if (drive.root.back() != L'/')
            {
                drive.root.push_back(L'/');
            }
            std::error_code error;
            std::filesystem::path const device = std::filesystem::canonical(mount.source, error);
            auto const label = error ? labels.end() : labels.find(device.string());
            drive.name = mount.point == "/"      ? std::wstring(L"/")
                         : label != labels.end() ? posix::FromNative(label->second)
                                                 : posix::FromNative(mount.point);
            drive.device = posix::FromNative(mount.source);
            drive.totalBytes = std::uint64_t{ space.f_blocks } * space.f_frsize;
            drive.freeBytes = std::uint64_t{ space.f_bfree } * space.f_frsize;
            drive.remote = IsRemote(mount.type);
            byRoot.insert_or_assign(mount.point, std::move(drive));
        }
        // The same disk can be mounted in several places (bind mounts, btrfs
        // subvolumes): each is listed, under its own folder, in path order.
        std::vector<Drive> drives;
        drives.reserve(byRoot.size());
        for (auto& [root, drive] : byRoot)
        {
            drives.push_back(std::move(drive));
        }
        return drives;
    }

    std::vector<std::wstring> ThemeFolders()
    {
        std::vector<std::wstring> folders;
        if (char const* const config = std::getenv("XDG_CONFIG_HOME"); config != nullptr && config[0] == '/')
        {
            folders.push_back(posix::FromNative(config) + L"/SpaceWindow/Themes");
        }
        else if (char const* const home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
        {
            folders.push_back(posix::FromNative(home) + L"/.config/SpaceWindow/Themes");
        }
        // An AppImage runs from a mount of its own: the folder that matters is the AppImage's.
        QString program = QCoreApplication::applicationDirPath();
        if (char const* const appImage = std::getenv("APPIMAGE"); appImage != nullptr && appImage[0] == '/')
        {
            program = QFileInfo(QFile::decodeName(appImage)).absolutePath();
        }
        folders.push_back(posix::FromNative(QFile::encodeName(program).toStdString()) + L"/Themes");
        return folders;
    }

    bool OpenFolder(std::wstring const& path)
    {
        QString const name = FileName(path);
        if (!QDir().mkpath(name))
        {
            return false;
        }
        return Open(path);
    }
}
