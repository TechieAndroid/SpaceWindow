// SpaceWindow - keeping track of which folders change below a scanned folder (Linux: inotify).
//
// inotify watches folders one at a time, not trees.  The scanner hands the
// watch every folder it opens, before listing it (Add), and the watch adds
// the folders that come later itself: one made or moved in is watched with
// everything in it, one deleted or moved out is forgotten.  Each watch takes
// kernel memory, and the system allows only so many (fs.inotify.
// max_user_watches): past half of those, or kMaxWatches, the watch gives up -
// it reports that it lost track, and the reload scans everything again.
#include "pch.h"
#include "FolderWatch.h"

#include "FolderTree.h"
#include "Posix.h"

#include <cerrno>
#include <fstream>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sm
{
    namespace
    {
        // As FolderWatch.cpp: with more changed folders than this, scanning again
        // is about as quick; more changed files than this are not looked at one by one.
        constexpr std::size_t kMaxFolders = 20'000;
        constexpr std::size_t kMaxFiles = 5'000;

        constexpr std::size_t kMaxWatches = 200'000;

        constexpr std::uint32_t kWatched = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | IN_ATTRIB |
                                           IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_EXCL_UNLINK;

        std::size_t WatchBudget()
        {
            std::ifstream limit("/proc/sys/fs/inotify/max_user_watches");
            std::size_t system = 8192;  // the kernel's old default
            limit >> system;
            return std::min(kMaxWatches, system / 2);  // leave the rest to other programs
        }

        std::wstring Child(std::wstring const& folder, std::wstring_view name)
        {
            return folder.empty() ? std::wstring(name) : std::wstring(folder).append(1, L'/').append(name);
        }

        bool IsWithin(std::wstring_view path, std::wstring_view folder) noexcept
        {
            return path.starts_with(folder) && (path.size() == folder.size() || path[folder.size()] == L'/');
        }
    }

    std::shared_ptr<FolderWatch> FolderWatch::Start(std::wstring const& folder, bool remote)
    {
        if (remote || folder.empty() || folder.back() != L'/')
        {
            return nullptr;  // inotify does not hear of changes made by other machines
        }
        std::shared_ptr<FolderWatch> watch(new FolderWatch());  // the constructor is private
        watch->inotify_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        watch->wake_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (watch->inotify_ < 0 || watch->wake_ < 0)
        {
            return nullptr;
        }
        watch->root_ = posix::ToNative(folder);
        watch->rootLength_ = folder.size();
        watch->budget_.store(WatchBudget(), std::memory_order_relaxed);
        watch->thread_ = std::thread([raw = watch.get()] { raw->Run(); });
        return watch;
    }

    FolderWatch::~FolderWatch()
    {
        if (thread_.joinable())
        {
            std::uint64_t const one = 1;
            (void)!write(wake_, &one, sizeof(one));
            thread_.join();
        }
        for (int const fd : { inotify_, wake_ })
        {
            if (fd >= 0)
            {
                close(fd);
            }
        }
    }

    void FolderWatch::Add(int directory, std::wstring const& path)
    {
        if (path.size() < rootLength_)
        {
            return;  // not below the watched folder (the scanner's paths all are)
        }
        std::wstring relative = path.substr(rootLength_);
        while (!relative.empty() && relative.back() == L'/')
        {
            relative.pop_back();
        }
        // The open folder itself, whatever its path has become since it was opened.
        std::string const link = "/proc/self/fd/" + std::to_string(directory);
        if (!AddWatch(link.c_str(), relative) && errno == ENOENT)
        {
            (void)AddWatch(posix::ToNative(path).c_str(), relative);  // no /proc
        }
    }

    bool FolderWatch::AddWatch(char const* path, std::wstring const& relative)
    {
        {
            std::scoped_lock const lock(mutex_);
            if (!complete_)
            {
                errno = 0;
                return false;  // a full scan is due anyway
            }
        }
        // Outside the lock: the scanner's threads add their folders at the same time.
        int descriptor = -1;
        int error = ENOSPC;
        if (watching_.fetch_add(1, std::memory_order_relaxed) < budget_.load(std::memory_order_relaxed))
        {
            descriptor = inotify_add_watch(inotify_, path, kWatched);
            error = errno;
        }
        std::scoped_lock const lock(mutex_);
        if (descriptor >= 0 && complete_)
        {
            // The same folder twice (by two paths, or added again) has one descriptor.
            if (!watched_.insert_or_assign(descriptor, relative).second)
            {
                watching_.fetch_sub(1, std::memory_order_relaxed);
            }
            return true;
        }
        watching_.fetch_sub(1, std::memory_order_relaxed);
        if (descriptor < 0 && (error == ENOSPC || error == ENOMEM))
        {
            // Out of watches: as LoseTrack(), which would take the lock again.
            outOfWatches_ = true;
            complete_ = false;
            folders_.clear();
            files_.clear();
            for (auto const& [watched, folder] : watched_)
            {
                (void)inotify_rm_watch(inotify_, watched);
            }
            watched_.clear();
        }
        errno = descriptor < 0 ? error : 0;
        return false;
    }

    void FolderWatch::AddTree(std::wstring const& relative)
    {
        std::vector<std::wstring> pending{ relative };
        while (!pending.empty())
        {
            std::wstring const folder = std::move(pending.back());
            pending.pop_back();
            int const directory = posix::OpenDirectory(root_ + posix::ToNative(folder));
            if (directory < 0)
            {
                continue;  // gone again, or unreadable
            }
            std::string const link = "/proc/self/fd/" + std::to_string(directory);
            bool watched = AddWatch(link.c_str(), folder);
            if (!watched && errno == ENOENT)
            {
                watched = AddWatch((root_ + posix::ToNative(folder)).c_str(), folder);
            }
            if (watched)
            {
                if (DIR* const listing = fdopendir(directory))
                {
                    while (dirent const* const entry = readdir(listing))
                    {
                        std::string_view const name(entry->d_name);
                        if (name == "." || name == "..")
                        {
                            continue;
                        }
                        struct stat status{};
                        bool const isFolder = entry->d_type == DT_DIR || (entry->d_type == DT_UNKNOWN &&
                                                                          fstatat(dirfd(listing), entry->d_name,
                                                                                  &status, AT_SYMLINK_NOFOLLOW) == 0 &&
                                                                          S_ISDIR(status.st_mode));
                        if (isFolder)
                        {
                            pending.push_back(Child(folder, posix::FromNative(name)));
                        }
                    }
                    closedir(listing);
                    continue;
                }
            }
            close(directory);
        }
    }

    void FolderWatch::Run() noexcept
    {
        // Big enough for a burst of events; inotify never splits one.
        alignas(inotify_event) std::byte buffer[64 * 1024];
        std::array<pollfd, 2> waits{ { { inotify_, POLLIN, 0 }, { wake_, POLLIN, 0 } } };
        for (;;)
        {
            if (poll(waits.data(), waits.size(), -1) < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                LoseTrack();
                return;
            }
            if (waits[1].revents != 0)
            {
                return;  // stopping
            }
            for (;;)
            {
                ssize_t const bytes = read(inotify_, buffer, sizeof(buffer));
                if (bytes <= 0)
                {
                    break;  // EAGAIN: read them all
                }
                try
                {
                    Record(buffer, static_cast<std::size_t>(bytes));
                }
                catch (...)
                {
                    LoseTrack();  // out of memory
                }
            }
        }
    }

    void FolderWatch::Record(std::byte const* events, std::size_t bytes)
    {
        std::vector<std::wstring> came;  // folders made or moved in: watched once the lock is let go
        {
            std::scoped_lock const lock(mutex_);
            if (!complete_)
            {
                return;  // a full scan is due anyway
            }
            for (std::size_t offset = 0; bytes - offset >= sizeof(inotify_event);)
            {
                inotify_event header{};
                std::memcpy(&header, events + offset, sizeof(header));
                if (header.len > bytes - offset - sizeof(header))
                {
                    break;
                }
                std::string_view name(reinterpret_cast<char const*>(events + offset + sizeof(header)), header.len);
                name = name.substr(0, name.find('\0'));  // padded with zeros
                offset += sizeof(header) + header.len;

                if ((header.mask & IN_Q_OVERFLOW) != 0)
                {
                    complete_ = false;  // more changed at once than the kernel kept
                    folders_.clear();
                    files_.clear();
                    return;
                }
                auto const watched = watched_.find(header.wd);
                if (watched == watched_.end())
                {
                    continue;
                }
                if ((header.mask & IN_IGNORED) != 0)
                {
                    watched_.erase(watched);  // the folder went
                    watching_.fetch_sub(1, std::memory_order_relaxed);
                    continue;
                }
                std::wstring const folder = watched->second;
                if ((header.mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0)
                {
                    if (folder.empty())
                    {
                        complete_ = false;  // the watched folder itself went
                        folders_.clear();
                        files_.clear();
                        return;
                    }
                    continue;  // its folder hears of it too
                }

                // The folder holding what changed lists it differently now.
                folders_.insert(folder);
                if (folders_.size() > kMaxFolders)
                {
                    complete_ = false;
                    folders_.clear();
                    files_.clear();
                    return;
                }
                if (name.empty())
                {
                    continue;
                }
                std::wstring const path = Child(folder, posix::FromNative(name));
                bool const gone = (header.mask & (IN_DELETE | IN_MOVED_FROM)) != 0;
                if (!gone && files_.size() < kMaxFiles)
                {
                    files_.insert(path);
                }
                if ((header.mask & IN_ISDIR) != 0)
                {
                    if (gone)
                    {
                        // Its watches go: one moved elsewhere would report under its old name.
                        for (auto at = watched_.begin(); at != watched_.end();)
                        {
                            if (IsWithin(at->second, path))
                            {
                                // Its IN_IGNORED event finds it gone, and counts nothing.
                                (void)inotify_rm_watch(inotify_, at->first);
                                at = watched_.erase(at);
                                watching_.fetch_sub(1, std::memory_order_relaxed);
                            }
                            else
                            {
                                ++at;
                            }
                        }
                    }
                    else if ((header.mask & (IN_CREATE | IN_MOVED_TO)) != 0)
                    {
                        came.push_back(path);
                    }
                }
            }
        }
        for (std::wstring const& folder : came)
        {
            AddTree(folder);
        }
    }

    void FolderWatch::Limit(std::size_t folders) noexcept
    {
        budget_.store(std::min(budget_.load(std::memory_order_relaxed), folders), std::memory_order_relaxed);
    }

    bool FolderWatch::OutOfWatches()
    {
        std::scoped_lock const lock(mutex_);
        return outOfWatches_;
    }

    void FolderWatch::LoseTrack() noexcept
    {
        std::scoped_lock const lock(mutex_);
        complete_ = false;
        folders_.clear();
        files_.clear();
    }

    FolderWatch::Changes FolderWatch::Take()
    {
        std::scoped_lock const lock(mutex_);
        Changes changes;
        changes.complete = complete_;
        changes.folders.assign(folders_.begin(), folders_.end());
        changes.files.assign(files_.begin(), files_.end());
        folders_.clear();
        files_.clear();
        std::ranges::sort(changes.folders);
        std::ranges::sort(changes.files);
        return changes;
    }
}
