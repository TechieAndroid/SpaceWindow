// SpaceWindow - spreading a loop over the processors.
#include "pch.h"
#include "Parallel.h"

namespace sm
{
    namespace
    {
        // One ParallelFor: the threads that join in take the next index until none is left.
        struct Loop
        {
            std::function<void(std::size_t)> const& body;
            std::size_t const count;
            std::atomic<std::size_t> next{ 0 };
            std::atomic<bool> failed{ false };
            std::exception_ptr error;  // written once, by the thread that set `failed`

            void Run() noexcept
            {
                for (std::size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1))
                {
                    try
                    {
                        body(i);
                    }
                    catch (...)
                    {
                        if (!failed.exchange(true))
                        {
                            error = std::current_exception();
                        }
                        next.store(count);  // the calls not started yet are skipped
                    }
                }
            }
        };

        void CALLBACK RunLoop(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK) noexcept
        {
            static_cast<Loop*>(context)->Run();
        }
    }

    unsigned ParallelThreads() noexcept
    {
        // What it is used for - decoding records, building a tree - is held
        // up by memory more than by processors: on a 16-thread PC, four
        // threads build the tree as fast as sixteen, in half the processor time.
        return std::clamp(std::thread::hardware_concurrency(), 1u, 4u);
    }

    void ParallelFor(std::size_t count, std::function<void(std::size_t)> const& body)
    {
        if (count == 0)
        {
            return;
        }
        Loop loop{ body, count };
        std::size_t const helpers = std::min<std::size_t>(ParallelThreads(), count) - 1;
        {
            // Declared after `loop`, so it waits for every helper before `loop` goes.
            wil::unique_threadpool_work_nocancel work;
            if (helpers > 0)
            {
                work.reset(CreateThreadpoolWork(RunLoop, &loop, nullptr));
                for (std::size_t i = 0; work && i < helpers; ++i)
                {
                    SubmitThreadpoolWork(work.get());
                }
            }
            loop.Run();  // without helpers (no thread pool), all of it here
        }
        if (loop.error)
        {
            std::rethrow_exception(loop.error);
        }
    }
}
