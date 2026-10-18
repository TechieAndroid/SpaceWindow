// SpaceWindow (Qt) - the main window.
//
// Ported from src/App/MainWindow*.cpp (WinUI), which replaced Mainfrm.cpp,
// FolderView.cpp and the command handlers of SpaceMonger.cpp in SpaceMonger
// 1.4.0 (Copyright (c) 1998-2020 Sean Werkema, MIT License).
#include "MainWindow.h"

#include <optional>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QStatusBar>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "Compat.h"
#include "Desktop.h"
#include "Dialogs.h"
#include "FolderWatch.h"
#include "Format.h"
#include "Lang.h"
#include "MapWidget.h"
#include "Posix.h"
#include "QtCompat.h"
#include "QtText.h"
#include "Shell.h"
#include "Text.h"

namespace sm::qt
{
    namespace
    {
        constexpr auto kProgressInterval = std::chrono::milliseconds(100);
        constexpr auto kQuietUpdate = std::chrono::milliseconds(400);  // a quicker update shows no progress card
        constexpr unsigned kLiveMapTicks = 5;                          // progress ticks between two live maps
        constexpr std::size_t kLiveMapEntries = 30'000;                // more than a map can show anyway
        constexpr int kDefaultWidth = 1100;
        constexpr int kDefaultHeight = 740;
        constexpr auto kStopWait = std::chrono::seconds(2);  // closing waits this long for work to stop

        std::atomic<bool> leftWorkersRunning{ false };

        // The command table's keys (Windows' virtual-key codes) as Qt's.
        int ToQtKey(std::uint16_t key) noexcept
        {
            switch (key)
            {
            case VK_LEFT: return Qt::Key_Left;
            case VK_UP: return Qt::Key_Up;
            case VK_RIGHT: return Qt::Key_Right;
            case VK_DOWN: return Qt::Key_Down;
            case VK_RETURN: return Qt::Key_Return;
            case VK_BACK: return Qt::Key_Backspace;
            case VK_HOME: return Qt::Key_Home;
            case VK_ESCAPE: return Qt::Key_Escape;
            case VK_DELETE: return Qt::Key_Delete;
            case VK_APPS: return Qt::Key_Menu;
            default: break;
            }
            if (key >= VK_F1 && key <= VK_F24)
            {
                return Qt::Key_F1 + (key - VK_F1);
            }
            return key;  // letters and digits are the same
        }

        std::uint16_t FromQtKey(int key) noexcept
        {
            switch (key)
            {
            case Qt::Key_Left: return VK_LEFT;
            case Qt::Key_Up: return VK_UP;
            case Qt::Key_Right: return VK_RIGHT;
            case Qt::Key_Down: return VK_DOWN;
            case Qt::Key_Return:
            case Qt::Key_Enter: return VK_RETURN;
            case Qt::Key_Backspace: return VK_BACK;
            case Qt::Key_Home: return VK_HOME;
            case Qt::Key_Escape: return VK_ESCAPE;
            case Qt::Key_Delete: return VK_DELETE;
            case Qt::Key_Menu: return VK_APPS;
            default: break;
            }
            if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
            {
                return static_cast<std::uint16_t>(VK_F1 + (key - Qt::Key_F1));
            }
            if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9))
            {
                return static_cast<std::uint16_t>(key);
            }
            return 0;
        }

        std::uint8_t FromQtModifiers(Qt::KeyboardModifiers modifiers) noexcept
        {
            return static_cast<std::uint8_t>(((modifiers & Qt::ControlModifier) != 0 ? kCtrl : 0) |
                                             ((modifiers & Qt::ShiftModifier) != 0 ? kShift : 0) |
                                             ((modifiers & Qt::AltModifier) != 0 ? kAlt : 0));
        }

        QKeySequence ToSequence(KeyBinding const& binding)
        {
            int combination = ToQtKey(binding.key);
            combination |= (binding.modifiers & kCtrl) != 0 ? static_cast<int>(Qt::ControlModifier) : 0;
            combination |= (binding.modifiers & kShift) != 0 ? static_cast<int>(Qt::ShiftModifier) : 0;
            combination |= (binding.modifiers & kAlt) != 0 ? static_cast<int>(Qt::AltModifier) : 0;
            return { combination };
        }

        // A command's caption in a language, '&' before its access key, and its toolbar label.
        struct CommandText
        {
            std::wstring caption;
            std::wstring label;
        };

        CommandText TextOf(Command command, Lang const& lang)
        {
            auto const text = [](std::wstring_view caption, std::wstring_view label) {
                return CommandText{ std::wstring(caption),
                                    label.empty() ? ParseCaption(caption).text : std::wstring(label) };
            };
            switch (command)
            {
            case Command::Open: return text(lang.openDrive, lang.tbOpen);
            case Command::Reload: return text(lang.rescanDrive, lang.tbReload);
            case Command::FullRescan: return text(lang.fullRescan, {});
            case Command::RescanFolder: return text(lang.rescanFolder, {});
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
            case Command::ShowInExplorer: return text(WordingFor(lang).showInFileManager, {});
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

        // An icon from the desktop's theme, or the first of its fallbacks the theme has.
        QIcon ThemeIcon(std::initializer_list<char const*> names, QStyle::StandardPixmap fallback)
        {
            for (char const* const name : names)
            {
                QIcon const icon = QIcon::fromTheme(QString::fromLatin1(name));
                if (!icon.isNull())
                {
                    return icon;
                }
            }
            return QApplication::style()->standardIcon(fallback);
        }

        // The palette of a theme's window colours.
        QPalette PaletteOf(WindowColors const& w, bool dark)
        {
            constexpr Rgb kWhite{ 0xFF, 0xFF, 0xFF };
            constexpr Rgb kBlack{ 0x00, 0x00, 0x00 };
            Rgb const link = Legible(Legible(w.accent, w.text, w.surface, 4.5), w.text, w.background, 4.5);
            Rgb const disabled = Mix(w.secondaryText, w.background, 0.45);
            QPalette palette;
            auto const set = [&palette](QPalette::ColorRole role, Rgb color) {
                palette.setColor(QPalette::All, role, ToQColor(color));
            };
            set(QPalette::Window, w.background);
            set(QPalette::WindowText, w.text);
            set(QPalette::Base, dark ? Mix(w.background, w.surface, 0.5) : Mix(w.surface, kWhite, 0.6));
            set(QPalette::AlternateBase, w.surface);
            set(QPalette::ToolTipBase, w.surface);
            set(QPalette::ToolTipText, w.text);
            set(QPalette::PlaceholderText, w.secondaryText);
            set(QPalette::Text, w.text);
            set(QPalette::Button, w.surface);
            set(QPalette::ButtonText, w.text);
            set(QPalette::BrightText, w.accentText);
            set(QPalette::Light, Mix(w.surface, kWhite, 0.25));
            set(QPalette::Midlight, Mix(w.surface, kWhite, 0.12));
            set(QPalette::Mid, w.border);
            set(QPalette::Dark, Mix(w.background, kBlack, 0.35));
            set(QPalette::Shadow, kBlack);
            set(QPalette::Highlight, w.accent);
            set(QPalette::HighlightedText, w.accentText);
#if SPACEWINDOW_QT_API >= QT_VERSION_CHECK(6, 6, 0)
            set(QPalette::Accent, w.accent);
#endif
            set(QPalette::Link, link);
            set(QPalette::LinkVisited, link);
            for (QPalette::ColorRole const role : { QPalette::WindowText, QPalette::Text, QPalette::ButtonText })
            {
                palette.setColor(QPalette::Disabled, role, ToQColor(disabled));
            }
            return palette;
        }

        Rgb ToRgb(QColor const& color) noexcept
        {
            return { static_cast<std::uint8_t>(color.red()), static_cast<std::uint8_t>(color.green()),
                     static_cast<std::uint8_t>(color.blue()) };
        }

        Rgb TextOn(Rgb accent) noexcept
        {
            constexpr Rgb kWhite{ 0xFF, 0xFF, 0xFF };
            return Contrast(accent, Rgb{}) > Contrast(accent, kWhite) ? Rgb{} : kWhite;
        }

        // Windows 11's dark colours, for "Dark" where the desktop offers no dark palette.
        WindowColors DarkWindowColors(QColor const& accent)
        {
            Rgb const background{ 0x20, 0x20, 0x20 };
            Rgb const shown = Legible(ToRgb(accent), Rgb{ 0xFF, 0xFF, 0xFF }, background, 3.0);
            return { background, { 0x2C, 0x2C, 0x2C }, { 0xFF, 0xFF, 0xFF }, { 0xC5, 0xC5, 0xC5 },
                     shown,      TextOn(shown),        { 0x45, 0x45, 0x45 } };
        }

        // And its light ones, for "Light" where the desktop is dark and stays so.
        WindowColors LightWindowColors(QColor const& accent)
        {
            Rgb const background{ 0xF3, 0xF3, 0xF3 };
            Rgb const shown = Legible(ToRgb(accent), Rgb{}, background, 3.0);
            return { background, { 0xFB, 0xFB, 0xFB }, { 0x1B, 0x1B, 0x1B }, { 0x5D, 0x5D, 0x5D },
                     shown,      TextOn(shown),        { 0xE5, 0xE5, 0xE5 } };
        }

        bool IsDarkPalette(QPalette const& palette)
        {
            return palette.color(QPalette::Window).lightness() < 128;
        }

        // Whether the desktop says it is dark, where it says (its portal, Qt 6.5
        // and later): the AppImage on GNOME hears it, but its palette stays light.
        std::optional<bool> DesktopSaysDark()
        {
#if SPACEWINDOW_QT_API >= QT_VERSION_CHECK(6, 5, 0)
            switch (QGuiApplication::styleHints()->colorScheme())
            {
            case Qt::ColorScheme::Dark: return true;
            case Qt::ColorScheme::Light: return false;
            default: break;
            }
#endif
            return std::nullopt;
        }

        // The style the desktop gave the program (Breeze on KDE, Fusion elsewhere),
        // noted before the window first changes it.
        QString const& DesktopStyle()
        {
            static QString const style = QApplication::style()->name();
            return style;
        }

        void UseStyle(QString const& name)
        {
            if (QApplication::style()->name().compare(name, Qt::CaseInsensitive) != 0)
            {
                QApplication::setStyle(name);
            }
        }

        // Where the window's size and place are kept between runs: Qt's own
        // record of them (frame, screen and maximized size included), in the
        // XDG state folder - not settings, so not in settings.ini.
        QString PlacementFile()
        {
            QByteArray const state = qgetenv("XDG_STATE_HOME");
            QString const folder =
                state.startsWith('/') ? QFile::decodeName(state) : QDir::homePath() + QStringLiteral("/.local/state");
            return folder + QStringLiteral("/SpaceWindow/state.ini");
        }
    }

    // ---------------------------------------------------------------- setup

    MainWindow::MainWindow()
    {
        (void)DesktopStyle();  // before ApplyTheme changes it
        settings_ = LoadSettings();
        lang_ =
            &FindLanguage(settings_.language.empty() ? DefaultLanguageCode() : std::wstring_view(settings_.language));
        setAcceptDrops(true);
        setWindowIcon(QApplication::windowIcon());

        // The map, with the welcome screen and the scan's progress over it.
        auto* const central = new QWidget(this);
        auto* const column = new QVBoxLayout(central);
        column->setContentsMargins(8, 4, 8, 0);
        column->setSpacing(6);

        errorBar_ = new QFrame(central);
        errorBar_->setObjectName(QStringLiteral("ErrorBar"));
        errorBar_->setFrameShape(QFrame::StyledPanel);
        auto* const errorRow = new QHBoxLayout(errorBar_);
        errorText_ = new QLabel(errorBar_);
        errorText_->setWordWrap(true);
        errorRow->addWidget(errorText_, 1);
        auto* const errorClose = new QToolButton(errorBar_);
        errorClose->setText(QStringLiteral("✕"));
        errorClose->setAutoRaise(true);
        connect(errorClose, &QToolButton::clicked, errorBar_, &QWidget::hide);
        errorRow->addWidget(errorClose);
        errorBar_->hide();
        column->addWidget(errorBar_);

        auto* const area = new QWidget(central);
        auto* const stack = new QGridLayout(area);
        stack->setContentsMargins(0, 0, 0, 0);
        mapWidget_ = new MapWidget(area);
        stack->addWidget(mapWidget_, 0, 0);

        emptyState_ = new QWidget(area);
        auto* const empty = new QVBoxLayout(emptyState_);
        empty->setSpacing(12);
        auto* const emptyIcon = new QLabel(emptyState_);
        emptyIcon->setPixmap(QApplication::windowIcon().pixmap(96, 96));
        emptyIcon->setAlignment(Qt::AlignCenter);
        empty->addWidget(emptyIcon);
        emptyTitle_ = new QLabel(emptyState_);
        QFont titleFont = emptyTitle_->font();
        titleFont.setPointSizeF(titleFont.pointSizeF() * 1.5);
        titleFont.setBold(true);
        emptyTitle_->setFont(titleFont);
        emptyTitle_->setAlignment(Qt::AlignCenter);
        empty->addWidget(emptyTitle_);
        emptyText_ = new QLabel(emptyState_);
        emptyText_->setWordWrap(true);
        emptyText_->setAlignment(Qt::AlignCenter);
        emptyText_->setMaximumWidth(460);
        empty->addWidget(emptyText_, 0, Qt::AlignHCenter);
        emptyOpen_ = new QPushButton(emptyState_);
        emptyOpen_->setDefault(true);
        connect(emptyOpen_, &QPushButton::clicked, this, [this] { Run(Command::Open); });
        empty->addWidget(emptyOpen_, 0, Qt::AlignHCenter);
        stack->addWidget(emptyState_, 0, 0, Qt::AlignCenter);

        scanCard_ = new QFrame(area);
        scanCard_->setObjectName(QStringLiteral("ScanCard"));
        scanCard_->setFrameShape(QFrame::StyledPanel);
        scanCard_->setAutoFillBackground(true);
        scanCard_->setBackgroundRole(QPalette::AlternateBase);
        scanCard_->setFixedWidth(540);
        auto* const card = new QVBoxLayout(scanCard_);
        card->setContentsMargins(24, 20, 24, 20);
        card->setSpacing(10);
        scanTitle_ = new QLabel(scanCard_);
        QFont cardFont = scanTitle_->font();
        cardFont.setPointSizeF(cardFont.pointSizeF() * 1.3);
        cardFont.setBold(true);
        scanTitle_->setFont(cardFont);
        card->addWidget(scanTitle_);
        scanPath_ = new QLabel(scanCard_);
        scanPath_->setForegroundRole(QPalette::PlaceholderText);  // the secondary colour
        scanPath_->setMinimumWidth(1);
        scanPath_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        card->addWidget(scanPath_);
        scanBar_ = new QProgressBar(scanCard_);
        scanBar_->setTextVisible(false);
        card->addWidget(scanBar_);
        auto* const counts = new QHBoxLayout();
        counts->setSpacing(24);
        scanFiles_ = new QLabel(scanCard_);
        scanFolders_ = new QLabel(scanCard_);
        counts->addWidget(scanFiles_);
        counts->addWidget(scanFolders_);
        counts->addStretch();
        scanCancel_ = new QPushButton(scanCard_);
        connect(scanCancel_, &QPushButton::clicked, this, [this] { scanStop_.request_stop(); });
        counts->addWidget(scanCancel_);
        card->addLayout(counts);
        scanCard_->hide();
        stack->addWidget(scanCard_, 0, 0, Qt::AlignCenter);
        column->addWidget(area, 1);
        setCentralWidget(central);

        // The status bar: what is selected or shown, how big, and the unreadable folders.
        statusPath_ = new QLabel(this);
        statusPath_->setObjectName(QStringLiteral("StatusPath"));
        statusPath_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        statusPath_->setMinimumWidth(1);
        statusPath_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        statusInfo_ = new QLabel(this);
        statusInfo_->setObjectName(QStringLiteral("StatusInfo"));
        statusInfo_->setForegroundRole(QPalette::PlaceholderText);
        unreadable_ = new QPushButton(this);
        unreadable_->setFlat(true);
        unreadable_->setCursor(Qt::PointingHandCursor);
        unreadable_->setStyleSheet(QStringLiteral("QPushButton { color: palette(link); border: none; padding: 0; }"));
        unreadable_->hide();
        connect(unreadable_, &QPushButton::clicked, this, [this] {
            if (map_.Tree() != nullptr && !scanning_)
            {
                ShowUnreadable(this, *lang_, map_.Tree()->UnreadableFolders());
            }
        });
        statusBar()->addWidget(statusPath_, 1);
        statusBar()->addPermanentWidget(statusInfo_);
        statusBar()->addPermanentWidget(unreadable_);

        BuildToolbar();
        BuildShortcuts();

        mapWidget_->SetSource([this] { return Scene(); }, [this] { return Overlay(); });
        connect(mapWidget_, &MapWidget::GeometryChanged, this, &MainWindow::OnGeometryChanged);
        connect(mapWidget_, &MapWidget::PointerMoved, this, &MainWindow::OnPointerMoved);
        connect(mapWidget_, &MapWidget::PointerLeft, this, [this] {
            lastPointer_ = { -1.0, -1.0 };
            map_.EndHover();
            HideTips();
            mapWidget_->Refresh();
        });
        connect(mapWidget_, &MapWidget::Pressed, this, &MainWindow::OnPressed);
        connect(mapWidget_, &MapWidget::DoubleClicked, this, &MainWindow::OnDoubleClicked);
        connect(mapWidget_, &MapWidget::ContextMenuAt, this, [this](QPointF pixel, QPoint global) {
            if (map_.Tree() == nullptr || scanning_)
            {
                return;
            }
            if (pixel.x() >= 0.0)
            {
                map_.Select(map_.ItemAt(pixel.x(), pixel.y()));
                Redraw();
            }
            else if (map_.SelectedItem() >= 0)
            {
                // The Menu key: at the selection.
                PiePoint const anchor = map_.ItemAnchor(map_.SelectedItem());
                global = mapWidget_->mapToGlobal(QPointF(anchor.x, anchor.y) / mapWidget_->Scale()).toPoint();
            }
            ShowContextMenu(global);
        });
        connect(mapWidget_, &MapWidget::KeyPressed, this, &MainWindow::OnMapKey);

        progressTimer_.setInterval(kProgressInterval);
        connect(&progressTimer_, &QTimer::timeout, this, &MainWindow::OnProgressTick);
        nameTipTimer_.setSingleShot(true);
        connect(&nameTipTimer_, &QTimer::timeout, this, &MainWindow::ShowNameTip);
        infoTipTimer_.setSingleShot(true);
        connect(&infoTipTimer_, &QTimer::timeout, this, &MainWindow::ShowInfoTip);
        stillTimer_.setSingleShot(true);
        stillTimer_.setInterval(BackdropClock::kInactive + std::chrono::milliseconds(100));
        connect(&stillTimer_, &QTimer::timeout, this, &MainWindow::UpdateBackdrop);

        desktop_ = new Desktop(this);
        connect(desktop_, &Desktop::Changed, this, &MainWindow::OnDesktopChanged);
        highContrast_ = desktop_->HighContrast();
        LoadThemes();
        ApplyTheme();
        ApplyLanguage();
        RestorePlacement();
        map_.Configure(settings_, map_.Geometry());
        UpdateAll();
        if (!themeErrors_.empty())
        {
            QString message = Q(lang_->themeErrors);
            for (std::wstring const& error : themeErrors_)
            {
                message += QLatin1Char('\n') + Q(error);
            }
            ShowError(message);
        }
    }

    MainWindow::~MainWindow()
    {
        // No result reaches the window from here on.  Work told to stop usually
        // does within milliseconds; a thread stuck in the system (a dead network
        // drive does that) is left behind rather than holding the window.
        scanStop_.request_stop();
        std::unique_lock lock(gate_->mutex);
        gate_->open = false;
        bool const stopped = gate_->idle.wait_for(lock, kStopWait, [this] { return gate_->running == 0; });
        lock.unlock();
        bool stuck = false;
        for (Worker& worker : workers_)
        {
            lock.lock();
            bool const finished = *worker.finished;
            lock.unlock();
            if (finished)
            {
                worker.thread.join();  // it is past its last step
            }
            else
            {
                worker.thread.detach();
                stuck = true;
            }
        }
        if (stuck || !stopped)
        {
            // What it reads must outlive the window: the tree stays, unowned, until the program ends.
            [[maybe_unused]] FolderTree const* const left = map_.TakeTree().release();
            leftWorkersRunning = true;
        }
    }

    bool MainWindow::LeftWorkersRunning() noexcept
    {
        return leftWorkersRunning;
    }

    template <typename Work, typename Done>
    void MainWindow::Background(Work work, Done done)
    {
        using Result = decltype(work());
        std::uint64_t const id = ++lastWorker_;
        auto finished = std::make_shared<bool>(false);
        auto then = std::make_shared<Done>(std::move(done));
        std::shared_ptr<Gate> const gate = gate_;
        {
            std::scoped_lock const lock(gate->mutex);
            ++gate->running;
        }
        std::jthread thread([this, id, gate, work = std::move(work), then, finished]() mutable {
            auto result = std::make_shared<Result>(work());
            std::scoped_lock const lock(gate->mutex);
            if (gate->open)
            {
                // Queued on the window, which then joins this thread.
                QMetaObject::invokeMethod(
                    this,
                    [this, id, then, result] {
                        Finish(id);
                        (*then)(std::move(*result));
                    },
                    Qt::QueuedConnection);
            }
            *finished = true;
            --gate->running;
            gate->idle.notify_all();
        });
        workers_.push_back({ id, std::move(thread), std::move(finished) });
    }

    void MainWindow::Finish(std::uint64_t worker)
    {
        auto const done = std::ranges::find(workers_, worker, &Worker::id);
        if (done != workers_.end())
        {
            done->thread.join();  // it posted its result as its last step
            workers_.erase(done);
        }
    }

    void MainWindow::BuildToolbar()
    {
        toolbar_ = addToolBar(QStringLiteral("Toolbar"));
        toolbar_->setObjectName(QStringLiteral("Toolbar"));
        toolbar_->setMovable(false);
        toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toolbar_->setIconSize(QSize(20, 20));
        auto const add = [this](Command command, std::initializer_list<char const*> icons,
                                QStyle::StandardPixmap fallback, bool checkable = false) {
            auto* const action = new QAction(ThemeIcon(icons, fallback), QString(), this);
            action->setCheckable(checkable);
            connect(action, &QAction::triggered, this, [this, command] {
                Run(command);
                UpdateCommands();  // a toggle the command did not take goes back
            });
            actions_[command] = action;
            return action;
        };
        // The toolbar of 1.4.0: Open, Reload | Zoom Full, Zoom In, Zoom Out | Free Space (+ pie charts) | Run, Delete | Setup | About.
        toolbar_->addAction(add(Command::Open, { "document-open-folder", "document-open" }, QStyle::SP_DirOpenIcon));
        toolbar_->addAction(add(Command::Reload, { "view-refresh" }, QStyle::SP_BrowserReload));
        toolbar_->addSeparator();
        toolbar_->addAction(add(Command::ZoomFull, { "zoom-fit-best", "zoom-original" }, QStyle::SP_ArrowUp));
        toolbar_->addAction(add(Command::ZoomIn, { "zoom-in" }, QStyle::SP_ArrowForward));
        toolbar_->addAction(add(Command::ZoomOut, { "zoom-out" }, QStyle::SP_ArrowBack));
        toolbar_->addSeparator();
        toolbar_->addAction(
            add(Command::FreeSpace, { "drive-harddisk", "drive-harddisk-symbolic" }, QStyle::SP_DriveHDIcon, true));
        toolbar_->addAction(
            add(Command::PieChart, { "office-chart-pie", "view-statistics" }, QStyle::SP_FileDialogContentsView, true));
        toolbar_->addAction(add(Command::InvertedPie, { "office-chart-ring", "office-chart-donut", "chart-donut" },
                                QStyle::SP_FileDialogInfoView, true));
        toolbar_->addSeparator();
        toolbar_->addAction(add(Command::Run, { "system-run", "media-playback-start" }, QStyle::SP_MediaPlay));
        toolbar_->addAction(add(Command::Delete, { "user-trash", "edit-delete" }, QStyle::SP_TrashIcon));
        toolbar_->addSeparator();
        toolbar_->addAction(
            add(Command::Settings, { "configure", "preferences-system" }, QStyle::SP_FileDialogDetailedView));
        toolbar_->addAction(add(Command::About, { "help-about" }, QStyle::SP_MessageBoxInformation));
        // The rest, as the WinUI command bar's overflow: in a menu at the end.
        add(Command::Properties, { "document-properties" }, QStyle::SP_FileDialogInfoView);
        add(Command::ShowInExplorer, { "system-file-manager", "folder-open" }, QStyle::SP_DirIcon);
        add(Command::CopyPath, { "edit-copy" }, QStyle::SP_FileIcon);
        add(Command::FullRescan, { "view-refresh" }, QStyle::SP_BrowserReload);
        auto* const more = new QToolButton(toolbar_);
        more->setIcon(
            ThemeIcon({ "application-menu", "open-menu-symbolic", "overflow-menu" }, QStyle::SP_TitleBarUnshadeButton));
        more->setPopupMode(QToolButton::InstantPopup);
        auto* const moreMenu = new QMenu(more);
        moreMenu->addAction(actions_[Command::Properties]);
        moreMenu->addAction(actions_[Command::ShowInExplorer]);
        moreMenu->addAction(actions_[Command::CopyPath]);
        more->setMenu(moreMenu);
        toolbar_->addWidget(more);
    }

    void MainWindow::BuildShortcuts()
    {
        // The window's keys, from the table Settings lists them from; the map's own come from it too (OnMapKey).
        for (KeyBinding const& binding : KeyBindings())
        {
            if (binding.map)
            {
                continue;
            }
            auto* const shortcut = new QShortcut(ToSequence(binding), this);
            shortcut->setContext(Qt::WindowShortcut);
            Command const command = binding.command;
            connect(shortcut, &QShortcut::activated, this, [this, command] { Run(command); });
        }
    }

    // ---------------------------------------------------------------- chrome

    void MainWindow::LoadThemes()
    {
        themeErrors_.clear();
        themes_ = sm::LoadThemes(shell::ThemeFolders(), themeErrors_);
    }

    // The application's palette, not the window's: widgets take a new one only
    // once the events it sends them are handled (Qt 6.10).
    bool MainWindow::IsDark() const
    {
        return IsDarkPalette(QApplication::palette());
    }

    // The classic themes are the desktop's look - its style and its palette, as
    // the Windows build uses Windows' own - in the light or dark the theme asks
    // for, where the desktop has it.  The other themes bring their own colours,
    // which Fusion takes whole on every desktop.
    void MainWindow::ApplyTheme()
    {
        theme_ = FindTheme(themes_, ShownThemeId(Picked().theme, highContrast_));
        bool const system = theme_.classic && theme_.base == ThemeBase::System;
        bool const dark = theme_.base == ThemeBase::Dark;
#if SPACEWINDOW_QT_API >= QT_VERSION_CHECK(6, 8, 0)
        // Desktops that can (GNOME, Windows) switch the program's palette between light and dark.
        int const scheme = system ? 0 : (dark ? 2 : 1);
        if (scheme != requestedScheme_)
        {
            requestedScheme_ = scheme;
            QStyleHints* const hints = QGuiApplication::styleHints();
            if (system)
            {
                hints->unsetColorScheme();
            }
            else
            {
                hints->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
            }
        }
#endif
        UseStyle(DesktopStyle());
        QApplication::setPalette(QPalette());  // the desktop's
        QPalette const desktop = QApplication::palette();
        // The light or dark asked for: "Use system setting" asks for the desktop's, where it says.
        std::optional<bool> const wantDark = system ? DesktopSaysDark() : std::optional<bool>(dark);
        if (!theme_.classic || (wantDark && IsDarkPalette(desktop) != *wantDark))
        {
            // A colour theme, or light or dark where the desktop's palette (KDE's, say) keeps to the other.
            bool const shownDark = theme_.classic ? *wantDark : dark;
            QColor const accent = desktop.color(QPalette::Highlight);
            WindowColors const colors =
                !theme_.classic ? theme_.window : (shownDark ? DarkWindowColors(accent) : LightWindowColors(accent));
            UseStyle(QStringLiteral("Fusion"));
            QApplication::setPalette(PaletteOf(colors, shownDark));
        }
        // WinUI's error bar colours, in the window's light or dark.
        errorBar_->setStyleSheet(QStringLiteral("QFrame#ErrorBar { background: %1; border-radius: 4px; }")
                                     .arg(IsDark() ? QStringLiteral("#442726") : QStringLiteral("#fde7e9")));
        mapWidget_->InvalidateBase();
    }

    void MainWindow::changeEvent(QEvent* event)
    {
        QMainWindow::changeEvent(event);
        switch (event->type())
        {
        case QEvent::ThemeChange:
            if (theme_.classic)
            {
                ApplyTheme();  // the desktop changed its look, or switched between light and dark
            }
            UpdateBackdrop();  // its animation effects may have been switched too
            break;
        case QEvent::ActivationChange:
            if (isActiveWindow())
            {
                stillTimer_.stop();
            }
            else
            {
                backdropClock_.Deactivated(std::chrono::steady_clock::now());
                stillTimer_.start();
            }
            UpdateBackdrop();
            break;
        case QEvent::WindowStateChange:
        case QEvent::StyleChange: UpdateBackdrop(); break;
        default: break;
        }
    }

    void MainWindow::ApplyLanguage()
    {
        Lang const& lang = *lang_;
        for (auto const& [command, action] : actions_)
        {
            CommandText const text = TextOf(command, lang);
            action->setText(Q(text.caption));
            action->setIconText(Q(text.label));
            // Its keys, from the command table.  Zoom In's is Enter (Activate).
            Command const keys = command == Command::ZoomIn ? Command::Activate : command;
            std::wstring const shortcut = CommandKeys(keys, lang);
            QString tip = Q(ParseCaption(text.caption).text);
            if (!shortcut.empty())
            {
                tip += QStringLiteral(" (") + Q(shortcut) + QLatin1Char(')');
            }
            action->setToolTip(tip);
        }
        mapWidget_->setAccessibleName(Q(WordingFor(lang).mapName));
        emptyTitle_->setText(Q(lang.emptyTitle));
        emptyText_->setText(Q(lang.emptyText));
        emptyOpen_->setText(Q(ParseCaption(lang.openDrive).text));
        scanTitle_->setText(Q(lang.scanning));
        scanCancel_->setText(Q(lang.cancel));
        mapWidget_->InvalidateBase();
        UpdateAll();
    }

    void MainWindow::UpdateAll()
    {
        UpdateCommands();
        UpdateStatus();
        UpdateTitle();
        UpdateBackdrop();
    }

    BackdropConditions MainWindow::Conditions() const
    {
        BackdropConditions conditions;
        conditions.picked = Picked().backdrop;
        conditions.room = map_.PieMode() || (map_.Tree() == nullptr && !map_.HasPreview());
        conditions.visible = isVisible() && !isMinimized();
        conditions.animations = QApplication::style()->styleHint(QStyle::SH_Widget_Animation_Duration) != 0;
        conditions.highContrast = highContrast_;
        conditions.active = isActiveWindow();
        conditions.powerSaving = desktop_ != nullptr && desktop_->PowerSaving();
        return conditions;
    }

    // Plays the background animation, holds it still, or takes it away, as
    // the window and the desktop are now.
    void MainWindow::UpdateBackdrop()
    {
        if (mapWidget_ == nullptr)
        {
            return;
        }
        BackdropConditions const conditions = Conditions();
        auto const now = std::chrono::steady_clock::now();
        Backdrop const shown = BackdropClock::Shown(conditions);
        (void)backdropClock_.Time(conditions, now);  // a pause starts, or ends, now
        mapWidget_->SetAnimating(shown != Backdrop::None && !backdropClock_.Paused(conditions, now));
        if (shown != backdropShown_)
        {
            backdropShown_ = shown;
            mapWidget_->Refresh();  // with it, the map is drawn on nothing; without it, on the window's colour
            // The welcome screen's words, light over the dark animation.
            QPalette words;
            if (shown != Backdrop::None)
            {
                words.setColor(QPalette::WindowText, QColor(0xF2, 0xF2, 0xF2));
                words.setColor(QPalette::PlaceholderText, QColor(0xC8, 0xC8, 0xC8));
            }
            emptyTitle_->setPalette(words);
            emptyText_->setPalette(words);
        }
    }

    void MainWindow::OnDesktopChanged()
    {
        if (bool const contrast = desktop_->HighContrast(); contrast != highContrast_)
        {
            // High contrast puts the desktop's own look over any theme, as Windows does.
            highContrast_ = contrast;
            ApplyTheme();
            Redraw();
        }
        UpdateBackdrop();
    }

    void MainWindow::showEvent(QShowEvent* event)
    {
        QMainWindow::showEvent(event);
        UpdateBackdrop();
    }

    void MainWindow::hideEvent(QHideEvent* event)
    {
        QMainWindow::hideEvent(event);
        UpdateBackdrop();
    }

    void MainWindow::UpdateCommands()
    {
        for (auto const& [command, action] : actions_)
        {
            action->setEnabled(CanRun(command));
        }
        emptyOpen_->setEnabled(CanRun(Command::Open));
        actions_[Command::FreeSpace]->setChecked(map_.ShowFreeSpace());
        actions_[Command::PieChart]->setChecked(settings_.viewMode == ViewMode::Pie);
        actions_[Command::InvertedPie]->setChecked(settings_.viewMode == ViewMode::InvertedPie);
    }

    void MainWindow::UpdateStatus()
    {
        Lang const& lang = *lang_;
        FolderTree const* const tree = map_.Tree();
        bool const unreadable = tree != nullptr && tree->unreadableFolders != 0;
        unreadable_->setVisible(unreadable);
        if (unreadable)
        {
            std::wstring const count = FormatCount(tree->unreadableFolders, lang);
            unreadable_->setText(Q(FormatText(lang.unreadableFolders, { count })));
        }
        statusPath_->setText(Q(map_.StatusPath()));
        statusPath_->setToolTip(statusPath_->text());
        statusInfo_->setText(Q(map_.StatusInfo(lang)));
    }

    void MainWindow::UpdateTitle()
    {
        setWindowTitle(Q(map_.Title(*lang_)));
    }

    void MainWindow::ShowError(QString const& message)
    {
        errorText_->setText(message);
        errorBar_->show();
    }

    // Saved always, restored when Settings asks for it, as on Windows.  Qt puts a
    // window that would be off every screen now back on one; on Wayland, where
    // programs cannot place their windows, the size and maximized state return
    // and the desktop chooses the place.
    void MainWindow::RestorePlacement()
    {
        resize(kDefaultWidth, kDefaultHeight);
        if (settings_.savePosition)
        {
            QSettings const state(PlacementFile(), QSettings::IniFormat);
            (void)restoreGeometry(QByteArray::fromBase64(state.value(QStringLiteral("Window/Geometry")).toByteArray()));
        }
    }

    void MainWindow::SavePlacement()
    {
        QSettings state(PlacementFile(), QSettings::IniFormat);
        state.setValue(QStringLiteral("Window/Geometry"), QString::fromLatin1(saveGeometry().toBase64()));
    }

    void MainWindow::closeEvent(QCloseEvent* event)
    {
        scanStop_.request_stop();
        SavePlacement();
        settings_.Validate();
        (void)SaveSettings(settings_);
        QMainWindow::closeEvent(event);
    }

    // ---------------------------------------------------------------- commands

    bool MainWindow::CanRun(Command command) const
    {
        return map_.CanRun(command, CommandContext{ scanning_ });
    }

    void MainWindow::Run(Command command)
    {
        if (!CanRun(command))
        {
            return;
        }
        switch (command)
        {
        case Command::Open: OpenDriveOrFolder(); break;
        case Command::Reload: Rescan(); break;
        case Command::FullRescan: StartScan(lastPath_); break;
        case Command::RescanFolder: RescanFolder(map_.RescanTarget()); break;
        case Command::ZoomFull: Animate(map_.ZoomFull()); break;
        case Command::ZoomIn: Animate(map_.ZoomIntoSelection()); break;
        case Command::ZoomOut: Animate(map_.ZoomOut()); break;
        case Command::GoDown: Animate(map_.GoDown()); break;
        case Command::GoBack: Animate(map_.GoBack()); break;
        case Command::GoForward: Animate(map_.GoForward()); break;
        case Command::FreeSpace:
            map_.SetShowFreeSpace(!map_.ShowFreeSpace());
            Redraw();
            break;
        case Command::PieChart:
        case Command::InvertedPie:
        {
            // Each a toggle back to the treemap.
            ViewMode const mode = command == Command::PieChart ? ViewMode::Pie : ViewMode::InvertedPie;
            SetViewMode(settings_.viewMode == mode ? ViewMode::Treemap : mode);
            break;
        }
        case Command::Run: RunSelected(); break;
        case Command::Delete: DeleteSelected(); break;
        case Command::Settings: ShowSettings(); break;
        case Command::About: ShowAbout(this, *lang_); break;
        case Command::Properties: shell::ShowProperties(map_.SelectedPath()); break;
        case Command::ShowInExplorer: shell::ShowInFileManager(map_.SelectedPath()); break;
        case Command::CopyPath: CopySelectedPath(); break;
        case Command::Activate:
            // As a double-click: a folder zooms in, a file runs or opens.
            if (map_.SelectedEntry()->IsFolder())
            {
                Animate(map_.ZoomIntoSelection());
            }
            else
            {
                RunSelected();
            }
            break;
        case Command::ClearSelection:
            map_.Select(-1);
            Redraw();
            break;
        case Command::ContextMenu:
        {
            QPoint global = mapWidget_->mapToGlobal(QPoint(8, 8));
            if (map_.SelectedItem() >= 0)
            {
                PiePoint const anchor = map_.ItemAnchor(map_.SelectedItem());
                global = mapWidget_->mapToGlobal(QPointF(anchor.x, anchor.y) / mapWidget_->Scale()).toPoint();
            }
            ShowContextMenu(global);
            break;
        }
        case Command::SelectNearest: break;  // takes a direction: OnMapKey moves the selection
        }
    }

    void MainWindow::Animate(std::optional<ZoomMotion> const& motion)
    {
        HideTips();
        if (motion && QApplication::style()->styleHint(QStyle::SH_Widget_Animation_Duration) != 0)
        {
            mapWidget_->StartZoom(*motion);
        }
        mapWidget_->InvalidateBase();
        UpdateAll();
    }

    void MainWindow::Redraw()
    {
        mapWidget_->InvalidateBase();
        UpdateAll();
    }

    void MainWindow::SetViewMode(ViewMode mode)
    {
        if (settings_.viewMode != mode)
        {
            // The selection and the zoomed-in folder carry over; only the drawing changes.
            settings_.viewMode = mode;
            mapWidget_->StopZoom();
            map_.Configure(settings_, map_.Geometry());
        }
        Redraw();
    }

    void MainWindow::OpenDriveOrFolder()
    {
        std::wstring const current = map_.Tree() != nullptr ? map_.Tree()->Volume().root : std::wstring();
        if (std::optional<std::wstring> const path = PickDriveOrFolder(this, *lang_, current))
        {
            StartScan(*path);
        }
    }

    void MainWindow::RunSelected()
    {
        if (map_.SelectedEntry() != nullptr)
        {
            (void)shell::Open(map_.SelectedPath());
        }
    }

    void MainWindow::CopySelectedPath()
    {
        QApplication::clipboard()->setText(Q(map_.SelectedPath()));
    }

    void MainWindow::DeleteSelected()
    {
        Entry const* const entry = map_.SelectedEntry();
        if (entry == nullptr)
        {
            return;
        }
        Lang const& lang = *lang_;
        Wording const& wording = WordingFor(lang);
        MapView::Selection const selection = map_.CurrentSelection();
        std::wstring const path = map_.SelectedPath();
        std::wstring const name(selection.folder->Name(selection.index));
        std::wstring const size = FormatSize(entry->size, lang);

        // 1.4.0 deleted without asking; confirm first, with "Cancel" the default.
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(Q(wording.confirmDeleteTitle));
        box.setText(Q(FormatText(wording.confirmDelete, { name, size })));
        QPushButton const* const remove = box.addButton(Q(ParseCaption(lang.del).text), QMessageBox::DestructiveRole);
        QPushButton* const cancel = box.addButton(Q(lang.cancel), QMessageBox::RejectRole);
        box.setDefaultButton(cancel);
        box.exec();
        if (box.clickedButton() != remove)
        {
            return;
        }
        if (!shell::Trash(path))
        {
            ShowError(Q(FormatText(wording.deleteFailed, { name })));
            return;
        }
        if (settings_.autoRescan)
        {
            Rescan();
            return;
        }
        HideTips();
        map_.RemoveSelected();
        Redraw();
    }

    void MainWindow::ShowSettings()
    {
        LoadThemes();  // theme files may have come and gone
        Settings edited = settings_;
        SettingsHooks hooks;
        hooks.preview = [this](Settings const& tried) { Try(tried); };
        hooks.reloadThemes = [this](std::vector<std::wstring>& errors) {
            LoadThemes();
            errors = themeErrors_;
            return themes_;
        };
        bool const accepted = EditSettings(this, *lang_, edited, themes_, themeErrors_, hooks);
        trying_.reset();
        if (accepted)
        {
            edited.viewMode = settings_.viewMode;
            edited.window = settings_.window;
            settings_ = edited;
            (void)SaveSettings(settings_);
        }
        lang_ =
            &FindLanguage(settings_.language.empty() ? DefaultLanguageCode() : std::wstring_view(settings_.language));
        ApplyTheme();
        ApplyLanguage();
        map_.Configure(settings_, map_.Geometry());
        Redraw();
    }

    // The window as it would look with what is picked in Settings: theme,
    // language and animation.  Settings recolours itself with the window.
    void MainWindow::Try(Settings const& tried)
    {
        trying_ = tried;
        lang_ = &FindLanguage(tried.language.empty() ? DefaultLanguageCode() : std::wstring_view(tried.language));
        ApplyTheme();
        ApplyLanguage();
    }

    void MainWindow::ShowContextMenu(QPoint global)
    {
        Lang const& lang = *lang_;
        Entry const* const entry = map_.SelectedEntry();
        bool const folder = entry != nullptr && entry->IsFolder();
        QMenu menu(this);
        auto const add = [&](Command command, bool emphasized) {
            QAction* const action = actions_[command];
            action->setEnabled(CanRun(command));
            menu.addAction(action);
            if (emphasized)
            {
                menu.setDefaultAction(action);  // 1.4.0 made the double-click action the menu's default (bold) item
            }
        };
        add(Command::ZoomIn, folder);
        add(Command::ZoomOut, false);
        add(Command::ZoomFull, false);
        menu.addSeparator();
        add(Command::Run, entry != nullptr && !folder);
        add(Command::Delete, false);
        menu.addSeparator();
        add(Command::Open, false);
        add(Command::Reload, false);
        add(Command::FullRescan, false);
        if (Folder const* const target = map_.RescanTarget(); target != nullptr && target->parent != nullptr)
        {
            // The folder's name inside a caption, where '&' marks an access key.
            std::wstring name(target->parent->Name(target->indexInParent));
            for (std::size_t at = name.find(L'&'); at != std::wstring::npos; at = name.find(L'&', at + 2))
            {
                name.insert(at, 1, L'&');
            }
            QAction* const rescan = menu.addAction(ThemeIcon({ "view-refresh" }, QStyle::SP_BrowserReload),
                                                   Q(FormatText(lang.rescanFolder, { name })));
            rescan->setEnabled(CanRun(Command::RescanFolder));
            connect(rescan, &QAction::triggered, this, [this] { Run(Command::RescanFolder); });
        }
        add(Command::FreeSpace, false);
        add(Command::PieChart, false);
        add(Command::InvertedPie, false);
        menu.addSeparator();
        add(Command::Properties, false);
        add(Command::ShowInExplorer, false);
        add(Command::CopyPath, false);
        menu.exec(global);
        UpdateCommands();
    }

    // ---------------------------------------------------------------- the map

    MapScene MainWindow::Scene() const
    {
        MapScene scene;
        scene.tree = map_.Shown();
        if (map_.PieMode())
        {
            scene.pie = &map_.Pie();
        }
        else
        {
            scene.layout = &map_.Treemap();
        }
        scene.settings = &map_.CurrentSettings();
        scene.lang = lang_;
        scene.theme = &theme_;
        scene.darkTheme = IsDark();
        scene.colors = ThemeMapColors(theme_, scene.darkTheme);
        QColor const window = QApplication::palette().color(QPalette::Window);
        scene.background =
            theme_.classic ? Rgb{ static_cast<std::uint8_t>(window.red()), static_cast<std::uint8_t>(window.green()),
                                  static_cast<std::uint8_t>(window.blue()) }
                           : theme_.window.background;
        BackdropConditions const conditions = Conditions();
        scene.backdrop = BackdropClock::Shown(conditions);
        scene.time = backdropClock_.Time(conditions, std::chrono::steady_clock::now());
        return scene;
    }

    MapOverlay MainWindow::Overlay() const
    {
        MapOverlay overlay;
        if (!map_.HasPreview())
        {
            overlay.selected = map_.SelectedItem();
            if (map_.CurrentSettings().rolloverBox || map_.PieMode())
            {
                overlay.highlighted = map_.Highlighted();
            }
        }
        overlay.accent =
            theme_.classic ? QApplication::palette().color(QPalette::Highlight) : ToQColor(theme_.window.accent);
        return overlay;
    }

    void MainWindow::OnGeometryChanged()
    {
        MapRenderer const& renderer = mapWidget_->Renderer();
        map_.Configure(settings_, MapGeometry{ mapWidget_->PixelWidth(), mapWidget_->PixelHeight(), mapWidget_->Scale(),
                                               renderer.BorderWidth(), renderer.HeaderHeight() });
        HideTips();
        mapWidget_->InvalidateBase();
    }

    void MainWindow::OnPointerMoved(QPointF pixel)
    {
        if (map_.Tree() == nullptr || scanning_)
        {
            return;
        }
        QPointF const whole(std::floor(pixel.x()), std::floor(pixel.y()));
        if (whole == lastPointer_)
        {
            return;
        }
        lastPointer_ = whole;
        lastPointerDips_ = pixel / mapWidget_->Scale();
        if (map_.Hover(whole.x(), whole.y()))
        {
            mapWidget_->Refresh();
        }
        // As with 1.4.0's tips, any movement hides them and restarts their delay.
        HideTips();
        if (map_.HoverItem() >= 0)
        {
            if (settings_.showNameTips)
            {
                nameTipTimer_.start(std::max(1, settings_.nameTipDelay));
            }
            if (settings_.showInfoTips)
            {
                infoTipTimer_.start(std::max(1, settings_.infoTipDelay));
            }
        }
    }

    void MainWindow::OnPressed(QPointF pixel)
    {
        if (map_.Tree() == nullptr || scanning_)
        {
            return;
        }
        HideTips();
        map_.Select(map_.ItemAt(pixel.x(), pixel.y()));
        mapWidget_->Refresh();
        UpdateAll();
    }

    void MainWindow::OnDoubleClicked(QPointF pixel)
    {
        if (map_.Tree() == nullptr || scanning_)
        {
            return;
        }
        if (map_.PieMode())
        {
            PieLayout const& pie = map_.Pie();
            int const hit = pie.HitTest(pixel.x(), pixel.y());
            if (pie.HubContains(pixel.x(), pixel.y()) ||
                (hit >= 0 && pie.Items()[static_cast<std::size_t>(hit)].IsFolded()))
            {
                // The hub is the current folder: double-clicking it goes up a level.  A
                // slice of grouped small entries zooms into their folder to show them.
                Animate(hit >= 0 ? map_.ZoomIn(hit) : map_.ZoomOut());
                return;
            }
        }
        // As in 1.4.0: double-clicking a folder zooms in, a file runs or opens.
        int const item = map_.ItemAt(pixel.x(), pixel.y());
        map_.Select(item);
        if (item < 0)
        {
            Redraw();
            return;
        }
        if (map_.Item(item).IsFolder())
        {
            Animate(map_.ZoomIn(item));
        }
        else
        {
            mapWidget_->Refresh();
            UpdateAll();
            RunSelected();
        }
    }

    void MainWindow::OnMapKey(int key, Qt::KeyboardModifiers modifiers, bool& handled)
    {
        std::uint16_t const code = FromQtKey(key);
        std::optional<Command> const command =
            code != 0 ? CommandForKey(code, FromQtModifiers(modifiers), true) : std::nullopt;
        if (!command || !CanRun(*command))
        {
            return;  // not the map's (the window's shortcuts take Alt+arrows), or nothing to do
        }
        handled = true;
        if (*command != Command::SelectNearest)
        {
            Run(*command);
            return;
        }
        Direction direction = Direction::Right;
        switch (code)
        {
        case VK_LEFT: direction = Direction::Left; break;
        case VK_UP: direction = Direction::Up; break;
        case VK_DOWN: direction = Direction::Down; break;
        default: break;
        }
        map_.MoveSelection(direction);
        HideTips();
        mapWidget_->Refresh();
        UpdateAll();
    }

    // ---------------------------------------------------------------- tips

    void MainWindow::HideTips()
    {
        nameTipTimer_.stop();
        infoTipTimer_.stop();
        mapWidget_->HideTips();
    }

    // A name that does not fit its box pops up over the box (1.4.0's "name
    // tips"); in the pie chart, over the middle of the slice.
    void MainWindow::ShowNameTip()
    {
        int const hover = map_.HoverItem();
        if (map_.Tree() == nullptr || hover < 0 || !std::cmp_less(hover, map_.ItemCount()) ||
            !map_.Item(hover).IsSelectable())
        {
            return;
        }
        MapScene const scene = Scene();
        MapRenderer& renderer = mapWidget_->Renderer();
        bool const selected = hover == map_.SelectedItem();
        bool const highlighted = std::ranges::find(map_.Highlighted(), hover) != map_.Highlighted().end();
        double const scale = mapWidget_->Scale();
        BoxColors colors;
        QPointF at;
        if (map_.PieMode())
        {
            PieSlice const& slice = map_.Pie().Items()[static_cast<std::size_t>(hover)];
            if (!renderer.IsSliceLabelTruncated(scene, slice))
            {
                return;
            }
            colors = renderer.SliceColors(scene, slice, highlighted, selected);
            PiePoint const anchor = map_.Pie().Centroid(slice);
            at = QPointF(anchor.x, anchor.y) / scale;
        }
        else
        {
            DisplayItem const& item = map_.Treemap().Items()[static_cast<std::size_t>(hover)];
            if (!renderer.IsLabelTruncated(scene, item))
            {
                return;
            }
            colors = renderer.ItemColors(scene, item, highlighted, selected);
            double const line = renderer.LineWidth();
            at = QPointF((item.x + line) / scale,
                         item.IsFolder() ? (item.y + line) / scale : (item.y + item.h / 2.0) / scale - 8.0);
        }
        MapItem const& item = map_.Item(hover);
        mapWidget_->ShowNameTip(Q(item.source->Name(item.index)), ToQColor(colors.face), ToQColor(scene.colors.outline),
                                ToQColor(TextColor(scene.colors, colors.face)), at, map_.PieMode());
    }

    void MainWindow::ShowInfoTip()
    {
        std::optional<InfoTip> const tip = map_.InfoTipOf(map_.HoverItem(), *lang_);
        if (!tip)
        {
            return;
        }
        QIcon const icon = tip->iconPath.empty() ? QIcon() : shell::IconOf(tip->iconPath);
        mapWidget_->ShowInfoTip(tip->showName ? Q(tip->name) : QString(),
                                tip->showPath ? Q(tip->folderPath) : QString(), Q(tip->details), icon,
                                lastPointerDips_ + QPointF(16.0, 16.0));
    }

    // ---------------------------------------------------------------- scanning

    void MainWindow::StartScan(std::wstring const& path)
    {
        if (scanning_ || path.empty())
        {
            return;
        }
        // Rescanning the same drive keeps the view zoomed where it was (1.4.0 zoomed all the way out).
        bool const again = map_.Tree() != nullptr && NormalizeScanPath(path) == map_.Tree()->RootPath();
        std::vector<std::wstring> viewPath = again ? map_.ViewPath() : std::vector<std::wstring>();
        // Reloads then list again only the folders that changed - unless the tree
        // proved to have more folders than the system lets a program watch.
        bool const watch = !(again && map_.Tree()->tooBigToWatch);

        scanning_ = true;
        scanStop_ = std::stop_source{};
        auto const progress = std::make_shared<ScanProgress>();
        scanProgress_ = progress;
        progressTicks_ = 0;
        HideTips();
        errorBar_->hide();
        ShowScanOverlay(path);
        UpdateCommands();
        progressTimer_.start();

        struct Outcome
        {
            ScanResult result;
            bool failed = false;
        };
        std::stop_token const stop = scanStop_.get_token();
        Background(
            [path, progress, stop, watch] {
                Outcome outcome;
                try
                {
                    ScanOptions options;
                    options.watch = watch;
                    outcome.result = ScanFolder(path, *progress, stop, options);
                    if (outcome.result.tree && !watch)
                    {
                        outcome.result.tree->tooBigToWatch = true;
                    }
                }
                catch (...)
                {
                    outcome.failed = true;  // e.g. out of memory
                }
                return outcome;
            },
            [this, path, again, viewPath = std::move(viewPath)](Outcome outcome) {
                progressTimer_.stop();
                scanning_ = false;
                scanProgress_.reset();
                HideScanOverlay();
                if (!outcome.failed && outcome.result.status == ScanStatus::Completed)
                {
                    if (!again)
                    {
                        map_.History().Clear();  // where the view went elsewhere means nothing here
                    }
                    lastPath_ = outcome.result.tree->RootPath();
                    map_.SetTree(std::move(outcome.result.tree), viewPath);
                    Shown();
                }
                else
                {
                    map_.ClearPreview();  // back to the tree there was, if any
                    if (outcome.failed || outcome.result.status == ScanStatus::NotFound)
                    {
                        ShowError(Q(FormatText(lang_->scanFailed, { path })));
                    }
                }
                emptyState_->setVisible(map_.Tree() == nullptr);
                Redraw();
                mapWidget_->setFocus(Qt::OtherFocusReason);
            });
    }

    void MainWindow::Shown()
    {
        if (capture_.isEmpty())
        {
            return;
        }
        QString const file = std::exchange(capture_, QString());
        QTimer::singleShot(500, this, [this, file] {
            (void)grab().save(file);
            close();
        });
    }

    void MainWindow::CaptureWhenShown(QString file)
    {
        capture_ = std::move(file);
    }

    // Brings the tree up to date: from what its watch saw change, or by scanning again.
    void MainWindow::Rescan()
    {
        FolderTree const* const tree = map_.Tree();
        if (tree == nullptr)
        {
            return;
        }
        if (tree->watch)
        {
            UpdateFromChanges();
        }
        else
        {
            StartScan(lastPath_);
        }
    }

    // Only the folders the watch saw change are listed again and patched into
    // the tree: a moment, where scanning takes seconds.  When the watch lost
    // track (too much changed at once), everything is scanned again.
    void MainWindow::UpdateFromChanges()
    {
        std::wstring const path = lastPath_;
        FolderTree* const tree = map_.Tree();  // only read until the update is applied, here
        FolderWatch::Changes changes = tree->watch->Take();
        if (!changes.complete)
        {
            tree->tooBigToWatch = tree->watch->OutOfWatches();  // folders came: past what may be watched
            StartScan(path);
            return;
        }
        scanning_ = true;
        scanStop_ = std::stop_source{};
        HideTips();
        errorBar_->hide();
        UpdateCommands();
        // A quick update does without the progress card, which would only flash.
        auto* const quiet = new QTimer(this);
        quiet->setSingleShot(true);
        connect(quiet, &QTimer::timeout, this, [this] {
            if (scanning_ && !scanProgress_)
            {
                ShowScanOverlay(lastPath_, false);
            }
        });
        quiet->start(kQuietUpdate);

        struct Outcome
        {
            std::optional<FolderReload> reload;
            bool failed = false;
        };
        std::stop_token const stop = scanStop_.get_token();
        Background(
            [tree = static_cast<FolderTree const*>(tree), changes = std::move(changes), stop] {
                Outcome outcome;
                try
                {
                    ScanProgress progress;  // counts what is found in folders new to the tree
                    outcome.reload = PrepareReload(*tree, changes.folders, changes.files, progress, stop);
                }
                catch (...)
                {
                    outcome.failed = true;  // e.g. out of memory: scan instead
                }
                return outcome;
            },
            [this, path, quiet](Outcome outcome) {
                quiet->stop();
                quiet->deleteLater();
                bool scan = outcome.failed;
                if (outcome.reload)
                {
                    // Folders the view or the selection point to may go: both are found
                    // again by name, as after a scan.
                    std::vector<std::wstring> const viewPath = map_.ViewPath();
                    std::unique_ptr<FolderTree> updated = map_.TakeTree();
                    try
                    {
                        ApplyReload(*updated, std::move(*outcome.reload));
                    }
                    catch (...)
                    {
                        scan = true;  // half applied: the totals cannot be trusted
                    }
                    map_.SetTree(std::move(updated), viewPath);
                }
                else if (!outcome.failed)
                {
                    map_.Tree()->watch.reset();  // cancelled: the changes taken are lost, the next reload scans
                }
                scanning_ = false;
                HideScanOverlay();
                Redraw();
                Shown();
                mapWidget_->setFocus(Qt::OtherFocusReason);
                if (scan)
                {
                    StartScan(path);
                }
            });
    }

    // Scans one folder again, everything below it - its place in the tree, and
    // everything else, stays as it is.
    void MainWindow::RescanFolder(Folder* folder)
    {
        FolderTree const* const tree = map_.Tree();
        if (tree == nullptr || folder == nullptr)
        {
            return;
        }
        std::wstring const path = tree->PathOf(*folder);
        scanning_ = true;
        scanStop_ = std::stop_source{};
        auto const progress = std::make_shared<ScanProgress>();
        scanProgress_ = progress;
        HideTips();
        errorBar_->hide();
        ShowScanOverlay(path);
        UpdateCommands();
        progressTimer_.start();

        struct Outcome
        {
            std::unique_ptr<Folder> fresh;
            bool failed = false;
        };
        std::stop_token const stop = scanStop_.get_token();
        Background(
            [tree, folder, progress, stop] {
                Outcome outcome;
                try
                {
                    outcome.fresh = ScanAgain(*tree, *folder, *progress, stop);
                }
                catch (...)
                {
                    outcome.failed = true;
                }
                return outcome;
            },
            [this, folder, path](Outcome outcome) {
                progressTimer_.stop();
                scanning_ = false;
                scanProgress_.reset();
                HideScanOverlay();
                bool scan = false;
                if (outcome.failed)
                {
                    ShowError(Q(FormatText(lang_->scanFailed, { path })));
                }
                else if (outcome.fresh)
                {
                    std::vector<std::wstring> const viewPath = map_.ViewPath();
                    std::unique_ptr<FolderTree> updated = map_.TakeTree();
                    try
                    {
                        ReplaceContents(*updated, *folder, std::move(outcome.fresh));
                    }
                    catch (...)
                    {
                        scan = true;  // half replaced: the totals cannot be trusted
                    }
                    map_.SetTree(std::move(updated), viewPath);
                }
                Redraw();
                mapWidget_->setFocus(Qt::OtherFocusReason);
                if (scan)
                {
                    StartScan(lastPath_);
                }
            });
    }

    void MainWindow::ShowScanOverlay(std::wstring const& path, bool counts)
    {
        Lang const& lang = *lang_;
        scanPath_->setText(Q(path));
        scanFiles_->setText(Q(Join(lang.filesFound, L" 0")));
        scanFolders_->setText(Q(Join(lang.foldersFound, L" 0")));
        scanFiles_->setVisible(counts);
        scanFolders_->setVisible(counts);
        scanBar_->setRange(0, 0);  // indeterminate until the drive's used space is known
        emptyState_->hide();
        auto* const stack = static_cast<QGridLayout*>(scanCard_->parentWidget()->layout());
        stack->setAlignment(scanCard_, Qt::AlignCenter);
        scanCard_->show();
        scanCard_->raise();
        scanCancel_->setFocus();
    }

    void MainWindow::HideScanOverlay()
    {
        scanCard_->hide();
        emptyState_->setVisible(map_.Tree() == nullptr);
    }

    void MainWindow::OnProgressTick()
    {
        std::shared_ptr<ScanProgress> const progress = scanProgress_;
        if (!progress)
        {
            return;
        }
        Lang const& lang = *lang_;
        scanFiles_->setText(
            Q(Join(lang.filesFound, L" ", FormatCount(progress->files.load(std::memory_order_relaxed), lang))));
        scanFolders_->setText(
            Q(Join(lang.foldersFound, L" ", FormatCount(progress->folders.load(std::memory_order_relaxed), lang))));
        scanPath_->setText(Q(progress->CurrentPath()));
        // Like 1.4.0's bar: bytes found so far against the drive's used space.
        std::uint64_t const expected = progress->expectedBytes.load(std::memory_order_relaxed);
        if (expected != 0)
        {
            scanBar_->setRange(0, 1000);
            scanBar_->setValue(static_cast<int>(PerMille(progress->bytes.load(std::memory_order_relaxed), expected)));
        }
        if (++progressTicks_ % kLiveMapTicks == 0 && !snapshotting_)
        {
            TakeSnapshot(progress);
        }
    }

    // A live map of what the scan found so far, drawn in place of the map while it runs.
    void MainWindow::TakeSnapshot(std::shared_ptr<ScanProgress> const& progress)
    {
        snapshotting_ = true;
        Background(
            [progress]() -> std::unique_ptr<FolderTree> {
                try
                {
                    return progress->Snapshot(kLiveMapEntries);
                }
                catch (...)
                {
                    return nullptr;  // no live map this time (out of memory)
                }
            },
            [this, progress](std::unique_ptr<FolderTree> snapshot) {
                snapshotting_ = false;
                if (!snapshot || !scanning_ || scanProgress_ != progress)
                {
                    return;  // nothing listed yet, or that scan is over
                }
                if (!map_.HasPreview())
                {
                    // The map shows through; the card makes room at the bottom.
                    auto* const stack = static_cast<QGridLayout*>(scanCard_->parentWidget()->layout());
                    stack->setAlignment(scanCard_, Qt::AlignHCenter | Qt::AlignBottom);
                }
                map_.SetPreview(std::move(snapshot));
                mapWidget_->InvalidateBase();
            });
    }

    // ---------------------------------------------------------------- drag and drop

    void MainWindow::dragEnterEvent(QDragEnterEvent* event)
    {
        QList<QUrl> const urls = event->mimeData()->urls();
        if (!scanning_ && !urls.isEmpty() && shell::LocalPath(urls.front()))
        {
            event->acceptProposedAction();
        }
    }

    void MainWindow::dropEvent(QDropEvent* event)
    {
        QList<QUrl> const urls = event->mimeData()->urls();
        std::optional<std::wstring> const dropped = urls.isEmpty() ? std::nullopt : shell::LocalPath(urls.front());
        if (scanning_ || !dropped)
        {
            return;
        }
        event->acceptProposedAction();
        StartScan(shell::FolderOf(*dropped));  // a dropped file stands for its folder
    }
}
