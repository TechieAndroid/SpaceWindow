// SpaceWindow - pie chart layout.
//
// New in SpaceWindow: the pie chart view shows the same tree as the treemap.  The
// ring around the hub is a pie of the current folder's contents, largest
// first, clockwise from twelve o'clock; each ring further out shows the
// contents of the folders in the ring inside it, within their folder's
// angle.  The hub stands for the current folder itself.
//
// As a pie chart should, it never draws slivers: entries whose slice would
// be too thin to see are folded into one "smaller items" slice per folder
// (free space and unscanned space excepted: they always keep their slice).
// Only as many rings are used as the tree is deep (up to the limit), and
// every length is in pixels, like the treemap's.
//
// Experimental: inverted, the rings travel inward instead.  The current
// folder is a rim around the edge, its contents the ring inside the rim, and
// each ring further in shows the contents of the folders in the ring outside
// it; there is no hub, and the innermost ring's slices meet in the centre.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "MapItem.h"

namespace sm
{
    struct PieOptions
    {
        int rings = 4;              // most rings around the hub
        double minArc = 10.0;       // pixels along the middle of its ring: thinner slices are folded
        double gap = 2.0;           // pixels of background between neighbouring slices and rings
        double margin = 8.0;        // pixels between the pie and the edge of the area
        double hubFraction = 0.22;  // radius of the hub, as a fraction of the pie's
        double rimFraction = 0.10;  // inverted: width of the rim, as a fraction of the pie's radius
        bool showFreeSpace = true;
        bool inverted = false;  // experimental: the folder as a rim, deeper levels further in, to the centre

        // Rings and the thinnest slice for a density setting at a given scale
        // (density -3 .. 2, from "too few files" to "too many files", as for the treemap).
        static PieOptions ForDensity(int density, double scale);
    };

    struct PiePoint
    {
        double x = 0.0;
        double y = 0.0;
    };

    // A slice; an entry of kNoEntry stands for `folded` small entries of `source`.
    struct PieSlice : MapItem
    {
        int ring = 0;              // 0 = the folder's own contents: around the hub, or inside the rim when inverted
        double start = 0.0;        // radians, clockwise from twelve o'clock
        double sweep = 0.0;        // radians
        std::uint64_t bytes = 0;   // the space the slice stands for
        std::uint32_t folded = 0;  // entries folded into this slice (0 for a slice of one entry)

        bool IsFolded() const noexcept { return folded != 0; }
        bool ContainsAngle(double angle) const noexcept { return angle >= start && angle < start + sweep; }
    };

    class PieLayout
    {
    public:
        // Lays out `folder` in a width x height pixel area.  `depth` is the
        // folder's depth in the whole tree, so colours match the treemap's.
        void Build(Folder* folder, int depth, int width, int height, PieOptions const& options);
        void Clear() noexcept;

        std::span<PieSlice const> Items() const noexcept { return items_; }
        PieOptions const& Options() const noexcept { return options_; }
        Folder* Root() const noexcept { return root_; }
        std::uint64_t Total() const noexcept { return total_; }  // the bytes the whole pie stands for
        int Rings() const noexcept { return rings_; }            // rings in use

        // Geometry, in pixels.
        PiePoint Center() const noexcept { return { centerX_, centerY_ }; }
        double Radius() const noexcept { return radius_; }

        // The current folder itself: the hub in the middle (from 0), or the rim
        // around the edge when inverted.
        double HubInner() const noexcept { return hubInner_; }
        double HubOuter() const noexcept { return hubOuter_; }

        // Where the rings are: from the hub to the edge, or from the centre to
        // the rim when inverted (the innermost ring's slices then meet in the centre).
        double RingsInner() const noexcept { return ringsInner_; }
        double RingsOuter() const noexcept { return ringsOuter_; }
        double InnerRadius(int ring) const noexcept
        {
            return options_.inverted ? (rings_ - 1 - ring) * ringWidth_ : ringsInner_ + ring * ringWidth_;
        }
        double OuterRadius(int ring) const noexcept { return InnerRadius(ring) + ringWidth_; }

        // Where a slice's label goes: halfway along its arc, halfway through its
        // ring - or, for a slice that comes to a point in the centre, where it
        // is wider: further out (the centre itself for a whole disc).
        PiePoint Centroid(PieSlice const& slice) const noexcept;

        // The slice at a point - of any kind, so grouped small entries and
        // free space get tips too - or -1 for the hub (or rim) and empty space.
        int HitTest(double x, double y) const noexcept;
        bool HubContains(double x, double y) const noexcept;  // the hub, or the rim when inverted

        // The slice at a point and the folders' slices inside it, innermost
        // first (for the rollover highlight).
        void ContainersAt(double x, double y, std::vector<int>& result) const;

        // The slice that shows `folder`'s own entry (its slice in the parent), or -1.
        int FindFolder(Folder const* folder) const noexcept;

        // The slice that shows entry `index` of `folder`, or -1.
        int Find(Folder const* folder, std::uint32_t index) const noexcept;

    private:
        void LayoutFolder(Folder* folder, int depth, int ring, double start, double sweep, std::uint64_t total);
        void Fold(Folder* folder, std::size_t first, int depth, int ring, double start, double sweep,
                  double radiansPerByte);
        int Add(Folder* source, std::uint32_t index, int depth, int ring, double start, double sweep,
                std::uint64_t bytes);
        std::uint64_t Weight(Entry const& entry) const noexcept;
        std::uint64_t TotalOf(Folder const& folder) const noexcept;
        int RingsNeeded(Folder const& folder, int limit) const;
        int RingAt(double radius) const noexcept;
        void ToPolar(double x, double y, double& radius, double& angle) const noexcept;

        PieOptions options_;
        std::vector<PieSlice> items_;
        Folder* root_ = nullptr;
        std::uint64_t total_ = 0;
        int rings_ = 0;
        double centerX_ = 0.0;
        double centerY_ = 0.0;
        double radius_ = 0.0;
        double hubInner_ = 0.0;
        double hubOuter_ = 0.0;
        double ringsInner_ = 0.0;
        double ringsOuter_ = 0.0;
        double ringWidth_ = 0.0;
    };
}
