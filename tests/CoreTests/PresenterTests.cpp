// Tests for the window's logic without a toolkit (src/Presenter/MapView): what
// the map shows, selecting, zooming, Back and Forward, the keys, and the text.
#include "pch.h"
#include "TestHarness.h"

#include "TreeBuilder.h"
#include "../../src/Core/Lang.h"
#include "../../src/Presenter/MapView.h"
#include "../../src/Presenter/Presentation.h"

namespace
{
    // Totals, counts and order for a hand-built tree, deepest folders first.
    void Total(sm::Folder& folder)
    {
        for (sm::Entry& entry : folder.entries)
        {
            if (entry.child)
            {
                Total(*entry.child);
            }
        }
        sm::UpdateTotals(folder);
    }

    // root/
    //   big/        (a.bin 600000, b.bin 300000, inner/ (c.bin 200000))
    //   small/      (d.bin 50000)
    //   e.bin       (400000)
    std::unique_ptr<sm::FolderTree> SampleTree()
    {
        auto root = std::make_unique<sm::Folder>();
        sm::Folder& big = test::AddFolder(*root, L"big");
        test::AddFile(big, L"a.bin", 600000);
        test::AddFile(big, L"b.bin", 300000);
        sm::Folder& inner = test::AddFolder(big, L"inner");
        test::AddFile(inner, L"c.bin", 200000);
        sm::Folder& small = test::AddFolder(*root, L"small");
        test::AddFile(small, L"d.bin", 50000);
        test::AddFile(*root, L"e.bin", 400000);
        Total(*root);
        std::wstring const rootPath = std::wstring(L"root") + sm::kPathSeparator;
        return std::make_unique<sm::FolderTree>(std::move(root), rootPath, sm::VolumeInfo{}, false);
    }

    sm::MapView SampleView(sm::ViewMode mode = sm::ViewMode::Treemap)
    {
        sm::MapView view;
        sm::Settings settings;
        settings.viewMode = mode;
        view.Configure(settings, sm::MapGeometry{ 800, 600, 1.0, 3, 15 });
        view.SetTree(SampleTree(), {});
        return view;
    }

    // The item that shows the entry called `name` in the folder `folder` is in.
    int ItemNamed(sm::MapView const& view, std::wstring_view name)
    {
        for (int i = 0; std::cmp_less(i, view.ItemCount()); ++i)
        {
            sm::MapItem const& item = view.Item(i);
            if (item.HasEntry() && item.source->Name(item.index) == name)
            {
                return i;
            }
        }
        return -1;
    }

    sm::PiePoint Middle(sm::MapView const& view, int item)
    {
        return view.ItemAnchor(item);
    }

    sm::Lang const& Us()
    {
        return sm::FindLanguage(L"us");
    }
}

TEST_CASE(MapViewSelectsWhatIsUnderThePointer)
{
    sm::MapView view = SampleView();
    CHECK(view.ItemCount() > 0);
    int const file = ItemNamed(view, L"e.bin");
    CHECK(file >= 0);
    sm::PiePoint const at = Middle(view, file);
    CHECK_EQ(view.ItemAt(at.x, at.y), file);

    view.Select(view.ItemAt(at.x, at.y));
    CHECK_EQ(view.SelectedItem(), file);
    CHECK(view.SelectedEntry() != nullptr && view.SelectedEntry()->size == 400000);
    CHECK_EQ(view.SelectedPath(), std::wstring(L"root") + sm::kPathSeparator + L"e.bin");
    CHECK_EQ(view.StatusPath(), view.SelectedPath());
    CHECK(view.StatusInfo(Us()).find(L"400,000 bytes") != std::wstring::npos);
    CHECK(view.Title(Us()).ends_with(L"SpaceWindow"));

    sm::CommandContext const idle{};
    CHECK(view.CanRun(sm::Command::Delete, idle));
    CHECK(view.CanRun(sm::Command::Run, idle));
    CHECK(!view.CanRun(sm::Command::ZoomIn, idle));                        // a file
    CHECK(!view.CanRun(sm::Command::ZoomOut, idle));                       // at the top already
    CHECK(!view.CanRun(sm::Command::Delete, sm::CommandContext{ true }));  // not while scanning
    CHECK(view.CanRun(sm::Command::PieChart, sm::CommandContext{ true }));

    view.Select(-1);
    CHECK(view.SelectedEntry() == nullptr);
    CHECK(!view.CanRun(sm::Command::Delete, idle));
    CHECK(view.StatusInfo(Us()).find(L"Files Total: 5") != std::wstring::npos);
}

TEST_CASE(MapViewZoomsInAndBackOutAndRemembersTheWay)
{
    sm::MapView view = SampleView();
    sm::Folder const* const root = view.Tree()->Root();
    int const big = ItemNamed(view, L"big");
    CHECK(big >= 0 && view.Item(big).IsFolder());

    std::optional<sm::ZoomMotion> const in = view.ZoomIn(big);
    CHECK(in.has_value() && !in->pie);  // animated zoom is on by default
    CHECK(view.ViewRoot() != root);
    CHECK_EQ(view.ViewPath(), (std::vector<std::wstring>{ L"big" }));
    CHECK(view.CanRun(sm::Command::ZoomOut, {}));

    // Zoom Out selects the folder the view came out of.
    (void)view.ZoomOut();
    CHECK(view.ViewRoot() == root);
    CHECK(view.SelectedEntry() != nullptr && view.SelectedEntry()->IsFolder());
    CHECK_EQ(view.SelectedPath(), std::wstring(L"root") + sm::kPathSeparator + L"big");

    // Back returns to "big", Forward comes back up.
    (void)view.GoBack();
    CHECK_EQ(view.ViewPath(), (std::vector<std::wstring>{ L"big" }));
    (void)view.GoForward();
    CHECK(view.ViewRoot() == root);

    // Alt+Down retraces the way: into "big" again, then "inner".
    view.Select(-1);
    (void)view.GoDown();
    CHECK_EQ(view.ViewPath(), (std::vector<std::wstring>{ L"big" }));
    int const inner = ItemNamed(view, L"inner");
    CHECK(inner >= 0);
    (void)view.ZoomIn(inner);
    CHECK_EQ(view.ViewPath(), (std::vector<std::wstring>{ L"big", L"inner" }));
    (void)view.ZoomFull();
    CHECK(view.ViewRoot() == root);
    (void)view.GoDown();
    (void)view.GoDown();
    CHECK_EQ(view.ViewPath(), (std::vector<std::wstring>{ L"big", L"inner" }));

    // Without animated zoom, nothing to animate.
    sm::Settings still = view.CurrentSettings();
    still.animatedZoom = false;
    view.Configure(still, view.Geometry());
    CHECK(!view.ZoomFull().has_value());
}

TEST_CASE(MapViewArrowKeysMoveToTheNearestBox)
{
    sm::MapView view = SampleView();
    view.MoveSelection(sm::Direction::Right);  // nothing selected: the first box
    int const first = view.SelectedItem();
    CHECK(first >= 0);
    // Every move goes the way it was asked, or nowhere.
    for (sm::Direction const direction :
         { sm::Direction::Right, sm::Direction::Down, sm::Direction::Left, sm::Direction::Up })
    {
        int const before = view.SelectedItem();
        sm::PiePoint const from = view.ItemAnchor(before);
        view.MoveSelection(direction);
        sm::PiePoint const to = view.ItemAnchor(view.SelectedItem());
        switch (direction)
        {
        case sm::Direction::Right: CHECK(view.SelectedItem() == before || to.x > from.x); break;
        case sm::Direction::Left: CHECK(view.SelectedItem() == before || to.x < from.x); break;
        case sm::Direction::Down: CHECK(view.SelectedItem() == before || to.y > from.y); break;
        case sm::Direction::Up: CHECK(view.SelectedItem() == before || to.y < from.y); break;
        }
    }
}

TEST_CASE(MapViewKeepsTheSelectionAcrossViews)
{
    sm::MapView view = SampleView();
    int const file = ItemNamed(view, L"e.bin");
    view.Select(file);
    sm::Settings pie = view.CurrentSettings();
    pie.viewMode = sm::ViewMode::Pie;
    view.Configure(pie, view.Geometry());
    CHECK(view.PieMode());
    CHECK(view.SelectedItem() >= 0);
    CHECK_EQ(view.SelectedPath(), std::wstring(L"root") + sm::kPathSeparator + L"e.bin");

    // The hub is no slice; hovering a slice highlights it.
    sm::PiePoint const center = view.Pie().Center();
    CHECK_EQ(view.ItemAt(center.x, center.y), -1);
    sm::PiePoint const at = view.ItemAnchor(view.SelectedItem());
    CHECK(view.Hover(at.x, at.y));
    CHECK_EQ(view.HoverItem(), view.SelectedItem());
    CHECK_EQ(view.Highlighted().size(), std::size_t{ 1 });
    CHECK(!view.Hover(at.x, at.y));  // nothing changed
    std::optional<sm::InfoTip> const tip = view.InfoTipOf(view.HoverItem(), Us());
    CHECK(tip.has_value() && tip->name == L"e.bin");
    CHECK(tip->details.find(L"400,000 bytes") != std::wstring::npos);
}

TEST_CASE(MapViewForgetsWhatWasDeleted)
{
    sm::MapView view = SampleView();
    std::uint64_t const before = view.Tree()->Root()->SizeTotal();
    view.Select(ItemNamed(view, L"e.bin"));
    view.RemoveSelected();
    CHECK(view.SelectedEntry() == nullptr);
    CHECK_EQ(view.Tree()->Root()->SizeTotal(), before - 400000);
    CHECK_EQ(ItemNamed(view, L"e.bin"), -1);
    CHECK_EQ(view.Tree()->Root()->fileCount, std::uint64_t{ 4 });
}

TEST_CASE(MapViewFindsItsWayBackAfterANewTree)
{
    // A reload brings a new tree: the view goes back to the same folder, by name.
    sm::MapView view = SampleView();
    (void)view.ZoomIn(ItemNamed(view, L"big"));
    std::vector<std::wstring> const path = view.ViewPath();
    std::unique_ptr<sm::FolderTree> tree = view.TakeTree();
    CHECK(view.Tree() == nullptr && view.ItemCount() == 0);
    view.SetTree(std::move(tree), path);
    CHECK_EQ(view.ViewPath(), path);
    // A folder gone leaves the view at the deepest one still there.
    view.SetTree(SampleTree(), { L"big", L"no-longer-there" });
    CHECK_EQ(view.ViewPath(), (std::vector<std::wstring>{ L"big" }));
}

TEST_CASE(HighContrastShowsTheDesktopsOwnLook)
{
    CHECK(sm::ShownThemeId(L"dracula", false) == L"dracula");
    CHECK(sm::ShownThemeId(L"dracula", true) == L"system");
    CHECK(sm::ShownThemeId(L"dark", true) == L"system");
}

TEST_CASE(TheBackgroundAnimationPlaysWhereThereIsRoomForIt)
{
    sm::BackdropConditions conditions;
    conditions.picked = sm::Backdrop::Tunnel;
    CHECK(sm::BackdropClock::Shown(conditions) == sm::Backdrop::None);  // the treemap fills the window
    conditions.room = true;                                             // a pie chart, or the welcome screen
    CHECK(sm::BackdropClock::Shown(conditions) == sm::Backdrop::Tunnel);
    for (bool sm::BackdropConditions::* const off :
         { &sm::BackdropConditions::visible, &sm::BackdropConditions::animations })
    {
        sm::BackdropConditions changed = conditions;
        changed.*off = false;
        CHECK(sm::BackdropClock::Shown(changed) == sm::Backdrop::None);
    }
    sm::BackdropConditions contrast = conditions;
    contrast.highContrast = true;
    CHECK(sm::BackdropClock::Shown(contrast) == sm::Backdrop::None);
    conditions.picked = sm::Backdrop::None;
    CHECK(sm::BackdropClock::Shown(conditions) == sm::Backdrop::None);
}

TEST_CASE(TheBackgroundAnimationHoldsStillAndCarriesOn)
{
    using namespace std::chrono_literals;
    auto const start = sm::BackdropClock::Clock::time_point{} + 1h;
    sm::BackdropClock clock(start);
    sm::BackdropConditions conditions;
    conditions.picked = sm::Backdrop::Fire;
    conditions.room = true;
    CHECK(clock.Time(conditions, start + 2s) == 2.0);

    // In the background, it plays on for a minute, then holds still.
    conditions.active = false;
    clock.Deactivated(start + 2s);
    CHECK(!clock.Paused(conditions, start + 61s));
    CHECK(clock.Time(conditions, start + 61s) == 61.0);
    CHECK(clock.Paused(conditions, start + 62s));
    CHECK(clock.Time(conditions, start + 62s) == 62.0);
    CHECK(clock.Time(conditions, start + 100s) == 62.0);  // stopped

    // Back in front, it carries on from where it stopped.
    conditions.active = true;
    CHECK(!clock.Paused(conditions, start + 100s));
    CHECK(clock.Time(conditions, start + 100s) == 62.0);
    CHECK(clock.Time(conditions, start + 101s) == 63.0);

    // Saving energy, it holds still whatever the window.
    conditions.powerSaving = true;
    CHECK(clock.Paused(conditions, start + 101s));
    CHECK(clock.Time(conditions, start + 101s) == 63.0);
    CHECK(clock.Time(conditions, start + 200s) == 63.0);
}

TEST_CASE(TheBackgroundAnimationPlaysInAWindowThatNeverHadTheKeyboard)
{
    // Opened without the keyboard (from a terminal, on Wayland): it plays,
    // and holds still a minute later, as after losing the keyboard.
    using namespace std::chrono_literals;
    auto const start = sm::BackdropClock::Clock::time_point{} + 5h;
    sm::BackdropClock const clock(start);
    sm::BackdropConditions conditions;
    conditions.picked = sm::Backdrop::Life;
    conditions.room = true;
    conditions.active = false;
    CHECK(!clock.Paused(conditions, start + 1s));
    CHECK(!clock.Paused(conditions, start + 59s));
    CHECK(clock.Paused(conditions, start + 61s));
}
