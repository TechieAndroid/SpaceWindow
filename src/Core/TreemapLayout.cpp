// SpaceWindow - treemap layout.
//
// A port of BuildFolderLayout() and SizeFolders() from FolderView.cpp of
// SpaceMonger 1.4.0 (Copyright (c) 1998-2020 Sean Werkema, MIT License).
#include "pch.h"
#include "TreemapLayout.h"

#include "FolderTree.h"

namespace sm
{
    namespace
    {
        // Minimum box sizes per density at 96 DPI, from 1.4.0.
        constexpr int kMinSizes[][2] = {
            { 96, 64 }, { 64, 48 }, { 48, 32 }, { 32, 24 }, { 24, 16 }, { 16, 12 }, { 8, 6 },
        };

        int Scaled(int value, double scale) noexcept
        {
            return std::max(1, static_cast<int>(std::lround(value * scale)));
        }
    }

    LayoutOptions LayoutOptions::ForDensity(int density, double scale)
    {
        auto const row =
            static_cast<std::size_t>(std::clamp(density + 3, 0, static_cast<int>(std::size(kMinSizes)) - 1));
        LayoutOptions options;
        options.minWidth = Scaled(kMinSizes[row][0], scale);
        options.minHeight = Scaled(kMinSizes[row][1], scale);
        options.border = Scaled(3, scale);
        options.header = std::max(options.border + 1, Scaled(12, scale));
        return options;
    }

    void TreemapLayout::Build(Folder* folder, int depth, int width, int height, LayoutOptions const& options)
    {
        options_ = options;
        items_.clear();
        if (folder == nullptr || width < 2 || height < 2)
        {
            return;
        }

        // A screenful holds at most about one box per minimum-size cell.
        std::size_t const cells = (static_cast<std::size_t>(width / std::max(1, options_.minWidth)) + 1) *
                                  (static_cast<std::size_t>(height / std::max(1, options_.minHeight)) + 1);
        items_.reserve(std::min<std::size_t>(cells * 2, std::size_t{ 1 } << 20));

        // As in 1.4.0, the outermost outline runs along the last pixel row and column.
        LayoutFolder(0, 0, width - 1, height - 1, folder, depth);
    }

    void TreemapLayout::LayoutFolder(int x, int y, int w, int h, Folder* folder, int depth)
    {
        if (folder->entries.empty())
        {
            return;
        }
        std::vector<std::uint32_t> items(folder->entries.size());
        for (std::uint32_t i = 0; i < items.size(); ++i)
        {
            items[i] = i;
        }
        SizeFolders(x, y, w, h, folder, items, depth);
    }

    std::uint64_t TreemapLayout::Weight(Folder const& folder, std::uint32_t index) const noexcept
    {
        Entry const& entry = folder.entries[index];
        return entry.IsSpecial() && !options_.showFreeSpace ? 0 : entry.size;
    }

    void TreemapLayout::SizeFolders(int x, int y, int w, int h, Folder* folder, std::span<std::uint32_t> items,
                                    int depth)
    {
        // Deal the items - sorted largest first - into whichever list is lighter.
        // List 1 grows from the front of the scratch buffer and list 2 from the
        // back; both are then copied over `items` in their original order.
        std::size_t const count = items.size();
        if (scratch_.size() < count)
        {
            scratch_.resize(count);
        }
        std::size_t count1 = 0;
        std::size_t count2 = 0;
        std::uint64_t sum1 = 0;
        std::uint64_t sum2 = 0;
        for (std::uint32_t const item : items)
        {
            std::uint64_t const weight = Weight(*folder, item);
            if (weight == 0)
            {
                continue;  // empty files take no room
            }
            if (sum1 <= sum2)
            {
                scratch_[count1++] = item;
                sum1 += weight;
            }
            else
            {
                scratch_[count - 1 - count2++] = item;
                sum2 += weight;
            }
        }
        if (sum1 + sum2 == 0)
        {
            return;
        }
        std::copy_n(scratch_.begin(), count1, items.begin());
        for (std::size_t i = 0; i < count2; ++i)
        {
            items[count1 + i] = scratch_[count - 1 - i];
        }
        std::span<std::uint32_t> const list1 = items.first(count1);
        std::span<std::uint32_t> const list2 = items.subspan(count1, count2);

        // Split the rectangle in proportion.  The bias stretches one dimension
        // before deciding which way to cut.  1.4.0 computed w * bytes in 64-bit
        // integers, which overflows for petabyte totals; a double is exact enough.
        int widthBias = 8;
        int heightBias = 8;
        if (options_.bias > 0)
        {
            widthBias += options_.bias;
        }
        else if (options_.bias < 0)
        {
            heightBias -= options_.bias;
        }
        double const fraction = static_cast<double>(sum1) / (static_cast<double>(sum1) + static_cast<double>(sum2));
        if (w * widthBias / 8 > h * heightBias / 8)
        {
            int const split = static_cast<int>(w * fraction);
            Place(x, y, split, h, folder, list1, depth);
            Place(x + split, y, w - split, h, folder, list2, depth);
        }
        else
        {
            int const split = static_cast<int>(h * fraction);
            Place(x, y, w, split, folder, list1, depth);
            Place(x, y + split, w, h - split, folder, list2, depth);
        }
    }

    void TreemapLayout::Place(int x, int y, int w, int h, Folder* folder, std::span<std::uint32_t> items, int depth)
    {
        if (items.empty())
        {
            return;
        }

        bool const roomy = w > options_.minWidth && h > options_.minHeight;
        if (items.size() > 1 && roomy)
        {
            SizeFolders(x, y, w, h, folder, items, depth);
            return;
        }
        if (!roomy)
        {
            // Too small to label: an anonymous box stands for everything here.
            Add(folder, kNoEntry, depth, x, y, w, h, 0);
            return;
        }

        std::uint32_t const index = items.front();
        Folder* const child = folder->entries[index].child.get();
        int const item = Add(folder, index, depth, x, y, w, h, child != nullptr ? std::uint32_t{ DisplayFolder } : 0u);
        int const b = options_.border;
        int const innerWidth = w - 2 * b;
        int const innerHeight = h - options_.header - b;
        // At the densest setting a box can be roomy yet shorter than its own
        // title band; 1.4.0 then laid the contents out with a negative height.
        if (child != nullptr && innerWidth > 0 && innerHeight > 0)
        {
            LayoutFolder(x + b, y + options_.header, innerWidth, innerHeight, child, depth + 1);
            items_[static_cast<std::size_t>(item)].subtreeEnd = static_cast<std::uint32_t>(items_.size());
        }
    }

    int TreemapLayout::Add(Folder* source, std::uint32_t index, int depth, int x, int y, int w, int h,
                           std::uint32_t flags)
    {
        DisplayItem item;
        item.source = source;
        item.index = index;
        item.depth = depth;
        item.flags = flags;
        if (index != kNoEntry && source->entries[index].IsSpecial())
        {
            item.depth = -1;
            item.flags |= DisplaySpecial;
        }
        item.x = x;
        item.y = y;
        item.w = w;
        item.h = h;
        item.subtreeEnd = static_cast<std::uint32_t>(items_.size() + 1);
        items_.push_back(item);
        return static_cast<int>(items_.size() - 1);
    }

    int TreemapLayout::HitTest(int px, int py) const noexcept
    {
        std::size_t i = 0;
        while (i < items_.size())
        {
            DisplayItem const& item = items_[i];
            if (!item.Contains(px, py))
            {
                i = item.subtreeEnd;  // nothing inside this box can contain the point either
                continue;
            }
            if (item.IsFolder())
            {
                bool const onFrame = px < item.x + options_.border || py < item.y + options_.header ||
                                     px > item.x + item.w - options_.border || py > item.y + item.h - options_.border;
                if (!onFrame)
                {
                    ++i;  // look among the folder's contents
                    continue;
                }
            }
            return item.IsSelectable() ? static_cast<int>(i) : -1;
        }
        return -1;
    }

    void TreemapLayout::ContainersAt(int px, int py, std::vector<int>& result) const
    {
        result.clear();
        std::size_t i = 0;
        while (i < items_.size())
        {
            DisplayItem const& item = items_[i];
            if (!item.Contains(px, py))
            {
                i = item.subtreeEnd;
                continue;
            }
            if (item.IsSelectable())
            {
                result.push_back(static_cast<int>(i));
            }
            ++i;
        }
    }

    int TreemapLayout::FindFolder(Folder const* folder) const noexcept
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

    int TreemapLayout::Find(Folder const* folder, std::uint32_t index) const noexcept
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
