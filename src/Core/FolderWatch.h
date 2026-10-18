// SpaceWindow - keeping track of which folders change below a scanned folder.
//
// New in SpaceWindow, after the macOS port of SpaceMonger, which records
// changed folders with FSEvents.  A tree read from an NTFS master file table
// is brought up to date from the change journal (ChangeJournal.h); any other
// tree - a folder, a drive scanned without administrator rights, a network
// share - is watched from the moment its scan starts (ReadDirectoryChangesW),
// so a reload only lists again the folders that changed.  When more changes
// at once than the watch can keep up with, it says so, and the reload scans
// everything again.
//
// On Linux (FolderWatchPosix.cpp) the watch is inotify's, which watches one
// folder at a time: the scanner hands it each folder as it opens it, before
// listing it, so nothing that changes meanwhile is missed either.  A tree
// with more folders than the watch takes on is not watched (a reload scans
// it again), and neither is a network share, whose changes inotify cannot see.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <wil/resource.h>
#else
#include <atomic>
#include <cstddef>
#include <unordered_map>
#endif

namespace sm
{
    class FolderWatch
    {
    public:
        // Starts watching everything below `folder` (a full path).  `remote`:
        // it is on a network share.  Nothing if it cannot be watched.
        static std::shared_ptr<FolderWatch> Start(std::wstring const& folder, bool remote);

        FolderWatch(FolderWatch const&) = delete;
        FolderWatch& operator=(FolderWatch const&) = delete;
        FolderWatch(FolderWatch&&) = delete;
        FolderWatch& operator=(FolderWatch&&) = delete;
        ~FolderWatch();

        // Paths are relative to the watched folder; the empty path is the folder itself.
        struct Changes
        {
            bool complete = true;               // false: the watch lost track; scan everything again
            std::vector<std::wstring> folders;  // folders whose contents changed
            std::vector<std::wstring> files;    // files and folders made or changed, as far as there were not too many
        };

        // What changed since the watch started, or since the last call.
        Changes Take();

#ifndef _WIN32
        // Watches the folder open as `directory`, at `path` (a full path, ending
        // with a separator) below the watched folder.  The scanner calls this
        // for each folder before it lists it.  Any thread.
        void Add(int directory, std::wstring const& path);

        // Watches at most `folders` folders (fewer than the system allows),
        // giving up past them.  Call it before the first Add.
        void Limit(std::size_t folders) noexcept;

        // It gave up for want of watches: the tree has more folders than it may
        // watch, and watching it again would only give up again.
        bool OutOfWatches();
#endif

    private:
        FolderWatch() = default;
        void LoseTrack() noexcept;
#ifdef _WIN32
        bool Watch() noexcept;  // asks for the next batch of changes
        void Run() noexcept;
        void Record(std::byte const* notifications, std::size_t bytes);

        wil::unique_hfile folder_;
        wil::unique_event done_;
        wil::unique_event stop_;
        OVERLAPPED overlapped_{};
        std::vector<std::byte> buffer_;
#else
        void Run() noexcept;
        void Record(std::byte const* events, std::size_t bytes);
        void AddTree(std::wstring const& relative);  // a folder that came, and everything in it
        bool AddWatch(char const* path, std::wstring const& relative);

        int inotify_ = -1;
        int wake_ = -1;                                  // an eventfd that stops Run
        std::string root_;                               // the watched folder, as the system names it, ending with '/'
        std::size_t rootLength_ = 0;                     // of the watched folder's wide path
        std::atomic<std::size_t> budget_{ 0 };           // the most folders watched
        std::atomic<std::size_t> watching_{ 0 };         // folders watched, or about to be
        std::unordered_map<int, std::wstring> watched_;  // folders by watch descriptor; guarded by mutex_
        bool outOfWatches_ = false;                      // guarded by mutex_
#endif
        std::mutex mutex_;
        bool complete_ = true;                      // guarded by mutex_
        std::unordered_set<std::wstring> folders_;  // guarded by mutex_
        std::unordered_set<std::wstring> files_;    // guarded by mutex_
        std::thread thread_;
    };
}
