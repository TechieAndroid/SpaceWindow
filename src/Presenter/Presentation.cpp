// SpaceWindow - how the window looks and what moves behind the map.
#include "Presentation.h"

namespace sm
{
    std::wstring_view ShownThemeId(std::wstring_view picked, bool highContrast) noexcept
    {
        return highContrast ? std::wstring_view(L"system") : picked;
    }

    Backdrop BackdropClock::Shown(BackdropConditions const& conditions) noexcept
    {
        if (conditions.picked == Backdrop::None || !conditions.room || !conditions.visible || !conditions.animations ||
            conditions.highContrast)
        {
            return Backdrop::None;
        }
        return conditions.picked;
    }

    bool BackdropClock::Paused(BackdropConditions const& conditions, Clock::time_point now) const noexcept
    {
        return conditions.powerSaving || (!conditions.active && now - deactivatedAt_ >= kInactive);
    }

    double BackdropClock::Time(BackdropConditions const& conditions, Clock::time_point now) noexcept
    {
        if (Paused(conditions, now))
        {
            if (!pausedAt_)
            {
                pausedAt_ = now;
            }
        }
        else if (pausedAt_)
        {
            start_ += now - *pausedAt_;  // it carries on from where it stopped
            pausedAt_.reset();
        }
        return std::chrono::duration<double>(pausedAt_.value_or(now) - start_).count();
    }
}
