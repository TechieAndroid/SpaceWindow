// SpaceWindow - main window: the map - its layout, drawing, selection and zoom, the mouse and the keys.
#include "pch.h"
#include "MainWindowShared.h"

namespace winrt::SpaceWindow::implementation
{
    namespace
    {
        D2D1_RECT_F Lerp(D2D1_RECT_F const& a, D2D1_RECT_F const& b, float t) noexcept
        {
            return { a.left + (b.left - a.left) * t, a.top + (b.top - a.top) * t, a.right + (b.right - a.right) * t,
                     a.bottom + (b.bottom - a.bottom) * t };
        }

        sm::ui::Sector Lerp(sm::ui::Sector const& a, sm::ui::Sector const& b, float t) noexcept
        {
            return { a.start + (b.start - a.start) * t, a.sweep + (b.sweep - a.sweep) * t,
                     a.inner + (b.inner - a.inner) * t, a.outer + (b.outer - a.outer) * t };
        }

        // The folder called `name` in `folder`, if there is one.
        sm::Folder* SubfolderNamed(sm::Folder const& folder, std::wstring_view name) noexcept
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
        bool IsInside(sm::Folder const* folder, sm::Folder const* outer) noexcept
        {
            return folder->parent != nullptr && folder->parent->IsWithin(outer);
        }
    }

    void MainWindow::SetTree(std::unique_ptr<sm::FolderTree> tree, std::vector<std::wstring> const& viewPath)
    {
        tree_ = std::move(tree);
        lastPath_ = tree_->RootPath();
        sm::Folder* const view = FindView(viewPath);
        selection_ = {};
        if (!tree_->IsWholeVolume())
        {
            showFreeSpace_ = true;  // nothing to hide; keep the toggle in its default state
        }
        EmptyState().Visibility(mux::Visibility::Collapsed);
        SetViewRoot(view);
        Relayout();
    }

    void MainWindow::SetViewRoot(sm::Folder* folder)
    {
        viewRoot_ = folder;
        viewDepth_ = 0;
        for (sm::Folder const* f = folder; f != nullptr && f->parent != nullptr; f = f->parent)
        {
            ++viewDepth_;
        }
        history_.Arrive(ViewPath());
    }

    // The folder at `path` (names from the root) - or, where part of it is
    // gone, the deepest folder on the way there that is still in the tree.
    sm::Folder* MainWindow::FindView(std::vector<std::wstring> const& path) const
    {
        sm::Folder* view = tree_ ? tree_->Root() : nullptr;
        for (std::wstring const& name : path)
        {
            sm::Folder* const next = view != nullptr ? SubfolderNamed(*view, name) : nullptr;
            if (next == nullptr)
            {
                break;
            }
            view = next;
        }
        return view;
    }

    // True if going to `path` would leave the view where it is: a folder
    // deleted since leads to the deepest folder on the way that is still there.
    bool MainWindow::StaysInView(sm::ViewHistory::Path const& path) const
    {
        return FindView(path) == viewRoot_;
    }

    // The folder in the view that the way back down (Alt+Down) leads to, if any.
    sm::Folder* MainWindow::TrailFolder() const
    {
        std::wstring const* const name = viewRoot_ != nullptr ? history_.TrailBelow(ViewPath()) : nullptr;
        return name != nullptr ? SubfolderNamed(*viewRoot_, *name) : nullptr;
    }

    // Moves the view to `target`, up, down or across the tree, and selects
    // `selection` - by default the folder the view came up out of, or the
    // one the trail goes on down to (where Alt+Down leads).  The zoom
    // animation runs as for Zoom In or Zoom Out where the way is on the map.
    void MainWindow::ShowFolder(sm::Folder* target, Selection selection)
    {
        if (target == nullptr || viewRoot_ == nullptr || target == viewRoot_)
        {
            return;
        }
        sm::Folder const* const previous = viewRoot_;
        bool const pie = PieMode();
        bool const up = IsInside(previous, target);
        int const from = !up && IsInside(target, previous) ? FindFolderItem(target) : -1;  // going down, from this item
        sm::ui::Sector const fromSector = from >= 0 && pie ? SliceSector(from) : sm::ui::Sector{};
        D2D1_RECT_F const fromRect = from >= 0 && !pie ? ItemRect(from) : D2D1_RECT_F{};

        SetViewRoot(target);
        if (selection.folder == nullptr)
        {
            sm::Folder const* next = TrailFolder();
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
                    sm::ui::Sector const to = SliceSector(item);
                    StartZoomAnimation(FullSector(to.start + to.sweep / 2.0f), to);
                }
                else
                {
                    StartZoomAnimation(FullSector(0.0f), HubSector());
                }
            }
            else
            {
                StartZoomAnimation(FullRect(), item >= 0 ? ItemRect(item) : CenterRect());
            }
        }
        else if (from >= 0)
        {
            if (pie)
            {
                StartZoomAnimation(fromSector, FullSector(fromSector.start + fromSector.sweep / 2.0f));
            }
            else
            {
                StartZoomAnimation(fromRect, FullRect());
            }
        }
        UpdateAll();
    }

    std::vector<std::wstring> MainWindow::ViewPath() const
    {
        std::vector<std::wstring> names;
        for (sm::Folder const* f = viewRoot_; f != nullptr && f->parent != nullptr; f = f->parent)
        {
            names.emplace_back(f->parent->Name(f->indexInParent));
        }
        std::ranges::reverse(names);
        return names;
    }

    void MainWindow::Relayout()
    {
        hoverItem_ = -1;
        hoverChain_.clear();
        HideTips();
        layout_.Clear();
        pie_.Clear();
        // A live map of a scan running shows the whole of what it found so far.
        sm::Folder* const view = preview_ ? preview_->Root() : viewRoot_;
        int const depth = preview_ ? 0 : viewDepth_;
        if ((!preview_ && !tree_) || view == nullptr)
        {
            selectedItem_ = -1;
        }
        else
        {
            if (PieMode())
            {
                sm::PieOptions options = sm::PieOptions::ForDensity(settings_.density, renderer_.Scale());
                options.showFreeSpace = showFreeSpace_;
                options.inverted = settings_.viewMode == sm::ViewMode::InvertedPie;
                pie_.Build(view, depth, renderer_.PixelWidth(), renderer_.PixelHeight(), options);
            }
            else
            {
                sm::LayoutOptions options = sm::LayoutOptions::ForDensity(settings_.density, renderer_.Scale());
                options.bias = settings_.bias;
                options.showFreeSpace = showFreeSpace_;
                options.border = renderer_.BorderWidth();
                options.header = renderer_.HeaderHeight();
                layout_.Build(view, depth, renderer_.PixelWidth(), renderer_.PixelHeight(), options);
            }
            selectedItem_ =
                !preview_ && selection_.folder != nullptr ? FindItem(selection_.folder, selection_.index) : -1;
        }
        renderer_.InvalidateBase();
        RequestRender();
    }

    void MainWindow::RequestRender()
    {
        if (!renderingToken_)
        {
            renderingToken_ = muxm::CompositionTarget::Rendering({ get_weak(), &MainWindow::OnRendering });
        }
    }

    void MainWindow::OnRendering(IInspectable const&, IInspectable const&)
    {
        sm::ui::MapScene const scene = Scene();
        auto const now = std::chrono::steady_clock::now();
        // The animation behind the map plays on - unless it holds still for now.
        bool const backdrop = scene.backdrop != sm::Backdrop::None && !pausedAt_;
        if (backdrop)
        {
            // At up to 60 frames a second, however fast the display refreshes.
            if (!zoom_ && now - lastFrame_ < 15ms)
            {
                return;
            }
            lastFrame_ = now;
        }
        bool animating = false;
        sm::ui::MapOverlay overlay;
        overlay.selected = selectedItem_;
        if (settings_.rolloverBox || PieMode())
        {
            overlay.highlighted = hoverChain_;
        }
        AnimationFrame(overlay, animating);
        animating = animating || backdrop;
        overlay.accent = theme_.classic ? accent_ : ToD2D(ToColor(theme_.window.accent));

        bool presented = true;
        try
        {
            presented = renderer_.Render(scene, overlay);
        }
        catch (winrt::hresult_error const& error)
        {
            ShowError(std::wstring(error.message()));
        }
        if (presented && !animating && renderingToken_)
        {
            muxm::CompositionTarget::Rendering(renderingToken_);
            renderingToken_ = {};
        }
    }

    // The background animation to play now: the one picked (or being tried
    // in Settings), where the map leaves room for it - around a pie chart, or
    // behind the welcome screen - while the window can be seen, unless
    // Windows' animation effects are off.
    sm::Backdrop MainWindow::ShownBackdrop() const
    {
        sm::Backdrop const picked = backdropPreview_.value_or(settings_.backdrop);
        bool const room = PieMode() || (!tree_ && !preview_);
        if (picked == sm::Backdrop::None || !room || !windowVisible_ || !animationsEnabled_)
        {
            return sm::Backdrop::None;
        }
        return picked;
    }

    // The background animation holds its last frame while Windows saves
    // energy (battery saver), and once the window has been in the background
    // a while - it is left on screen often enough, behind other windows.
    bool MainWindow::BackdropPaused(std::chrono::steady_clock::time_point now) const noexcept
    {
        constexpr auto kInactive = 1min;
        return energySaver_ || (!windowActive_ && now - deactivatedAt_ >= kInactive);
    }

    // The background animation's clock, in seconds: stopped while it holds still.
    double MainWindow::BackdropTime(std::chrono::steady_clock::time_point now)
    {
        if (BackdropPaused(now))
        {
            if (!pausedAt_)
            {
                pausedAt_ = now;
            }
        }
        else if (pausedAt_)
        {
            backdropStart_ += now - *pausedAt_;  // it carries on from where it stopped
            pausedAt_.reset();
        }
        return std::chrono::duration<double>(pausedAt_.value_or(now) - backdropStart_).count();
    }

    sm::ui::MapScene MainWindow::Scene()
    {
        sm::ui::MapScene scene;
        scene.tree = preview_ ? preview_.get() : tree_.get();
        if (PieMode())
        {
            scene.pie = &pie_;
        }
        else
        {
            scene.layout = &layout_;
        }
        scene.settings = &settings_;
        scene.lang = lang_;
        scene.theme = &theme_;
        scene.darkTheme = IsDarkTheme();
        scene.colors = sm::ThemeMapColors(theme_, scene.darkTheme);
        if (!theme_.classic)
        {
            scene.background = theme_.window.background;
        }
        scene.backdrop = ShownBackdrop();
        scene.time = BackdropTime(std::chrono::steady_clock::now());
        return scene;
    }

    std::size_t MainWindow::ItemCount() const noexcept
    {
        return PieMode() ? pie_.Items().size() : layout_.Items().size();
    }

    sm::MapItem const& MainWindow::Item(int item) const
    {
        auto const index = static_cast<std::size_t>(item);
        if (PieMode())
        {
            return pie_.Items()[index];
        }
        return layout_.Items()[index];
    }

    int MainWindow::FindItem(sm::Folder const* folder, std::uint32_t index) const noexcept
    {
        return PieMode() ? pie_.Find(folder, index) : layout_.Find(folder, index);
    }

    int MainWindow::FindFolderItem(sm::Folder const* folder) const noexcept
    {
        return PieMode() ? pie_.FindFolder(folder) : layout_.FindFolder(folder);
    }

    Point MainWindow::ItemAnchor(int item) const
    {
        if (PieMode())
        {
            sm::PiePoint const point = pie_.Centroid(pie_.Items()[static_cast<std::size_t>(item)]);
            return { static_cast<float>(point.x), static_cast<float>(point.y) };
        }
        sm::DisplayItem const& box = layout_.Items()[static_cast<std::size_t>(item)];
        return { static_cast<float>(box.x + box.w / 2.0), static_cast<float>(box.y + box.h / 2.0) };
    }

    int MainWindow::ItemAt(Point point) const
    {
        float const scale = renderer_.Scale();
        if (PieMode())
        {
            int const item = pie_.HitTest(point.X * scale, point.Y * scale);
            return item >= 0 && Item(item).IsSelectable() ? item : -1;
        }
        return layout_.HitTest(static_cast<int>(point.X * scale), static_cast<int>(point.Y * scale));
    }

    void MainWindow::Select(int item)
    {
        if (item >= 0 && std::cmp_less(item, ItemCount()) && Item(item).IsSelectable())
        {
            sm::MapItem const& selected = Item(item);
            selection_ = { selected.source, selected.index };
        }
        else
        {
            item = -1;
            selection_ = {};
        }
        selectedItem_ = item;
        RequestRender();
        UpdateAll();
    }

    sm::Entry const* MainWindow::SelectedEntry() const noexcept
    {
        if (!tree_ || selection_.folder == nullptr || selection_.index >= selection_.folder->entries.size())
        {
            return nullptr;
        }
        sm::Entry const& entry = selection_.folder->entries[selection_.index];
        return entry.IsSpecial() ? nullptr : &entry;
    }

    std::wstring MainWindow::SelectedPath() const
    {
        return SelectedEntry() != nullptr ? tree_->PathOf(*selection_.folder, selection_.index) : std::wstring();
    }

    // Arrow keys move to the nearest box (or slice) in that direction - the
    // map can be used without a mouse (1.4.0 had no keyboard support at all).
    void MainWindow::MoveSelection(Windows::System::VirtualKey key)
    {
        using Windows::System::VirtualKey;
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

        Point const from = ItemAnchor(selectedItem_);
        int best = -1;
        double bestScore = std::numeric_limits<double>::infinity();
        for (int i = 0; i < count; ++i)
        {
            if (i == selectedItem_ || !Item(i).IsSelectable())
            {
                continue;
            }
            Point const to = ItemAnchor(i);
            double const dx = static_cast<double>(to.X) - from.X;
            double const dy = static_cast<double>(to.Y) - from.Y;
            double along = 0.0;
            double across = 0.0;
            switch (key)
            {
            case VirtualKey::Right:
                along = dx;
                across = dy;
                break;
            case VirtualKey::Left:
                along = -dx;
                across = dy;
                break;
            case VirtualKey::Down:
                along = dy;
                across = dx;
                break;
            case VirtualKey::Up:
                along = -dy;
                across = dx;
                break;
            default: return;
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

    D2D1_RECT_F MainWindow::FullRect() const noexcept
    {
        return D2D1::RectF(1.0f, 1.0f, static_cast<float>(renderer_.PixelWidth() - 1),
                           static_cast<float>(renderer_.PixelHeight() - 1));
    }

    D2D1_RECT_F MainWindow::CenterRect() const noexcept
    {
        float const x = renderer_.PixelWidth() / 2.0f;
        float const y = renderer_.PixelHeight() / 2.0f;
        return D2D1::RectF(x, y, x, y);
    }

    D2D1_RECT_F MainWindow::ItemRect(int item) const
    {
        sm::DisplayItem const& box = layout_.Items()[static_cast<std::size_t>(item)];
        return D2D1::RectF(static_cast<float>(box.x + 1), static_cast<float>(box.y + 1),
                           static_cast<float>(box.x + box.w), static_cast<float>(box.y + box.h));
    }

    sm::ui::Sector MainWindow::SliceSector(int item) const
    {
        sm::PieSlice const& slice = pie_.Items()[static_cast<std::size_t>(item)];
        return { static_cast<float>(slice.start), static_cast<float>(slice.sweep),
                 static_cast<float>(pie_.InnerRadius(slice.ring)), static_cast<float>(pie_.OuterRadius(slice.ring)) };
    }

    // Every ring, as one segment whose middle is at `middle` radians: a slice
    // zoomed into opens out around its own middle.
    sm::ui::Sector MainWindow::FullSector(float middle) const
    {
        constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
        return { middle - kTwoPi / 2.0f, kTwoPi, static_cast<float>(pie_.RingsInner()),
                 static_cast<float>(pie_.RingsOuter()) };
    }

    sm::ui::Sector MainWindow::HubSector() const
    {
        // The hub, or the rim of the inverted pie: the current folder itself.
        return { 0.0f, 2.0f * std::numbers::pi_v<float>, static_cast<float>(pie_.HubInner()),
                 static_cast<float>(pie_.HubOuter()) };
    }

    void MainWindow::StartZoomAnimation(D2D1_RECT_F const& from, D2D1_RECT_F const& to)
    {
        // 1.4.0 blocked the UI for 400 ms drawing XOR rectangles; this runs on the
        // compositor's frame clock and honours Windows' "Animation effects" setting.
        if (!settings_.animatedZoom || !animationsEnabled_)
        {
            return;
        }
        zoom_ = ZoomAnimation{ from, to, {}, {}, false, std::chrono::steady_clock::now() };
        RequestRender();
    }

    void MainWindow::StartZoomAnimation(sm::ui::Sector const& from, sm::ui::Sector const& to)
    {
        if (!settings_.animatedZoom || !animationsEnabled_)
        {
            return;
        }
        zoom_ = ZoomAnimation{ {}, {}, from, to, true, std::chrono::steady_clock::now() };
        RequestRender();
    }

    void MainWindow::AnimationFrame(sm::ui::MapOverlay& overlay, bool& running)
    {
        running = false;
        if (!zoom_)
        {
            return;
        }
        double const t = std::chrono::duration<double>(std::chrono::steady_clock::now() - zoom_->start) / kZoomDuration;
        if (t >= 1.0)
        {
            zoom_.reset();
            return;
        }
        running = true;
        auto const eased = static_cast<float>(1.0 - std::pow(1.0 - t, 3.0));  // ease-out
        if (zoom_->pie)
        {
            overlay.zoomSector = Lerp(zoom_->fromSector, zoom_->toSector, eased);
        }
        else
        {
            overlay.zoomOutline = Lerp(zoom_->from, zoom_->to, eased);
        }
    }

    void MainWindow::OnMapSizeChanged(IInspectable const&, mux::SizeChangedEventArgs const&)
    {
        auto const panel = MapPanel();
        if (renderer_.SetSize(panel.ActualWidth(), panel.ActualHeight(), panel.CompositionScaleX(),
                              panel.CompositionScaleY()))
        {
            Relayout();
        }
    }

    void MainWindow::OnMapScaleChanged(muxc::SwapChainPanel const& panel, IInspectable const&)
    {
        if (renderer_.SetSize(panel.ActualWidth(), panel.ActualHeight(), panel.CompositionScaleX(),
                              panel.CompositionScaleY()))
        {
            Relayout();
        }
    }

    void MainWindow::OnMapPointerMoved(IInspectable const&, muxi::PointerRoutedEventArgs const& args)
    {
        if (!tree_ || scanning_)
        {
            return;
        }
        Point const point = args.GetCurrentPoint(MapPanel()).Position();
        float const scale = renderer_.Scale();
        POINT const pixel{ static_cast<LONG>(point.X * scale), static_cast<LONG>(point.Y * scale) };
        if (pixel.x == lastPointer_.x && pixel.y == lastPointer_.y)
        {
            return;
        }
        lastPointer_ = pixel;
        lastPointerDips_ = point;

        if (PieMode())
        {
            // Any slice gets a tip, and the slice under the mouse lifts (with
            // rollover boxes, so do the folders' slices inside it).
            hoverItem_ = pie_.HitTest(pixel.x, pixel.y);
            if (settings_.rolloverBox)
            {
                pie_.ContainersAt(pixel.x, pixel.y, chainScratch_);
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
            hoverItem_ = layout_.HitTest(pixel.x, pixel.y);
            if (settings_.rolloverBox)
            {
                layout_.ContainersAt(pixel.x, pixel.y, chainScratch_);
            }
        }
        if ((PieMode() || settings_.rolloverBox) && chainScratch_ != hoverChain_)
        {
            hoverChain_.swap(chainScratch_);
            RequestRender();
        }

        // As with 1.4.0's tips, any movement hides them and restarts their delay.
        HideTips();
        if (hoverItem_ >= 0)
        {
            RestartTipTimers();
        }
    }

    void MainWindow::OnMapPointerExited(IInspectable const&, muxi::PointerRoutedEventArgs const&)
    {
        lastPointer_ = { -1, -1 };
        hoverItem_ = -1;
        HideTips();
        if (!hoverChain_.empty())
        {
            hoverChain_.clear();
            RequestRender();
        }
    }

    void MainWindow::OnMapPointerPressed(IInspectable const&, muxi::PointerRoutedEventArgs const& args)
    {
        if (!tree_ || scanning_)
        {
            return;
        }
        HideTips();
        Select(ItemAt(args.GetCurrentPoint(MapPanel()).Position()));
        // A click gives the map the keyboard (arrow keys, Enter, Delete ...).
        // Focus set while the press is being routed does not stick - the map
        // loses it once WinUI has finished with the press - so focus it after.
        dispatcher_.TryEnqueue([weak = get_weak()] {
            if (auto const self = weak.get())
            {
                self->MapFocus().Focus(mux::FocusState::Pointer);
            }
        });
    }

    void MainWindow::OnMapDoubleTapped(IInspectable const&, muxi::DoubleTappedRoutedEventArgs const& args)
    {
        if (!tree_ || Busy())
        {
            return;
        }
        Point const position = args.GetPosition(MapPanel());
        if (PieMode())
        {
            float const scale = renderer_.Scale();
            double const x = position.X * scale;
            double const y = position.Y * scale;
            int const hit = pie_.HitTest(x, y);
            if (pie_.HubContains(x, y) || (hit >= 0 && pie_.Items()[static_cast<std::size_t>(hit)].IsFolded()))
            {
                // The hub is the current folder: double-clicking it goes up a level.  A
                // slice of grouped small entries zooms into their folder to show them.
                if (hit >= 0)
                {
                    ZoomIn(hit);
                }
                else
                {
                    ZoomOut();
                }
                args.Handled(true);
                return;
            }
        }

        // As in 1.4.0: double-clicking a folder zooms in, a file runs or opens.
        int const item = ItemAt(position);
        Select(item);
        if (item < 0)
        {
            return;
        }
        if (Item(item).IsFolder())
        {
            ZoomIn(item);
        }
        else
        {
            RunSelected();
        }
        args.Handled(true);
    }

    void MainWindow::OnMapRightTapped(IInspectable const&, muxi::RightTappedRoutedEventArgs const& args)
    {
        if (!tree_ || Busy())
        {
            return;
        }
        Select(ItemAt(args.GetPosition(MapPanel())));
        ShowContextMenu(args.GetPosition(MapFocus()));
        args.Handled(true);
    }
}
