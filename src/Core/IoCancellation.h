// SpaceWindow - breaking off a scan's blocked I/O when it is stopped.
//
// New in SpaceWindow.  A folder on a network share that stopped answering, or
// a disk still spinning up, can keep a thread inside CreateFile or a folder
// listing for tens of seconds before Windows gives up on it.  Threads that
// enlist here have that I/O cancelled (CancelSynchronousIo) the moment the
// scan is stopped, so Cancel takes effect at once.
#pragma once

#include <mutex>
#include <stop_token>
#include <vector>

#include <windows.h>
#include <wil/resource.h>

namespace sm
{
    class IoCancellation
    {
    public:
        explicit IoCancellation(std::stop_token const& stop);
        IoCancellation(IoCancellation const&) = delete;
        IoCancellation& operator=(IoCancellation const&) = delete;
        IoCancellation(IoCancellation&&) = delete;
        IoCancellation& operator=(IoCancellation&&) = delete;
        ~IoCancellation() = default;

        // While it lives, a stop request cancels whatever synchronous I/O the
        // thread that created it is blocked in.  Best effort: a thread that
        // cannot be enlisted (Windows is out of handles) runs all the same,
        // and a stop waits for its I/O to end by itself.
        class Enlistment
        {
        public:
            explicit Enlistment(IoCancellation& owner) noexcept;
            Enlistment(Enlistment const&) = delete;
            Enlistment& operator=(Enlistment const&) = delete;
            Enlistment(Enlistment&&) = delete;
            Enlistment& operator=(Enlistment&&) = delete;
            ~Enlistment();

        private:
            IoCancellation& owner_;
            DWORD threadId_;
        };

    private:
        struct Thread
        {
            DWORD id = 0;
            wil::unique_handle handle;
        };

        struct Canceller
        {
            IoCancellation* self;
            void operator()() const noexcept { self->CancelAll(); }
        };

        void CancelAll() noexcept;

        std::mutex mutex_;
        std::vector<Thread> threads_;             // guarded by mutex_
        std::stop_callback<Canceller> callback_;  // last: it may run as soon as it is constructed
    };
}
