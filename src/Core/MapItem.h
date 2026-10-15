// SpaceWindow - an entry as drawn by one of the map views.
//
// The treemap (TreemapLayout) draws entries as boxes and the pie chart
// (PieLayout) as slices.  Both produce a flat list of items in depth-first
// order - a folder is followed by its contents - so the window can select,
// hit-test and zoom the same way in either view.
#pragma once

#include <cstdint>

namespace sm
{
    struct Folder;
    struct Entry;

    enum DisplayFlags : std::uint32_t
    {
        DisplayFolder = 1,   // a folder; its contents follow it in the item list
        DisplaySpecial = 2,  // the free-space or unscanned-space block
    };

    inline constexpr std::uint32_t kNoEntry = UINT32_MAX;

    struct MapItem
    {
        Folder* source = nullptr;        // folder that holds the entry
        std::uint32_t index = kNoEntry;  // entry within source, or kNoEntry for an item standing for small entries
        std::int32_t depth = 0;          // nesting depth (picks the rainbow colour); -1 for special blocks
        std::uint32_t flags = 0;         // DisplayFlags
        std::uint32_t subtreeEnd = 0;    // index one past this item's last descendant

        bool IsFolder() const noexcept { return (flags & DisplayFolder) != 0; }
        bool IsSpecial() const noexcept { return (flags & DisplaySpecial) != 0; }
        bool HasEntry() const noexcept { return index != kNoEntry && source != nullptr; }
        // Files and folders can be selected; free space and grouped small entries cannot.
        bool IsSelectable() const noexcept { return HasEntry() && !IsSpecial(); }
        Entry const* GetEntry() const noexcept;
        Folder* ChildFolder() const noexcept;
    };
}
