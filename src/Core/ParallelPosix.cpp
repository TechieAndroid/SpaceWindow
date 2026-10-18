// SpaceWindow - spreading a loop over the processors (other systems than Windows).
//
// As Parallel.cpp, with threads of its own rather than Windows' thread pool:
// here the loops run only while a tree is built from NTFS records, in tests.
#include "pch.h"
#include "Parallel.h"

namespace sm
{
    unsigned ParallelThreads() noexcept
    {
        return std::clamp(std::thread::hardware_concurrency(), 1u, 4u);
    }

    void ParallelFor(std::size_t count, std::function<void(std::size_t)> const& body)
    {
        if (count == 0)
        {
            return;
        }
        std::atomic<std::size_t> next{ 0 };
        std::atomic<bool> failed{ false };
        std::exception_ptr error;  // written once, by the thread that set `failed`
        auto const run = [&]() noexcept {
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
        };
        {
            std::vector<std::jthread> helpers;
            std::size_t const wanted = std::min<std::size_t>(ParallelThreads(), count) - 1;
            for (std::size_t i = 0; i < wanted; ++i)
            {
                try
                {
                    helpers.emplace_back(run);
                }
                catch (std::system_error const&)
                {
                    break;  // no more threads: the ones there are do the rest
                }
            }
            run();
        }  // joins the helpers
        if (error)
        {
            std::rethrow_exception(error);
        }
    }
}
