// SpaceWindow - breaking off a scan's blocked I/O when it is stopped.
#include "pch.h"
#include "IoCancellation.h"

namespace sm
{
    IoCancellation::IoCancellation(std::stop_token const& stop) : callback_(stop, Canceller{ this }) {}

    IoCancellation::Enlistment::Enlistment(IoCancellation& owner) : owner_(owner), threadId_(GetCurrentThreadId())
    {
        // CancelSynchronousIo needs a real handle with THREAD_TERMINATE access,
        // not the pseudo-handle GetCurrentThread returns.
        wil::unique_handle thread(OpenThread(THREAD_TERMINATE, FALSE, threadId_));
        THROW_LAST_ERROR_IF(!thread);
        std::scoped_lock const lock(owner_.mutex_);
        owner_.threads_.push_back({ threadId_, std::move(thread) });
    }

    IoCancellation::Enlistment::~Enlistment()
    {
        std::scoped_lock const lock(owner_.mutex_);
        auto const found = std::ranges::find(owner_.threads_, threadId_, &Thread::id);
        if (found != owner_.threads_.end())
        {
            owner_.threads_.erase(found);
        }
    }

    void IoCancellation::CancelAll() noexcept
    {
        std::scoped_lock const lock(mutex_);
        for (Thread const& thread : threads_)
        {
            (void)CancelSynchronousIo(thread.handle.get());  // fails harmlessly when no I/O is pending
        }
    }
}
