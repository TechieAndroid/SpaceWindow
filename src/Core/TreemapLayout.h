// SpaceWindow - treemap layout.
//
// A port of BuildFolderLayout() and SizeFolders() from FolderView.cpp of
// SpaceMonger 1.4.0 (Copyright (c) 1998-2020 Sean Werkema, MIT License).
// The algorithm - greedily deal the items, largest first, into two lists of
// equal weight, split the rectangle in proportion, recurse - is unchanged,
// so maps look exactly like SpaceMonger's.  What changed:
//  * Coordinates are 32-bit (1.4.0 used 16-bit shorts) and the split is
//    computed without the 64-bit overflow of width * bytes.
//  * The item lists are partitioned in place in one reusable buffer
//    instead of two new[]'d arrays per call (which 1.4.0 freed with a
//    mismatched delete).
//  * Display items live in one contiguous vector rather than a linked list
//    of separately allocated nodes, and each folder records where its
//    sub-tree ends, so hit-testing skips whole sub-trees.
//  * Border and header sizes are parameters, so the map scales with DPI.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "MapItem.h"

namespace sm
{
    struct LayoutOptions
    {
        int minWidth = 32;   // smallest box that gets subdivided or labelled (1.4.0's hmin)
        int minHeight = 24;  // (1.4.0's vmin)
        int bias = 0;        // -20 favours horizontal strips .. +20 vertical strips
        bool showFreeSpace = true;
        int border = 3;   // inset of a folder's contents from its outline
        int header = 12;  // distance from a folder's top to its contents (title band)

        // The minimum box size for a density setting at a given scale, from
        // the 1.4.0 table (density -3 .. 2 maps to rows 0 .. 5).
        static LayoutOptions ForDensity(int density, double scale);
    };

    // A box; an entry of kNoEntry is an unlabelled filler box standing for
    // entries too small to draw.
    struct DisplayItem : MapItem
    {
        std::int32_t x = 0;  // The box covers the pixels [x, x + w] x [y, y + h]:
        std::int32_t y = 0;  // neighbours share their outline, exactly as in 1.4.0.
        std::int32_t w = 0;
        std::int32_t h = 0;

        // Strictly inside the outline (1.4.0's test: the shared outline belongs to nobody).
        bool Contains(int px, int py) const noexcept { return px > x && py > y && px < x + w && py < y + h; }
    };

    class TreemapLayout
    {
    public:
        // Lays out `folder` in a width x height pixel area.  `depth` is the
        // folder's depth in the whole tree, so colours stay put while zooming.
        void Build(Folder* folder, int depth, int width, int height, LayoutOptions const& options);
        void Clear() noexcept { items_.clear(); }

        std::span<DisplayItem const> Items() const noexcept { return items_; }
        LayoutOptions const& Options() const noexcept { return options_; }

        // The selectable item at a point: a file, or a folder whose outline or
        // title band is under the point.  Like 1.4.0, filler boxes and the
        // free-space block cannot be selected.  Returns -1 for none.
        int HitTest(int px, int py) const noexcept;

        // Every labelled box containing the point, outermost first (for the
        // rollover highlight).
        void ContainersAt(int px, int py, std::vector<int>& result) const;

        // The item that shows `folder`'s own entry (its box in the parent), or -1.
        int FindFolder(Folder const* folder) const noexcept;

        // The item that shows entry `index` of `folder`, or -1.
        int Find(Folder const* folder, std::uint32_t index) const noexcept;

    private:
        void LayoutFolder(int x, int y, int w, int h, Folder* folder, int depth);
        void SizeFolders(int x, int y, int w, int h, Folder* folder, std::span<std::uint32_t> items, int depth);
        void Place(int x, int y, int w, int h, Folder* folder, std::span<std::uint32_t> items, int depth);
        int Add(Folder* source, std::uint32_t index, int depth, int x, int y, int w, int h, std::uint32_t flags);
        std::uint64_t Weight(Folder const& folder, std::uint32_t index) const noexcept;

        LayoutOptions options_;
        std::vector<DisplayItem> items_;
        std::vector<std::uint32_t> scratch_;  // partition buffer shared by every SizeFolders call
    };
}
