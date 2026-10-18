// SpaceWindow - how the window looks and what moves behind the map, decided
// without a toolkit: the theme shown (high contrast overrides the one picked)
// and the background animation's clock - when it plays, when it holds still.
//
// From the WinUI window (src/App/MainWindow.xaml.cpp: ApplyTheme;
// MainWindowMap.cpp: ShownBackdrop, BackdropPaused, BackdropTime), so that
// the Qt window decides the same way.
#pragma once

#include <chrono>
#include <optional>
#include <string_view>

#include "Settings.h"

namespace sm
{
    // The theme the window shows: the one picked - unless the desktop is in
    // high contrast, which overrides every theme with the desktop's own
    // colours, as "Use system setting" (the "system" theme) has them.
    std::wstring_view ShownThemeId(std::wstring_view picked, bool highContrast) noexcept;

    // What decides whether the background animation plays.
    struct BackdropConditions
    {
        Backdrop picked = Backdrop::None;  // in Settings, or being tried there
        bool room = false;                 // the map leaves space: a pie chart, or the welcome screen
        bool visible = true;               // the window can be seen (it is not minimized)
        bool animations = true;            // the desktop's animation effects are on
        bool highContrast = false;         // nothing moves behind the map then
        bool active = true;                // the window has the keyboard
        bool powerSaving = false;          // the desktop saves energy (battery saver, a power-saver profile)
    };

    // The background animation's clock.
    class BackdropClock
    {
    public:
        using Clock = std::chrono::steady_clock;
        static constexpr std::chrono::minutes kInactive{ 1 };  // in the background this long, it holds still

        // A window that never had the keyboard counts as in the background since `start`.
        explicit BackdropClock(Clock::time_point start = Clock::now()) noexcept : start_(start), deactivatedAt_(start)
        {
        }

        // The animation to play now: the one picked, where the map leaves room
        // for it, while the window can be seen - unless the desktop's
        // animation effects are off, or it is in high contrast.
        static Backdrop Shown(BackdropConditions const& conditions) noexcept;

        // It holds its last frame while the desktop saves energy, and once the
        // window has been in the background a while - it is left on screen
        // often enough, behind other windows.
        bool Paused(BackdropConditions const& conditions, Clock::time_point now) const noexcept;

        // Seconds into the animation: the clock stops while it holds still,
        // and carries on from there.
        double Time(BackdropConditions const& conditions, Clock::time_point now) noexcept;

        // The window lost the keyboard: the minute it has before it holds still starts.
        void Deactivated(Clock::time_point now) noexcept { deactivatedAt_ = now; }
        Clock::time_point DeactivatedAt() const noexcept { return deactivatedAt_; }

    private:
        Clock::time_point start_;
        Clock::time_point deactivatedAt_;
        std::optional<Clock::time_point> pausedAt_;
    };
}
