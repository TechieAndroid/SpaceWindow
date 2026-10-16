// SpaceWindow - main window: commands, keyboard shortcuts, the context menu, drag and drop.
//
// Every command runs through Run, whichever way it is asked for - a toolbar
// button, a key, the context menu - and Run asks CanRun first.  CanRun also
// enables the toolbar's buttons and the menu's items, so what can be clicked
// and what a key does always agree.  The keys come from one table
// (src/Core/Commands.cpp), which Settings lists too.
#include "pch.h"
#include "MainWindowShared.h"

#include "../Core/Format.h"

namespace winrt::SpaceWindow::implementation
{
    namespace
    {
        // Windows' modifier keys as the command table writes them.
        std::uint8_t ToKeyModifiers(Windows::System::VirtualKeyModifiers modifiers) noexcept
        {
            using Windows::System::VirtualKeyModifiers;
            auto const held = [modifiers](VirtualKeyModifiers key) {
                return (modifiers & key) != VirtualKeyModifiers::None;
            };
            return static_cast<std::uint8_t>((held(VirtualKeyModifiers::Control) ? sm::kCtrl : 0) |
                                             (held(VirtualKeyModifiers::Shift) ? sm::kShift : 0) |
                                             (held(VirtualKeyModifiers::Menu) ? sm::kAlt : 0));
        }

        // The modifier keys held down now (with the key being handled).
        std::uint8_t HeldModifiers() noexcept
        {
            auto const held = [](int key) { return (GetKeyState(key) & 0x8000) != 0; };
            return static_cast<std::uint8_t>((held(VK_CONTROL) ? sm::kCtrl : 0) | (held(VK_SHIFT) ? sm::kShift : 0) |
                                             (held(VK_MENU) ? sm::kAlt : 0));
        }
    }

    Windows::System::VirtualKeyModifiers ToVirtualKeyModifiers(std::uint8_t modifiers) noexcept
    {
        using Windows::System::VirtualKeyModifiers;
        VirtualKeyModifiers result = VirtualKeyModifiers::None;
        if ((modifiers & sm::kCtrl) != 0)
        {
            result |= VirtualKeyModifiers::Control;
        }
        if ((modifiers & sm::kShift) != 0)
        {
            result |= VirtualKeyModifiers::Shift;
        }
        if ((modifiers & sm::kAlt) != 0)
        {
            result |= VirtualKeyModifiers::Menu;
        }
        return result;
    }

    CommandText TextOf(sm::Command command, sm::Lang const& lang)
    {
        using sm::Command;
        auto const text = [](std::wstring_view caption, std::wstring_view label) {
            return CommandText{ caption, label.empty() ? sm::ParseCaption(caption).text : std::wstring(label) };
        };
        switch (command)
        {
        case Command::Open: return text(lang.openDrive, lang.tbOpen);
        case Command::Reload: return text(lang.rescanDrive, lang.tbReload);
        case Command::FullRescan: return text(lang.fullRescan, {});
        case Command::RescanFolder: return text(lang.rescanFolder, {});  // "{0}" stands for the folder's name
        case Command::ZoomFull: return text(lang.zoomFull, lang.tbZoomFull);
        case Command::ZoomIn: return text(lang.zoomIn, lang.tbZoomIn);
        case Command::ZoomOut: return text(lang.zoomOut, lang.tbZoomOut);
        case Command::FreeSpace: return text(lang.showFreeSpace, lang.tbFreeSpace);
        case Command::PieChart: return text(lang.pieChart, lang.tbPieChart);
        case Command::InvertedPie: return text(lang.invertedPie, lang.tbInvertedPie);
        case Command::Run: return text(lang.run, lang.tbRun);
        case Command::Delete: return text(lang.del, lang.tbDelete);
        case Command::Settings: return text(lang.settings, lang.tbSetup);
        case Command::About: return text(lang.aboutApp, lang.tbAbout);
        case Command::Properties: return text(lang.properties, {});
        case Command::ShowInExplorer: return text(lang.showInExplorer, {});
        case Command::CopyPath: return text(lang.copyPath, {});
        case Command::GoDown:
        case Command::GoBack:
        case Command::GoForward:
        case Command::SelectNearest:
        case Command::Activate:
        case Command::ClearSelection:
        case Command::ContextMenu: break;  // keys only
        }
        return {};
    }

    bool MainWindow::CanRun(sm::Command command) const noexcept
    {
        using sm::Command;
        bool const open = tree_ != nullptr && !scanning_;
        sm::Entry const* const entry = open ? SelectedEntry() : nullptr;
        switch (command)
        {
        case Command::Open:
        case Command::Settings: return !scanning_;
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
        case Command::Delete:
            return entry != nullptr && !settings_.disableDelete;  // 1.4.0 checked it only when drawing the toolbar
        case Command::PieChart:
        case Command::InvertedPie:
        case Command::About: return true;  // also during a scan: its live map changes view
        }
        return false;
    }

    void MainWindow::Run(sm::Command command)
    {
        using sm::Command;
        if (dialogOpen_ || !CanRun(command))
        {
            return;
        }
        switch (command)
        {
        case Command::Open: OpenDriveOrFolder(); break;
        case Command::Reload: Rescan(); break;
        case Command::FullRescan: StartScan(lastPath_); break;
        case Command::RescanFolder: RescanFolder(RescanTarget()); break;
        case Command::ZoomFull: ZoomFull(); break;
        case Command::ZoomIn: ZoomIntoSelection(); break;
        case Command::ZoomOut: ZoomOut(); break;
        case Command::GoDown: GoDown(); break;
        case Command::GoBack: GoBack(); break;
        case Command::GoForward: GoForward(); break;
        case Command::FreeSpace: SetShowFreeSpace(!showFreeSpace_); break;
        case Command::PieChart:
        case Command::InvertedPie:
        {
            // Each a toggle back to the treemap.
            sm::ViewMode const mode = command == Command::PieChart ? sm::ViewMode::Pie : sm::ViewMode::InvertedPie;
            SetViewMode(settings_.viewMode == mode ? sm::ViewMode::Treemap : mode);
            break;
        }
        case Command::Run: RunSelected(); break;
        case Command::Delete: DeleteSelected(); break;
        case Command::Settings: ShowSettings(); break;
        case Command::About: ShowAbout(); break;
        case Command::Properties: sm::shell::ShowProperties(WindowHandle(), SelectedPath()); break;
        case Command::ShowInExplorer: sm::shell::ShowInExplorer(SelectedPath()); break;
        case Command::CopyPath: CopySelectedPath(); break;
        case Command::Activate:
            // As a double-click: a folder zooms in, a file runs or opens.
            if (SelectedEntry()->IsFolder())
            {
                ZoomIntoSelection();
            }
            else
            {
                RunSelected();
            }
            break;
        case Command::ClearSelection: Select(-1); break;
        case Command::ContextMenu: ShowContextMenu(SelectionMenuPosition()); break;
        case Command::SelectNearest: break;  // takes a direction: OnMapKeyDown moves the selection
        }
    }

    // The window's keyboard shortcuts (made from the command table in InitializeComponent).
    void MainWindow::OnShortcut(muxi::KeyboardAccelerator const& accelerator,
                                muxi::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        std::optional<sm::Command> const command = sm::CommandForKey(static_cast<std::uint16_t>(accelerator.Key()),
                                                                     ToKeyModifiers(accelerator.Modifiers()), false);
        args.Handled(command.has_value());
        if (command)
        {
            Run(*command);
        }
    }

    // The map's own keys, while it has the focus: arrows, Enter, Esc and the Menu key.
    void MainWindow::OnMapKeyDown(IInspectable const&, muxi::KeyRoutedEventArgs const& args)
    {
        std::optional<sm::Command> const command =
            sm::CommandForKey(static_cast<std::uint16_t>(args.Key()), HeldModifiers(), true);
        if (!command || Busy() || !CanRun(*command))
        {
            return;  // not the map's (Alt+arrows, Alt+Enter: the window's shortcuts), or nothing to do
        }
        if (*command == sm::Command::SelectNearest)
        {
            MoveSelection(args.Key());
        }
        else
        {
            Run(*command);
        }
        args.Handled(true);
    }

    // The Menu key opens the context menu at the selection, or the map's corner.
    Point MainWindow::SelectionMenuPosition() const
    {
        if (selectedItem_ < 0)
        {
            return { 8.0f, 8.0f };
        }
        Point const anchor = ItemAnchor(selectedItem_);
        float const scale = renderer_.Scale();
        return { anchor.X / scale, anchor.Y / scale };
    }

    void MainWindow::ZoomIn(int item)
    {
        if (item < 0 || !std::cmp_less(item, ItemCount()))
        {
            return;
        }
        sm::MapItem const& target = Item(item);
        sm::Folder* folder = target.ChildFolder();
        if (!target.HasEntry() && PieMode())
        {
            folder = target.source;  // a slice of grouped small entries: show them in their own folder
        }
        if (folder == nullptr || folder == viewRoot_)
        {
            return;
        }
        history_.Leave(ViewPath());
        if (PieMode())
        {
            sm::ui::Sector const from = SliceSector(item);
            SetViewRoot(folder);
            selection_ = {};
            Relayout();
            StartZoomAnimation(from, FullSector(from.start + from.sweep / 2.0f));
        }
        else
        {
            D2D1_RECT_F const from = ItemRect(item);
            SetViewRoot(folder);
            selection_ = {};
            Relayout();
            StartZoomAnimation(from, FullRect());
        }
        UpdateAll();
    }

    void MainWindow::ZoomIntoSelection()
    {
        sm::Entry const* const entry = SelectedEntry();
        if (entry == nullptr || !entry->child)
        {
            return;
        }
        if (selectedItem_ >= 0)
        {
            ZoomIn(selectedItem_);
            return;
        }
        history_.Leave(ViewPath());
        SetViewRoot(entry->child.get());  // selected, but too small to be on the map
        selection_ = {};
        Relayout();
        UpdateAll();
    }

    void MainWindow::ZoomOut()
    {
        if (!tree_ || viewRoot_ == nullptr || viewRoot_->parent == nullptr)
        {
            return;
        }
        history_.Leave(ViewPath());
        ShowFolder(viewRoot_->parent, {});  // selects the folder we came out of, so it is easy to spot on the map
    }

    void MainWindow::ZoomFull()
    {
        if (!tree_ || viewRoot_ == tree_->Root())
        {
            return;
        }
        history_.Leave(ViewPath());
        SetViewRoot(tree_->Root());
        selection_ = {};
        Relayout();
        if (PieMode())
        {
            StartZoomAnimation(FullSector(0.0f), HubSector());
        }
        else
        {
            StartZoomAnimation(FullRect(), CenterRect());
        }
        UpdateAll();
    }

    // Alt+Down: one level down - towards the selection, or else back down the
    // way the view came up (the trail).
    void MainWindow::GoDown()
    {
        if (!tree_ || viewRoot_ == nullptr)
        {
            return;
        }
        sm::Folder* next = nullptr;
        Selection keep;
        if (sm::Entry const* const entry = SelectedEntry())
        {
            // The folder selected, or the one in the view that holds the selection.
            sm::Folder* const selected = entry->child.get();
            for (sm::Folder* f = selected != nullptr ? selected : selection_.folder; f != nullptr && f != viewRoot_;
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
        if (next != nullptr)
        {
            history_.Leave(ViewPath());
            ShowFolder(next, keep);
        }
    }

    // Alt+Left and Alt+Right: back to where the view was, and forward again.
    void MainWindow::GoBack()
    {
        if (tree_)
        {
            GoTo(history_.Back(ViewPath(), [this](sm::ViewHistory::Path const& path) { return StaysInView(path); }));
        }
    }

    void MainWindow::GoForward()
    {
        if (tree_)
        {
            GoTo(history_.Forward(ViewPath(), [this](sm::ViewHistory::Path const& path) { return StaysInView(path); }));
        }
    }

    void MainWindow::GoTo(std::optional<sm::ViewHistory::Path> const& path)
    {
        if (path)
        {
            ShowFolder(FindView(*path), {});
        }
    }

    void MainWindow::SetShowFreeSpace(bool show)
    {
        showFreeSpace_ = show;
        Relayout();
        UpdateAll();
    }

    void MainWindow::SetViewMode(sm::ViewMode mode)
    {
        if (settings_.viewMode != mode)
        {
            // The selection and the zoomed-in folder carry over; only the drawing changes.
            settings_.viewMode = mode;
            zoom_.reset();
            Relayout();
        }
        UpdateAll();  // also puts the toolbar toggle right if it was clicked
    }

    void MainWindow::RunSelected()
    {
        if (SelectedEntry() != nullptr)
        {
            sm::shell::Open(WindowHandle(), SelectedPath());  // the shell reports any failure itself
        }
    }

    void MainWindow::CopySelectedPath()
    {
        Windows::ApplicationModel::DataTransfer::DataPackage package;
        package.SetText(hstring(SelectedPath()));
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
    }

    winrt::fire_and_forget MainWindow::DeleteSelected()
    {
        auto const strong = get_strong();
        sm::Entry const* const entry = SelectedEntry();
        if (entry == nullptr)
        {
            co_return;
        }
        sm::Folder* const folder = selection_.folder;
        std::uint32_t const index = selection_.index;
        std::wstring const path = tree_->PathOf(*folder, index);
        std::wstring const name(folder->Name(index));
        sm::Lang const& lang = *lang_;

        // 1.4.0 deleted without asking (FOF_NOCONFIRMATION); confirm first, and
        // make "Cancel" the default for this destructive action.
        muxc::ContentDialog dialog = MakeDialog(lang.confirmDeleteTitle);
        muxc::TextBlock message;
        message.TextWrapping(mux::TextWrapping::Wrap);
        std::wstring const size = sm::FormatSize(entry->size, lang);
        message.Text(hstring(sm::FormatText(lang.confirmDelete, { name, size })));
        dialog.Content(message);
        dialog.PrimaryButtonText(hstring(sm::ParseCaption(lang.del).text));
        dialog.CloseButtonText(hstring(lang.cancel));
        dialog.DefaultButton(muxc::ContentDialogButton::Close);

        dialogOpen_ = true;
        muxc::ContentDialogResult const answer = co_await dialog.ShowAsync();
        dialogOpen_ = false;
        if (answer != muxc::ContentDialogResult::Primary)
        {
            co_return;
        }

        switch (sm::shell::Recycle(WindowHandle(), path))
        {
        case sm::shell::DeleteResult::Deleted:
            if (settings_.autoRescan)
            {
                Rescan();
            }
            else
            {
                // The files whose names go.  A hard-linked one of them is then
                // counted under another of its names - if the tree has any.
                std::vector<std::uint32_t> records;
                if (tree_->rootRecord != 0 && tree_->hardLinks)
                {
                    sm::Entry const& gone = folder->entries[index];
                    records = gone.child ? sm::FileRecords(*gone.child) : std::vector<std::uint32_t>();
                    if (!gone.child && gone.record != 0)
                    {
                        records.push_back(gone.record);
                    }
                }
                selection_ = {};
                tree_->Remove(*folder, index);
                if (!records.empty())
                {
                    sm::CountHardLinksOnce(*tree_, records, [this](sm::Folder const& in, std::size_t at) {
                        return sm::ReadSizeOnDisk(tree_->PathOf(in, at), in.entries[at].attributes);
                    });
                }
                tree_->RefreshFreeSpace();  // the recycled bytes now show as unscanned space
                Relayout();
                UpdateAll();
            }
            break;
        case sm::shell::DeleteResult::Failed: ShowError(sm::FormatText(lang.deleteFailed, { name })); break;
        case sm::shell::DeleteResult::Cancelled: break;
        }
    }

    void MainWindow::ShowContextMenu(Point position)
    {
        using sm::Command;
        sm::Lang const& lang = *lang_;
        sm::Entry const* const entry = SelectedEntry();
        bool const folder = entry != nullptr && entry->IsFolder();

        muxc::MenuFlyout menu;
        auto const click = [weak = get_weak()](Command command) {
            return [weak, command](auto&&, auto&&) {
                if (auto const self = weak.get())
                {
                    self->Run(command);
                }
            };
        };
        auto const add = [&](Command command, wchar_t const* glyph, bool emphasized, std::wstring_view caption = {}) {
            sm::Caption const parsed = sm::ParseCaption(caption.empty() ? TextOf(command, lang).caption : caption);
            muxc::MenuFlyoutItem item;
            item.Text(hstring(parsed.text));
            item.AccessKey(hstring(parsed.accessKey));
            muxc::FontIcon icon;
            icon.Glyph(glyph);
            item.Icon(icon);
            item.IsEnabled(CanRun(command));
            if (emphasized)
            {
                // 1.4.0 made the double-click action the menu's default (bold) item.
                item.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
            }
            item.Click(click(command));
            menu.Items().Append(item);
        };
        auto const toggle = [&](Command command, bool on) {
            sm::Caption const parsed = sm::ParseCaption(TextOf(command, lang).caption);
            muxc::ToggleMenuFlyoutItem item;
            item.Text(hstring(parsed.text));
            item.AccessKey(hstring(parsed.accessKey));
            item.IsChecked(on);
            item.IsEnabled(CanRun(command));
            item.Click(click(command));
            menu.Items().Append(item);
        };

        add(Command::ZoomIn, L"\xE8A3", folder);
        add(Command::ZoomOut, L"\xE71F", false);
        add(Command::ZoomFull, L"\xE9A6", false);
        menu.Items().Append(muxc::MenuFlyoutSeparator());
        add(Command::Run, L"\xE768", entry != nullptr && !folder);
        add(Command::Delete, L"\xE74D", false);
        menu.Items().Append(muxc::MenuFlyoutSeparator());
        add(Command::Open, L"\xE838", false);
        add(Command::Reload, L"\xE72C", false);
        add(Command::FullRescan, L"\xE895", false);
        if (sm::Folder const* const target = RescanTarget(); target != nullptr && target->parent != nullptr)
        {
            // The folder's name inside a caption, where '&' marks an access key.
            std::wstring name(target->parent->Name(target->indexInParent));
            for (std::size_t at = name.find(L'&'); at != std::wstring::npos; at = name.find(L'&', at + 2))
            {
                name.insert(at, 1, L'&');
            }
            add(Command::RescanFolder, L"\xE838", false, sm::FormatText(lang.rescanFolder, { name }));
        }
        toggle(Command::FreeSpace, showFreeSpace_);
        toggle(Command::PieChart, settings_.viewMode == sm::ViewMode::Pie);
        toggle(Command::InvertedPie, settings_.viewMode == sm::ViewMode::InvertedPie);
        menu.Items().Append(muxc::MenuFlyoutSeparator());
        add(Command::Properties, L"\xE946", false);
        add(Command::ShowInExplorer, L"\xEC50", false);
        add(Command::CopyPath, L"\xE8C8", false);

        muxc::Primitives::FlyoutShowOptions options;
        options.Position(position);
        menu.ShowAt(MapFocus(), options);
    }

    // Drag and drop.

    void MainWindow::OnDragOver(IInspectable const&, mux::DragEventArgs const& args)
    {
        namespace dt = Windows::ApplicationModel::DataTransfer;
        if (!Busy() && args.DataView().Contains(dt::StandardDataFormats::StorageItems()))
        {
            args.AcceptedOperation(dt::DataPackageOperation::Link);
        }
    }

    winrt::fire_and_forget MainWindow::OnDrop(IInspectable sender, mux::DragEventArgs args)
    {
        namespace dt = Windows::ApplicationModel::DataTransfer;
        auto const strong = get_strong();
        (void)sender;
        if (Busy() || !args.DataView().Contains(dt::StandardDataFormats::StorageItems()))
        {
            co_return;
        }

        auto const deferral = args.GetDeferral();
        Windows::Foundation::Collections::IVectorView<Windows::Storage::IStorageItem> items{ nullptr };
        try
        {
            items = co_await args.DataView().GetStorageItemsAsync();
        }
        catch (winrt::hresult_error const&)  // NOLINT(bugprone-empty-catch): nothing usable was dropped
        {
        }
        deferral.Complete();
        if (!items || items.Size() == 0)
        {
            co_return;
        }

        Windows::Storage::IStorageItem const item = items.GetAt(0);
        std::wstring path(item.Path());
        if (!item.IsOfType(Windows::Storage::StorageItemTypes::Folder))
        {
            path.resize(path.find_last_of(L'\\') + 1);  // a dropped file stands for its folder
        }
        if (!path.empty())
        {
            StartScan(std::move(path));
        }
    }
}
