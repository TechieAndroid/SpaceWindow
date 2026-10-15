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
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <windows.h>
#include <wil/resource.h>

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

    private:
        FolderWatch() = default;
        bool Watch() noexcept;  // asks for the next batch of changes
        void Run() noexcept;
        void Record(std::byte const* notifications, std::size_t bytes);
        void LoseTrack() noexcept;

        wil::unique_hfile folder_;
        wil::unique_event done_;
        wil::unique_event stop_;
        OVERLAPPED overlapped_{};
        std::vector<std::byte> buffer_;
        std::mutex mutex_;
        bool complete_ = true;                      // guarded by mutex_
        std::unordered_set<std::wstring> folders_;  // guarded by mutex_
        std::unordered_set<std::wstring> files_;    // guarded by mutex_
        std::thread thread_;
    };
}
