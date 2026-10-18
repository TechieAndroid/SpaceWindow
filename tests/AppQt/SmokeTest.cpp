// A smoke test of the Qt app: the real window, driven by simulated mouse and
// keys (QTest) on a folder tree made for it - the Linux counterpart of
// tests/Ui/Smoke.ps1.  It runs on Qt's offscreen platform unless
// QT_QPA_PLATFORM says otherwise (CTest runs it at 100 %, 125 % and 200 %
// with QT_SCALE_FACTOR), with settings, the window's state and the trash in
// a scratch folder: the user's are left alone.
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <type_traits>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include "Desktop.h"
#include "MapWidget.h"
#include "Posix.h"
#include "QtCompat.h"
#include "QtText.h"
#include "Settings.h"
#include "SmokeTest.h"
#include "Theme.h"

using sm::qt::MainWindow;

namespace
{
    constexpr int kWindowTimeout = 10'000;  // ms for a dialog or a menu to come up, on a busy machine too
    constexpr int kScanTimeout = 15'000;
    constexpr char const* kNotUtf8 = "caf\xE9.bin";  // ISO 8859-1, as old archives have their names

    void WriteFile(std::filesystem::path const& path, std::size_t size)
    {
        std::ofstream file(path, std::ios::binary);
        std::string const data(size, 'x');
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
    }

    // The point (in the widget's coordinates) of the item showing the entry called `name`.
    std::optional<QPoint> PointOf(MainWindow const& window, std::wstring_view name)
    {
        sm::MapView const& view = window.View();
        for (int i = 0; std::cmp_less(i, view.ItemCount()); ++i)
        {
            sm::MapItem const& item = view.Item(i);
            if (item.HasEntry() && item.source->Name(item.index) == name)
            {
                sm::PiePoint anchor = view.ItemAnchor(i);
                if (!view.PieMode() && item.IsFolder())
                {
                    // A folder's own part of its box is its title band: its middle holds what is in it.
                    sm::DisplayItem const& box = view.Treemap().Items()[static_cast<std::size_t>(i)];
                    anchor.y = box.y + view.Treemap().Options().header / 2.0;
                }
                double const scale = window.Map()->Scale();
                return QPointF(anchor.x / scale, anchor.y / scale).toPoint();
            }
        }
        return std::nullopt;
    }

    // The entry of the tree's top folder called `name`.
    sm::Entry const* TopEntry(MainWindow const& window, std::wstring_view name)
    {
        sm::Folder const& top = *window.View().Tree()->Root();
        for (std::size_t i = 0; i < top.entries.size(); ++i)
        {
            if (top.Name(i) == name)
            {
                return &top.entries[i];
            }
        }
        return nullptr;
    }

    QString Text(MainWindow const& window, char const* label)
    {
        auto const* const widget = window.findChild<QLabel*>(QString::fromLatin1(label));
        return widget != nullptr ? widget->text() : QString();
    }

    // The pointer moved over the widget (offscreen, QTest::mouseMove moves only the cursor).
    void MoveTo(QWidget* widget, QPoint point)
    {
        QMouseEvent move(QEvent::MouseMove, QPointF(point), widget->mapToGlobal(QPointF(point)), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &move);
    }

    // With SMOKE_SHOTS=<folder>, a picture of the window at this step, as tests/Ui/Smoke.ps1 keeps them.
    void Shot(QWidget* window, char const* name)
    {
        QString const folder = qEnvironmentVariable("SMOKE_SHOTS");
        if (!folder.isEmpty())
        {
            (void)window->grab().save(folder + QLatin1Char('/') + QString::fromLatin1(name) + QStringLiteral(".png"));
        }
    }

    QAction* ActionLabelled(MainWindow& window, QString const& label)
    {
        for (QAction* const action : window.findChildren<QAction*>())
        {
            if (action->iconText() == label)
            {
                return action;
            }
        }
        return nullptr;
    }

    // Answers the modal dialog or the menu that `open` brings up, once it is
    // there: looked for every 20 ms until a deadline, so that a slow machine
    // only takes longer.  `Window` is QMenu for a menu, the dialog's class
    // otherwise.  False if none came (whatever came instead is closed).
    template <typename Window>
    bool Answer(std::function<void()> const& open, std::function<void(Window&)> const& answer)
    {
        bool answered = false;
        QElapsedTimer clock;
        clock.start();
        QTimer poll;
        poll.setInterval(20);
        QObject::connect(&poll, &QTimer::timeout, [&] {
            QWidget* const up = std::is_base_of_v<QMenu, Window> ? QApplication::activePopupWidget()
                                                                 : QApplication::activeModalWidget();
            if (auto* const window = qobject_cast<Window*>(up); window != nullptr && window->isVisible())
            {
                poll.stop();
                answered = true;
                answer(*window);
            }
            else if (clock.elapsed() > kWindowTimeout)
            {
                poll.stop();
                if (up != nullptr)
                {
                    up->close();
                }
            }
        });
        poll.start();
        open();
        (void)QTest::qWaitFor([&poll] { return !poll.isActive(); }, kWindowTimeout + 1000);
        return answered;
    }

    // The smallest rectangle holding every pixel where two pictures differ.
    QRect Difference(QImage const& a, QImage const& b)
    {
        QImage const first = a.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QImage const second = b.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QRect box;
        for (int y = 0; y < first.height(); ++y)
        {
            auto const* const one = reinterpret_cast<QRgb const*>(first.constScanLine(y));
            auto const* const other = reinterpret_cast<QRgb const*>(second.constScanLine(y));
            for (int x = 0; x < first.width(); ++x)
            {
                if (one[x] != other[x])
                {
                    box |= QRect(x, y, 1, 1);
                }
            }
        }
        return box;
    }

    // Drags a file or folder from a file manager and drops it on the window.
    bool Drop(QWidget* window, std::filesystem::path const& path)
    {
        QMimeData data;
        data.setUrls({ QUrl::fromLocalFile(QString::fromStdString(path.string())) });
        QPoint const at = window->rect().center();
#if SPACEWINDOW_QT_API >= QT_VERSION_CHECK(6, 12, 0)
        QDragEnterEvent enter(QPointF(at), Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
#else
        // Qt 6.12 added a constructor taking QPointF and calls this one old (a baseline build there compiles it).
        QT_WARNING_PUSH
        QT_WARNING_DISABLE_DEPRECATED
        QDragEnterEvent enter(at, Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
        QT_WARNING_POP
#endif
        enter.setAccepted(false);
        QApplication::sendEvent(window, &enter);
        if (!enter.isAccepted())
        {
            return false;
        }
        QDropEvent drop(QPointF(at), Qt::CopyAction, &data, Qt::LeftButton, Qt::NoModifier);
        drop.setAccepted(false);
        QApplication::sendEvent(window, &drop);
        return drop.isAccepted();
    }

    // Settings, with `pick` done in it while it is open, then OK or Cancel.
    bool InSettings(MainWindow& window, std::function<void(QDialog&)> const& pick)
    {
        QAction* const settings = ActionLabelled(window, QStringLiteral("Setup"));
        if (settings == nullptr)
        {
            return false;
        }
        bool const done = Answer<QDialog>([settings] { settings->trigger(); }, pick);
        // The window manager gives the window the keyboard back; offscreen, nobody does.
        window.activateWindow();
        return done && QTest::qWaitForWindowActive(&window);
    }

    // Picks the item with `data` in whichever of the dialog's lists has it.
    void Choose(QDialog& dialog, QVariant const& data, int list = -1)
    {
        QList<QComboBox*> const boxes = dialog.findChildren<QComboBox*>();
        for (int i = 0; i < boxes.size(); ++i)
        {
            if (int const index = boxes[i]->findData(data); index >= 0 && (list < 0 || list == i))
            {
                boxes[i]->setCurrentIndex(index);
                return;
            }
        }
    }

    // The background animation list: the one with an entry for each animation, and nothing else.
    QComboBox* BackdropList(QDialog& dialog)
    {
        for (QComboBox* const box : dialog.findChildren<QComboBox*>())
        {
            if (box->count() == sm::kBackdropCount && box->itemData(0).toInt() == 0 &&
                box->itemData(sm::kBackdropCount - 1).toInt() == sm::kBackdropCount - 1)
            {
                return box;
            }
        }
        return nullptr;
    }

    void ClickDestructive(QMessageBox& box)
    {
        for (QAbstractButton* const button : box.buttons())
        {
            if (box.buttonRole(button) == QMessageBox::DestructiveRole)
            {
                button->click();
                return;
            }
        }
    }
}

void SmokeTest::initTestCase()
{
    QVERIFY(folder_.isValid());
    root_ = std::filesystem::path(folder_.path().toStdString()) / "tree";
    std::filesystem::create_directories(root_ / "photos" / "2024");
    std::filesystem::create_directories(root_ / "music");
    WriteFile(root_ / "video.mkv", 3 << 20);
    WriteFile(root_ / "notes.txt", 200'000);
    WriteFile(root_ / "photos" / "a.jpg", 900'000);
    WriteFile(root_ / "photos" / "b.jpg", 700'000);
    WriteFile(root_ / "photos" / "2024" / "c.jpg", 600'000);
    WriteFile(root_ / "music" / "song.flac", 1'500'000);

    window_ = std::make_unique<MainWindow>();
    window_->resize(1000, 700);
    window_->show();
    window_->activateWindow();
    QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
}

void SmokeTest::scansAndDraws()
{
    window_->StartScan(sm::posix::FromNative(root_.string()));
    QTRY_VERIFY_WITH_TIMEOUT(window_->View().Tree() != nullptr && !window_->IsBusy(), kScanTimeout);
    sm::Folder const& top = *window_->View().Tree()->Root();
    QCOMPARE(top.fileCount, std::uint64_t{ 6 });
    QCOMPARE(top.folderCount, std::uint64_t{ 3 });
    QVERIFY(window_->View().ItemCount() >= 9);
    QVERIFY(Text(*window_, "StatusInfo").contains(QStringLiteral("Files Total: 6")));
    QVERIFY(window_->windowTitle().endsWith(QStringLiteral("SpaceWindow")));
    // The map was drawn: not one colour all over.
    QImage const shot = window_->Map()->grab().toImage();
    QVERIFY(shot.pixelColor(5, 5) != shot.pixelColor(shot.width() / 2, shot.height() / 2));
}

void SmokeTest::drawsTheMapPixelForPixel()
{
    // What the window shows of the map, as the window is put together: the
    // device pixels the map widget covers.  At 125 % its edges fall between
    // device pixels, and Qt rounds each of them on its own.  (The window drawn
    // whole, as the screen gets it; the offscreen screen's own grab gives only
    // as many device pixels as the window has DIPs.)
    sm::qt::MapWidget* const map = window_->Map();
    auto const shown = [this, map] {
        double const scale = map->devicePixelRatioF();
        QPoint const origin = map->mapTo(window_.get(), QPoint(0, 0));
        QRect const covered(qRound(origin.x() * scale), qRound(origin.y() * scale),
                            qRound((origin.x() + map->width()) * scale) - qRound(origin.x() * scale),
                            qRound((origin.y() + map->height()) * scale) - qRound(origin.y() * scale));
        return window_->grab().toImage().copy(covered);
    };

    // With nothing selected or under the pointer, the map exactly as drawn,
    // whatever the window's size: no scaling on the way, no row cut or left stale.
    map->setFocus();
    QTest::keyClick(map, Qt::Key_Escape);
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(map, &leave);
    QSize const size = window_->size();
    for (int extra = 0; extra < 6; ++extra)
    {
        window_->resize(size.width() + extra, size.height() + extra);
        QTRY_VERIFY(map->Base().size() == QSize(map->PixelWidth(), map->PixelHeight()));
        QImage const screen = shown();
        QCOMPARE(screen.size(), map->Base().size());
        QVERIFY2(Difference(screen, map->Base()).isEmpty(), "the window does not show the map pixel for pixel");
    }
    window_->resize(size);
    QTRY_VERIFY(map->Base().size() == QSize(map->PixelWidth(), map->PixelHeight()));

    // The selection is drawn over it, within the selected box only.
    std::optional<QPoint> const video = PointOf(*window_, L"video.mkv");
    if (!video)
    {
        QFAIL("video is not on the map");
    }
    QTest::mouseClick(map, Qt::LeftButton, {}, *video);
    int const selected = window_->View().SelectedItem();
    QVERIFY(selected >= 0);
    sm::DisplayItem const& box = window_->View().Treemap().Items()[static_cast<std::size_t>(selected)];
    QRect const changed = Difference(shown(), map->Base());
    QVERIFY2(!changed.isEmpty(), "the selection does not show");
    QVERIFY2(QRect(box.x, box.y, box.w + 1, box.h + 1).contains(changed), "the selection spills out of its box");
}

void SmokeTest::selectsWithTheMouse()
{
    std::optional<QPoint> const video = PointOf(*window_, L"video.mkv");
    if (!video)
    {
        QFAIL("video is not on the map");
    }
    QTest::mouseClick(window_->Map(), Qt::LeftButton, {}, *video);
    QVERIFY(window_->View().SelectedEntry() != nullptr);
    QVERIFY(Text(*window_, "StatusPath").endsWith(QStringLiteral("video.mkv")));
    QVERIFY(Text(*window_, "StatusInfo").contains(QStringLiteral("3,145,728 bytes")));
    QVERIFY(window_->windowTitle().contains(QStringLiteral("video.mkv")));
    Shot(window_.get(), "selected");
}

void SmokeTest::zoomsInAndOut()
{
    std::optional<QPoint> const photos = PointOf(*window_, L"photos");
    if (!photos)
    {
        QFAIL("photos is not on the map");
    }
    QTest::mouseDClick(window_->Map(), Qt::LeftButton, {}, *photos);
    QCOMPARE(window_->View().ViewPath(), (std::vector<std::wstring>{ L"photos" }));
    QVERIFY(PointOf(*window_, L"2024").has_value());

    window_->Map()->setFocus();
    QTest::keyClick(window_->Map(), Qt::Key_Backspace);  // Zoom Out, a shortcut of the window
    QVERIFY(window_->View().ViewPath().empty());
    QVERIFY(Text(*window_, "StatusPath").endsWith(QStringLiteral("photos")));  // the folder it came out of

    QTest::keyClick(window_->Map(), Qt::Key_Left, Qt::AltModifier);  // Back
    QCOMPARE(window_->View().ViewPath(), (std::vector<std::wstring>{ L"photos" }));
    QTest::keyClick(window_->Map(), Qt::Key_Home);  // Zoom Full
    QVERIFY(window_->View().ViewPath().empty());
}

void SmokeTest::movesWithTheKeys()
{
    window_->Map()->setFocus();
    QTest::keyClick(window_->Map(), Qt::Key_Escape);  // clears the selection
    QVERIFY(window_->View().SelectedEntry() == nullptr);
    QTest::keyClick(window_->Map(), Qt::Key_Right);  // the first box
    int const first = window_->View().SelectedItem();
    QVERIFY(first >= 0);
    for (Qt::Key const key : { Qt::Key_Right, Qt::Key_Down, Qt::Key_Left, Qt::Key_Up })
    {
        QTest::keyClick(window_->Map(), key);
        QVERIFY(window_->View().SelectedItem() >= 0);
    }
}

void SmokeTest::togglesThePieChart()
{
    QTest::keyClick(window_->Map(), Qt::Key_P, Qt::ControlModifier);
    QVERIFY(window_->View().PieMode());
    QVERIFY(PointOf(*window_, L"video.mkv").has_value());
    QImage const pie = window_->Map()->grab().toImage();
    QVERIFY(!pie.isNull());
    Shot(window_.get(), "pie");
    QTest::keyClick(window_->Map(), Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);  // inverted
    QCOMPARE(window_->View().CurrentSettings().viewMode, sm::ViewMode::InvertedPie);
    QTest::keyClick(window_->Map(), Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);  // back to the treemap
    QVERIFY(!window_->View().PieMode());
}

void SmokeTest::showsTips()
{
    std::optional<QPoint> const notes = PointOf(*window_, L"notes.txt");
    if (!notes)
    {
        QFAIL("notes is not on the map");
    }
    MoveTo(window_->Map(), *notes);
    QVERIFY2(window_->View().HoverItem() >= 0, "the pointer is over no box");
    auto* const tip = window_->Map()->findChild<QFrame*>(QStringLiteral("InfoTip"));
    QVERIFY(tip != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(tip->isVisible(), 3000);
    // By default the tip gives the size, the date and the icon (Settings adds the name and the path).
    bool sized = false;
    for (QLabel const* const label : tip->findChildren<QLabel*>())
    {
        sized = sized || (label->isVisible() && label->text().contains(QStringLiteral("200,000 bytes")));
    }
    QVERIFY(sized);
    Shot(window_.get(), "info-tip");
}

void SmokeTest::showsTheContextMenu()
{
    std::optional<QPoint> const photos = PointOf(*window_, L"photos");
    if (!photos)
    {
        QFAIL("photos is not on the map");
    }
    QTest::mouseClick(window_->Map(), Qt::LeftButton, {}, *photos);
    bool hasRescan = false;
    bool const shown = Answer<QMenu>(
        [this] {
            window_->Map()->setFocus();
            QTest::keyClick(window_->Map(), Qt::Key_Menu);
        },
        [&hasRescan](QMenu& menu) {
            for (QAction const* const action : menu.actions())
            {
                hasRescan = hasRescan || action->text().contains(QStringLiteral("photos"));
            }
            menu.close();
        });
    QVERIFY(shown);
    QVERIFY(hasRescan);  // "Rescan “photos”"
}

void SmokeTest::reloadsWhatChanged()
{
    WriteFile(root_ / "music" / "album.flac", 2'500'000);
    std::filesystem::remove(root_ / "notes.txt");
    QTest::qWait(300);  // the watch records changes on a thread of its own
    window_->Map()->setFocus();
    QTest::keyClick(window_->Map(), Qt::Key_F5);  // Reload: only what changed
    QTRY_VERIFY_WITH_TIMEOUT(!window_->IsBusy(), kScanTimeout);
    sm::Folder const& top = *window_->View().Tree()->Root();
    QCOMPARE(top.fileCount, std::uint64_t{ 6 });  // one more, one less
    QVERIFY(!PointOf(*window_, L"notes.txt").has_value());
    std::optional<QPoint> const music = PointOf(*window_, L"music");
    if (!music)
    {
        QFAIL("music is not on the map");
    }
}

void SmokeTest::rescansAFolderOrEverything()
{
    // "Rescan “photos”" from the menu: what changed in that folder shows.
    WriteFile(root_ / "photos" / "d.jpg", 400'000);
    std::optional<QPoint> const photos = PointOf(*window_, L"photos");
    if (!photos)
    {
        QFAIL("photos is not on the map");
    }
    QTest::mouseClick(window_->Map(), Qt::LeftButton, {}, *photos);
    bool found = false;
    bool const shown = Answer<QMenu>(
        [this] {
            window_->Map()->setFocus();
            QTest::keyClick(window_->Map(), Qt::Key_Menu);
        },
        [&found](QMenu& menu) {
            menu.close();
            for (QAction* const action : menu.actions())
            {
                if (action->text().contains(QStringLiteral("photos")))
                {
                    found = true;
                    action->trigger();
                    return;
                }
            }
        });
    QVERIFY(shown && found);
    QTRY_VERIFY_WITH_TIMEOUT(!window_->IsBusy(), kScanTimeout);
    sm::Entry const* const folder = TopEntry(*window_, L"photos");
    QVERIFY(folder != nullptr && folder->child != nullptr);
    QCOMPARE(folder->child->fileCount, std::uint64_t{ 4 });

    // Full Rescan: everything again.
    WriteFile(root_ / "music" / "extra.flac", 100'000);
    QAction* const fullRescan = ActionLabelled(*window_, QStringLiteral("Full Rescan"));
    QVERIFY(fullRescan != nullptr && fullRescan->isEnabled());
    fullRescan->trigger();
    QVERIFY(window_->IsBusy());
    QTRY_VERIFY_WITH_TIMEOUT(!window_->IsBusy(), kScanTimeout);
    QCOMPARE(window_->View().Tree()->Root()->fileCount, std::uint64_t{ 8 });
}

void SmokeTest::scansWhatIsDroppedOnIt()
{
    // A folder dropped on the window is scanned; a file stands for its folder.
    QVERIFY(Drop(window_.get(), root_ / "photos"));
    QTRY_VERIFY_WITH_TIMEOUT(!window_->IsBusy(), kScanTimeout);
    QCOMPARE(window_->View().Tree()->RootPath(),
             sm::NormalizeScanPath(sm::posix::FromNative((root_ / "photos").string())));
    QVERIFY(Drop(window_.get(), root_ / "video.mkv"));
    QTRY_VERIFY_WITH_TIMEOUT(!window_->IsBusy(), kScanTimeout);
    QCOMPARE(window_->View().Tree()->RootPath(), sm::NormalizeScanPath(sm::posix::FromNative(root_.string())));
}

void SmokeTest::opensSettings()
{
    QAction* const settings = ActionLabelled(*window_, QStringLiteral("Setup"));
    QVERIFY(settings != nullptr);
    bool const opened = Answer<QDialog>([settings] { settings->trigger(); },
                                        [](QDialog& dialog) { dialog.accept(); });  // OK: the settings as they were
    QVERIFY(opened);
    QVERIFY(window_->View().Tree() != nullptr);
    // The window manager gives the window the keyboard back; offscreen, nobody does.
    window_->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(window_.get()));
}

void SmokeTest::switchesThemes()
{
    // Picks a theme in Settings and presses OK.
    auto const pick = [this](QString const& id) {
        QAction* const settings = ActionLabelled(*window_, QStringLiteral("Setup"));
        bool const picked = Answer<QDialog>([settings] { settings->trigger(); },
                                            [&id](QDialog& dialog) {
                                                for (QComboBox* const box : dialog.findChildren<QComboBox*>())
                                                {
                                                    if (int const index = box->findData(id); index >= 0)
                                                    {
                                                        box->setCurrentIndex(index);
                                                    }
                                                }
                                                dialog.accept();
                                            });
        // The window manager gives the window the keyboard back; offscreen, nobody does.
        window_->activateWindow();
        return picked && QTest::qWaitForWindowActive(window_.get());
    };
    auto const style = [] { return QApplication::style()->name().toLower(); };
    auto const window = [] { return QApplication::palette().color(QPalette::Window); };
    QColor const desktop = window();  // "Use system setting", the default: the desktop's palette

    // A colour theme: Fusion, in the theme's colours.
    sm::Theme const& dracula = sm::FindTheme(sm::BuiltInThemes(), L"dracula");
    QVERIFY(pick(QStringLiteral("dracula")));
    QCOMPARE(style(), QStringLiteral("fusion"));
    QCOMPARE(window(), sm::qt::ToQColor(dracula.window.background));
    QTRY_COMPARE(window_->palette().color(QPalette::Window), window());  // once its events are handled

    // Dark where the desktop is light: Fusion, in Windows 11's dark colours.
    QVERIFY(pick(QStringLiteral("dark")));
    QCOMPARE(style(), QStringLiteral("fusion"));
    QVERIFY(window().lightness() < 128);

    // "Use system setting": the desktop's own style and palette again (main() made it "Windows").
    QVERIFY(pick(QStringLiteral("system")));
    QCOMPARE(style(), QStringLiteral("windows"));
    QCOMPARE(window(), desktop);
}

void SmokeTest::previewsWhatIsPicked()
{
    // A theme or a language picked in Settings shows at once; Cancel takes it back.
    sm::Theme const& dracula = sm::FindTheme(sm::BuiltInThemes(), L"dracula");
    QColor const before = QApplication::palette().color(QPalette::Window);
    QColor during;
    QString title;
    QString setup;
    QVERIFY(InSettings(*window_, [&](QDialog& dialog) {
        Choose(dialog, QStringLiteral("dracula"));
        during = QApplication::palette().color(QPalette::Window);
        Choose(dialog, QStringLiteral("fr"));
        title = dialog.windowTitle();
        setup = ActionLabelled(*window_, QStringLiteral("Paramètres")) != nullptr ? QStringLiteral("fr") : QString();
        dialog.reject();
    }));
    QCOMPARE(during, sm::qt::ToQColor(dracula.window.background));
    QCOMPARE(title, QStringLiteral("Paramètres de SpaceWindow"));  // the dialog speaks it too
    QCOMPARE(setup, QStringLiteral("fr"));                         // and so does the window's toolbar
    QCOMPARE(QApplication::palette().color(QPalette::Window), before);
    QVERIFY(ActionLabelled(*window_, QStringLiteral("Setup")) != nullptr);
}

void SmokeTest::playsTheBackgroundAnimation()
{
    // The tunnel, around the pie chart: it moves.
    QVERIFY(InSettings(*window_, [](QDialog& dialog) {
        if (QComboBox* const list = BackdropList(dialog))
        {
            list->setCurrentIndex(static_cast<int>(sm::Backdrop::Tunnel));
        }
        dialog.accept();
    }));
    QCOMPARE(window_->View().CurrentSettings().backdrop, sm::Backdrop::Tunnel);
    sm::qt::MapWidget* const map = window_->Map();
    QVERIFY(!map->IsAnimating());  // no room around the treemap
    map->setFocus();
    QTest::keyClick(map, Qt::Key_P, Qt::ControlModifier);
    QVERIFY(window_->View().PieMode());
    QVERIFY(map->IsAnimating());
    QImage const first = map->grab().toImage();
    QTest::qWait(150);
    QVERIFY2(!Difference(first, map->grab().toImage()).isEmpty(), "the animation does not move");
    Shot(window_.get(), "tunnel");

    // Saving energy, it holds still; in high contrast, it goes, with the theme picked.
    window_->DesktopState()->Pretend(std::nullopt, true);
    QVERIFY(!map->IsAnimating());
    window_->DesktopState()->Pretend(true, std::nullopt);
    QVERIFY(!map->IsAnimating());
    QCOMPARE(window_->ShownTheme().id, std::wstring(L"system"));
    window_->DesktopState()->Pretend(std::nullopt, std::nullopt);
    QVERIFY(map->IsAnimating());

    // With SMOKE_SHOTS, a picture of each animation.
    if (!qEnvironmentVariableIsEmpty("SMOKE_SHOTS"))
    {
        for (int kind = 1; kind < sm::kBackdropCount; ++kind)
        {
            QVERIFY(InSettings(*window_, [kind](QDialog& dialog) {
                BackdropList(dialog)->setCurrentIndex(kind);
                dialog.accept();
            }));
            QTest::qWait(700);
            Shot(window_.get(), QStringLiteral("backdrop-%1").arg(kind).toLatin1().constData());
        }
    }

    QTest::keyClick(map, Qt::Key_P, Qt::ControlModifier);  // back to the treemap
    QVERIFY(!map->IsAnimating());
    QVERIFY(InSettings(*window_, [](QDialog& dialog) {
        BackdropList(dialog)->setCurrentIndex(0);
        dialog.accept();
    }));
}

void SmokeTest::movesToTheTrash()
{
    std::optional<QPoint> const video = PointOf(*window_, L"video.mkv");
    if (!video)
    {
        QFAIL("video is not on the map");
    }
    QTest::mouseClick(window_->Map(), Qt::LeftButton, {}, *video);
    QVERIFY2(window_->View().SelectedEntry() != nullptr, "nothing is selected");
    QVERIFY2(window_->View().CanRun(sm::Command::Delete, {}), "Delete cannot run");
    bool cancelByDefault = false;
    bool const asked = Answer<QMessageBox>(
        [this] {
            window_->Map()->setFocus();
            QTest::keyClick(window_->Map(), Qt::Key_Delete);
        },
        [&cancelByDefault](QMessageBox& box) {
            cancelByDefault =
                box.defaultButton() != nullptr && box.buttonRole(box.defaultButton()) == QMessageBox::RejectRole;
            ClickDestructive(box);
        });
    QVERIFY(asked);
    QVERIFY(cancelByDefault);
    window_->activateWindow();  // as after any dialog: offscreen, nobody gives the window the keyboard back
    QVERIFY(QTest::qWaitForWindowActive(window_.get()));
    auto const* const error = window_->findChild<QFrame*>(QStringLiteral("ErrorBar"));
    QVERIFY2(error == nullptr || !error->isVisible(), "the file could not be moved to the trash");
    QVERIFY(!std::filesystem::exists(root_ / "video.mkv"));
    // In the scratch trash ($XDG_DATA_HOME is on the same file system as the tree).
    std::filesystem::path const trash = qEnvironmentVariable("XDG_DATA_HOME").toStdString();
    QVERIFY(std::filesystem::exists(trash / "Trash" / "files" / "video.mkv"));
    QVERIFY(!PointOf(*window_, L"video.mkv").has_value());
    QCOMPARE(window_->View().Tree()->Root()->fileCount, std::uint64_t{ 7 });
}

void SmokeTest::trashesNamesThatAreNotUtf8()
{
    // A name in ISO 8859-1, as old archives have them: on the map, and into the trash whole.
    std::string const name = kNotUtf8;
    WriteFile(root_ / name, 800'000);
    QTest::qWait(300);
    window_->Map()->setFocus();
    QTest::keyClick(window_->Map(), Qt::Key_F5);
    QTRY_VERIFY_WITH_TIMEOUT(!window_->IsBusy(), kScanTimeout);
    std::optional<QPoint> const file = PointOf(*window_, sm::posix::FromNative(name));
    if (!file)
    {
        QFAIL("the file is not on the map");
    }
    QTest::mouseClick(window_->Map(), Qt::LeftButton, {}, *file);
    bool const asked = Answer<QMessageBox>(
        [this] {
            window_->Map()->setFocus();
            QTest::keyClick(window_->Map(), Qt::Key_Delete);
        },
        ClickDestructive);
    QVERIFY(asked);
    window_->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(window_.get()));
    QVERIFY(!std::filesystem::exists(root_ / name));
    std::filesystem::path const trash =
        std::filesystem::path(qEnvironmentVariable("XDG_DATA_HOME").toStdString()) / "Trash";
    QVERIFY(std::filesystem::exists(trash / "files" / name));
    std::ifstream info(trash / "info" / (name + ".trashinfo"));
    std::string const text((std::istreambuf_iterator<char>(info)), {});
    info.close();
    QVERIFY(text.find("/caf%E9.bin\n") != std::string::npos);
    // QTemporaryDir cannot delete what Qt cannot name.
    std::filesystem::remove(trash / "files" / name);
    std::filesystem::remove(trash / "info" / (name + ".trashinfo"));
}

void SmokeTest::remembersWhereItWas()
{
    // With "Remember window position" on, a window opens as big as the last
    // one closed (and, where the desktop lets programs place windows, there).
    sm::Settings settings = sm::LoadSettings();
    settings.savePosition = true;
    QVERIFY(sm::SaveSettings(settings));
    QRect placed;
    {
        MainWindow first;
        first.show();
        QVERIFY(QTest::qWaitForWindowExposed(&first));
        // Well inside the screen (the offscreen one is 800 x 600 pixels: smaller in DIPs at 200 %).
        QRect const screen = first.screen()->availableGeometry();
        first.setGeometry(
            QRect(screen.topLeft() + QPoint(screen.width() / 10, screen.height() / 10), screen.size() * 4 / 5));
        QTRY_VERIFY(first.geometry() != QRect());
        placed = first.geometry();
        first.close();
    }
    MainWindow second;
    second.show();
    QVERIFY(QTest::qWaitForWindowExposed(&second));
    QCOMPARE(second.geometry(), placed);
    second.close();
    settings.savePosition = false;
    QVERIFY(sm::SaveSettings(settings));
}

void SmokeTest::closesWhileScanning()
{
    // Closing stops a scan that is still going, and does not wait on it long.
    auto window = std::make_unique<MainWindow>();
    window->show();
    window->StartScan(L"/usr/");
    QVERIFY(window->IsBusy());
    QTest::qWait(150);  // well into it
    QElapsedTimer clock;
    clock.start();
    window->close();
    window.reset();
    QVERIFY2(clock.elapsed() < 1500, "closing waited on the scan");
    QVERIFY(!MainWindow::LeftWorkersRunning());
}

void SmokeTest::cleanupTestCase()
{
    window_->close();
    window_.reset();
    std::filesystem::remove(root_ / kNotUtf8);  // if a step failed before trashing it: QTemporaryDir cannot
}

int main(int argc, char** argv)
{
    // Settings, the window's state and the trash in a scratch folder; no screen needed.
    QTemporaryDir const home;
    for (char const* const folder : { "/config", "/data", "/state" })
    {
        std::filesystem::create_directories(home.path().toStdString() + folder);  // as ~/.config and the rest are
    }
    // $HOME too: Qt 6.4's trash takes the home trash only for files on $HOME's file
    // system (the tree is made beside it), and the trash atop their own for others.
    qputenv("HOME", home.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", (home.path() + QStringLiteral("/config")).toUtf8());
    qputenv("XDG_DATA_HOME", (home.path() + QStringLiteral("/data")).toUtf8());
    qputenv("XDG_STATE_HOME", (home.path() + QStringLiteral("/state")).toUtf8());
    qputenv("LANGUAGE", "en_US");
    // No desktop to talk to: what it would say (high contrast, saving energy)
    // the tests pretend, and nothing reaches the user's own file manager.
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/spacewindow-test-bus");
    qputenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/spacewindow-test-bus");
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication const application(argc, argv);
    // A desktop style other than Fusion, as Breeze is on KDE: the classic themes must keep it.
    QApplication::setStyle(QStringLiteral("Windows"));
    SmokeTest test;
    return QTest::qExec(&test, argc, argv);
}
