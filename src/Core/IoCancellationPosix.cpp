// SpaceWindow - breaking off a scan's blocked I/O when it is stopped (other systems than Windows).
//
// A stop sends each enlisted thread a signal whose handler does nothing.  It
// is installed without SA_RESTART, so a system call it interrupts fails with
// EINTR instead of carrying on: the scanner then counts the folder it was
// listing as unreadable, and the scan - stopped - is thrown away anyway.
#include "pch.h"
#include "IoCancellation.h"

#include <csignal>

namespace sm
{
    namespace
    {
        int CancelSignal() noexcept
        {
            return SIGRTMIN + 7;  // a real-time signal nothing else is known to use
        }

        extern "C" void OnCancelSignal(int) {}

        // Installs the handler once - unless the program already handles the signal itself.
        bool InstallHandler() noexcept
        {
            static bool const installed = [] {
                struct sigaction current{};
                if (sigaction(CancelSignal(), nullptr, &current) != 0 || current.sa_handler != SIG_DFL)
                {
                    return current.sa_handler == OnCancelSignal;
                }
                struct sigaction action{};
                action.sa_handler = OnCancelSignal;
                sigemptyset(&action.sa_mask);
                action.sa_flags = 0;  // no SA_RESTART: interrupted calls fail with EINTR
                return sigaction(CancelSignal(), &action, nullptr) == 0;
            }();
            return installed;
        }
    }

    IoCancellation::IoCancellation(std::stop_token const& stop) : callback_(stop, Canceller{ this }) {}

    IoCancellation::Enlistment::Enlistment(IoCancellation& owner) noexcept : owner_(owner), thread_(pthread_self())
    {
        if (!InstallHandler())
        {
            return;  // not enlisted: a stop waits for the thread's I/O to end by itself
        }
        try
        {
            std::scoped_lock const lock(owner_.mutex_);
            owner_.threads_.push_back({ thread_ });
        }
        catch (...)  // NOLINT(bugprone-empty-catch): not enlisted, as above
        {
        }
    }

    IoCancellation::Enlistment::~Enlistment()
    {
        std::scoped_lock const lock(owner_.mutex_);
        auto const found = std::ranges::find_if(
            owner_.threads_, [this](Thread const& thread) { return pthread_equal(thread.id, thread_); });
        if (found != owner_.threads_.end())
        {
            owner_.threads_.erase(found);
        }
    }

    void IoCancellation::CancelAll() noexcept
    {
        // The threads stay enlisted - alive - while the lock is held.
        std::scoped_lock const lock(mutex_);
        for (Thread const& thread : threads_)
        {
            (void)pthread_kill(thread.id, CancelSignal());
        }
    }
}
