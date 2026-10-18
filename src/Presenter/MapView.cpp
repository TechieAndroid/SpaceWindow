// SpaceWindow - the map a window shows, and how it moves.
//
// Ported from MainWindowMap.cpp, MainWindowCommands.cpp, MainWindowTips.cpp
// and MainWindow.xaml.cpp (src/App), which still hold the WinUI window's copy.
#include "MapView.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

#include "Format.h"
#include "Lang.h"
#include "Text.h"

namespace sm
{
    namespace
    {
        constexpr wchar_t kSeparator[] = L"   ·   ";

        // The folder called `name` in `folder`, if there is one.
        Folder* SubfolderNamed(Folder const& folder, std::wstring_view name) noexcept
        {
            for (std::size_t i = 0; i < folder.entries.size(); ++i)
            {
                if (folder.entries[i].child && folder.Name(i) == name)
                {
                    return folder.entries[i].child.get();
                }
            }
            return nullptr;
        }

        // True if `folder` is somewhere inside `outer` (not `outer` itself).
        bool IsInside(Folder const* folder, Folder const* outer) noexcept
        {
            return folder->parent != nullptr && folder->parent->IsWithin(outer);
        }

        float Lerp(float a, float b, float t) noexcept
        {
            return a + (b - a) * t;
        }
    }

    RectF ZoomMotion::RectAt(float t) const noexcept
    {
        return { Lerp(from.left, to.left, t), Lerp(from.top, to.top, t), Lerp(from.right, to.right, t),
                 Lerp(from.bottom, to.bottom, t) };
    }

    Sector ZoomMotion::SectorAt(float t) const noexcept
    {
        return { Lerp(fromSector.start, toSector.start, t), Lerp(fromSector.sweep, toSector.sweep, t),
                 Lerp(fromSector.inner, toSector.inner, t), Lerp(fromSector.outer, toSector.outer, t) };
    }

    // ---------------------------------------------------------------- the tree

    void MapView::SetTree(std::unique_ptr<FolderTree> tree, std::vector<std::wstring> const& viewPath)
    {
        tree_ = std::move(tree);
        preview_.reset();
        selection_ = {};
        if (tree_ && !tree_->IsWholeVolume())
        {
            showFreeSpace_ = true;  // nothing to hide; keep the toggle in its default state
        }
        SetViewRoot(FindView(viewPath));
        Relayout();
    }

    std::unique_ptr<FolderTree> MapView::TakeTree() noexcept
    {
        layout_.Clear();
        pie_.Clear();
        selection_ = {};
        selectedItem_ = -1;
        hoverItem_ = -1;
        hoverChain_.clear();
        viewRoot_ = nullptr;
        return std::move(tree_);
    }

    void MapView::SetPreview(std::unique_ptr<FolderTree> preview)
    {
        preview_ = std::move(preview);
        Relayout();
    }

    void MapView::ClearPreview()
    {
        if (preview_)
        {
            preview_.reset();
            Relayout();
        }
    }

    void MapView::SetViewRoot(Folder* folder)
    {
        viewRoot_ = folder;
        viewDepth_ = 0;
        for (Folder const* f = folder; f != nullptr && f->parent != nullptr; f = f->parent)
        {
            ++viewDepth_;
        }
        history_.Arrive(ViewPath());
    }

    // The folder at `path` (names from the root) - or, where part of it is
    // gone, the deepest folder on the way there that is still in the tree.
    Folder* MapView::FindView(std::vector<std::wstring> const& path) const
    {
        Folder* view = tree_ ? tree_->Root() : nullptr;
        for (std::wstring const& name : path)
        {
            Folder* const next = view != nullptr ? SubfolderNamed(*view, name) : nullptr;
            if (next == nullptr)
            {
                break;
            }
            view = next;
        }
        return view;
    }

    bool MapView::StaysInView(ViewHistory::Path const& path) const
    {
        return FindView(path) == viewRoot_;
    }

    Folder* MapView::TrailFolder() const
    {
        std::wstring const* const name = viewRoot_ != nullptr ? history_.TrailBelow(ViewPath()) : nullptr;
        return name != nullptr ? SubfolderNamed(*viewRoot_, *name) : nullptr;
    }

    std::vector<std::wstring> MapView::ViewPath() const
    {
        std::vector<std::wstring> names;
        for (Folder const* f = viewRoot_; f != nullptr && f->parent != nullptr; f = f->parent)
        {
            names.emplace_back(f->parent->Name(f->indexInParent));
        }
        std::ranges::reverse(names);
        return names;
    }

    // ---------------------------------------------------------------- layout

    void MapView::Configure(Settings const& settings, MapGeometry const& geometry)
    {
        settings_ = settings;
        geometry_ = geometry;
        Relayout();
    }

    void MapView::SetShowFreeSpace(bool show)
    {
        showFreeSpace_ = show;
        Relayout();
    }

    void MapView::Relayout()
    {
        hoverItem_ = -1;
        hoverChain_.clear();
        layout_.Clear();
        pie_.Clear();
        // A live map of a scan running shows the whole of what it found so far.
        Folder* const view = preview_ ? preview_->Root() : viewRoot_;
        int const depth = preview_ ? 0 : viewDepth_;
        if ((!preview_ && !tree_) || view == nullptr)
        {
            selectedItem_ = -1;
            return;
        }
        if (PieMode())
        {
            PieOptions options = PieOptions::ForDensity(settings_.density, geometry_.scale);
            options.showFreeSpace = showFreeSpace_;
            options.inverted = settings_.viewMode == ViewMode::InvertedPie;
            pie_.Build(view, depth, geometry_.width, geometry_.height, options);
        }
        else
        {
            LayoutOptions options = LayoutOptions::ForDensity(settings_.density, geometry_.scale);
            options.bias = settings_.bias;
            options.showFreeSpace = showFreeSpace_;
            options.border = geometry_.border;
            options.header = geometry_.header;
            layout_.Build(view, depth, geometry_.width, geometry_.height, options);
        }
        selectedItem_ = !preview_ && selection_.folder != nullptr ? FindItem(selection_.folder, selection_.index) : -1;
    }

    std::size_t MapView::ItemCount() const noexcept
    {
        return PieMode() ? pie_.Items().size() : layout_.Items().size();
    }

    MapItem const& MapView::Item(int item) const
    {
        auto const index = static_cast<std::size_t>(item);
        if (PieMode())
        {
            return pie_.Items()[index];
        }
        return layout_.Items()[index];
    }

    int MapView::FindItem(Folder const* folder, std::uint32_t index) const noexcept
    {
        return PieMode() ? pie_.Find(folder, index) : layout_.Find(folder, index);
    }

    int MapView::FindFolderItem(Folder const* folder) const noexcept
    {
        return PieMode() ? pie_.FindFolder(folder) : layout_.FindFolder(folder);
    }

    PiePoint MapView::ItemAnchor(int item) const
    {
        if (PieMode())
        {
            return pie_.Centroid(pie_.Items()[static_cast<std::size_t>(item)]);
        }
        DisplayItem const& box = layout_.Items()[static_cast<std::size_t>(item)];
        return { box.x + box.w / 2.0, box.y + box.h / 2.0 };
    }

    int MapView::ItemAt(double x, double y) const
    {
        if (PieMode())
        {
            int const item = pie_.HitTest(x, y);
            return item >= 0 && Item(item).IsSelectable() ? item : -1;
        }
        return layout_.HitTest(static_cast<int>(x), static_cast<int>(y));
    }

    // ---------------------------------------------------------------- selection and mouse

    void MapView::Select(int item)
    {
        if (item >= 0 && std::cmp_less(item, ItemCount()) && Item(item).IsSelectable())
        {
            MapItem const& selected = Item(item);
            selection_ = { selected.source, selected.index };
        }
        else
        {
            item = -1;
            selection_ = {};
        }
        selectedItem_ = item;
    }

    Entry const* MapView::SelectedEntry() const noexcept
    {
        if (!tree_ || selection_.folder == nullptr || selection_.index >= selection_.folder->entries.size())
        {
            return nullptr;
        }
        Entry const& entry = selection_.folder->entries[selection_.index];
        return entry.IsSpecial() ? nullptr : &entry;
    }

    std::wstring MapView::SelectedPath() const
    {
        return SelectedEntry() != nullptr ? tree_->PathOf(*selection_.folder, selection_.index) : std::wstring();
    }

    // Arrow keys move to the nearest box (or slice) in that direction - the
    // map can be used without a mouse (1.4.0 had no keyboard support at all).
    void MapView::MoveSelection(Direction direction)
    {
        int const count = static_cast<int>(ItemCount());
        if (selectedItem_ < 0)
        {
            for (int i = 0; i < count; ++i)
            {
                if (Item(i).IsSelectable())
                {
                    Select(i);
                    return;
                }
            }
            return;
        }

        PiePoint const from = ItemAnchor(selectedItem_);
        int best = -1;
        double bestScore = std::numeric_limits<double>::infinity();
        for (int i = 0; i < count; ++i)
        {
            if (i == selectedItem_ || !Item(i).IsSelectable())
            {
                continue;
            }
            PiePoint const to = ItemAnchor(i);
            double const dx = to.x - from.x;
            double const dy = to.y - from.y;
            double along = 0.0;
            double across = 0.0;
            switch (direction)
            {
            case Direction::Right:
                along = dx;
                across = dy;
                break;
            case Direction::Left:
                along = -dx;
                across = dy;
                break;
            case Direction::Down:
                along = dy;
                across = dx;
                break;
            case Direction::Up:
                along = -dy;
                across = dx;
                break;
            }
            if (along <= 0.0)
            {
                continue;
            }
            double const score = along + 2.0 * std::abs(across);
            if (score < bestScore)
            {
                bestScore = score;
                best = i;
            }
        }
        if (best >= 0)
        {
            Select(best);
        }
    }

    bool MapView::Hover(double x, double y)
    {
        if (PieMode())
        {
            // Any slice gets a tip, and the slice under the mouse lifts (with
            // rollover boxes, so do the folders' slices inside it).
            hoverItem_ = pie_.HitTest(x, y);
            if (settings_.rolloverBox)
            {
                pie_.ContainersAt(x, y, chainScratch_);
            }
            else
            {
                chainScratch_.clear();
                if (hoverItem_ >= 0)
                {
                    chainScratch_.push_back(hoverItem_);
                }
            }
        }
        else
        {
            hoverItem_ = layout_.HitTest(static_cast<int>(x), static_cast<int>(y));
            chainScratch_.clear();
            if (settings_.rolloverBox)
            {
                layout_.ContainersAt(static_cast<int>(x), static_cast<int>(y), chainScratch_);
            }
        }
        if (chainScratch_ != hoverChain_)
        {
            hoverChain_.swap(chainScratch_);
            return true;
        }
        return false;
    }

    void MapView::EndHover() noexcept
    {
        hoverItem_ = -1;
        hoverChain_.clear();
    }

    std::span<int const> MapView::Highlighted() const noexcept
    {
        return hoverChain_;
    }

    // ---------------------------------------------------------------- moving

    std::optional<ZoomMotion> MapView::Motion(RectF from, RectF to) const
    {
        if (!settings_.animatedZoom)
        {
            return std::nullopt;
        }
        ZoomMotion motion;
        motion.from = from;
        motion.to = to;
        return motion;
    }

    std::optional<ZoomMotion> MapView::Motion(Sector from, Sector to) const
    {
        if (!settings_.animatedZoom)
        {
            return std::nullopt;
        }
        ZoomMotion motion;
        motion.pie = true;
        motion.fromSector = from;
        motion.toSector = to;
        return motion;
    }

    std::optional<ZoomMotion> MapView::ZoomIn(int item)
    {
        if (item < 0 || !std::cmp_less(item, ItemCount()))
        {
            return std::nullopt;
        }
        MapItem const& target = Item(item);
        Folder* folder = target.ChildFolder();
        if (!target.HasEntry() && PieMode())
        {
            folder = target.source;  // a slice of grouped small entries: show them in their own folder
        }
        if (folder == nullptr || folder == viewRoot_)
        {
            return std::nullopt;
        }
        history_.Leave(ViewPath());
        if (PieMode())
        {
            Sector const from = SliceSector(item);
            SetViewRoot(folder);
            selection_ = {};
            Relayout();
            return Motion(from, FullSector(from.start + from.sweep / 2.0f));
        }
        RectF const from = ItemRect(item);
        SetViewRoot(folder);
        selection_ = {};
        Relayout();
        return Motion(from, FullRect());
    }

    std::optional<ZoomMotion> MapView::ZoomIntoSelection()
    {
        Entry const* const entry = SelectedEntry();
        if (entry == nullptr || !entry->child)
        {
            return std::nullopt;
        }
        if (selectedItem_ >= 0)
        {
            return ZoomIn(selectedItem_);
        }
        history_.Leave(ViewPath());
        SetViewRoot(entry->child.get());  // selected, but too small to be on the map
        selection_ = {};
        Relayout();
        return std::nullopt;
    }

    std::optional<ZoomMotion> MapView::ZoomOut()
    {
        if (!tree_ || viewRoot_ == nullptr || viewRoot_->parent == nullptr)
        {
            return std::nullopt;
        }
        history_.Leave(ViewPath());
        return ShowFolder(viewRoot_->parent, {});  // selects the folder we came out of
    }

    std::optional<ZoomMotion> MapView::ZoomFull()
    {
        if (!tree_ || viewRoot_ == tree_->Root())
        {
            return std::nullopt;
        }
        history_.Leave(ViewPath());
        SetViewRoot(tree_->Root());
        selection_ = {};
        Relayout();
        return PieMode() ? Motion(FullSector(0.0f), HubSector()) : Motion(FullRect(), CenterRect());
    }

    // Alt+Down: one level down - towards the selection, or else back down the
    // way the view came up (the trail).
    std::optional<ZoomMotion> MapView::GoDown()
    {
        if (!tree_ || viewRoot_ == nullptr)
        {
            return std::nullopt;
        }
        Folder* next = nullptr;
        Selection keep;
        if (Entry const* const entry = SelectedEntry())
        {
            // The folder selected, or the one in the view that holds the selection.
            Folder* const selected = entry->child.get();
            for (Folder* f = selected != nullptr ? selected : selection_.folder; f != nullptr && f != viewRoot_;
                 f = f->parent)
            {
                if (f->parent == viewRoot_)
                {
                    next = f;
                    break;
                }
            }
            if (next != nullptr && next != selected)
            {
                keep = selection_;  // still in the view below
            }
        }
        if (next == nullptr)
        {
            next = TrailFolder();
        }
        if (next == nullptr)
        {
            return std::nullopt;
        }
        history_.Leave(ViewPath());
        return ShowFolder(next, keep);
    }

    std::optional<ZoomMotion> MapView::GoBack()
    {
        if (!tree_)
        {
            return std::nullopt;
        }
        return GoTo(history_.Back(ViewPath(), [this](ViewHistory::Path const& path) { return StaysInView(path); }));
    }

    std::optional<ZoomMotion> MapView::GoForward()
    {
        if (!tree_)
        {
            return std::nullopt;
        }
        return GoTo(history_.Forward(ViewPath(), [this](ViewHistory::Path const& path) { return StaysInView(path); }));
    }

    std::optional<ZoomMotion> MapView::GoTo(std::optional<ViewHistory::Path> const& path)
    {
        return path ? ShowFolder(FindView(*path), {}) : std::nullopt;
    }

    // Moves the view to `target`, up, down or across the tree, and selects
    // `selection` - by default the folder the view came up out of, or the
    // one the trail goes on down to (where Alt+Down leads).
    std::optional<ZoomMotion> MapView::ShowFolder(Folder* target, Selection selection)
    {
        if (target == nullptr || viewRoot_ == nullptr || target == viewRoot_)
        {
            return std::nullopt;
        }
        Folder const* const previous = viewRoot_;
        bool const pie = PieMode();
        bool const up = IsInside(previous, target);
        int const from = !up && IsInside(target, previous) ? FindFolderItem(target) : -1;  // going down, from this item
        Sector const fromSector = from >= 0 && pie ? SliceSector(from) : Sector{};
        RectF const fromRect = from >= 0 && !pie ? ItemRect(from) : RectF{};

        SetViewRoot(target);
        if (selection.folder == nullptr)
        {
            Folder const* next = TrailFolder();
            if (up)
            {
                next = previous;  // the folder the view came up out of
                while (next->parent != target)
                {
                    next = next->parent;
                }
            }
            if (next != nullptr)
            {
                selection = { target, next->indexInParent };
            }
        }
        selection_ = selection;
        Relayout();

        if (up)
        {
            // The outline closes in on where the view was.
            int const item = FindFolderItem(previous);
            if (pie)
            {
                if (item >= 0)
                {
                    Sector const to = SliceSector(item);
                    return Motion(FullSector(to.start + to.sweep / 2.0f), to);
                }
                return Motion(FullSector(0.0f), HubSector());
            }
            return Motion(FullRect(), item >= 0 ? ItemRect(item) : CenterRect());
        }
        if (from >= 0)
        {
            return pie ? Motion(fromSector, FullSector(fromSector.start + fromSector.sweep / 2.0f))
                       : Motion(fromRect, FullRect());
        }
        return std::nullopt;
    }

    Folder* MapView::RescanTarget() const noexcept
    {
        if (!tree_)
        {
            return nullptr;
        }
        if (Entry const* const entry = SelectedEntry(); entry != nullptr)
        {
            Folder* const folder = entry->child ? entry->child.get() : selection_.folder;
            if (folder != tree_->Root())
            {
                return folder;
            }
        }
        return viewRoot_ != tree_->Root() ? viewRoot_ : nullptr;
    }

    void MapView::RemoveSelected()
    {
        if (SelectedEntry() == nullptr)
        {
            return;
        }
        Folder* const folder = selection_.folder;
        std::uint32_t const index = selection_.index;
        // The files whose names go.  A hard-linked one of them is then counted
        // under another of its names - if the tree has any.
        std::vector<std::uint32_t> records;
        if (tree_->rootRecord != 0 && tree_->hardLinks)
        {
            Entry const& gone = folder->entries[index];
            records = gone.child ? FileRecords(*gone.child) : std::vector<std::uint32_t>();
            if (!gone.child && gone.record != 0)
            {
                records.push_back(gone.record);
            }
        }
        selection_ = {};
        tree_->Remove(*folder, index);
        if (!records.empty())
        {
            CountHardLinksOnce(*tree_, records, [this](Folder const& in, std::size_t at) {
                return ReadSizeOnDisk(tree_->PathOf(in, at), in.entries[at].attributes);
            });
        }
        tree_->RefreshFreeSpace();  // the space freed now shows as free (or unscanned)
        Relayout();
    }

    // ---------------------------------------------------------------- commands and text

    bool MapView::CanRun(Command command, CommandContext const& context) const noexcept
    {
        bool const open = tree_ != nullptr && !context.scanning;
        Entry const* const entry = open ? SelectedEntry() : nullptr;
        switch (command)
        {
        case Command::Open:
        case Command::Settings: return !context.scanning;
        case Command::Reload:
        case Command::FullRescan:
        case Command::GoDown:
        case Command::GoBack:
        case Command::GoForward:
        case Command::SelectNearest:
        case Command::ClearSelection:
        case Command::ContextMenu: return open;
        case Command::RescanFolder: return open && RescanTarget() != nullptr;
        case Command::ZoomFull:
        case Command::ZoomOut: return open && viewRoot_ != tree_->Root();
        case Command::ZoomIn: return entry != nullptr && entry->IsFolder();
        case Command::FreeSpace: return open && tree_->IsWholeVolume();
        case Command::Run:
        case Command::Properties:
        case Command::ShowInExplorer:
        case Command::CopyPath:
        case Command::Activate: return entry != nullptr;
        case Command::Delete: return entry != nullptr && !settings_.disableDelete;
        case Command::PieChart:
        case Command::InvertedPie:
        case Command::About: return true;  // also during a scan: its live map changes view
        }
        return false;
    }

    std::uint64_t MapView::PercentBase() const noexcept
    {
        if (!tree_)
        {
            return 0;
        }
        return tree_->IsWholeVolume() ? tree_->Volume().totalBytes : tree_->Root()->SizeTotal();
    }

    std::uint64_t MapView::ViewSize() const noexcept
    {
        if (!tree_ || viewRoot_ == nullptr)
        {
            return 0;
        }
        // At the top of a drive 1.4.0 showed the drive's capacity ("Kludge").
        return viewRoot_ == tree_->Root() && tree_->IsWholeVolume() ? tree_->Volume().totalBytes
                                                                    : viewRoot_->SizeTotal();
    }

    std::wstring MapView::StatusPath() const
    {
        if (!tree_ || viewRoot_ == nullptr)
        {
            return {};
        }
        return SelectedEntry() != nullptr ? SelectedPath() : tree_->PathOf(*viewRoot_);
    }

    std::wstring MapView::StatusInfo(Lang const& lang) const
    {
        if (!tree_ || viewRoot_ == nullptr)
        {
            return {};
        }
        std::wstring info;
        if (Entry const* const entry = SelectedEntry())
        {
            info = Join(FormatSize(entry->size, lang), L"  (", FormatPercent(entry->size, PercentBase(), lang), L")");
            if (entry->child)
            {
                info.append(kSeparator).append(lang.filesTotal).append(L" ");
                info.append(FormatCount(entry->child->fileCount, lang)).append(L"   ").append(lang.foldersTotal);
                info.append(L" ").append(FormatCount(entry->child->folderCount, lang));
            }
            else
            {
                info.append(kSeparator).append(FormatBytes(entry->logicalSize, lang));
            }
            return info;
        }
        info = Join(FormatSize(ViewSize(), lang), L" ", lang.total);
        if (tree_->IsWholeVolume())
        {
            info.append(kSeparator).append(FormatSize(tree_->Volume().freeBytes, lang)).append(L" ").append(lang.free);
        }
        info.append(kSeparator).append(lang.filesTotal).append(L" ").append(FormatCount(viewRoot_->fileCount, lang));
        info.append(L"   ").append(lang.foldersTotal).append(L" ").append(FormatCount(viewRoot_->folderCount, lang));
        return info;
    }

    std::wstring MapView::Title(Lang const& lang) const
    {
        std::wstring title;
        if (tree_ && viewRoot_ != nullptr)
        {
            if (Entry const* const entry = SelectedEntry())
            {
                title = Join(SelectedPath(), L"  -  ", FormatPercent(entry->size, PercentBase(), lang), L"  -  ",
                             FormatSize(entry->size, lang), L"  -  ");
            }
            else
            {
                title = Join(tree_->PathOf(*viewRoot_), L"  -  ", FormatSize(ViewSize(), lang), L" ", lang.total);
                if (tree_->IsWholeVolume())
                {
                    title.append(L"  -  ")
                        .append(FormatSize(tree_->Volume().freeBytes, lang))
                        .append(L" ")
                        .append(lang.free);
                }
                title.append(L"  -  ");
            }
        }
        title.append(L"SpaceWindow");
        return title;
    }

    std::optional<InfoTip> MapView::InfoTipOf(int item, Lang const& lang) const
    {
        if (!tree_ || item < 0 || !std::cmp_less(item, ItemCount()))
        {
            return std::nullopt;
        }
        MapItem const& shown = Item(item);
        std::uint32_t const flags = settings_.infoTipFlags;
        InfoTip tip;
        auto const line = [&tip](std::wstring const& text) {
            if (!text.empty())
            {
                if (!tip.details.empty())
                {
                    tip.details.push_back(L'\n');
                }
                tip.details.append(text);
            }
        };

        if (Entry const* const entry = shown.IsSelectable() ? shown.GetEntry() : nullptr)
        {
            tip.folderPath = tree_->PathOf(*shown.source);
            tip.name = shown.source->Name(shown.index);
            if (flags & TipSize)
            {
                // 1.4.0 showed only the file length; the space it takes on disk is new.
                std::wstring const onDisk = FormatSize(entry->size, lang);
                line(Join(FormatBytes(entry->logicalSize, lang), L"  (", FormatText(lang.onDisk, { onDisk }), L")"));
                if (entry->child)
                {
                    line(Join(lang.filesTotal, L" ", FormatCount(entry->child->fileCount, lang), L"   ",
                              lang.foldersTotal, L" ", FormatCount(entry->child->folderCount, lang)));
                }
                if (entry->countedElsewhere)
                {
                    line(std::wstring(lang.hardLink));
                }
            }
            if (entry->child && entry->child->unreadable)
            {
                line(std::wstring(lang.unreadableFolder));
            }
            if (flags & TipAttrib)
            {
                line(FormatAttributes(entry->attributes, lang));
            }
            if (flags & TipDate)
            {
                line(FormatFileTime(entry->lastWriteTime, lang));
            }
            if (flags & TipIcon)
            {
                tip.iconPath = tip.folderPath + tip.name;
            }
        }
        else if (PieMode())
        {
            // A slice of grouped small entries, or of free space: what it is and how much.
            PieSlice const& slice = pie_.Items()[static_cast<std::size_t>(item)];
            if (slice.IsFolded())
            {
                std::wstring const count = FormatCount(slice.folded, lang);
                tip.name = FormatText(lang.smallerItems, { count });
                tip.folderPath = tree_->PathOf(*slice.source);
            }
            else
            {
                Entry const* const special = slice.GetEntry();
                tip.name =
                    special != nullptr && special->kind == EntryKind::FreeSpace ? lang.freeSpace : lang.unscanned;
                tip.folderPath = tree_->RootPath();
            }
            if (flags & TipSize)
            {
                line(
                    Join(FormatSize(slice.bytes, lang), L"  (", FormatPercent(slice.bytes, PercentBase(), lang), L")"));
            }
        }
        else
        {
            return std::nullopt;  // treemap filler boxes and free space have no tips, as in 1.4.0
        }
        // A grouped or free-space slice has no other way to say what it is, so its name always shows.
        tip.showName = (flags & TipName) != 0 || !shown.IsSelectable();
        tip.showPath = (flags & TipPath) != 0;
        if (!tip.showName && !tip.showPath && tip.details.empty() && tip.iconPath.empty())
        {
            return std::nullopt;
        }
        return tip;
    }

    RectF MapView::FullRect() const noexcept
    {
        return { 1.0f, 1.0f, static_cast<float>(geometry_.width - 1), static_cast<float>(geometry_.height - 1) };
    }

    RectF MapView::CenterRect() const noexcept
    {
        float const x = geometry_.width / 2.0f;
        float const y = geometry_.height / 2.0f;
        return { x, y, x, y };
    }

    RectF MapView::ItemRect(int item) const
    {
        DisplayItem const& box = layout_.Items()[static_cast<std::size_t>(item)];
        return { static_cast<float>(box.x + 1), static_cast<float>(box.y + 1), static_cast<float>(box.x + box.w),
                 static_cast<float>(box.y + box.h) };
    }

    Sector MapView::SliceSector(int item) const
    {
        PieSlice const& slice = pie_.Items()[static_cast<std::size_t>(item)];
        return { static_cast<float>(slice.start), static_cast<float>(slice.sweep),
                 static_cast<float>(pie_.InnerRadius(slice.ring)), static_cast<float>(pie_.OuterRadius(slice.ring)) };
    }

    // Every ring, as one segment whose middle is at `middle` radians: a slice
    // zoomed into opens out around its own middle.
    Sector MapView::FullSector(float middle) const
    {
        constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
        return { middle - kTwoPi / 2.0f, kTwoPi, static_cast<float>(pie_.RingsInner()),
                 static_cast<float>(pie_.RingsOuter()) };
    }

    Sector MapView::HubSector() const
    {
        // The hub, or the rim of the inverted pie: the current folder itself.
        return { 0.0f, 2.0f * std::numbers::pi_v<float>, static_cast<float>(pie_.HubInner()),
                 static_cast<float>(pie_.HubOuter()) };
    }
}
