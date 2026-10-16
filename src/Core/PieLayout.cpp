// SpaceWindow - pie chart layout.
#include "pch.h"
#include "PieLayout.h"

#include "FolderTree.h"

namespace sm
{
    namespace
    {
        constexpr double kTwoPi = 2.0 * std::numbers::pi;

        // Per density at 96 DPI ("too few files" .. "too many files"): the most
        // rings, and the thinnest slice drawn on its own, in pixels.  Even the
        // densest keeps slices well wider than the 2-pixel gap between them.
        constexpr int kRings[] = { 2, 3, 3, 4, 5, 6 };
        constexpr double kMinArc[] = { 28.0, 20.0, 14.0, 10.0, 7.0, 5.0 };
    }

    PieOptions PieOptions::ForDensity(int density, double scale)
    {
        auto const row = static_cast<std::size_t>(std::clamp(density + 3, 0, static_cast<int>(std::size(kRings)) - 1));
        PieOptions options;
        options.rings = kRings[row];
        options.minArc = kMinArc[row] * scale;
        options.gap = std::max(1.0, std::round(2.0 * scale));
        options.margin = 8.0 * scale;
        return options;
    }

    void PieLayout::Clear() noexcept
    {
        items_.clear();
        root_ = nullptr;
        total_ = 0;
        rings_ = 0;
        radius_ = 0.0;
        hubInner_ = 0.0;
        hubOuter_ = 0.0;
        ringsInner_ = 0.0;
        ringsOuter_ = 0.0;
        ringWidth_ = 0.0;
    }

    void PieLayout::Build(Folder* folder, int depth, int width, int height, PieOptions const& options)
    {
        Clear();
        options_ = options;
        root_ = folder;
        centerX_ = width / 2.0;
        centerY_ = height / 2.0;
        double const radius = std::min(width, height) / 2.0 - options_.margin;
        if (folder == nullptr || radius < 16.0)
        {
            return;
        }
        radius_ = radius;
        if (options_.inverted)
        {
            // The folder as a rim; its rings fill everything inside it.
            hubInner_ = radius * (1.0 - std::clamp(options_.rimFraction, 0.02, 0.5));
            hubOuter_ = radius;
            ringsInner_ = 0.0;
            ringsOuter_ = hubInner_;
        }
        else
        {
            hubInner_ = 0.0;
            hubOuter_ = radius * std::clamp(options_.hubFraction, 0.05, 0.9);
            ringsInner_ = hubOuter_;
            ringsOuter_ = radius;
        }

        total_ = TotalOf(*folder);
        if (total_ == 0)
        {
            return;  // an empty folder: just the hub (or rim)
        }

        // Only as many rings as there is something to show in, so a folder of
        // files is one wide ring rather than a thin one inside empty space.
        // The tree's depth is the first guess; folders whose contents all fold
        // into one slice need no ring of their own, so lay out again with
        // fewer, wider rings until the outermost one is in use.
        rings_ = RingsNeeded(*folder, std::max(1, options_.rings));
        items_.reserve(256);
        for (;;)
        {
            ringWidth_ = (ringsOuter_ - ringsInner_) / rings_;
            items_.clear();
            LayoutFolder(folder, depth, 0, 0.0, kTwoPi, total_);
            int used = 0;
            for (PieSlice const& slice : items_)
            {
                used = std::max(used, slice.ring + 1);
            }
            if (used >= rings_ || used == 0)
            {
                break;
            }
            rings_ = used;
        }
    }

    std::uint64_t PieLayout::Weight(Entry const& entry) const noexcept
    {
        return entry.IsSpecial() && !options_.showFreeSpace ? 0 : entry.size;
    }

    std::uint64_t PieLayout::TotalOf(Folder const& folder) const noexcept
    {
        std::uint64_t total = 0;
        for (Entry const& entry : folder.entries)
        {
            total += Weight(entry);
        }
        return total;
    }

    // One ring for the folder's entries, plus one per level of non-empty
    // sub-folders below them, up to `limit` (at most PieOptions::rings deep).
    int PieLayout::RingsNeeded(Folder const& folder, int limit) const
    {
        int deepest = 0;
        for (Entry const& entry : folder.entries)
        {
            if (deepest == limit - 1)
            {
                break;
            }
            if (entry.child && Weight(entry) > 0 && TotalOf(*entry.child) > 0)
            {
                deepest = std::max(deepest, RingsNeeded(*entry.child, limit - 1));
            }
        }
        return 1 + deepest;
    }

    void PieLayout::LayoutFolder(Folder* folder, int depth, int ring, double start, double sweep, std::uint64_t total)
    {
        double const middle = InnerRadius(ring) + ringWidth_ / 2.0;
        double angle = start;
        for (std::size_t i = 0; i < folder->entries.size(); ++i)
        {
            Entry const& entry = folder->entries[i];
            std::uint64_t const weight = Weight(entry);
            if (weight == 0)
            {
                continue;  // empty files take no room
            }
            // A double is exact enough for petabyte totals (an integer product would overflow).
            double const share = sweep * (static_cast<double>(weight) / static_cast<double>(total));
            if (share * middle < options_.minArc)
            {
                // Entries are sorted largest first, so everything from here on is thinner still.
                Fold(folder, i, depth, ring, angle, start + sweep - angle, sweep / static_cast<double>(total));
                return;
            }

            int const item = Add(folder, static_cast<std::uint32_t>(i), depth, ring, angle, share, weight);
            if (entry.child && ring + 1 < rings_)
            {
                if (std::uint64_t const childTotal = TotalOf(*entry.child); childTotal > 0)
                {
                    LayoutFolder(entry.child.get(), depth + 1, ring + 1, angle, share, childTotal);
                    items_[static_cast<std::size_t>(item)].subtreeEnd = static_cast<std::uint32_t>(items_.size());
                }
            }
            angle += share;
        }
    }

    void PieLayout::Fold(Folder* folder, std::size_t first, int depth, int ring, double start, double sweep,
                         double radiansPerByte)
    {
        // Free space and unscanned space are never grouped with files: each
        // keeps a slice of its own, however thin, so "Free Space" always shows it.
        double angle = start;
        std::uint64_t bytes = 0;
        std::uint32_t count = 0;
        std::size_t last = first;
        for (std::size_t i = first; i < folder->entries.size(); ++i)
        {
            Entry const& entry = folder->entries[i];
            std::uint64_t const weight = Weight(entry);
            if (weight == 0)
            {
                continue;
            }
            if (entry.IsSpecial())
            {
                double const share = radiansPerByte * static_cast<double>(weight);
                Add(folder, static_cast<std::uint32_t>(i), depth, ring, angle, share, weight);
                angle += share;
                continue;
            }
            bytes += weight;
            ++count;
            last = i;
        }
        double const rest = start + sweep - angle;
        if (count == 1)
        {
            // One entry gains nothing from being grouped: it keeps its own (thin) slice.
            Add(folder, static_cast<std::uint32_t>(last), depth, ring, angle, rest, bytes);
        }
        else if (count > 1)
        {
            int const item = Add(folder, kNoEntry, depth, ring, angle, rest, bytes);
            items_[static_cast<std::size_t>(item)].folded = count;
        }
    }

    int PieLayout::Add(Folder* source, std::uint32_t index, int depth, int ring, double start, double sweep,
                       std::uint64_t bytes)
    {
        PieSlice slice;
        slice.source = source;
        slice.index = index;
        slice.depth = depth;
        slice.ring = ring;
        slice.start = start;
        slice.sweep = sweep;
        slice.bytes = bytes;
        if (index != kNoEntry)
        {
            Entry const& entry = source->entries[index];
            if (entry.child)
            {
                slice.flags |= DisplayFolder;
            }
            if (entry.IsSpecial())
            {
                slice.flags |= DisplaySpecial;
                slice.depth = -1;
            }
        }
        slice.subtreeEnd = static_cast<std::uint32_t>(items_.size() + 1);
        items_.push_back(slice);
        return static_cast<int>(items_.size() - 1);
    }

    PiePoint PieLayout::Centroid(PieSlice const& slice) const noexcept
    {
        double const angle = slice.start + slice.sweep / 2.0;
        double const inner = InnerRadius(slice.ring);
        double radius = inner + ringWidth_ / 2.0;
        if (inner <= 0.0)
        {
            // A slice that comes to a point in the centre is wider further out;
            // one that goes all the way round is a disc, centred on the centre.
            radius = slice.sweep >= kTwoPi - 1e-9 ? 0.0 : 0.6 * ringWidth_;
        }
        return { centerX_ + radius * std::sin(angle), centerY_ - radius * std::cos(angle) };
    }

    // The ring at `radius` pixels from the centre (within RingsInner() .. RingsOuter()).
    int PieLayout::RingAt(double radius) const noexcept
    {
        double const across = options_.inverted ? ringsOuter_ - radius : radius - ringsInner_;  // from ring 0's edge
        return std::clamp(static_cast<int>(across / ringWidth_), 0, rings_ - 1);
    }

    void PieLayout::ToPolar(double x, double y, double& radius, double& angle) const noexcept
    {
        double const dx = x - centerX_;
        double const dy = y - centerY_;
        radius = std::hypot(dx, dy);
        angle = std::atan2(dx, -dy);  // 0 at twelve o'clock, growing clockwise (y points down)
        if (angle < 0.0)
        {
            angle += kTwoPi;
        }
    }

    bool PieLayout::HubContains(double x, double y) const noexcept
    {
        double radius = 0.0;
        double angle = 0.0;
        ToPolar(x, y, radius, angle);
        return radius_ > 0.0 && radius >= hubInner_ && radius < hubOuter_;
    }

    int PieLayout::HitTest(double x, double y) const noexcept
    {
        double radius = 0.0;
        double angle = 0.0;
        ToPolar(x, y, radius, angle);
        if (items_.empty() || radius < ringsInner_ || radius >= ringsOuter_)
        {
            return -1;  // the hub (or rim), or outside the pie
        }
        int const ring = RingAt(radius);
        std::size_t i = 0;
        while (i < items_.size())
        {
            PieSlice const& slice = items_[i];
            if (!slice.ContainsAngle(angle))
            {
                i = slice.subtreeEnd;  // nor does anything inside this slice
                continue;
            }
            if (slice.ring == ring)
            {
                return static_cast<int>(i);
            }
            ++i;  // the point is a ring deeper: look among the slice's contents
        }
        return -1;  // past a file, or past the last ring a folder's contents reach
    }

    void PieLayout::ContainersAt(double x, double y, std::vector<int>& result) const
    {
        result.clear();
        int const hit = HitTest(x, y);
        if (hit < 0)
        {
            return;
        }
        double radius = 0.0;
        double angle = 0.0;
        ToPolar(x, y, radius, angle);
        std::size_t i = 0;
        while (i < items_.size())
        {
            PieSlice const& slice = items_[i];
            if (!slice.ContainsAngle(angle))
            {
                i = slice.subtreeEnd;
                continue;
            }
            result.push_back(static_cast<int>(i));
            if (std::cmp_equal(i, hit))
            {
                return;
            }
            ++i;
        }
    }

    int PieLayout::FindFolder(Folder const* folder) const noexcept
    {
        for (std::size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i].IsFolder() && items_[i].ChildFolder() == folder)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    int PieLayout::Find(Folder const* folder, std::uint32_t index) const noexcept
    {
        for (std::size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i].source == folder && items_[i].index == index)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
}
