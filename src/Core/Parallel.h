// SpaceWindow - spreading a loop over the processors.
//
// New in SpaceWindow.  Reading a drive's master file table leaves the
// processors mostly idle while the disk delivers it; decoding its records and
// building the tree from them run on several of them (Ntfs.cpp).  The work
// runs on Windows' thread pool, so no threads are started or kept for it.
#pragma once

#include <cstddef>
#include <functional>

namespace sm
{
    // Calls `body(i)` for every i in [0, count), spread over a few processors
    // (ParallelThreads) - the calling thread works too - and returns once
    // every call has returned.  The calls run in no particular order, at the same time: they
    // must not touch the same data.  The first exception one of them throws is
    // thrown again here (the calls not started yet are skipped).
    void ParallelFor(std::size_t count, std::function<void(std::size_t)> const& body);

    // How many threads ParallelFor uses at most.
    unsigned ParallelThreads() noexcept;
}
