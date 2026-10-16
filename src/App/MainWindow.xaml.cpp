// SpaceWindow - main window.
//
// Replaces Mainfrm.cpp, FolderView.cpp, DriveDialog.cpp, SetupDlg.cpp,
// AboutDialog.cpp and the command handlers of SpaceMonger.cpp in
// SpaceMonger 1.4.0 (Copyright (c) 1998-2020 Sean Werkema, MIT License).
//
// Bugs of 1.4.0 fixed here:
//  * The context menu's Delete item ignored "Disable Delete" (its enable test
//    was inverted), and Zoom Out / Zoom Full were greyed out whenever nothing
//    was selected instead of when the view was already at the top.
//  * OnFileDelete() itself never checked "Disable Delete".
//  * The scan pumped window messages from inside the recursion, so the UI
//    was re-entrant while the tree was half built; scanning now runs on
//    worker threads and the UI only polls its progress.
//  * File icons fetched for info tips were never destroyed (a GDI handle
//    leak per hover), and SHFILEINFO was "cleared" with swapped memset arguments.
//  * Window placement was clamped to the primary monitor only.
#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"  // NOLINT(bugprone-suspicious-include): C++/WinRT XAML pattern
#endif

#include "resource.h"
#include "../Core/Format.h"
#include "MainWindowShared.h"

namespace winrt::SpaceWindow::implementation
{
    namespace
    {
        // Sets an AppBarButton's label, access key and tooltip from an
        // original-style caption (L"Zoom &In").
        template <typename Button>
        void SetCommand(Button const& button, std::wstring_view label, std::wstring_view caption)
        {
            sm::Caption const parsed = sm::ParseCaption(caption);
            button.Label(hstring(label));
            button.AccessKey(hstring(parsed.accessKey));
            muxc::ToolTipService::SetToolTip(button, box_value(hstring(parsed.text)));
        }

        bool HighContrastOn() noexcept
        {
            HIGHCONTRASTW contrast{};
            contrast.cbSize = sizeof(contrast);
            return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                   (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
        }
    }

    template <typename Visit>
    void MainWindow::ForEachToolbarButton(Visit&& visit)
    {
        using sm::Command;
        visit(OpenButton(), Command::Open);
        visit(RescanButton(), Command::Reload);
        visit(ZoomFullButton(), Command::ZoomFull);
        visit(ZoomInButton(), Command::ZoomIn);
        visit(ZoomOutButton(), Command::ZoomOut);
        visit(FreeSpaceButton(), Command::FreeSpace);
        visit(PieChartButton(), Command::PieChart);
        visit(InvertedPieButton(), Command::InvertedPie);
        visit(RunButton(), Command::Run);
        visit(DeleteButton(), Command::Delete);
        visit(SettingsButton(), Command::Settings);
        visit(AboutButton(), Command::About);
        visit(PropertiesButton(), Command::Properties);
        visit(ExplorerButton(), Command::ShowInExplorer);
        visit(CopyPathButton(), Command::CopyPath);
    }

    // ---------------------------------------------------------------- setup

    void MainWindow::InitializeComponent()
    {
        MainWindowT::InitializeComponent();

        settings_ = sm::LoadSettings();
        lang_ = &sm::FindLanguage(settings_.language.empty() ? sm::DefaultLanguageCode()
                                                             : std::wstring_view(settings_.language));
        dispatcher_ = DispatcherQueue();

        // Windows 11 chrome: the content extends under a custom title bar, on
        // Mica (or in the theme's own colour, see ApplyTheme).
        ExtendsContentIntoTitleBar(true);
        SetTitleBar(AppTitleBar());
        // The title bar puts its own title on the window once it is loaded - after a
        // quick scan from the command line has given the window the full one.
        AppTitleBar().Loaded([weak = get_weak()](auto&&, auto&&) {
            if (auto const self = weak.get())
            {
                self->UpdateTitle();
            }
        });
        auto const appWindow = AppWindow();
        appWindow.TitleBar().PreferredHeightOption(muw::TitleBarHeightOption::Tall);
        if (HICON const icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_SPACEWINDOW)))
        {
            appWindow.SetIcon(Microsoft::UI::GetIconIdFromIcon(icon));
        }

        RestorePlacement();
        renderer_.Attach(MapPanel());
        accent_ = ToD2D(uiSettings_.GetColorValue(Windows::UI::ViewManagement::UIColorType::Accent));
        LoadThemes();
        ApplyTheme(settings_.theme);
        ApplyLanguage();

        progressTimer_ = dispatcher_.CreateTimer();
        progressTimer_.Interval(kProgressInterval);
        progressTimer_.Tick({ get_weak(), &MainWindow::OnProgressTick });
        nameTipTimer_ = dispatcher_.CreateTimer();
        nameTipTimer_.IsRepeating(false);
        nameTipTimer_.Tick([weak = get_weak()](auto&&, auto&&) {
            if (auto const self = weak.get())
            {
                self->ShowNameTip();
            }
        });
        infoTipTimer_ = dispatcher_.CreateTimer();
        infoTipTimer_.IsRepeating(false);
        infoTipTimer_.Tick([weak = get_weak()](auto&&, auto&&) {
            if (auto const self = weak.get())
            {
                self->ShowInfoTip();
            }
        });

        RootGrid().ActualThemeChanged([weak = get_weak()](auto&&, auto&&) {
            if (auto const self = weak.get())
            {
                self->OnSystemColorsChanged();
            }
        });
        // The accent colour (used for the zoom animation) and high contrast can change at any time.
        uiSettings_.ColorValuesChanged([weak = get_weak(), dispatcher = dispatcher_](auto&&, auto&&) {
            dispatcher.TryEnqueue([weak] {
                if (auto const self = weak.get())
                {
                    self->OnSystemColorsChanged();
                }
            });
        });
        Activated([weak = get_weak()](auto&&, mux::WindowActivatedEventArgs const& args) {
            auto const self = weak.get();
            if (!self)
            {
                return;
            }
            bool const active = args.WindowActivationState() != mux::WindowActivationState::Deactivated;
            if (active != self->windowActive_)
            {
                // The background animation holds still once the window has been in the background a while.
                self->windowActive_ = active;
                self->deactivatedAt_ = std::chrono::steady_clock::now();
                self->RequestRender();
            }
            // Where Windows cannot say when its animation effects are switched off (before Windows 10 2004).
            self->animationsEnabled_ = self->uiSettings_.AnimationsEnabled();
            if (active && self->settingsOpen_)
            {
                // Back in the window with Settings open - from editing a theme
                // file, say: pick up the theme files' changes.
                self->RefreshThemes();
            }
        });
        Closed({ get_weak(), &MainWindow::OnClosed });
        // The background animation pauses while the window is minimized.
        VisibilityChanged([weak = get_weak()](auto&&, mux::WindowVisibilityChangedEventArgs const& args) {
            if (auto const self = weak.get())
            {
                self->windowVisible_ = args.Visible();
                self->RequestRender();
            }
        });

        // Windows' animation effects switched off: no zoom animation, no background animation.
        animationsEnabled_ = uiSettings_.AnimationsEnabled();
        if (Windows::Foundation::Metadata::ApiInformation::IsEventPresent(L"Windows.UI.ViewManagement.UISettings",
                                                                          L"AnimationsEnabledChanged"))
        {
            animationsChanged_ =
                uiSettings_.AnimationsEnabledChanged([weak = get_weak(), dispatcher = dispatcher_](auto&&, auto&&) {
                    dispatcher.TryEnqueue([weak] {
                        if (auto const self = weak.get())
                        {
                            self->animationsEnabled_ = self->uiSettings_.AnimationsEnabled();
                            self->RequestRender();
                        }
                    });
                });
        }
        // Battery saver on: the background animation holds still.
        try
        {
            namespace power = Windows::System::Power;
            energySaver_ = power::PowerManager::EnergySaverStatus() == power::EnergySaverStatus::On;
            energySaverChanged_ = power::PowerManager::EnergySaverStatusChanged(
                [weak = get_weak(), dispatcher = dispatcher_](auto&&, auto&&) {
                    dispatcher.TryEnqueue([weak] {
                        if (auto const self = weak.get())
                        {
                            self->energySaver_ =
                                power::PowerManager::EnergySaverStatus() == power::EnergySaverStatus::On;
                            self->RequestRender();
                        }
                    });
                });
        }
        catch (winrt::hresult_error const&)  // NOLINT(bugprone-empty-catch): a PC that cannot tell plays on
        {
        }

        // The toolbar's buttons run their commands (Run asks CanRun first).
        ForEachToolbarButton([this](auto const& button, sm::Command command) {
            button.Click([weak = get_weak(), command](auto&&, auto&&) {
                if (auto const self = weak.get())
                {
                    self->Run(command);
                    self->UpdateCommands();  // a toggle the command did not take goes back
                }
            });
        });
        EmptyOpenButton().Click([weak = get_weak()](auto&&, auto&&) {
            if (auto const self = weak.get())
            {
                self->Run(sm::Command::Open);
            }
        });
        // The window's keyboard shortcuts, from the table Settings lists them
        // from; the map's own keys come from it too (OnMapKeyDown).
        for (sm::KeyBinding const& binding : sm::KeyBindings())
        {
            if (!binding.map)
            {
                muxi::KeyboardAccelerator accelerator;
                accelerator.Key(static_cast<Windows::System::VirtualKey>(binding.key));
                accelerator.Modifiers(ToVirtualKeyModifiers(binding.modifiers));
                accelerator.Invoked({ get_weak(), &MainWindow::OnShortcut });
                RootGrid().KeyboardAccelerators().Append(accelerator);
            }
        }

        UpdateAll();
        if (!themeErrors_.empty())
        {
            ThemeBar().Message(hstring(JoinLines(themeErrors_)));
            ThemeBar().IsOpen(true);
        }
    }

    HWND MainWindow::WindowHandle()
    {
        return Microsoft::UI::GetWindowFromWindowId(AppWindow().Id());
    }

    double MainWindow::DpiScale()
    {
        return GetDpiForWindow(WindowHandle()) / 96.0;
    }

    void MainWindow::RestorePlacement()
    {
        HWND const window = WindowHandle();
        double const scale = DpiScale();
        auto const presenter = AppWindow().Presenter().try_as<muw::OverlappedPresenter>();
        if (presenter)
        {
            presenter.PreferredMinimumWidth(static_cast<int32_t>(kMinimumWidth * scale));
            presenter.PreferredMinimumHeight(static_cast<int32_t>(kMinimumHeight * scale));
        }

        // Saved placements come from GetWindowPlacement (as in 1.4.0), so they
        // are restored with SetWindowPlacement - but only onto a monitor that
        // still exists; 1.4.0 only knew about the primary screen.
        sm::WindowPlacement const& saved = settings_.window;
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (settings_.savePosition && saved.IsValid() && GetWindowPlacement(window, &placement))
        {
            RECT const rect{ saved.x, saved.y, saved.x + saved.width, saved.y + saved.height };
            if (MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != nullptr)
            {
                placement.rcNormalPosition = rect;
                placement.showCmd = SW_HIDE;  // position only; Activate() shows the window
                placement.flags = 0;
                SetWindowPlacement(window, &placement);
                if (saved.maximized && presenter)
                {
                    presenter.Maximize();
                }
                return;
            }
        }

        // A comfortable default size, centred on the monitor the window opens on.
        auto const area =
            muw::DisplayArea::GetFromWindowId(AppWindow().Id(), muw::DisplayAreaFallback::Primary).WorkArea();
        int32_t const width = std::min(area.Width, static_cast<int32_t>(kDefaultWidth * scale));
        int32_t const height = std::min(area.Height, static_cast<int32_t>(kDefaultHeight * scale));
        AppWindow().MoveAndResize(
            { area.X + (area.Width - width) / 2, area.Y + (area.Height - height) / 2, width, height });
    }

    void MainWindow::SavePlacement()
    {
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (!GetWindowPlacement(WindowHandle(), &placement))
        {
            return;
        }
        RECT const& rect = placement.rcNormalPosition;
        settings_.window = { rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                             placement.showCmd == SW_SHOWMAXIMIZED || (placement.flags & WPF_RESTORETOMAXIMIZED) != 0 };
    }

    void MainWindow::OnClosed(IInspectable const&, mux::WindowEventArgs const&)
    {
        scanStop_.request_stop();
        progressTimer_.Stop();
        nameTipTimer_.Stop();
        infoTipTimer_.Stop();
        if (renderingToken_)
        {
            muxm::CompositionTarget::Rendering(renderingToken_);
            renderingToken_ = {};
        }
        if (energySaverChanged_)
        {
            Windows::System::Power::PowerManager::EnergySaverStatusChanged(energySaverChanged_);
            energySaverChanged_ = {};
        }
        SavePlacement();
        sm::SaveSettings(settings_);
    }

    // ---------------------------------------------------------------- chrome

    bool MainWindow::IsDarkTheme()
    {
        return RootGrid().ActualTheme() == mux::ElementTheme::Dark;
    }

    // The built-in themes and the theme files (in %APPDATA%\SpaceWindow\Themes, and in Themes next to the program).
    void MainWindow::LoadThemes()
    {
        themeErrors_.clear();
        themes_ = sm::LoadThemes(sm::shell::ThemeFolders(), themeErrors_);
    }

    void MainWindow::ApplyTheme(std::wstring id)
    {
        themeId_ = std::move(id);
        // High contrast overrides every theme with Windows' own colours, as "Use system setting" has.
        highContrast_ = HighContrastOn();
        theme_ = highContrast_ ? sm::BuiltInThemes().front() : sm::FindTheme(themes_, themeId_);
        bool const resourcesChanged = chrome_.Apply(theme_);

        mux::ElementTheme requested = mux::ElementTheme::Default;
        if (theme_.base == sm::ThemeBase::Light)
        {
            requested = mux::ElementTheme::Light;
        }
        else if (theme_.base == sm::ThemeBase::Dark)
        {
            requested = mux::ElementTheme::Dark;
        }
        // WinUI looks theme resources up again when an element's theme changes,
        // so new ones need a switch to the other theme and back.
        auto const setTheme = [resourcesChanged](mux::FrameworkElement const& element, mux::ElementTheme theme) {
            if (resourcesChanged)
            {
                element.RequestedTheme(element.ActualTheme() == mux::ElementTheme::Dark ? mux::ElementTheme::Light
                                                                                        : mux::ElementTheme::Dark);
            }
            element.RequestedTheme(theme);
        };
        setTheme(RootGrid(), requested);
        for (muxc::ContentDialog const& dialog : { driveDialog_, settingsDialog_, aboutDialog_ })
        {
            if (dialog)
            {
                setTheme(dialog, RootGrid().ActualTheme());
            }
        }

        // The classic themes sit on Mica; the others paint the window in their own colour.
        if (theme_.classic)
        {
            if (!SystemBackdrop())
            {
                SystemBackdrop(muxm::MicaBackdrop());
            }
            RootGrid().Background(muxm::SolidColorBrush(Microsoft::UI::Colors::Transparent()));
        }
        else
        {
            SystemBackdrop(nullptr);
            RootGrid().Background(muxm::SolidColorBrush(ToColor(theme_.window.background)));
        }
        UpdateCaptionButtons();
        renderer_.InvalidateBase();
        RequestRender();
    }

    // The light/dark theme, the accent colour or high contrast changed: refresh what depends on them.
    void MainWindow::OnSystemColorsChanged()
    {
        if (HighContrastOn() != highContrast_)
        {
            ApplyTheme(themeId_);
        }
        accent_ = ToD2D(uiSettings_.GetColorValue(Windows::UI::ViewManagement::UIColorType::Accent));
        UpdateCaptionButtons();
        renderer_.InvalidateBase();
        RequestRender();
    }

    void MainWindow::UpdateCaptionButtons()
    {
        // As in the WinUI Gallery: the caption buttons do not follow a theme change by themselves.
        auto const titleBar = AppWindow().TitleBar();
        Windows::UI::Color const clear{ 0, 0, 0, 0 };
        titleBar.ButtonBackgroundColor(clear);
        titleBar.ButtonInactiveBackgroundColor(clear);
        if (!theme_.classic)
        {
            sm::WindowColors const& colors = theme_.window;
            sm::Rgb const text = colors.text;
            titleBar.ButtonForegroundColor(ToColor(text));
            titleBar.ButtonHoverForegroundColor(ToColor(text));
            titleBar.ButtonPressedForegroundColor(ToColor(text));
            titleBar.ButtonInactiveForegroundColor(ToColor(sm::Mix(text, colors.background, 0.45)));
            titleBar.ButtonHoverBackgroundColor(Windows::UI::Color{ 24, text.r, text.g, text.b });
            titleBar.ButtonPressedBackgroundColor(Windows::UI::Color{ 48, text.r, text.g, text.b });
            return;
        }
        bool const dark = IsDarkTheme();
        Windows::UI::Color const foreground =
            dark ? Windows::UI::Color{ 0xFF, 0xFF, 0xFF, 0xFF } : Windows::UI::Color{ 0xFF, 0x00, 0x00, 0x00 };
        titleBar.ButtonForegroundColor(foreground);
        titleBar.ButtonHoverForegroundColor(foreground);
        titleBar.ButtonPressedForegroundColor(nullptr);
        titleBar.ButtonInactiveForegroundColor(nullptr);
        titleBar.ButtonHoverBackgroundColor(dark ? Windows::UI::Color{ 24, 0xFF, 0xFF, 0xFF }
                                                 : Windows::UI::Color{ 24, 0x00, 0x00, 0x00 });
        titleBar.ButtonPressedBackgroundColor(nullptr);
    }

    void MainWindow::ApplyLanguage()
    {
        sm::Lang const& lang = *lang_;
        ForEachToolbarButton([&lang](auto const& button, sm::Command command) {
            CommandText const text = TextOf(command, lang);
            SetCommand(button, text.label, text.caption);
            // Its keys, from the command table.  Zoom In's is Enter, which zooms
            // into the selected folder (and runs a selected file).
            sm::Command const keys = command == sm::Command::ZoomIn ? sm::Command::Activate : command;
            button.KeyboardAcceleratorTextOverride(hstring(sm::CommandKeys(keys, lang)));
        });

        EmptyTitle().Text(hstring(lang.emptyTitle));
        EmptyText().Text(hstring(lang.emptyText));
        EmptyOpenButton().Content(box_value(hstring(sm::ParseCaption(lang.openDrive).text)));
        ThemeBar().Title(hstring(lang.themeErrors));
        ScanTitle().Text(hstring(lang.scanning));
        ScanCancelButton().Content(box_value(hstring(lang.cancel)));

        renderer_.InvalidateBase();
        RequestRender();
        UpdateStatus();
        UpdateTitle();
    }

    void MainWindow::UpdateAll()
    {
        UpdateCommands();
        UpdateStatus();
        UpdateTitle();
    }

    void MainWindow::UpdateCommands()
    {
        ForEachToolbarButton([this](auto const& button, sm::Command command) { button.IsEnabled(CanRun(command)); });
        EmptyOpenButton().IsEnabled(CanRun(sm::Command::Open));
        FreeSpaceButton().IsChecked(showFreeSpace_);
        PieChartButton().IsChecked(settings_.viewMode == sm::ViewMode::Pie);
        InvertedPieButton().IsChecked(settings_.viewMode == sm::ViewMode::InvertedPie);
    }

    std::uint64_t MainWindow::PercentBase() const noexcept
    {
        if (!tree_)
        {
            return 0;
        }
        return tree_->IsWholeVolume() ? tree_->Volume().totalBytes : tree_->Root()->SizeTotal();
    }

    std::uint64_t MainWindow::ViewSize() const noexcept
    {
        if (!tree_ || viewRoot_ == nullptr)
        {
            return 0;
        }
        // At the top of a drive 1.4.0 showed the drive's capacity ("Kludge").
        return viewRoot_ == tree_->Root() && tree_->IsWholeVolume() ? tree_->Volume().totalBytes
                                                                    : viewRoot_->SizeTotal();
    }

    void MainWindow::UpdateStatus()
    {
        sm::Lang const& lang = *lang_;
        bool const unreadable = tree_ && tree_->unreadableFolders != 0;
        UnreadableButton().Visibility(unreadable ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        if (unreadable)
        {
            std::wstring const count = sm::FormatCount(tree_->unreadableFolders, lang);
            UnreadableText().Text(hstring(sm::FormatText(lang.unreadableFolders, { count })));
        }
        if (!tree_)
        {
            StatusPath().Text({});
            StatusInfo().Text({});
            return;
        }

        std::wstring info;
        if (sm::Entry const* const entry = SelectedEntry())
        {
            StatusPath().Text(hstring(SelectedPath()));
            info = sm::Join(sm::FormatSize(entry->size, lang), L"  (",
                            sm::FormatPercent(entry->size, PercentBase(), lang), L")");
            if (entry->child)
            {
                info.append(kSeparator).append(lang.filesTotal).append(L" ");
                info.append(sm::FormatCount(entry->child->fileCount, lang)).append(L"   ").append(lang.foldersTotal);
                info.append(L" ").append(sm::FormatCount(entry->child->folderCount, lang));
            }
            else
            {
                info.append(kSeparator).append(sm::FormatBytes(entry->logicalSize, lang));
            }
        }
        else
        {
            StatusPath().Text(hstring(tree_->PathOf(*viewRoot_)));
            info = sm::Join(sm::FormatSize(ViewSize(), lang), L" ", lang.total);
            if (tree_->IsWholeVolume())
            {
                info.append(kSeparator)
                    .append(sm::FormatSize(tree_->Volume().freeBytes, lang))
                    .append(L" ")
                    .append(lang.free);
            }
            info.append(kSeparator)
                .append(lang.filesTotal)
                .append(L" ")
                .append(sm::FormatCount(viewRoot_->fileCount, lang));
            info.append(L"   ")
                .append(lang.foldersTotal)
                .append(L" ")
                .append(sm::FormatCount(viewRoot_->folderCount, lang));
        }
        if (unreadable)
        {
            info.append(L"   ·");  // the link to the unreadable folders follows
        }
        StatusInfo().Text(hstring(info));
    }

    void MainWindow::UpdateTitle()
    {
        // The window (taskbar) title follows 1.4.0's UpdateTitleBar().
        sm::Lang const& lang = *lang_;
        std::wstring title;
        if (tree_)
        {
            if (sm::Entry const* const entry = SelectedEntry())
            {
                title = sm::Join(SelectedPath(), L"  -  ", sm::FormatPercent(entry->size, PercentBase(), lang),
                                 L"  -  ", sm::FormatSize(entry->size, lang), L"  -  ");
            }
            else
            {
                title =
                    sm::Join(tree_->PathOf(*viewRoot_), L"  -  ", sm::FormatSize(ViewSize(), lang), L" ", lang.total);
                if (tree_->IsWholeVolume())
                {
                    title.append(L"  -  ")
                        .append(sm::FormatSize(tree_->Volume().freeBytes, lang))
                        .append(L" ")
                        .append(lang.free);
                }
                title.append(L"  -  ");
            }
        }
        title.append(L"SpaceWindow");
        Title(hstring(title));
        AppTitleBar().Subtitle(tree_ ? hstring(tree_->PathOf(*viewRoot_)) : hstring());
    }

    void MainWindow::ShowError(std::wstring const& message)
    {
        ErrorBar().Message(hstring(message));
        ErrorBar().IsOpen(true);
    }
}
