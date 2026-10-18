// SpaceWindow (Qt) - the main window.
//
// The Qt counterpart of src/App/MainWindow*.cpp: the toolbar, the map, the
// status bar, scanning in the background with a live map, reloads, the
// context menu, the keyboard and drag and drop.  What the map shows and how
// it moves is the shared MapView's (src/Presenter).
#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <QMainWindow>
#include <QPointer>
#include <QTimer>

#include "Commands.h"
#include "FolderTree.h"
#include "MapView.h"
#include "Presentation.h"
#include "Settings.h"
#include "Theme.h"

class QAction;
class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;
class QToolBar;

namespace sm
{
    struct Lang;
}

namespace sm::qt
{
    class Desktop;
    class MapWidget;
    struct MapScene;
    struct MapOverlay;

    class MainWindow : public QMainWindow
    {
        Q_OBJECT

    public:
        MainWindow();
        ~MainWindow() override;

        // Scans a drive or folder and shows it (also for the command line).
        void StartScan(std::wstring const& path);

        // For tests and screenshots: once the first scan is shown, save the
        // window as a picture and quit.
        void CaptureWhenShown(QString file);

        // For tests (tests/AppQt): what the window shows, whether it is busy,
        // and what the desktop says (which tests can pretend).
        MapView const& View() const noexcept { return map_; }
        MapWidget* Map() const noexcept { return mapWidget_; }
        bool IsBusy() const noexcept { return scanning_; }
        Desktop* DesktopState() const noexcept { return desktop_; }
        Theme const& ShownTheme() const noexcept { return theme_; }

        // True if a window closed with work still running that would not stop (a
        // scan stuck on a dead network drive): the program should end at once.
        static bool LeftWorkersRunning() noexcept;

    protected:
        void closeEvent(QCloseEvent* event) override;
        void dragEnterEvent(QDragEnterEvent* event) override;
        void dropEvent(QDropEvent* event) override;
        void changeEvent(QEvent* event) override;
        void showEvent(QShowEvent* event) override;
        void hideEvent(QHideEvent* event) override;

    private:
        // Commands: whether one can run now (the toolbar and the menu are
        // enabled by it), and running one, from a button, a key or the menu.
        bool CanRun(Command command) const;
        void Run(Command command);
        void OpenDriveOrFolder();
        void Animate(std::optional<ZoomMotion> const& motion);
        void SetViewMode(ViewMode mode);
        void RunSelected();
        void DeleteSelected();
        void CopySelectedPath();
        void ShowSettings();
        void Try(Settings const& tried);  // a pick in Settings, shown while it is open
        Settings const& Picked() const noexcept { return trying_ ? *trying_ : settings_; }
        void ShowContextMenu(QPoint global);

        // The map.
        MapScene Scene() const;
        MapOverlay Overlay() const;
        void OnGeometryChanged();
        void OnPointerMoved(QPointF pixel);
        void OnPressed(QPointF pixel);
        void OnDoubleClicked(QPointF pixel);
        void OnMapKey(int key, Qt::KeyboardModifiers modifiers, bool& handled);
        void Redraw();  // after the layout or the selection changed
        bool IsDark() const;

        // The background animation, and what the desktop says.
        BackdropConditions Conditions() const;
        void UpdateBackdrop();
        void OnDesktopChanged();

        // Tips.
        void HideTips();
        void ShowNameTip();
        void ShowInfoTip();

        // Scanning.
        void Rescan();
        void UpdateFromChanges();
        void RescanFolder(Folder* folder);
        void ShowScanOverlay(std::wstring const& path, bool counts = true);
        void HideScanOverlay();
        void OnProgressTick();
        void TakeSnapshot(std::shared_ptr<ScanProgress> const& progress);
        void Shown();  // a scan or reload put its tree on the map

        // Runs `work` on a thread of its own, then `done` with its result on the window's.
        template <typename Work, typename Done>
        void Background(Work work, Done done);
        void Finish(std::uint64_t worker);

        // Window chrome.
        void BuildToolbar();
        void BuildShortcuts();
        void LoadThemes();
        void ApplyTheme();
        void ApplyLanguage();
        void UpdateCommands();
        void UpdateStatus();
        void UpdateTitle();
        void UpdateAll();
        void ShowError(QString const& message);
        void RestorePlacement();
        void SavePlacement();

        Settings settings_;
        std::optional<Settings> trying_;  // picked in Settings, shown until OK or Cancel
        Lang const* lang_ = nullptr;
        std::vector<Theme> themes_;
        std::vector<std::wstring> themeErrors_;
        Theme theme_;
        int requestedScheme_ = -1;  // the light or dark last asked of the desktop: 0 its own, 1 light, 2 dark
        Desktop* desktop_ = nullptr;
        bool highContrast_ = false;
        mutable BackdropClock backdropClock_;  // Scene(), which reads it, runs it
        Backdrop backdropShown_ = Backdrop::None;
        QTimer stillTimer_;  // the minute in the background before the animation holds still
        MapView map_;
        std::wstring lastPath_;

        MapWidget* mapWidget_ = nullptr;
        QToolBar* toolbar_ = nullptr;
        std::map<Command, QAction*> actions_;
        QWidget* emptyState_ = nullptr;
        QLabel* emptyTitle_ = nullptr;
        QLabel* emptyText_ = nullptr;
        QPushButton* emptyOpen_ = nullptr;
        QFrame* scanCard_ = nullptr;
        QLabel* scanTitle_ = nullptr;
        QLabel* scanPath_ = nullptr;
        QProgressBar* scanBar_ = nullptr;
        QLabel* scanFiles_ = nullptr;
        QLabel* scanFolders_ = nullptr;
        QPushButton* scanCancel_ = nullptr;
        QFrame* errorBar_ = nullptr;
        QLabel* errorText_ = nullptr;
        QLabel* statusPath_ = nullptr;
        QLabel* statusInfo_ = nullptr;
        QPushButton* unreadable_ = nullptr;

        QTimer progressTimer_;
        QTimer nameTipTimer_;
        QTimer infoTipTimer_;
        unsigned progressTicks_ = 0;
        bool snapshotting_ = false;
        QPointF lastPointer_{ -1.0, -1.0 };  // map pixels
        QPointF lastPointerDips_;

        bool scanning_ = false;
        std::stop_source scanStop_;
        std::shared_ptr<ScanProgress> scanProgress_;

        // Workers hand their results to the window through it, and only while it is open.
        struct Gate
        {
            std::mutex mutex;
            std::condition_variable idle;
            bool open = true;
            std::size_t running = 0;
        };
        struct Worker
        {
            std::uint64_t id = 0;
            std::jthread thread;
            std::shared_ptr<bool> finished;  // under the gate's mutex
        };
        std::shared_ptr<Gate> gate_ = std::make_shared<Gate>();
        std::vector<Worker> workers_;
        std::uint64_t lastWorker_ = 0;

        QString capture_;
    };
}
