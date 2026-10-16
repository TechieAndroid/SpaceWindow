// SpaceWindow - where the view of the map has been: Back and Forward
// (Alt+Left, Alt+Right) and the way back down (Alt+Down).  New in SpaceWindow;
// SpaceMonger 1.4.0 could only zoom in, out, or all the way out.
//
// A view is kept as the names of the folders from the root of the scan down to
// it, not as a folder: reloads and rescans replace the folders.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sm
{
    class ViewHistory
    {
    public:
        using Path = std::vector<std::wstring>;      // folder names from the root of the scan
        static constexpr std::size_t kLength = 100;  // views Back can return to

        // Before the view moves away from `view`: Back returns to it, and
        // there is nothing left to go Forward to.
        void Leave(Path view);

        // After the view moved to `view`.  The trail runs from the root down to
        // the deepest folder the view went to on its way here: going up leaves
        // it as it is, so that the view can go back down it; going anywhere off
        // it starts a new one.
        void Arrive(Path const& view);

        // The name of the folder in `view` that the trail goes on down to, or
        // nullptr where it ends.
        std::wstring const* TrailBelow(Path const& view) const noexcept;

        // Back and Forward from `view`: where to go, or nothing.  `stays` says
        // whether a step would leave the view where it is - folders deleted
        // since lead to the deepest folder on the way that is still there -
        // and such steps are passed over.
        std::optional<Path> Back(Path const& view, std::function<bool(Path const&)> const& stays);
        std::optional<Path> Forward(Path const& view, std::function<bool(Path const&)> const& stays);

        // For another drive or folder.
        void Clear() noexcept;

    private:
        static std::optional<Path> Step(std::vector<Path>& from, std::vector<Path>& to, Path const& view,
                                        std::function<bool(Path const&)> const& stays);

        std::vector<Path> back_;
        std::vector<Path> forward_;
        Path trail_;
    };
}
