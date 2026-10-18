// SpaceWindow - running something when a scope is left, however it is left.
#pragma once

#include <utility>

namespace sm
{
    // As wil::scope_exit, which only Windows builds have.
    template <typename F>
    class ScopeExit
    {
    public:
        explicit ScopeExit(F f) noexcept : f_(std::move(f)) {}
        ScopeExit(ScopeExit const&) = delete;
        ScopeExit& operator=(ScopeExit const&) = delete;
        ~ScopeExit() { reset(); }

        // Runs it now rather than at the end of the scope.
        void reset() noexcept
        {
            if (active_)
            {
                active_ = false;
                f_();
            }
        }

    private:
        F f_;
        bool active_ = true;
    };
}
