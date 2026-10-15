// SpaceWindow - keeping track of which folders change below a scanned folder.
#include "pch.h"
#include "FolderWatch.h"

#include "FolderTree.h"

namespace sm
{
    namespace
    {
        // Windows keeps changes in a buffer of this size between two reads; a
        // network share takes no more than 64 KB.
        constexpr std::size_t kLocalBufferBytes = std::size_t{ 256 } * 1024;
        constexpr std::size_t kRemoteBufferBytes = std::size_t{ 64 } * 1024;

        // With more changed folders than this, scanning again is about as
        // quick; more changed files than this are not looked at one by one.
        constexpr std::size_t kMaxFolders = 20'000;
        constexpr std::size_t kMaxFiles = 5'000;

        constexpr DWORD kWatched = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                   FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
    }

    std::shared_ptr<FolderWatch> FolderWatch::Start(std::wstring const& folder, bool remote)
    {
        std::shared_ptr<FolderWatch> watch(new FolderWatch());  // the constructor is private
        watch->folder_.reset(CreateFileW(ToExtendedLengthPath(folder).c_str(), FILE_LIST_DIRECTORY,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr));
        if (!watch->folder_ || !watch->done_.try_create(wil::EventOptions::ManualReset, nullptr) ||
            !watch->stop_.try_create(wil::EventOptions::ManualReset, nullptr))
        {
            return nullptr;
        }
        watch->buffer_.resize(remote ? kRemoteBufferBytes : kLocalBufferBytes);
        // Windows records changes from the first read on, so ask before the
        // scan starts: nothing that changes during it is missed.
        if (!watch->Watch())
        {
            return nullptr;
        }
        watch->thread_ = std::thread([raw = watch.get()] { raw->Run(); });
        return watch;
    }

    FolderWatch::~FolderWatch()
    {
        if (thread_.joinable())
        {
            stop_.SetEvent();
            thread_.join();
        }
        else if (folder_)
        {
            // Never started: a read may still be pending, writing to buffer_.
            (void)CancelIoEx(folder_.get(), &overlapped_);
            DWORD ignored = 0;
            (void)GetOverlappedResult(folder_.get(), &overlapped_, &ignored, TRUE);
        }
    }

    bool FolderWatch::Watch() noexcept
    {
        done_.ResetEvent();
        overlapped_ = {};
        overlapped_.hEvent = done_.get();
        return ReadDirectoryChangesW(folder_.get(), buffer_.data(), static_cast<DWORD>(buffer_.size()), TRUE, kWatched,
                                     nullptr, &overlapped_, nullptr) != FALSE;
    }

    void FolderWatch::Run() noexcept
    {
        std::array<HANDLE, 2> const events{ done_.get(), stop_.get() };
        for (;;)
        {
            if (WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, INFINITE) != WAIT_OBJECT_0)
            {
                break;  // stopping
            }
            DWORD bytes = 0;
            if (!GetOverlappedResult(folder_.get(), &overlapped_, &bytes, FALSE))
            {
                LoseTrack();  // the folder went away, or the share stopped answering
                return;
            }
            if (bytes == 0)
            {
                LoseTrack();  // more changed at once than the buffer held
            }
            else
            {
                try
                {
                    Record(buffer_.data(), bytes);
                }
                catch (...)
                {
                    LoseTrack();  // out of memory
                }
            }
            if (!Watch())
            {
                LoseTrack();
                return;
            }
        }
        // The read in flight writes to buffer_: let it finish before the buffer goes.
        (void)CancelIoEx(folder_.get(), &overlapped_);
        DWORD ignored = 0;
        (void)GetOverlappedResult(folder_.get(), &overlapped_, &ignored, TRUE);
    }

    void FolderWatch::Record(std::byte const* notifications, std::size_t bytes)
    {
        std::scoped_lock const lock(mutex_);
        if (!complete_)
        {
            return;  // a full scan is due anyway
        }
        constexpr std::size_t kHeader = offsetof(FILE_NOTIFY_INFORMATION, FileName);
        for (std::size_t offset = 0; bytes - offset >= kHeader;)
        {
            FILE_NOTIFY_INFORMATION info{};
            std::memcpy(&info, notifications + offset, kHeader);
            if (info.FileNameLength > bytes - offset - kHeader)
            {
                break;
            }
            std::wstring path(info.FileNameLength / sizeof(wchar_t), L'\0');
            std::memcpy(path.data(), notifications + offset + kHeader, path.size() * sizeof(wchar_t));

            // The folder holding what changed lists it differently now.
            std::size_t const slash = path.rfind(L'\\');
            folders_.emplace(slash == std::wstring::npos ? std::wstring() : path.substr(0, slash));
            if (folders_.size() > kMaxFolders)
            {
                complete_ = false;  // as LoseTrack(), which would take the lock again
                folders_.clear();
                files_.clear();
                return;
            }
            if (info.Action != FILE_ACTION_REMOVED && info.Action != FILE_ACTION_RENAMED_OLD_NAME &&
                files_.size() < kMaxFiles)
            {
                files_.insert(std::move(path));
            }

            if (info.NextEntryOffset == 0)
            {
                break;
            }
            offset += info.NextEntryOffset;
        }
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
