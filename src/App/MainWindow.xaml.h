// SpaceWindow - main window.
//
// Replaces CMainFrame (Mainfrm.cpp), the command handlers of CSpaceMonger
// (SpaceMonger.cpp), CFolderView (FolderView.cpp) and the dialogs CDriveDialog,
// CSettingsDialog, CAboutDialog and CFolderDialog of SpaceMonger 1.4.0
// (Copyright (c) 1998-2020 Sean Werkema, MIT License).
#pragma once

#include "MainWindow.g.h"

#include "ChromeTheme.h"
#include "MapRenderer.h"
#include "ShellOps.h"
#include "../Core/FolderTree.h"
#include "../Core/Lang.h"
#include "../Core/PieLayout.h"
#include "../Core/Settings.h"
#include "../Core/Theme.h"
#include "../Core/TreemapLayout.h"
#include "../Core/ViewHistory.h"

namespace winrt::SpaceWindow::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow() = default;

        // Runs after the XAML is loaded; the constructor cannot touch named elements yet.
        // Hiding the generated base version is how C++/WinRT expects this to be extended.
        void InitializeComponent();  // NOLINT(bugprone-derived-method-shadowing-base-method)

        // Scans a drive or folder and shows it (also used for the command line).
        winrt::fire_and_forget StartScan(std::wstring path);

        // Commands.
        void OnOpen(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnRescan(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnZoomFull(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnZoomIn(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnZoomOut(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnToggleFreeSpace(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnTogglePieChart(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnRun(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnDelete(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnProperties(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnShowInExplorer(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCopyPath(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSettings(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnAbout(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCancelScan(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnShowUnreadable(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnShortcut(Microsoft::UI::Xaml::Input::KeyboardAccelerator const& accelerator,
                        Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const& args);

        // The map.
        void OnMapPointerMoved(Windows::Foundation::IInspectable const&,
                               Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OnMapPointerExited(Windows::Foundation::IInspectable const&,
                                Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OnMapPointerPressed(Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OnMapDoubleTapped(Windows::Foundation::IInspectable const&,
                               Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        void OnMapRightTapped(Windows::Foundation::IInspectable const&,
                              Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const&);
        void OnMapKeyDown(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
        void OnMapSizeChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        void OnMapScaleChanged(Microsoft::UI::Xaml::Controls::SwapChainPanel const&, Windows::Foundation::IInspectable const&);

        // Drag a drive or folder from File Explorer onto the window.
        void OnDragOver(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);
        winrt::fire_and_forget OnDrop(Windows::Foundation::IInspectable sender, Microsoft::UI::Xaml::DragEventArgs args);

        // Dialogs.
        void OnDriveDoubleTapped(Windows::Foundation::IInspectable const&,
                                 Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        void OnDriveSelectionChanged(Windows::Foundation::IInspectable const&,
                                     Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnBrowseFolder(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnLanguagePreview(Windows::Foundation::IInspectable const&,
                               Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnThemePreview(Windows::Foundation::IInspectable const&,
                            Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnOpenThemesFolder(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnTipOptionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        // The selected entry, by identity, so it survives a new layout.
        struct Selection
        {
            sm::Folder* folder = nullptr;
            std::uint32_t index = sm::kNoEntry;
        };

        // An outline flying from the item zoomed into (or out of) to the whole map:
        // a rectangle over the treemap, a ring segment over the pie chart.
        struct ZoomAnimation
        {
            D2D1_RECT_F from{};
            D2D1_RECT_F to{};
            sm::ui::Sector fromSector{};
            sm::ui::Sector toSector{};
            bool pie = false;
            std::chrono::steady_clock::time_point start;
        };

        // Commands.
        bool Busy() const noexcept { return scanning_ || dialogOpen_; }
        winrt::fire_and_forget OpenDriveOrFolder();
        void ZoomIn(int item);
        void ZoomIntoSelection();
        void ZoomOut();
        void ZoomFull();
        void GoDown();
        void GoBack();
        void GoForward();
        void GoTo(std::optional<sm::ViewHistory::Path> const& path);
        void SetShowFreeSpace(bool show);
        void SetViewMode(sm::ViewMode mode);
        void RunSelected();
        winrt::fire_and_forget DeleteSelected();
        void ShowSelectedProperties();
        void ShowSelectedInExplorer();
        void CopySelectedPath();
        winrt::fire_and_forget ShowSettings();
        winrt::fire_and_forget ShowAbout();
        void ShowContextMenu(Windows::Foundation::Point position);

        // The map.
        void SetTree(std::unique_ptr<sm::FolderTree> tree, std::vector<std::wstring> const& viewPath);
        void SetViewRoot(sm::Folder* folder);
        sm::Folder* FindView(std::vector<std::wstring> const& path) const;
        void ShowFolder(sm::Folder* target, Selection selection);  // an empty selection: the default one
        bool StaysInView(sm::ViewHistory::Path const& path) const;
        sm::Folder* TrailFolder() const;
        std::vector<std::wstring> ViewPath() const;
        void Relayout();
        void RequestRender();
        void OnRendering(Windows::Foundation::IInspectable const&, Windows::Foundation::IInspectable const&);
        sm::ui::MapScene Scene();
        int ItemAt(Windows::Foundation::Point point) const;
        void Select(int item);
        void MoveSelection(Windows::System::VirtualKey key);
        sm::Entry const* SelectedEntry() const noexcept;
        std::wstring SelectedPath() const;
        std::uint64_t PercentBase() const noexcept;
        std::uint64_t ViewSize() const noexcept;

        // The items of whichever view is showing: treemap boxes or pie slices.
        bool PieMode() const noexcept { return settings_.viewMode == sm::ViewMode::Pie; }
        std::size_t ItemCount() const noexcept;
        sm::MapItem const& Item(int item) const;
        int FindItem(sm::Folder const* folder, std::uint32_t index) const noexcept;
        int FindFolderItem(sm::Folder const* folder) const noexcept;
        Windows::Foundation::Point ItemAnchor(int item) const;  // pixels: a box's middle, a slice's label point

        // Zoom animation.
        D2D1_RECT_F FullRect() const noexcept;
        D2D1_RECT_F CenterRect() const noexcept;
        D2D1_RECT_F ItemRect(int item) const;
        sm::ui::Sector SliceSector(int item) const;
        sm::ui::Sector FullSector(float middle) const;
        sm::ui::Sector HubSector() const;
        void StartZoomAnimation(D2D1_RECT_F const& from, D2D1_RECT_F const& to);
        void StartZoomAnimation(sm::ui::Sector const& from, sm::ui::Sector const& to);
        void AnimationFrame(sm::ui::MapOverlay& overlay, bool& running);

        // Tips.
        void HideTips();
        void RestartTipTimers();
        void ShowNameTip();
        void ShowInfoTip();
        void PlaceTip(Microsoft::UI::Xaml::FrameworkElement const& tip, double x, double y, bool centered = false);

        // Scanning.
        void Rescan();
        void FullRescan();
        sm::Folder* RescanTarget() const noexcept;
        winrt::fire_and_forget UpdateFromChanges();
        winrt::fire_and_forget RescanFolder(sm::Folder* folder);
        winrt::fire_and_forget ShowUnreadable();
        void ShowScanOverlay(std::wstring const& path, bool counts = true);
        void HideScanOverlay();
        winrt::fire_and_forget TakeSnapshot(std::shared_ptr<sm::ScanProgress> progress);
        void EndPreview();
        void OnProgressTick(Microsoft::UI::Dispatching::DispatcherQueueTimer const&,
                            Windows::Foundation::IInspectable const&);

        // Window chrome.
        void ApplyLanguage();
        void LoadThemes();
        void ApplyTheme(std::wstring id);
        void OnSystemColorsChanged();
        void UpdateCaptionButtons();
        void UpdateCommands();
        void UpdateStatus();
        void UpdateTitle();
        void UpdateAll();
        void ShowError(std::wstring const& message);
        bool IsDarkTheme();
        HWND WindowHandle();
        double DpiScale();
        void RestorePlacement();
        void SavePlacement();
        void OnClosed(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::WindowEventArgs const&);

        // Dialogs.
        Microsoft::UI::Xaml::Controls::ContentDialog MakeDialog(std::wstring_view title);
        Microsoft::UI::Xaml::Controls::ContentDialog HostDialog(Microsoft::UI::Xaml::Controls::ContentDialog& slot,
                                                                Microsoft::UI::Xaml::FrameworkElement const& content);
        void FillDriveList(std::vector<sm::shell::DriveInfo> const& drives);
        void PopulateSettings();
        void FillSettingsLists(std::wstring_view theme, int density, int fileColor, int folderColor);
        void ApplySettingsText();
        void FillShortcuts();
        void ReadSettings();
        void UpdateTipControls();
        std::wstring PickedTheme();
        void RefreshThemes();
        void ShowThemeErrors();

        sm::Settings settings_;
        sm::Lang const* lang_ = nullptr;

        // Themes: the built-in ones and the theme files found at start-up or when Settings opens.
        std::vector<sm::Theme> themes_;
        std::vector<std::wstring> themeErrors_;  // theme files that could not be used
        std::wstring themeId_;                   // the theme asked for (shown, unless high contrast is on)
        sm::Theme theme_;                        // the theme shown
        bool highContrast_ = false;
        sm::ui::ChromeTheme chrome_;

        std::unique_ptr<sm::FolderTree> tree_;
        sm::Folder* viewRoot_ = nullptr;  // the zoomed-in folder
        int viewDepth_ = 0;
        std::unique_ptr<sm::FolderTree> preview_;  // what a scan found so far: the map shows it while it runs
        sm::ViewHistory history_;                  // Back, Forward and the way back down (Alt+arrows)
        bool snapshotting_ = false;
        unsigned progressTicks_ = 0;
        bool showFreeSpace_ = true;
        std::wstring lastPath_;

        sm::TreemapLayout layout_;  // the treemap, or
        sm::PieLayout pie_;         // the pie chart, as settings_.viewMode says
        sm::ui::MapRenderer renderer_;
        Selection selection_;
        int selectedItem_ = -1;  // layout item of the selection, -1 if it is not on the map
        int hoverItem_ = -1;     // the item under the mouse (in the pie chart, also grouped entries and free space)
        std::vector<int> hoverChain_;
        std::vector<int> chainScratch_;
        POINT lastPointer_{ -1, -1 };  // pixels
        Windows::Foundation::Point lastPointerDips_{};
        std::optional<ZoomAnimation> zoom_;
        D2D1_COLOR_F accent_{ 0.0f, 0.47f, 0.84f, 1.0f };
        winrt::event_token renderingToken_{};

        Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer progressTimer_{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer nameTipTimer_{ nullptr };
        Microsoft::UI::Dispatching::DispatcherQueueTimer infoTipTimer_{ nullptr };
        Windows::UI::ViewManagement::UISettings uiSettings_;

        bool scanning_ = false;
        std::stop_source scanStop_;
        std::shared_ptr<sm::ScanProgress> scanProgress_;

        bool dialogOpen_ = false;
        bool settingsOpen_ = false;
        bool populatingSettings_ = false;
        bool driveAccepted_ = false;
        bool browseRequested_ = false;
        Microsoft::UI::Xaml::Controls::ContentDialog driveDialog_{ nullptr };
        Microsoft::UI::Xaml::Controls::ContentDialog settingsDialog_{ nullptr };
        Microsoft::UI::Xaml::Controls::ContentDialog aboutDialog_{ nullptr };
    };
}

namespace winrt::SpaceWindow::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
