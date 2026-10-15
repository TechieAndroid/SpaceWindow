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
#include "../Core/ChangeJournal.h"
#include "../Core/FolderWatch.h"
#include "../Core/Format.h"
#include "../Core/Palette.h"

namespace winrt::SpaceWindow::implementation
{
    using namespace std::chrono_literals;
    using Windows::Foundation::IInspectable;
    using Windows::Foundation::Point;
    namespace mux = Microsoft::UI::Xaml;
    namespace muxc = Microsoft::UI::Xaml::Controls;
    namespace muxi = Microsoft::UI::Xaml::Input;
    namespace muxm = Microsoft::UI::Xaml::Media;
    namespace muw = Microsoft::UI::Windowing;

    namespace
    {
        constexpr std::chrono::milliseconds kZoomDuration = 280ms;
        constexpr std::chrono::milliseconds kProgressInterval = 100ms;
        constexpr std::chrono::milliseconds kQuietUpdate = 400ms;  // a quicker update shows no progress overlay
        constexpr unsigned kLiveMapTicks = 5;                      // progress ticks between two live maps of a scan
        constexpr std::size_t kLiveMapEntries = 30'000;            // more than a map can show anyway
        constexpr double kDefaultWidth = 1100.0;  // DIPs
        constexpr double kDefaultHeight = 740.0;
        constexpr double kMinimumWidth = 520.0;
        constexpr double kMinimumHeight = 400.0;
        constexpr wchar_t kVersion[] = L"1.0.0";
        constexpr wchar_t kSeparator[] = L"   ·   ";

        // Sets an AppBarButton's label, access key and tooltip from an
        // original-style caption (L"Zoom &In").
        template <typename Command>
        void SetCommand(Command const& command, std::wstring_view label, std::wstring_view caption)
        {
            sm::Caption const parsed = sm::ParseCaption(caption);
            command.Label(hstring(label));
            command.AccessKey(hstring(parsed.accessKey));
            muxc::ToolTipService::SetToolTip(command, box_value(hstring(parsed.text)));
        }

        Windows::UI::Color ToColor(sm::Rgb color) noexcept
        {
            return { 0xFF, color.r, color.g, color.b };
        }

        D2D1_COLOR_F ToD2D(Windows::UI::Color color) noexcept
        {
            return { color.R / 255.0f, color.G / 255.0f, color.B / 255.0f, color.A / 255.0f };
        }

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

        bool Checked(muxc::CheckBox const& box)
        {
            return unbox_value_or<bool>(box.IsChecked(), false);
        }

        int Delay(muxc::NumberBox const& box, int fallback)
        {
            double const value = box.Value();
            return std::isnan(value) ? fallback : static_cast<int>(std::lround(value));
        }

        bool SamePath(std::wstring const& a, std::wstring const& b) noexcept
        {
            return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()),
                                        TRUE) == CSTR_EQUAL;
        }

        bool AltDown() noexcept
        {
            return (GetKeyState(VK_MENU) & 0x8000) != 0;
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

        // True if `folder` is somewhere inside `outer`.
        bool IsInside(sm::Folder const* folder, sm::Folder const* outer) noexcept
        {
            for (sm::Folder const* f = folder->parent; f != nullptr; f = f->parent)
            {
                if (f == outer)
                {
                    return true;
                }
            }
            return false;
        }

        // Takes an element out of its panel so a dialog can host it.
        mux::FrameworkElement Detach(mux::FrameworkElement const& element)
        {
            if (auto const panel = element.Parent().try_as<muxc::Panel>())
            {
                uint32_t index = 0;
                if (panel.Children().IndexOf(element, index))
                {
                    panel.Children().RemoveAt(index);
                }
            }
            return element;
        }

        muxm::ImageSource ToImageSource(sm::shell::IconPixels const& icon)
        {
            if (icon.bgra.empty())
            {
                return nullptr;
            }
            muxm::Imaging::WriteableBitmap bitmap(icon.width, icon.height);
            auto const buffer = bitmap.PixelBuffer();
            std::memcpy(buffer.data(), icon.bgra.data(), std::min<std::size_t>(buffer.Capacity(), icon.bgra.size()));
            bitmap.Invalidate();
            return bitmap;
        }

        // Concatenates strings, views and literals.  The parts bind by reference
        // (no string_view over a temporary can outlive it).
        template <typename... Parts>
        std::wstring Join(Parts const&... parts)
        {
            std::wstring result;
            (result.append(parts), ...);
            return result;
        }

        bool HighContrastOn() noexcept
        {
            HIGHCONTRASTW contrast{};
            contrast.cbSize = sizeof(contrast);
            return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                   (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
        }

        std::wstring JoinLines(std::vector<std::wstring> const& lines)
        {
            std::wstring text;
            for (std::wstring const& line : lines)
            {
                if (!text.empty())
                {
                    text.push_back(L'\n');
                }
                text.append(line);
            }
            return text;
        }
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
        // Back in the window with Settings open - from editing a theme file,
        // say: pick up the theme files' changes.
        Activated([weak = get_weak()](auto&&, mux::WindowActivatedEventArgs const& args) {
            auto const self = weak.get();
            if (self && self->settingsOpen_ && args.WindowActivationState() != mux::WindowActivationState::Deactivated)
            {
                self->RefreshThemes();
            }
        });
        Closed({ get_weak(), &MainWindow::OnClosed });

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
        AppWindow().MoveAndResize({ area.X + (area.Width - width) / 2, area.Y + (area.Height - height) / 2, width, height });
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
        Windows::UI::Color const foreground = dark ? Windows::UI::Color{ 0xFF, 0xFF, 0xFF, 0xFF }
                                                   : Windows::UI::Color{ 0xFF, 0x00, 0x00, 0x00 };
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
        SetCommand(OpenButton(), lang.tbOpen, lang.openDrive);
        SetCommand(RescanButton(), lang.tbReload, lang.rescanDrive);
        SetCommand(ZoomFullButton(), lang.tbZoomFull, lang.zoomFull);
        SetCommand(ZoomInButton(), lang.tbZoomIn, lang.zoomIn);
        SetCommand(ZoomOutButton(), lang.tbZoomOut, lang.zoomOut);
        SetCommand(FreeSpaceButton(), lang.tbFreeSpace, lang.showFreeSpace);
        SetCommand(PieChartButton(), lang.tbPieChart, lang.pieChart);
        SetCommand(RunButton(), lang.tbRun, lang.run);
        SetCommand(DeleteButton(), lang.tbDelete, lang.del);
        SetCommand(SettingsButton(), lang.tbSetup, lang.settings);
        SetCommand(AboutButton(), lang.tbAbout, lang.aboutApp);
        SetCommand(PropertiesButton(), sm::ParseCaption(lang.properties).text, lang.properties);
        SetCommand(ExplorerButton(), sm::ParseCaption(lang.showInExplorer).text, lang.showInExplorer);
        SetCommand(CopyPathButton(), sm::ParseCaption(lang.copyPath).text, lang.copyPath);

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
        bool const open = tree_ != nullptr && !scanning_;
        sm::Entry const* const entry = open ? SelectedEntry() : nullptr;
        bool const zoomed = open && viewRoot_ != tree_->Root();
        OpenButton().IsEnabled(!scanning_);
        EmptyOpenButton().IsEnabled(!scanning_);
        RescanButton().IsEnabled(open);
        ZoomFullButton().IsEnabled(zoomed);
        ZoomOutButton().IsEnabled(zoomed);
        ZoomInButton().IsEnabled(entry != nullptr && entry->IsFolder());
        FreeSpaceButton().IsEnabled(open && tree_->IsWholeVolume());
        FreeSpaceButton().IsChecked(showFreeSpace_);
        PieChartButton().IsChecked(PieMode());
        RunButton().IsEnabled(entry != nullptr);
        DeleteButton().IsEnabled(entry != nullptr && !settings_.disableDelete);
        PropertiesButton().IsEnabled(entry != nullptr);
        ExplorerButton().IsEnabled(entry != nullptr);
        CopyPathButton().IsEnabled(entry != nullptr);
        SettingsButton().IsEnabled(!scanning_);
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
        return viewRoot_ == tree_->Root() && tree_->IsWholeVolume() ? tree_->Volume().totalBytes : viewRoot_->SizeTotal();
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
            info = Join(sm::FormatSize(entry->size, lang), L"  (", sm::FormatPercent(entry->size, PercentBase(), lang),
                          L")");
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
            info = Join(sm::FormatSize(ViewSize(), lang), L" ", lang.total);
            if (tree_->IsWholeVolume())
            {
                info.append(kSeparator).append(sm::FormatSize(tree_->Volume().freeBytes, lang)).append(L" ").append(lang.free);
            }
            info.append(kSeparator).append(lang.filesTotal).append(L" ").append(sm::FormatCount(viewRoot_->fileCount, lang));
            info.append(L"   ").append(lang.foldersTotal).append(L" ").append(sm::FormatCount(viewRoot_->folderCount, lang));
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
                title = Join(SelectedPath(), L"  -  ", sm::FormatPercent(entry->size, PercentBase(), lang), L"  -  ",
                               sm::FormatSize(entry->size, lang), L"  -  ");
            }
            else
            {
                title = Join(tree_->PathOf(*viewRoot_), L"  -  ", sm::FormatSize(ViewSize(), lang), L" ", lang.total);
                if (tree_->IsWholeVolume())
                {
                    title.append(L"  -  ").append(sm::FormatSize(tree_->Volume().freeBytes, lang)).append(L" ").append(lang.free);
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

    // ---------------------------------------------------------------- scanning

    winrt::fire_and_forget MainWindow::StartScan(std::wstring path)
    {
        auto const strong = get_strong();
        if (scanning_)
        {
            co_return;
        }

        // Rescanning the same drive keeps the view zoomed where it was (1.4.0 zoomed all the way out).
        bool const again = tree_ && SamePath(sm::NormalizeScanPath(path), tree_->RootPath());
        std::vector<std::wstring> viewPath;
        if (again)
        {
            viewPath = ViewPath();
        }

        scanning_ = true;
        scanStop_ = std::stop_source{};
        auto const progress = std::make_shared<sm::ScanProgress>();
        scanProgress_ = progress;
        HideTips();
        ErrorBar().IsOpen(false);
        ShowScanOverlay(path);
        UpdateCommands();
        progressTimer_.Start();

        std::stop_token const stop = scanStop_.get_token();
        sm::ScanResult result;
        bool failed = false;
        co_await winrt::resume_background();
        try
        {
            sm::ScanOptions options;
            options.watch = true;  // reloads then list again only the folders that changed
            result = sm::ScanFolder(path, *progress, stop, options);
        }
        catch (...)
        {
            failed = true;  // e.g. out of memory
        }
        try
        {
            co_await wil::resume_foreground(dispatcher_);
        }
        catch (winrt::hresult_error const&)
        {
            co_return;  // the window has closed
        }

        progressTimer_.Stop();
        scanning_ = false;
        scanProgress_.reset();
        HideScanOverlay();
        if (!failed && result.status == sm::ScanStatus::Completed)
        {
            preview_.reset();  // SetTree draws the map anew
            if (!again)
            {
                history_.Clear();  // where the view went on another drive means nothing on this one
            }
            SetTree(std::move(result.tree), viewPath);
        }
        else
        {
            EndPreview();  // back to the tree there was, if any
            if (failed || result.status == sm::ScanStatus::NotFound)
            {
                ShowError(sm::FormatText(lang_->scanFailed, { path }));
            }
        }
        UpdateAll();
        MapFocus().Focus(mux::FocusState::Programmatic);
    }

    // Brings the tree up to date: from the drive's change journal when it was
    // read from the master file table, from what its watch saw change when it
    // was listed folder by folder, otherwise by scanning again.
    void MainWindow::Rescan()
    {
        if (!tree_ || Busy())
        {
            return;
        }
        if (tree_->journal || tree_->watch)
        {
            UpdateFromChanges();
        }
        else
        {
            StartScan(lastPath_);
        }
    }

    // Scans everything again, even where a quicker update would do.
    void MainWindow::FullRescan()
    {
        if (tree_ && !Busy())
        {
            StartScan(lastPath_);
        }
    }

    // The folder that can be scanned again on its own: the one selected (or
    // holding the file selected), or else the one zoomed into.
    sm::Folder* MainWindow::RescanTarget() const noexcept
    {
        if (!tree_)
        {
            return nullptr;
        }
        if (sm::Entry const* const entry = SelectedEntry(); entry != nullptr)
        {
            sm::Folder* const folder = entry->child ? entry->child.get() : selection_.folder;
            if (folder != tree_->Root())
            {
                return folder;
            }
        }
        return viewRoot_ != tree_->Root() ? viewRoot_ : nullptr;
    }

    // Only what changed since the tree was read is read again and patched into
    // it: a moment, where scanning the drive takes seconds.  The files the
    // journal names are read from the master file table; the folders the
    // watch saw change are listed again.  When neither can tell (the journal
    // was reset, or too much changed at once), the drive is scanned again.
    winrt::fire_and_forget MainWindow::UpdateFromChanges()
    {
        auto const strong = get_strong();
        std::wstring const path = lastPath_;
        sm::FolderTree const* const tree = tree_.get();  // only read until the update is applied, here
        std::optional<sm::JournalPosition> const journal = tree_->journal;
        sm::FolderWatch::Changes changes;
        if (!journal)
        {
            changes = tree_->watch->Take();
            if (!changes.complete)
            {
                StartScan(path);
                co_return;
            }
        }
        scanning_ = true;
        scanStop_ = std::stop_source{};
        HideTips();
        ErrorBar().IsOpen(false);
        UpdateCommands();
        // A quick update does without the progress overlay, which would only flash.
        auto const overlay = dispatcher_.CreateTimer();
        overlay.IsRepeating(false);
        overlay.Interval(kQuietUpdate);
        overlay.Tick([weak = get_weak()](auto&&, auto&&) {
            if (auto const self = weak.get(); self && self->scanning_ && !self->scanProgress_)  // still this update
            {
                self->ShowScanOverlay(self->lastPath_, false);
            }
        });
        overlay.Start();

        std::stop_token const stop = scanStop_.get_token();
        enum class Outcome
        {
            Apply,
            Scan,
            Cancelled,
        };
        Outcome outcome = Outcome::Scan;
        sm::JournalRead read;
        std::optional<sm::FolderReload> reload;
        co_await winrt::resume_background();
        try
        {
            if (journal)
            {
                // Large files may have grown unseen while held open (ChangeJournal.h): read them again too.
                std::vector<std::uint32_t> const large = sm::LargeFiles(*tree->Root());
                read = sm::ReadJournalChanges(*journal, stop, 0, 0, large);
                outcome = read.status == sm::JournalReadStatus::Ready       ? Outcome::Apply
                          : read.status == sm::JournalReadStatus::Cancelled ? Outcome::Cancelled
                                                                            : Outcome::Scan;
            }
            else
            {
                sm::ScanProgress progress;  // counts what is found in folders new to the tree
                reload = sm::PrepareReload(*tree, changes.folders, changes.files, progress, stop);
                outcome = reload ? Outcome::Apply : Outcome::Cancelled;
            }
        }
        catch (...)
        {
            outcome = Outcome::Scan;  // e.g. out of memory: scan instead
        }
        try
        {
            co_await wil::resume_foreground(dispatcher_);
        }
        catch (winrt::hresult_error const&)
        {
            co_return;  // the window has closed
        }

        overlay.Stop();
        scanning_ = false;
        HideScanOverlay();
        bool scan = outcome == Outcome::Scan;
        if (outcome == Outcome::Apply)
        {
            // Folders the view or the selection point to may go: both are found
            // again by name, as after a scan.
            std::vector<std::wstring> const viewPath = ViewPath();
            std::unique_ptr<sm::FolderTree> updated = std::move(tree_);
            try
            {
                if (journal)
                {
                    sm::ApplyJournalChanges(*updated, read.changes);
                }
                else if (reload)
                {
                    sm::ApplyReload(*updated, std::move(*reload));
                }
            }
            catch (...)
            {
                scan = true;  // half applied: the totals cannot be trusted
            }
            SetTree(std::move(updated), viewPath);
        }
        else if (outcome == Outcome::Cancelled && !journal)
        {
            tree_->watch.reset();  // the changes taken from it are not applied: the next rescan is a full one
        }
        UpdateAll();
        MapFocus().Focus(mux::FocusState::Programmatic);
        if (scan)
        {
            StartScan(path);
        }
    }

    // Scans one folder again, everything below it - its place in the tree,
    // and everything else, stays as it is.
    winrt::fire_and_forget MainWindow::RescanFolder(sm::Folder* folder)
    {
        auto const strong = get_strong();
        if (!tree_ || folder == nullptr || Busy())
        {
            co_return;
        }
        std::wstring const path = tree_->PathOf(*folder);
        sm::FolderTree const* const tree = tree_.get();  // only read until the new contents go in, here
        scanning_ = true;
        scanStop_ = std::stop_source{};
        auto const progress = std::make_shared<sm::ScanProgress>();
        scanProgress_ = progress;
        HideTips();
        ErrorBar().IsOpen(false);
        ShowScanOverlay(path);
        UpdateCommands();
        progressTimer_.Start();

        std::stop_token const stop = scanStop_.get_token();
        std::unique_ptr<sm::Folder> fresh;
        bool failed = false;
        co_await winrt::resume_background();
        try
        {
            fresh = sm::ScanAgain(*tree, *folder, *progress, stop);
        }
        catch (...)
        {
            failed = true;  // e.g. out of memory
        }
        try
        {
            co_await wil::resume_foreground(dispatcher_);
        }
        catch (winrt::hresult_error const&)
        {
            co_return;  // the window has closed
        }

        progressTimer_.Stop();
        scanning_ = false;
        scanProgress_.reset();
        HideScanOverlay();
        bool scan = false;
        if (failed)
        {
            ShowError(sm::FormatText(lang_->scanFailed, { path }));
        }
        else if (fresh)
        {
            std::vector<std::wstring> const viewPath = ViewPath();
            std::unique_ptr<sm::FolderTree> updated = std::move(tree_);
            try
            {
                sm::ReplaceContents(*updated, *folder, std::move(fresh));
            }
            catch (...)
            {
                scan = true;  // half replaced: the totals cannot be trusted
            }
            SetTree(std::move(updated), viewPath);
        }
        UpdateAll();
        MapFocus().Focus(mux::FocusState::Programmatic);
        if (scan)
        {
            StartScan(lastPath_);
        }
    }

    void MainWindow::ShowScanOverlay(std::wstring const& path, bool counts)
    {
        sm::Lang const& lang = *lang_;
        ScanPath().Text(hstring(path));
        ScanFiles().Text(hstring(Join(lang.filesFound, L" 0")));
        ScanFolders().Text(hstring(Join(lang.foldersFound, L" 0")));
        mux::Visibility const shown = counts ? mux::Visibility::Visible : mux::Visibility::Collapsed;
        ScanFiles().Visibility(shown);
        ScanFolders().Visibility(shown);
        ScanProgressBar().IsIndeterminate(true);
        ScanProgressBar().Value(0);
        EmptyState().Visibility(mux::Visibility::Collapsed);
        ScanOverlay().Visibility(mux::Visibility::Visible);
        ScanCancelButton().Focus(mux::FocusState::Programmatic);
    }

    void MainWindow::HideScanOverlay()
    {
        ScanOverlay().Visibility(mux::Visibility::Collapsed);
        // Back to dimming the map, the card in the middle, for the next scan.
        ScanSmoke().Visibility(mux::Visibility::Visible);
        ScanCard().VerticalAlignment(mux::VerticalAlignment::Center);
        ScanCard().Margin({});
        EmptyState().Visibility(tree_ ? mux::Visibility::Collapsed : mux::Visibility::Visible);
    }

    // A live map of what the scan found so far: the folders listed, those
    // finished with their totals.  Taken off the UI thread; drawn in place of
    // the map while the scan runs.
    winrt::fire_and_forget MainWindow::TakeSnapshot(std::shared_ptr<sm::ScanProgress> progress)
    {
        auto const strong = get_strong();
        snapshotting_ = true;
        std::unique_ptr<sm::FolderTree> snapshot;
        co_await winrt::resume_background();
        try
        {
            snapshot = progress->Snapshot(kLiveMapEntries);
        }
        catch (...)  // NOLINT(bugprone-empty-catch): no live map this time (out of memory)
        {
        }
        try
        {
            co_await wil::resume_foreground(dispatcher_);
        }
        catch (winrt::hresult_error const&)
        {
            co_return;  // the window has closed
        }
        snapshotting_ = false;
        if (!snapshot || !scanning_ || scanProgress_ != progress)
        {
            co_return;  // nothing listed yet, or that scan is over
        }
        if (!preview_)
        {
            // The map shows through; the card makes room at the bottom.
            ScanSmoke().Visibility(mux::Visibility::Collapsed);
            ScanCard().VerticalAlignment(mux::VerticalAlignment::Bottom);
            ScanCard().Margin({ 0, 0, 0, 24 });
        }
        preview_ = std::move(snapshot);
        Relayout();
    }

    // Back to the map of the tree, if a live map was shown.
    void MainWindow::EndPreview()
    {
        if (preview_)
        {
            preview_.reset();
            Relayout();
        }
    }

    void MainWindow::OnProgressTick(Microsoft::UI::Dispatching::DispatcherQueueTimer const&, IInspectable const&)
    {
        std::shared_ptr<sm::ScanProgress> const progress = scanProgress_;
        if (!progress)
        {
            return;
        }
        sm::Lang const& lang = *lang_;
        ScanFiles().Text(hstring(Join(lang.filesFound, L" ", sm::FormatCount(progress->files.load(std::memory_order_relaxed), lang))));
        ScanFolders().Text(hstring(Join(lang.foldersFound, L" ", sm::FormatCount(progress->folders.load(std::memory_order_relaxed), lang))));
        ScanPath().Text(hstring(progress->CurrentPath()));

        // Like 1.4.0's bar: bytes found so far against the drive's used space.
        std::uint64_t const expected = progress->expectedBytes.load(std::memory_order_relaxed);
        if (expected != 0)
        {
            ScanProgressBar().IsIndeterminate(false);
            ScanProgressBar().Value(sm::PerMille(progress->bytes.load(std::memory_order_relaxed), expected));
        }

        if (++progressTicks_ % kLiveMapTicks == 0 && !snapshotting_)
        {
            TakeSnapshot(progress);
        }
    }

    void MainWindow::OnCancelScan(IInspectable const&, mux::RoutedEventArgs const&)
    {
        scanStop_.request_stop();
    }

    // ---------------------------------------------------------------- the map

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
            selectedItem_ = !preview_ && selection_.folder != nullptr ? FindItem(selection_.folder, selection_.index) : -1;
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
        bool animating = false;
        sm::ui::MapOverlay overlay;
        overlay.selected = selectedItem_;
        if (settings_.rolloverBox || PieMode())
        {
            overlay.highlighted = hoverChain_;
        }
        AnimationFrame(overlay, animating);
        overlay.accent = theme_.classic ? accent_ : ToD2D(ToColor(theme_.window.accent));

        bool presented = true;
        try
        {
            presented = renderer_.Render(Scene(), overlay);
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
            case VirtualKey::Right: along = dx; across = dy; break;
            case VirtualKey::Left: along = -dx; across = dy; break;
            case VirtualKey::Down: along = dy; across = dx; break;
            case VirtualKey::Up: along = -dy; across = dx; break;
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
        return D2D1::RectF(static_cast<float>(box.x + 1), static_cast<float>(box.y + 1), static_cast<float>(box.x + box.w),
                           static_cast<float>(box.y + box.h));
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
        return { middle - kTwoPi / 2.0f, kTwoPi, static_cast<float>(pie_.HubRadius()), static_cast<float>(pie_.Radius()) };
    }

    sm::ui::Sector MainWindow::HubSector() const
    {
        return { 0.0f, 2.0f * std::numbers::pi_v<float>, 0.0f, static_cast<float>(pie_.HubRadius()) };
    }

    void MainWindow::StartZoomAnimation(D2D1_RECT_F const& from, D2D1_RECT_F const& to)
    {
        // 1.4.0 blocked the UI for 400 ms drawing XOR rectangles; this runs on the
        // compositor's frame clock and honours Windows' "Animation effects" setting.
        if (!settings_.animatedZoom || !uiSettings_.AnimationsEnabled())
        {
            return;
        }
        zoom_ = ZoomAnimation{ from, to, {}, {}, false, std::chrono::steady_clock::now() };
        RequestRender();
    }

    void MainWindow::StartZoomAnimation(sm::ui::Sector const& from, sm::ui::Sector const& to)
    {
        if (!settings_.animatedZoom || !uiSettings_.AnimationsEnabled())
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

    void MainWindow::OnMapKeyDown(IInspectable const&, muxi::KeyRoutedEventArgs const& args)
    {
        using Windows::System::VirtualKey;
        if (!tree_ || Busy())
        {
            return;
        }
        switch (args.Key())
        {
        case VirtualKey::Enter:
            if (AltDown())
            {
                return;  // Alt+Enter belongs to Properties
            }
            if (sm::Entry const* const entry = SelectedEntry())
            {
                if (entry->IsFolder())
                {
                    ZoomIntoSelection();
                }
                else
                {
                    RunSelected();
                }
                args.Handled(true);
            }
            break;
        case VirtualKey::Escape:
            Select(-1);
            args.Handled(true);
            break;
        case VirtualKey::Left:
        case VirtualKey::Right:
        case VirtualKey::Up:
        case VirtualKey::Down:
            if (AltDown())
            {
                return;  // Alt+arrows go back, forward, up and down (OnShortcut)
            }
            MoveSelection(args.Key());
            args.Handled(true);
            break;
        case VirtualKey::Application:
        {
            Point position{ 8.0f, 8.0f };
            if (selectedItem_ >= 0)
            {
                Point const anchor = ItemAnchor(selectedItem_);
                float const scale = renderer_.Scale();
                position = { anchor.X / scale, anchor.Y / scale };
            }
            ShowContextMenu(position);
            args.Handled(true);
            break;
        }
        default:
            break;
        }
    }

    // ---------------------------------------------------------------- tips

    void MainWindow::HideTips()
    {
        if (nameTipTimer_)
        {
            nameTipTimer_.Stop();
            infoTipTimer_.Stop();
        }
        NameTip().Visibility(mux::Visibility::Collapsed);
        InfoTip().Visibility(mux::Visibility::Collapsed);
    }

    void MainWindow::RestartTipTimers()
    {
        if (settings_.showNameTips)
        {
            nameTipTimer_.Interval(std::chrono::milliseconds(std::max(1, settings_.nameTipDelay)));
            nameTipTimer_.Start();
        }
        if (settings_.showInfoTips)
        {
            infoTipTimer_.Interval(std::chrono::milliseconds(std::max(1, settings_.infoTipDelay)));
            infoTipTimer_.Start();
        }
    }

    void MainWindow::PlaceTip(mux::FrameworkElement const& tip, double x, double y, bool centered)
    {
        tip.Visibility(mux::Visibility::Visible);
        constexpr float unbounded = std::numeric_limits<float>::infinity();
        tip.Measure({ unbounded, unbounded });
        auto const size = tip.DesiredSize();
        if (centered)
        {
            x -= size.Width / 2.0;
            y -= size.Height / 2.0;
        }
        double const maxX = std::max(0.0, MapArea().ActualWidth() - size.Width);
        double const maxY = std::max(0.0, MapArea().ActualHeight() - size.Height);
        muxc::Canvas::SetLeft(tip, std::clamp(x, 0.0, maxX));
        muxc::Canvas::SetTop(tip, std::clamp(y, 0.0, maxY));
    }

    // A name that does not fit its box pops up over the box (1.4.0's "name
    // tips"); in the pie chart, over the middle of the slice.
    void MainWindow::ShowNameTip()
    {
        if (!tree_ || hoverItem_ < 0 || !std::cmp_less(hoverItem_, ItemCount()) || !Item(hoverItem_).IsSelectable())
        {
            return;
        }
        sm::ui::MapScene const scene = Scene();
        bool const selected = hoverItem_ == selectedItem_;
        bool const highlighted = std::ranges::find(hoverChain_, hoverItem_) != hoverChain_.end();
        float const scale = renderer_.Scale();
        sm::BoxColors colors;
        double x = 0.0;
        double y = 0.0;
        if (PieMode())
        {
            sm::PieSlice const& slice = pie_.Items()[static_cast<std::size_t>(hoverItem_)];
            if (!renderer_.IsSliceLabelTruncated(scene, slice))
            {
                return;
            }
            colors = renderer_.SliceColors(scene, slice, highlighted, selected);
            sm::PiePoint const anchor = pie_.Centroid(slice);
            x = anchor.x / scale;
            y = anchor.y / scale;
        }
        else
        {
            sm::DisplayItem const& item = layout_.Items()[static_cast<std::size_t>(hoverItem_)];
            if (!renderer_.IsLabelTruncated(scene, item))
            {
                return;
            }
            colors = renderer_.ItemColors(scene, item, highlighted, selected);
            x = (item.x + renderer_.LineWidth()) / scale;
            y = item.IsFolder() ? (item.y + renderer_.LineWidth()) / scale
                                : (item.y + item.h / 2.0) / scale - renderer_.FontSizeDips() * 0.7;
        }

        sm::MapItem const& item = Item(hoverItem_);
        NameTip().Background(muxm::SolidColorBrush(ToColor(colors.face)));
        NameTip().BorderBrush(muxm::SolidColorBrush(ToColor(scene.colors.outline)));
        NameTipText().Foreground(muxm::SolidColorBrush(ToColor(sm::TextColor(scene.colors, colors.face))));
        NameTipText().FontFamily(muxm::FontFamily(hstring(renderer_.FontFamily())));
        NameTipText().FontSize(renderer_.FontSizeDips());
        NameTipText().Text(hstring(item.source->Name(item.index)));
        PlaceTip(NameTip(), x, y, PieMode());
    }

    // Details of the file under the mouse (1.4.0's "info tips").
    void MainWindow::ShowInfoTip()
    {
        if (!tree_ || hoverItem_ < 0 || !std::cmp_less(hoverItem_, ItemCount()))
        {
            return;
        }
        sm::MapItem const& item = Item(hoverItem_);
        sm::Lang const& lang = *lang_;
        std::uint32_t const flags = settings_.infoTipFlags;
        std::wstring name;
        std::wstring folderPath;
        std::wstring details;
        auto const line = [&details](std::wstring const& text) {
            if (!text.empty())
            {
                if (!details.empty())
                {
                    details.push_back(L'\n');
                }
                details.append(text);
            }
        };
        muxm::ImageSource icon{ nullptr };

        if (sm::Entry const* const entry = item.IsSelectable() ? item.GetEntry() : nullptr)
        {
            folderPath = tree_->PathOf(*item.source);
            name = item.source->Name(item.index);
            if (flags & sm::TipSize)
            {
                // 1.4.0 showed only the file length; the space it takes on disk is new.
                std::wstring const onDisk = sm::FormatSize(entry->size, lang);
                line(Join(sm::FormatBytes(entry->logicalSize, lang), L"  (", sm::FormatText(lang.onDisk, { onDisk }), L")"));
                if (entry->child)
                {
                    line(Join(lang.filesTotal, L" ", sm::FormatCount(entry->child->fileCount, lang), L"   ",
                                lang.foldersTotal, L" ", sm::FormatCount(entry->child->folderCount, lang)));
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
            if (flags & sm::TipAttrib)
            {
                line(sm::FormatAttributes(entry->attributes, lang));
            }
            if (flags & sm::TipDate)
            {
                line(sm::FormatFileTime(entry->lastWriteTime, lang));
            }
            if (flags & sm::TipIcon)
            {
                icon = ToImageSource(sm::shell::GetIcon(folderPath + name,
                                                        static_cast<int>(std::lround(32 * renderer_.Scale()))));
            }
        }
        else if (PieMode())
        {
            // A slice of grouped small entries, or of free space: what it is and how much.
            sm::PieSlice const& slice = pie_.Items()[static_cast<std::size_t>(hoverItem_)];
            if (slice.IsFolded())
            {
                std::wstring const count = sm::FormatCount(slice.folded, lang);
                name = sm::FormatText(lang.smallerItems, { count });
                folderPath = tree_->PathOf(*slice.source);
            }
            else
            {
                sm::Entry const* const special = slice.GetEntry();
                name = special != nullptr && special->kind == sm::EntryKind::FreeSpace ? lang.freeSpace : lang.unscanned;
                folderPath = tree_->RootPath();
            }
            if (flags & sm::TipSize)
            {
                line(Join(sm::FormatSize(slice.bytes, lang), L"  (", sm::FormatPercent(slice.bytes, PercentBase(), lang),
                          L")"));
            }
        }
        else
        {
            return;  // treemap filler boxes and free space have no tips, as in 1.4.0
        }

        // A grouped or free-space slice has no other way to say what it is, so its name always shows.
        bool const showName = (flags & sm::TipName) != 0 || !item.IsSelectable();
        InfoTipName().Text(hstring(name));
        InfoTipName().Visibility(showName ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        InfoTipPath().Text(hstring(folderPath));
        InfoTipPath().Visibility((flags & sm::TipPath) ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        InfoTipDetails().Text(hstring(details));
        InfoTipDetails().Visibility(details.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        InfoTipIcon().Source(icon);
        InfoTipIcon().Visibility(icon ? mux::Visibility::Visible : mux::Visibility::Collapsed);

        if (!showName && (flags & sm::TipPath) == 0 && details.empty() && !icon)
        {
            return;  // every detail is switched off
        }
        PlaceTip(InfoTip(), lastPointerDips_.X + 16.0, lastPointerDips_.Y + 16.0);
    }

    // ---------------------------------------------------------------- commands

    void MainWindow::OnOpen(IInspectable const&, mux::RoutedEventArgs const&)
    {
        OpenDriveOrFolder();
    }

    void MainWindow::OnRescan(IInspectable const&, mux::RoutedEventArgs const&)
    {
        Rescan();
    }

    void MainWindow::OnZoomFull(IInspectable const&, mux::RoutedEventArgs const&)
    {
        if (!Busy())
        {
            ZoomFull();
        }
    }

    void MainWindow::OnZoomIn(IInspectable const&, mux::RoutedEventArgs const&)
    {
        if (!Busy())
        {
            ZoomIntoSelection();
        }
    }

    void MainWindow::OnZoomOut(IInspectable const&, mux::RoutedEventArgs const&)
    {
        if (!Busy())
        {
            ZoomOut();
        }
    }

    void MainWindow::OnToggleFreeSpace(IInspectable const&, mux::RoutedEventArgs const&)
    {
        SetShowFreeSpace(unbox_value_or<bool>(FreeSpaceButton().IsChecked(), true));
    }

    void MainWindow::OnTogglePieChart(IInspectable const&, mux::RoutedEventArgs const&)
    {
        SetViewMode(unbox_value_or<bool>(PieChartButton().IsChecked(), false) ? sm::ViewMode::Pie
                                                                              : sm::ViewMode::Treemap);
    }

    // The window's keyboard shortcuts (see MainWindow.xaml); each key is used
    // by one shortcut only.  The commands check for themselves whether they
    // can run now, as the toolbar buttons' enabled states do.
    void MainWindow::OnShortcut(muxi::KeyboardAccelerator const& accelerator,
                                muxi::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        using Windows::System::VirtualKey;
        args.Handled(true);
        switch (accelerator.Key())
        {
        case VirtualKey::O: OpenDriveOrFolder(); break;
        case VirtualKey::F5:
            if ((accelerator.Modifiers() & Windows::System::VirtualKeyModifiers::Shift) != Windows::System::VirtualKeyModifiers::None)
            {
                FullRescan();
            }
            else
            {
                Rescan();
            }
            break;
        case VirtualKey::Home: OnZoomFull(nullptr, nullptr); break;
        case VirtualKey::Back:
        case VirtualKey::Up: OnZoomOut(nullptr, nullptr); break;  // Backspace, or Alt+Up as in File Explorer
        case VirtualKey::Down: GoDown(); break;                   // Alt+Down
        case VirtualKey::Left: GoBack(); break;                   // Alt+Left
        case VirtualKey::Right: GoForward(); break;               // Alt+Right
        case VirtualKey::P: SetViewMode(PieMode() ? sm::ViewMode::Treemap : sm::ViewMode::Pie); break;
        case VirtualKey::Delete: DeleteSelected(); break;
        case VirtualKey::F1: ShowAbout(); break;
        case VirtualKey::Enter: ShowSelectedProperties(); break;
        case VirtualKey::C: CopySelectedPath(); break;
        default: args.Handled(false); break;
        }
    }

    void MainWindow::OnRun(IInspectable const&, mux::RoutedEventArgs const&)
    {
        RunSelected();
    }

    void MainWindow::OnDelete(IInspectable const&, mux::RoutedEventArgs const&)
    {
        DeleteSelected();
    }

    void MainWindow::OnProperties(IInspectable const&, mux::RoutedEventArgs const&)
    {
        ShowSelectedProperties();
    }

    void MainWindow::OnShowInExplorer(IInspectable const&, mux::RoutedEventArgs const&)
    {
        ShowSelectedInExplorer();
    }

    void MainWindow::OnCopyPath(IInspectable const&, mux::RoutedEventArgs const&)
    {
        CopySelectedPath();
    }

    void MainWindow::OnSettings(IInspectable const&, mux::RoutedEventArgs const&)
    {
        ShowSettings();
    }

    void MainWindow::OnAbout(IInspectable const&, mux::RoutedEventArgs const&)
    {
        ShowAbout();
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
        if (!tree_ || Busy() || viewRoot_ == nullptr)
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
        if (tree_ && !Busy())
        {
            GoTo(history_.Back(ViewPath(), [this](sm::ViewHistory::Path const& path) { return StaysInView(path); }));
        }
    }

    void MainWindow::GoForward()
    {
        if (tree_ && !Busy())
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
        if (!Busy() && SelectedEntry() != nullptr)
        {
            sm::shell::Open(WindowHandle(), SelectedPath());  // the shell reports any failure itself
        }
    }

    void MainWindow::ShowSelectedProperties()
    {
        if (!Busy() && SelectedEntry() != nullptr)
        {
            sm::shell::ShowProperties(WindowHandle(), SelectedPath());
        }
    }

    void MainWindow::ShowSelectedInExplorer()
    {
        if (!Busy() && SelectedEntry() != nullptr)
        {
            sm::shell::ShowInExplorer(SelectedPath());
        }
    }

    void MainWindow::CopySelectedPath()
    {
        if (Busy() || SelectedEntry() == nullptr)
        {
            return;
        }
        Windows::ApplicationModel::DataTransfer::DataPackage package;
        package.SetText(hstring(SelectedPath()));
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
    }

    void MainWindow::OnShowUnreadable(IInspectable const&, mux::RoutedEventArgs const&)
    {
        ShowUnreadable();
    }

    // The folders a scan could not list: their contents are missing from the map.
    winrt::fire_and_forget MainWindow::ShowUnreadable()
    {
        auto const strong = get_strong();
        if (!tree_ || Busy())
        {
            co_return;
        }
        sm::Lang const& lang = *lang_;
        std::vector<std::wstring> const paths = tree_->UnreadableFolders();
        std::wstring all;
        for (std::wstring const& path : paths)
        {
            all.append(path).append(L"\r\n");
        }
        constexpr std::size_t kListed = 1000;  // a longer list goes to the clipboard whole
        std::wstring listed;
        for (std::size_t i = 0; i < paths.size() && i < kListed; ++i)
        {
            listed.append(i == 0 ? L"" : L"\r\n").append(paths[i]);
        }
        if (paths.size() > kListed)
        {
            std::wstring const more = sm::FormatCount(paths.size() - kListed, lang);
            listed.append(L"\r\n").append(sm::FormatText(lang.moreFolders, { more }));
        }

        muxc::ContentDialog dialog = MakeDialog(lang.unreadableTitle);
        muxc::StackPanel content;
        content.Spacing(12);
        muxc::TextBlock intro;
        intro.Text(hstring(lang.unreadableText));
        intro.TextWrapping(mux::TextWrapping::Wrap);
        muxc::TextBox list;
        list.Text(hstring(listed));
        list.IsReadOnly(true);
        list.AcceptsReturn(true);
        list.TextWrapping(mux::TextWrapping::NoWrap);
        list.MaxHeight(320);
        muxc::ScrollViewer::SetHorizontalScrollBarVisibility(list, muxc::ScrollBarVisibility::Auto);
        muxc::ScrollViewer::SetVerticalScrollBarVisibility(list, muxc::ScrollBarVisibility::Auto);
        content.Children().Append(intro);
        content.Children().Append(list);
        dialog.Content(content);
        dialog.PrimaryButtonText(hstring(lang.copyList));
        dialog.CloseButtonText(hstring(lang.close));
        dialog.DefaultButton(muxc::ContentDialogButton::Close);

        dialogOpen_ = true;
        muxc::ContentDialogResult const answer = co_await dialog.ShowAsync();
        dialogOpen_ = false;
        if (answer == muxc::ContentDialogResult::Primary)
        {
            Windows::ApplicationModel::DataTransfer::DataPackage package;
            package.SetText(hstring(all));
            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
        }
    }

    winrt::fire_and_forget MainWindow::DeleteSelected()
    {
        auto const strong = get_strong();
        // 1.4.0 checked "Disable Delete" only when drawing the toolbar.
        if (Busy() || settings_.disableDelete)
        {
            co_return;
        }
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
                selection_ = {};
                tree_->Remove(*folder, index);
                if (tree_->rootRecord != 0)
                {
                    // A hard-linked file counted under a name now gone is counted under another.
                    sm::CountHardLinksOnce(*tree_, [this](sm::Folder const& in, std::size_t at) {
                        return sm::ReadSizeOnDisk(tree_->PathOf(in, at), in.entries[at].attributes);
                    });
                }
                tree_->RefreshFreeSpace();  // the recycled bytes now show as unscanned space
                Relayout();
                UpdateAll();
            }
            break;
        case sm::shell::DeleteResult::Failed:
            ShowError(sm::FormatText(lang.deleteFailed, { name }));
            break;
        case sm::shell::DeleteResult::Cancelled:
            break;
        }
    }

    void MainWindow::ShowContextMenu(Point position)
    {
        sm::Lang const& lang = *lang_;
        sm::Entry const* const entry = SelectedEntry();
        bool const selected = entry != nullptr;
        bool const folder = selected && entry->IsFolder();
        bool const zoomed = tree_ && viewRoot_ != tree_->Root();

        muxc::MenuFlyout menu;
        auto const add = [&](std::wstring_view caption, wchar_t const* glyph, bool enabled, bool emphasized,
                             void (*action)(MainWindow&)) {
            sm::Caption const parsed = sm::ParseCaption(caption);
            muxc::MenuFlyoutItem item;
            item.Text(hstring(parsed.text));
            item.AccessKey(hstring(parsed.accessKey));
            muxc::FontIcon icon;
            icon.Glyph(glyph);
            item.Icon(icon);
            item.IsEnabled(enabled);
            if (emphasized)
            {
                // 1.4.0 made the double-click action the menu's default (bold) item.
                item.FontWeight(Microsoft::UI::Text::FontWeights::SemiBold());
            }
            item.Click([weak = get_weak(), action](auto&&, auto&&) {
                if (auto const self = weak.get())
                {
                    action(*self);
                }
            });
            menu.Items().Append(item);
        };

        add(lang.zoomIn, L"", folder, folder, [](MainWindow& w) { w.ZoomIntoSelection(); });
        add(lang.zoomOut, L"", zoomed, false, [](MainWindow& w) { w.ZoomOut(); });
        add(lang.zoomFull, L"", zoomed, false, [](MainWindow& w) { w.ZoomFull(); });
        menu.Items().Append(muxc::MenuFlyoutSeparator());
        add(lang.run, L"", selected, selected && !folder, [](MainWindow& w) { w.RunSelected(); });
        add(lang.del, L"", selected && !settings_.disableDelete, false, [](MainWindow& w) { w.DeleteSelected(); });
        menu.Items().Append(muxc::MenuFlyoutSeparator());
        add(lang.openDrive, L"", true, false, [](MainWindow& w) { w.OpenDriveOrFolder(); });
        add(lang.rescanDrive, L"", tree_ != nullptr, false, [](MainWindow& w) { w.Rescan(); });
        add(lang.fullRescan, L"", tree_ != nullptr, false, [](MainWindow& w) { w.FullRescan(); });
        if (sm::Folder const* const target = RescanTarget(); target != nullptr && target->parent != nullptr)
        {
            // The folder's name inside a caption, where '&' marks an access key.
            std::wstring name(target->parent->Name(target->indexInParent));
            for (std::size_t at = name.find(L'&'); at != std::wstring::npos; at = name.find(L'&', at + 2))
            {
                name.insert(at, 1, L'&');
            }
            std::wstring const caption = sm::FormatText(lang.rescanFolder, { name });
            add(caption, L"", true, false, [](MainWindow& w) { w.RescanFolder(w.RescanTarget()); });
        }

        sm::Caption const freeCaption = sm::ParseCaption(lang.showFreeSpace);
        muxc::ToggleMenuFlyoutItem freeSpace;
        freeSpace.Text(hstring(freeCaption.text));
        freeSpace.AccessKey(hstring(freeCaption.accessKey));
        freeSpace.IsChecked(showFreeSpace_);
        freeSpace.IsEnabled(tree_ != nullptr && tree_->IsWholeVolume());
        freeSpace.Click([weak = get_weak()](auto&&, auto&&) {
            if (auto const self = weak.get())
            {
                self->SetShowFreeSpace(!self->showFreeSpace_);
            }
        });
        menu.Items().Append(freeSpace);

        sm::Caption const pieCaption = sm::ParseCaption(lang.pieChart);
        muxc::ToggleMenuFlyoutItem pieChart;
        pieChart.Text(hstring(pieCaption.text));
        pieChart.AccessKey(hstring(pieCaption.accessKey));
        pieChart.IsChecked(PieMode());
        pieChart.Click([weak = get_weak()](auto&&, auto&&) {
            if (auto const self = weak.get())
            {
                self->SetViewMode(self->PieMode() ? sm::ViewMode::Treemap : sm::ViewMode::Pie);
            }
        });
        menu.Items().Append(pieChart);
        menu.Items().Append(muxc::MenuFlyoutSeparator());
        add(lang.properties, L"", selected, false, [](MainWindow& w) { w.ShowSelectedProperties(); });
        add(lang.showInExplorer, L"", selected, false, [](MainWindow& w) { w.ShowSelectedInExplorer(); });
        add(lang.copyPath, L"", selected, false, [](MainWindow& w) { w.CopySelectedPath(); });

        muxc::Primitives::FlyoutShowOptions options;
        options.Position(position);
        menu.ShowAt(MapFocus(), options);
    }

    // ---------------------------------------------------------------- drag and drop

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

    // ---------------------------------------------------------------- dialogs

    muxc::ContentDialog MainWindow::MakeDialog(std::wstring_view title)
    {
        // As in the WinUI Gallery: a desktop app must give its dialogs a XamlRoot.
        muxc::ContentDialog dialog;
        dialog.XamlRoot(RootGrid().XamlRoot());
        dialog.Style(mux::Application::Current().Resources().Lookup(box_value(L"DefaultContentDialogStyle")).as<mux::Style>());
        dialog.Title(box_value(hstring(title)));
        dialog.RequestedTheme(RootGrid().ActualTheme());
        return dialog;
    }

    muxc::ContentDialog MainWindow::HostDialog(muxc::ContentDialog& slot, mux::FrameworkElement const& content)
    {
        if (!slot)
        {
            slot = MakeDialog({});
            slot.Content(Detach(content));
        }
        slot.RequestedTheme(RootGrid().ActualTheme());
        return slot;
    }

    winrt::fire_and_forget MainWindow::OpenDriveOrFolder()
    {
        auto const strong = get_strong();
        if (Busy())
        {
            co_return;
        }
        dialogOpen_ = true;

        // Network and removable drives can be slow to answer, so ask off the UI thread.
        co_await winrt::resume_background();
        std::vector<sm::shell::DriveInfo> const drives = sm::shell::EnumerateDrives();
        try
        {
            co_await wil::resume_foreground(dispatcher_);
        }
        catch (winrt::hresult_error const&)
        {
            co_return;  // the window has closed
        }

        sm::Lang const& lang = *lang_;
        FillDriveList(drives);
        muxc::ContentDialog dialog = HostDialog(driveDialog_, DriveContent());
        dialog.Title(box_value(hstring(lang.selectDrive)));
        dialog.PrimaryButtonText(hstring(lang.ok));
        dialog.CloseButtonText(hstring(lang.cancel));
        dialog.DefaultButton(muxc::ContentDialogButton::Primary);
        dialog.IsPrimaryButtonEnabled(DriveList().SelectedIndex() >= 0);
        BrowseButton().Content(box_value(hstring(lang.browseFolder)));
        driveAccepted_ = false;
        browseRequested_ = false;

        muxc::ContentDialogResult const result = co_await dialog.ShowAsync();
        std::wstring path;
        if ((result == muxc::ContentDialogResult::Primary || driveAccepted_) && DriveList().SelectedItem())
        {
            path = unbox_value<hstring>(DriveList().SelectedItem().as<muxc::ListViewItem>().Tag());
        }
        else if (browseRequested_)
        {
            try
            {
                // The Windows App SDK picker also works when the app runs elevated,
                // where the older Windows.Storage pickers fail.
                Microsoft::Windows::Storage::Pickers::FolderPicker picker(AppWindow().Id());
                auto const picked = co_await picker.PickSingleFolderAsync();
                if (picked)
                {
                    path = picked.Path();
                }
            }
            catch (winrt::hresult_error const&)
            {
                if (std::optional<std::wstring> const folder = sm::shell::PickFolder(WindowHandle()))
                {
                    path = *folder;
                }
            }
        }
        dialogOpen_ = false;
        if (!path.empty())
        {
            StartScan(std::move(path));
        }
    }

    void MainWindow::FillDriveList(std::vector<sm::shell::DriveInfo> const& drives)
    {
        sm::Lang const& lang = *lang_;
        auto const items = DriveList().Items();
        items.Clear();
        int const iconSize = static_cast<int>(std::lround(32 * DpiScale()));
        std::wstring const current = tree_ ? tree_->Volume().root : std::wstring();
        int select = drives.empty() ? -1 : 0;

        for (std::size_t i = 0; i < drives.size(); ++i)
        {
            sm::shell::DriveInfo const& drive = drives[i];
            muxc::Grid row;
            row.ColumnSpacing(12);
            row.Padding({ 0, 6, 0, 6 });
            muxc::ColumnDefinition iconColumn;
            iconColumn.Width(mux::GridLengthHelper::Auto());
            row.ColumnDefinitions().Append(iconColumn);
            row.ColumnDefinitions().Append(muxc::ColumnDefinition());

            muxc::Image icon;
            icon.Width(32);
            icon.Height(32);
            icon.VerticalAlignment(mux::VerticalAlignment::Center);
            icon.Source(ToImageSource(sm::shell::GetIcon(drive.root, iconSize)));
            row.Children().Append(icon);

            muxc::StackPanel text;
            text.Spacing(4);
            text.VerticalAlignment(mux::VerticalAlignment::Center);
            muxc::Grid::SetColumn(text, 1);
            muxc::TextBlock name;
            name.Text(hstring(drive.displayName));
            text.Children().Append(name);
            if (drive.ready && drive.totalBytes != 0)
            {
                // A capacity bar like File Explorer's, red when the drive is nearly full.
                std::uint32_t const used = sm::PerMille(drive.totalBytes - drive.freeBytes, drive.totalBytes);
                muxc::ProgressBar bar;
                bar.Maximum(1000);
                bar.Value(used);
                bar.ShowError(used >= 900);
                text.Children().Append(bar);
                muxc::TextBlock capacity;
                capacity.Style(mux::Application::Current().Resources().Lookup(box_value(L"CaptionTextBlockStyle")).as<mux::Style>());
                capacity.Text(hstring(Join(sm::FormatSize(drive.freeBytes, lang), L" ", lang.free, L"  /  ",
                                             sm::FormatSize(drive.totalBytes, lang), L" ", lang.total)));
                text.Children().Append(capacity);
            }
            row.Children().Append(text);

            muxc::ListViewItem item;
            item.Content(row);
            item.Tag(box_value(hstring(drive.root)));
            mux::Automation::AutomationProperties::SetName(item, hstring(drive.displayName));
            items.Append(item);
            if (!current.empty() && SamePath(drive.root, current))
            {
                select = static_cast<int>(i);
            }
        }
        DriveList().SelectedIndex(select);
    }

    void MainWindow::OnDriveDoubleTapped(IInspectable const&, muxi::DoubleTappedRoutedEventArgs const&)
    {
        // As in 1.4.0: double-clicking a drive opens it.
        if (driveDialog_ && DriveList().SelectedIndex() >= 0)
        {
            driveAccepted_ = true;
            driveDialog_.Hide();
        }
    }

    void MainWindow::OnDriveSelectionChanged(IInspectable const&, muxc::SelectionChangedEventArgs const&)
    {
        if (driveDialog_)
        {
            driveDialog_.IsPrimaryButtonEnabled(DriveList().SelectedIndex() >= 0);
        }
    }

    void MainWindow::OnBrowseFolder(IInspectable const&, mux::RoutedEventArgs const&)
    {
        if (driveDialog_)
        {
            browseRequested_ = true;
            driveDialog_.Hide();
        }
    }

    winrt::fire_and_forget MainWindow::ShowSettings()
    {
        auto const strong = get_strong();
        if (Busy())
        {
            co_return;
        }
        dialogOpen_ = true;
        settingsOpen_ = true;
        sm::Lang const* const originalLanguage = lang_;
        LoadThemes();  // theme files may have come and gone
        muxc::ContentDialog dialog = HostDialog(settingsDialog_, SettingsContent());
        PopulateSettings();
        dialog.DefaultButton(muxc::ContentDialogButton::Primary);

        muxc::ContentDialogResult const result = co_await dialog.ShowAsync();
        dialogOpen_ = false;
        settingsOpen_ = false;
        if (result == muxc::ContentDialogResult::Primary)
        {
            ReadSettings();
            settings_.Validate();
            sm::SaveSettings(settings_);
            Relayout();
        }
        else
        {
            lang_ = originalLanguage;  // the language was only being previewed
        }
        ApplyTheme(settings_.theme);  // the chosen theme, or back from a preview
        if (themeErrors_.empty())
        {
            ThemeBar().IsOpen(false);  // the theme files have been put right
        }
        else
        {
            ThemeBar().Message(hstring(JoinLines(themeErrors_)));
        }
        ApplyLanguage();
        UpdateAll();
    }

    void MainWindow::PopulateSettings()
    {
        populatingSettings_ = true;
        auto const done = wil::scope_exit([this] { populatingSettings_ = false; });

        auto const languages = LanguageBox().Items();
        languages.Clear();
        int index = 0;
        for (sm::Lang const& lang : sm::Languages())
        {
            languages.Append(box_value(hstring(lang.name)));
            if (&lang == lang_)
            {
                LanguageBox().SelectedIndex(index);
            }
            ++index;
        }
        FillSettingsLists(settings_.theme, settings_.density - sm::Settings::kMinDensity, settings_.fileColor,
                          settings_.folderColor);
        ApplySettingsText();
        ShowThemeErrors();
        std::vector<std::wstring> const folders = sm::shell::ThemeFolders();
        muxc::ToolTipService::SetToolTip(ThemesFolderButton(),
                                         folders.empty() ? nullptr : box_value(hstring(folders.front())));

        BiasSlider().Value(settings_.bias);
        NameTipsBox().IsChecked(settings_.showNameTips);
        NameTipDelayBox().Value(settings_.nameTipDelay);
        InfoTipsBox().IsChecked(settings_.showInfoTips);
        InfoTipDelayBox().Value(settings_.infoTipDelay);
        TipPathBox().IsChecked((settings_.infoTipFlags & sm::TipPath) != 0);
        TipNameBox().IsChecked((settings_.infoTipFlags & sm::TipName) != 0);
        TipIconBox().IsChecked((settings_.infoTipFlags & sm::TipIcon) != 0);
        TipDateBox().IsChecked((settings_.infoTipFlags & sm::TipDate) != 0);
        TipSizeBox().IsChecked((settings_.infoTipFlags & sm::TipSize) != 0);
        TipAttribBox().IsChecked((settings_.infoTipFlags & sm::TipAttrib) != 0);
        RolloverBox().IsChecked(settings_.rolloverBox);
        AutoRescanBox().IsChecked(settings_.autoRescan);
        DisableDeleteBox().IsChecked(settings_.disableDelete);
        AnimatedZoomBox().IsChecked(settings_.animatedZoom);
        SavePositionBox().IsChecked(settings_.savePosition);
        UpdateTipControls();
    }

    void MainWindow::FillSettingsLists(std::wstring_view theme, int density, int fileColor, int folderColor)
    {
        sm::Lang const& lang = *lang_;
        auto const fill = [](muxc::ComboBox const& box, std::span<std::wstring_view const> names, int selected) {
            auto const items = box.Items();
            items.Clear();
            for (std::wstring_view const name : names)
            {
                items.Append(box_value(hstring(name)));
            }
            box.SelectedIndex(selected);
        };

        // The classic themes have translated names; the others' names are their own.
        std::vector<std::wstring_view> themeNames;
        sm::Theme const& picked = sm::FindTheme(themes_, theme);
        int pickedIndex = 0;
        for (sm::Theme const& candidate : themes_)
        {
            if (&candidate == &picked)
            {
                pickedIndex = static_cast<int>(themeNames.size());
            }
            std::size_t const classic = candidate.base == sm::ThemeBase::System  ? 0
                                        : candidate.base == sm::ThemeBase::Light ? 1
                                                                                 : 2;
            themeNames.push_back(candidate.classic ? lang.themeNames.at(classic) : std::wstring_view(candidate.name));
        }
        fill(ThemeBox(), themeNames, pickedIndex);
        fill(DensityBox(), lang.densityNames, density);
        fill(FileColorBox(), lang.colorNames, fileColor);
        fill(FolderColorBox(), lang.colorNames, folderColor);
    }

    void MainWindow::ApplySettingsText()
    {
        sm::Lang const& lang = *lang_;
        auto const text = [](std::wstring_view value) { return box_value(hstring(value)); };
        if (settingsDialog_)
        {
            settingsDialog_.Title(text(lang.settings));
            settingsDialog_.PrimaryButtonText(hstring(lang.ok));
            settingsDialog_.CloseButtonText(hstring(lang.cancel));
        }
        AppearanceHeader().Text(hstring(lang.appearance));
        LanguageBox().Header(text(lang.language));
        ThemeBox().Header(text(lang.theme));
        ThemesFolderButton().Content(text(lang.themesFolder));
        ThemeErrorBar().Title(hstring(lang.themeErrors));
        LayoutHeader().Text(hstring(lang.layout));
        DensityBox().Header(text(lang.density));
        BiasSlider().Header(text(lang.bias));
        BiasHorz().Text(hstring(lang.horz));
        BiasEqual().Text(hstring(lang.equal));
        BiasVert().Text(hstring(lang.vert));
        ColorsHeader().Text(hstring(lang.displayColors));
        FileColorBox().Header(text(lang.files));
        FolderColorBox().Header(text(lang.folders));
        TipsHeader().Text(hstring(lang.toolTips));
        NameTipsBox().Content(text(lang.showNameTips));
        NameTipDelayBox().Header(text(lang.delay));
        InfoTipsBox().Content(text(lang.showInfoTips));
        InfoTipDelayBox().Header(text(lang.delay));
        TipPathBox().Content(text(lang.fullPath));
        TipNameBox().Content(text(lang.fileName));
        TipIconBox().Content(text(lang.icon));
        TipDateBox().Content(text(lang.dateTime));
        TipSizeBox().Content(text(lang.fileSize));
        TipAttribBox().Content(text(lang.attrib));
        RolloverBox().Content(text(lang.showRolloverBox));
        MiscHeader().Text(hstring(lang.miscOptions));
        AutoRescanBox().Content(text(lang.autoRescan));
        DisableDeleteBox().Content(text(lang.disableDelete));
        AnimatedZoomBox().Content(text(lang.animatedZoom));
        SavePositionBox().Content(text(lang.savePos));
        ShortcutsHeader().Text(hstring(lang.keyboardShortcuts));
        FillShortcuts();
    }

    // The keyboard shortcuts, as Settings lists them: their keys as key caps,
    // then what they do.
    void MainWindow::FillShortcuts()
    {
        namespace mua = mux::Automation;
        mux::ResourceDictionary const resources = mux::Application::Current().Resources();
        auto const style = [&resources](wchar_t const* name) { return resources.Lookup(box_value(name)).as<mux::Style>(); };
        mux::Style const keyCap = style(L"KeyCapStyle");
        mux::Style const caption = style(L"CaptionTextBlockStyle");
        auto const label = [&caption](std::wstring_view text) {
            muxc::TextBlock block;
            block.Style(caption);
            block.Text(hstring(text));
            block.HorizontalAlignment(mux::HorizontalAlignment::Center);
            block.VerticalAlignment(mux::VerticalAlignment::Center);
            mua::AutomationProperties::SetAccessibilityView(block, mua::Peers::AccessibilityView::Raw);  // read with the action
            return block;
        };

        muxc::Grid const list = ShortcutsList();
        list.Children().Clear();
        list.RowDefinitions().Clear();
        for (sm::Shortcut const& shortcut : lang_->shortcuts)
        {
            int const row = static_cast<int>(list.RowDefinitions().Size());
            muxc::RowDefinition definition;
            definition.Height({ 1.0, mux::GridUnitType::Auto });
            list.RowDefinitions().Append(definition);

            muxc::StackPanel keys;
            keys.Orientation(muxc::Orientation::Horizontal);
            keys.Spacing(4);
            for (std::vector<std::wstring_view> const& alternative : sm::SplitShortcut(shortcut.keys))
            {
                if (keys.Children().Size() != 0)
                {
                    muxc::TextBlock const slash = label(L"/");
                    slash.Opacity(0.6);
                    keys.Children().Append(slash);
                }
                for (std::wstring_view const key : alternative)
                {
                    muxc::Border cap;
                    cap.Style(keyCap);
                    cap.Child(label(key));
                    keys.Children().Append(cap);
                }
            }
            muxc::Grid::SetRow(keys, row);
            list.Children().Append(keys);

            muxc::TextBlock action;
            action.Text(hstring(shortcut.action));
            action.TextWrapping(mux::TextWrapping::Wrap);
            action.VerticalAlignment(mux::VerticalAlignment::Center);
            mua::AutomationProperties::SetName(action, hstring(Join(shortcut.keys, L": ", shortcut.action)));
            muxc::Grid::SetRow(action, row);
            muxc::Grid::SetColumn(action, 1);
            list.Children().Append(action);
        }
    }

    void MainWindow::OnLanguagePreview(IInspectable const&, muxc::SelectionChangedEventArgs const&)
    {
        if (populatingSettings_)
        {
            return;
        }
        auto const languages = sm::Languages();
        int const index = LanguageBox().SelectedIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= languages.size())
        {
            return;
        }

        // Like 1.4.0, the dialog switches language as soon as one is picked;
        // the main window follows, and Cancel switches both back.
        lang_ = &languages[static_cast<std::size_t>(index)];
        populatingSettings_ = true;
        FillSettingsLists(PickedTheme(), DensityBox().SelectedIndex(), FileColorBox().SelectedIndex(),
                          FolderColorBox().SelectedIndex());
        ApplySettingsText();
        populatingSettings_ = false;
        ApplyLanguage();
    }

    void MainWindow::OnThemePreview(IInspectable const&, muxc::SelectionChangedEventArgs const&)
    {
        // Like the language, a theme shows as soon as it is picked; Cancel switches back.
        if (!populatingSettings_ && ThemeBox().SelectedIndex() >= 0)
        {
            ApplyTheme(PickedTheme());
        }
    }

    // The id of the theme picked in Settings.
    std::wstring MainWindow::PickedTheme()
    {
        int const index = ThemeBox().SelectedIndex();
        return index >= 0 && std::cmp_less(index, themes_.size()) ? themes_[static_cast<std::size_t>(index)].id
                                                                  : settings_.theme;
    }

    // Reads the theme files again while Settings is open, and shows the
    // picked theme as its file now has it.
    void MainWindow::RefreshThemes()
    {
        std::wstring const picked = PickedTheme();
        LoadThemes();
        populatingSettings_ = true;
        FillSettingsLists(picked, DensityBox().SelectedIndex(), FileColorBox().SelectedIndex(),
                          FolderColorBox().SelectedIndex());
        populatingSettings_ = false;
        ShowThemeErrors();
        ApplyTheme(PickedTheme());
    }

    void MainWindow::ShowThemeErrors()
    {
        ThemeErrorBar().Message(hstring(JoinLines(themeErrors_)));
        ThemeErrorBar().IsOpen(!themeErrors_.empty());
    }

    void MainWindow::OnOpenThemesFolder(IInspectable const&, mux::RoutedEventArgs const&)
    {
        std::vector<std::wstring> const folders = sm::shell::ThemeFolders();
        if (!folders.empty())
        {
            (void)sm::shell::OpenFolder(folders.front());
        }
    }

    void MainWindow::OnTipOptionChanged(IInspectable const&, mux::RoutedEventArgs const&)
    {
        UpdateTipControls();
    }

    void MainWindow::UpdateTipControls()
    {
        bool const names = Checked(NameTipsBox());
        NameTipDelayBox().IsEnabled(names);
        bool const info = Checked(InfoTipsBox());
        for (muxc::Control const& control : { muxc::Control(InfoTipDelayBox()), muxc::Control(TipPathBox()),
                                             muxc::Control(TipNameBox()), muxc::Control(TipIconBox()),
                                             muxc::Control(TipDateBox()), muxc::Control(TipSizeBox()),
                                             muxc::Control(TipAttribBox()) })
        {
            control.IsEnabled(info);
        }
    }

    void MainWindow::ReadSettings()
    {
        // Out-of-range picks (-1: nothing selected) are clamped by Validate();
        // 1.4.0 stored CB_ERR - 3 as the density and indexed its tables with it.
        settings_.language = std::wstring(lang_->code);
        settings_.theme = PickedTheme();
        settings_.density = DensityBox().SelectedIndex() + sm::Settings::kMinDensity;
        settings_.fileColor = FileColorBox().SelectedIndex();
        settings_.folderColor = FolderColorBox().SelectedIndex();
        settings_.bias = static_cast<int>(std::lround(BiasSlider().Value()));
        settings_.showNameTips = Checked(NameTipsBox());
        settings_.nameTipDelay = Delay(NameTipDelayBox(), settings_.nameTipDelay);
        settings_.showInfoTips = Checked(InfoTipsBox());
        settings_.infoTipDelay = Delay(InfoTipDelayBox(), settings_.infoTipDelay);
        std::uint32_t flags = 0;
        flags |= Checked(TipPathBox()) ? sm::TipPath : 0u;
        flags |= Checked(TipNameBox()) ? sm::TipName : 0u;
        flags |= Checked(TipIconBox()) ? sm::TipIcon : 0u;
        flags |= Checked(TipDateBox()) ? sm::TipDate : 0u;
        flags |= Checked(TipSizeBox()) ? sm::TipSize : 0u;
        flags |= Checked(TipAttribBox()) ? sm::TipAttrib : 0u;
        settings_.infoTipFlags = flags;
        settings_.rolloverBox = Checked(RolloverBox());
        settings_.autoRescan = Checked(AutoRescanBox());
        settings_.disableDelete = Checked(DisableDeleteBox());
        settings_.animatedZoom = Checked(AnimatedZoomBox());
        settings_.savePosition = Checked(SavePositionBox());
    }

    winrt::fire_and_forget MainWindow::ShowAbout()
    {
        auto const strong = get_strong();
        if (dialogOpen_)
        {
            co_return;
        }
        dialogOpen_ = true;
        sm::Lang const& lang = *lang_;
        muxc::ContentDialog dialog = HostDialog(aboutDialog_, AboutContent());
        dialog.Title(box_value(hstring(lang.aboutApp)));
        dialog.CloseButtonText(hstring(lang.ok));
        dialog.DefaultButton(muxc::ContentDialogButton::Close);

        // The runtime is bundled with the app (self-contained), so report the
        // release it was built against; RuntimeInfo describes framework packages only.
        std::wstring const version =
            Join(L"SpaceWindow ", kVersion, L"  ·  WinUI 3  ·  Windows App SDK ",
                   std::to_wstring(SPACEWINDOW_WASDK_MAJOR), L".", std::to_wstring(SPACEWINDOW_WASDK_MINOR), L".",
                   std::to_wstring(SPACEWINDOW_WASDK_PATCH));
        AboutVersion().Text(hstring(version));
        AboutFreeware().Text(hstring(lang.freeware));
        AboutWarranty().Text(hstring(lang.warranty));
        AboutCredits().Text(hstring(lang.credits));

        co_await dialog.ShowAsync();
        dialogOpen_ = false;
    }
}
