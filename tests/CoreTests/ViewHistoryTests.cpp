// Tests for where the view has been: Back, Forward and the trail back down.
#include "pch.h"
#include "TestHarness.h"

#include "../../src/Core/ViewHistory.h"

namespace
{
    using Path = sm::ViewHistory::Path;

    // Moves the view as the main window does: Leave, move, Arrive.
    class Viewer
    {
    public:
        sm::ViewHistory history;
        Path view;  // the root of the scan at first
        Path gone;  // a folder deleted since, with everything in it

        void Go(Path to)
        {
            history.Leave(view);
            view = std::move(to);
            history.Arrive(view);
        }

        bool Back() { return Move(history.Back(view, Stays())); }
        bool Forward() { return Move(history.Forward(view, Stays())); }

    private:
        // Where `path` leads now: the deepest folder on the way that is still there.
        Path Resolve(Path path) const
        {
            if (!gone.empty() && path.size() >= gone.size() && std::equal(gone.begin(), gone.end(), path.begin()))
            {
                path.resize(gone.size() - 1);
            }
            return path;
        }

        std::function<bool(Path const&)> Stays() const
        {
            return [this](Path const& path) { return Resolve(path) == view; };
        }

        bool Move(std::optional<Path> const& to)
        {
            if (!to)
            {
                return false;
            }
            view = Resolve(*to);
            history.Arrive(view);
            return true;
        }
    };

    Path const kA{ L"A" };
    Path const kAB{ L"A", L"B" };
    Path const kABC{ L"A", L"B", L"C" };
}

TEST_CASE(BackAndForwardRetraceTheView)
{
    Viewer v;
    v.Go(kA);
    v.Go(kAB);
    v.Go(kA);  // up again
    CHECK(v.Back());
    CHECK(v.view == kAB);
    CHECK(v.Back());
    CHECK(v.view == kA);
    CHECK(v.Back());
    CHECK(v.view.empty());
    CHECK(!v.Back());
    CHECK(v.view.empty());

    CHECK(v.Forward());
    CHECK(v.view == kA);
    CHECK(v.Forward());
    CHECK(v.view == kAB);
    CHECK(v.Forward());
    CHECK(v.view == kA);
    CHECK(!v.Forward());

    // Going somewhere new drops what lay ahead.
    CHECK(v.Back());
    v.Go(Path{ L"X" });
    CHECK(!v.Forward());
    CHECK(v.Back());
    CHECK(v.view == kAB);
}

TEST_CASE(TheTrailLeadsBackDown)
{
    Viewer v;
    v.Go(kA);
    v.Go(kAB);
    v.Go(kABC);
    v.Go(kAB);
    v.Go(kA);
    v.Go(Path{});  // up to the top: the trail stays
    CHECK(v.history.TrailBelow(Path{}) != nullptr && *v.history.TrailBelow(Path{}) == L"A");
    CHECK(v.history.TrailBelow(kA) != nullptr && *v.history.TrailBelow(kA) == L"B");
    CHECK(v.history.TrailBelow(kAB) != nullptr && *v.history.TrailBelow(kAB) == L"C");
    CHECK(v.history.TrailBelow(kABC) == nullptr);

    v.Go(kA);  // back down it: it still goes on
    CHECK(v.history.TrailBelow(kA) != nullptr && *v.history.TrailBelow(kA) == L"B");

    v.Go(Path{ L"A", L"X" });  // off it: a new trail
    CHECK(v.history.TrailBelow(kA) != nullptr && *v.history.TrailBelow(kA) == L"X");
    CHECK(v.history.TrailBelow(Path{ L"A", L"X" }) == nullptr);
    CHECK(v.history.TrailBelow(Path{ L"Q" }) == nullptr);  // not on the trail

    // Back and Forward move along it too.
    CHECK(v.Back());
    CHECK(v.view == kA);
    CHECK(v.history.TrailBelow(kA) != nullptr && *v.history.TrailBelow(kA) == L"X");
}

TEST_CASE(StepsThatWouldNotMoveTheViewArePassedOver)
{
    Viewer v;
    v.Go(kA);
    v.Go(kAB);
    v.Go(kA);
    v.gone = kAB;  // deleted since: going back to it would show A again
    CHECK(v.Back());
    CHECK(v.view.empty());
    CHECK(v.Forward());
    CHECK(v.view == kA);
    CHECK(!v.Forward());
}

TEST_CASE(HistoryIsBounded)
{
    Viewer v;
    for (int i = 0; i < 150; ++i)
    {
        v.Go(Path{ std::to_wstring(i) });
    }
    std::size_t steps = 0;
    while (v.Back())
    {
        ++steps;
    }
    CHECK_EQ(steps, sm::ViewHistory::kLength);
    CHECK(v.view == Path{ L"49" });
}

TEST_CASE(ClearingForgetsWhereTheViewWent)
{
    Viewer v;
    v.Go(kA);
    v.Go(kAB);
    v.Go(kA);
    v.history.Clear();
    CHECK(!v.Back());
    CHECK(!v.Forward());
    CHECK(v.history.TrailBelow(Path{}) == nullptr);
}
