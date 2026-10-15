// SpaceWindow - where the view of the map has been.
#include "pch.h"
#include "ViewHistory.h"

namespace sm
{
    namespace
    {
        bool StartsWith(ViewHistory::Path const& path, ViewHistory::Path const& start) noexcept
        {
            return start.size() <= path.size() && std::equal(start.begin(), start.end(), path.begin());
        }
    }

    void ViewHistory::Leave(Path view)
    {
        back_.push_back(std::move(view));
        if (back_.size() > kLength)
        {
            back_.erase(back_.begin());
        }
        forward_.clear();
    }

    void ViewHistory::Arrive(Path const& view)
    {
        if (!StartsWith(trail_, view))
        {
            trail_ = view;
        }
    }

    std::wstring const* ViewHistory::TrailBelow(Path const& view) const noexcept
    {
        return trail_.size() > view.size() && StartsWith(trail_, view) ? &trail_[view.size()] : nullptr;
    }

    std::optional<ViewHistory::Path> ViewHistory::Back(Path const& view, std::function<bool(Path const&)> const& stays)
    {
        return Step(back_, forward_, view, stays);
    }

    std::optional<ViewHistory::Path> ViewHistory::Forward(Path const& view,
                                                          std::function<bool(Path const&)> const& stays)
    {
        return Step(forward_, back_, view, stays);
    }

    void ViewHistory::Clear() noexcept
    {
        back_.clear();
        forward_.clear();
        trail_.clear();
    }

    std::optional<ViewHistory::Path> ViewHistory::Step(std::vector<Path>& from, std::vector<Path>& to, Path const& view,
                                                       std::function<bool(Path const&)> const& stays)
    {
        while (!from.empty())
        {
            Path target = std::move(from.back());
            from.pop_back();
            if (!stays(target))
            {
                to.push_back(view);
                return target;
            }
        }
        return std::nullopt;
    }
}
