// Tests for the pie chart layout, on folder trees built in memory.
#include "pch.h"
#include "TestHarness.h"
#include "TreeBuilder.h"

#include "../../src/Core/PieLayout.h"
#include "../../src/Core/Settings.h"

namespace
{
    using test::AddFile;
    using test::AddFolder;
    using test::AddSpecial;
    using test::Finish;

    constexpr double kPi = std::numbers::pi;
    constexpr double kTwoPi = 2.0 * kPi;

    bool Near(double a, double b, double tolerance = 1e-9)
    {
        return std::abs(a - b) <= tolerance;
    }

    // A 1001 x 1001 area with no margin: the pie is centred on (500.5, 500.5)
    // with a radius of 500.5 and a hub of a fifth of that.  Nothing is folded.
    sm::PieOptions Options()
    {
        sm::PieOptions options;
        options.rings = 4;
        options.minArc = 0.001;
        options.margin = 0.0;
        options.hubFraction = 0.2;
        return options;
    }

    sm::PiePoint At(sm::PieLayout const& pie, double radius, double angle)
    {
        sm::PiePoint const center = pie.Center();
        return { center.x + radius * std::sin(angle), center.y - radius * std::cos(angle) };
    }

    int HitAt(sm::PieLayout const& pie, double radius, double angle)
    {
        sm::PiePoint const point = At(pie, radius, angle);
        return pie.HitTest(point.x, point.y);
    }

    void CheckInvariants(sm::PieLayout const& pie, int rootDepth)
    {
        auto const items = pie.Items();
        double ringZero = 0.0;
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            sm::PieSlice const& slice = items[i];
            CHECK(slice.sweep > 0.0);
            CHECK(slice.start >= 0.0 && slice.start + slice.sweep <= kTwoPi + 1e-9);
            CHECK(slice.ring >= 0 && slice.ring < pie.Rings());
            CHECK(slice.subtreeEnd > i && slice.subtreeEnd <= items.size());
            CHECK(slice.IsFolded() == !slice.HasEntry());
            if (!slice.IsSpecial())
            {
                CHECK_EQ(slice.depth, rootDepth + slice.ring);
            }
            if (slice.ring == 0)
            {
                ringZero += slice.sweep;
            }

            // Its contents fill its angle exactly, one after another, one ring further out.
            double angle = slice.start;
            double bytes = 0.0;
            for (std::size_t j = i + 1; j < slice.subtreeEnd; j = items[j].subtreeEnd)
            {
                CHECK_EQ(items[j].ring, slice.ring + 1);
                CHECK(Near(items[j].start, angle, 1e-9));
                angle += items[j].sweep;
                bytes += static_cast<double>(items[j].bytes);
            }
            if (slice.subtreeEnd > i + 1)
            {
                CHECK(slice.IsFolder());
                CHECK(Near(angle, slice.start + slice.sweep, 1e-9));
                CHECK(Near(bytes, static_cast<double>(slice.bytes), 0.5));
            }

            // The middle of every slice (where its label goes) hits the slice.
            if (slice.sweep > 1e-6)
            {
                sm::PiePoint const centroid = pie.Centroid(slice);
                CHECK_EQ(pie.HitTest(centroid.x, centroid.y), static_cast<int>(i));
            }
        }
        if (!items.empty())
        {
            CHECK(Near(ringZero, kTwoPi, 1e-9));
        }
    }
}

TEST_CASE(PieSlicesShareTheCircleInProportion)
{
    sm::Folder root;
    AddFile(root, L"twenty", 20);
    AddFile(root, L"fifty", 50);
    AddFile(root, L"thirty", 30);
    Finish(root);

    sm::PieLayout pie;
    pie.Build(&root, 3, 1001, 1001, Options());
    auto const items = pie.Items();
    CHECK_EQ(items.size(), std::size_t{ 3 });
    CHECK_EQ(pie.Total(), std::uint64_t{ 100 });
    CHECK_EQ(pie.Rings(), 1);

    // Largest first, clockwise from twelve o'clock.
    CHECK_EQ(root.Name(items[0].index), std::wstring_view(L"fifty"));
    CHECK_EQ(root.Name(items[1].index), std::wstring_view(L"thirty"));
    CHECK_EQ(root.Name(items[2].index), std::wstring_view(L"twenty"));
    CHECK(Near(items[0].start, 0.0) && Near(items[0].sweep, kPi));
    CHECK(Near(items[1].start, kPi) && Near(items[1].sweep, 0.6 * kPi));
    CHECK(Near(items[2].start, 1.6 * kPi) && Near(items[2].sweep, 0.4 * kPi));
    CHECK_EQ(items[1].bytes, std::uint64_t{ 30 });
    CHECK_EQ(items[0].depth, 3);  // the colour follows the depth in the whole tree, as in the treemap

    // One ring fills everything between the hub and the edge.
    CHECK(Near(pie.HubInner(), 0.0) && Near(pie.HubOuter(), 0.2 * pie.Radius()));
    CHECK(Near(pie.RingsInner(), pie.HubOuter()) && Near(pie.RingsOuter(), pie.Radius()));
    CHECK(Near(pie.InnerRadius(0), pie.HubOuter()) && Near(pie.OuterRadius(0), pie.Radius()));
    CheckInvariants(pie, 3);
}

TEST_CASE(PieRingsNestContentsInsideTheirFolder)
{
    sm::Folder root;
    sm::Folder& folder = AddFolder(root, L"folder");
    AddFile(folder, L"inner-a", 600);
    AddFile(folder, L"inner-b", 200);
    AddFile(root, L"file", 200);
    Finish(root);

    sm::PieLayout pie;
    pie.Build(&root, 0, 1001, 1001, Options());
    auto const items = pie.Items();
    CHECK_EQ(pie.Rings(), 2);
    CHECK_EQ(items.size(), std::size_t{ 4 });

    // Depth-first: the folder, its contents in the next ring, then the file.
    CHECK(items[0].IsFolder() && items[0].ring == 0 && items[0].subtreeEnd == 3);
    CHECK(Near(items[0].start, 0.0) && Near(items[0].sweep, 1.6 * kPi));
    CHECK(items[1].source == &folder && items[1].ring == 1 && items[1].depth == 1);
    CHECK(Near(items[1].start, 0.0) && Near(items[1].sweep, 1.2 * kPi));
    CHECK(Near(items[2].start, 1.2 * kPi) && Near(items[2].sweep, 0.4 * kPi));
    CHECK(items[3].source == &root && items[3].ring == 0 && !items[3].IsFolder());
    CHECK(Near(items[3].start, 1.6 * kPi) && Near(items[3].sweep, 0.4 * kPi));
    CheckInvariants(pie, 0);
}

TEST_CASE(PieFoldsSlicesTooThinToSee)
{
    sm::Folder root;
    AddFile(root, L"big", 1'000'000);
    for (int i = 0; i < 100; ++i)
    {
        AddFile(root, L"tiny", 1);
    }
    AddFile(root, L"empty", 0);
    AddSpecial(root, sm::EntryKind::FreeSpace, 3);
    Finish(root);

    sm::PieOptions options = Options();
    options.minArc = 10.0;
    sm::PieLayout pie;
    pie.Build(&root, 0, 1001, 1001, options);
    auto const items = pie.Items();
    CHECK_EQ(items.size(), std::size_t{ 3 });
    CHECK(items[0].HasEntry() && !items[0].IsFolded());

    // Free space is never grouped, however small: "Free Space" always shows it.
    CHECK(items[1].IsSpecial() && !items[1].IsFolded() && items[1].bytes == 3);

    // The hundred slivers become one slice for "100 smaller items"; the empty file takes no room.
    sm::PieSlice const& rest = items[2];
    CHECK(rest.IsFolded() && !rest.HasEntry() && !rest.IsSelectable());
    CHECK_EQ(rest.folded, std::uint32_t{ 100 });
    CHECK_EQ(rest.bytes, std::uint64_t{ 100 });
    CHECK(rest.source == &root);
    CHECK(Near(rest.start + rest.sweep, kTwoPi));
    CHECK_EQ(HitAt(pie, 300.0, rest.start + rest.sweep / 2.0), 2);  // it still gets a tip
    CheckInvariants(pie, 0);

    // A single small entry is not "grouped": it keeps its own slice.
    sm::Folder lone;
    AddFile(lone, L"big", 1'000'000);
    AddFile(lone, L"tiny", 1);
    Finish(lone);
    pie.Build(&lone, 0, 1001, 1001, options);
    CHECK_EQ(pie.Items().size(), std::size_t{ 2 });
    CHECK(pie.Items()[1].IsSelectable() && !pie.Items()[1].IsFolded());
    CHECK_EQ(lone.Name(pie.Items()[1].index), std::wstring_view(L"tiny"));
}

TEST_CASE(PieHidesFreeSpaceOnRequest)
{
    sm::Folder root;
    AddFile(root, L"a", 300);
    AddFile(root, L"b", 100);
    AddSpecial(root, sm::EntryKind::FreeSpace, 600);
    AddSpecial(root, sm::EntryKind::Unscanned, 50);
    Finish(root);

    sm::PieLayout pie;
    pie.Build(&root, 0, 1001, 1001, Options());
    CHECK_EQ(pie.Items().size(), std::size_t{ 4 });
    CHECK_EQ(pie.Total(), std::uint64_t{ 1050 });
    CHECK(pie.Items()[0].IsSpecial() && pie.Items()[0].depth == -1 && !pie.Items()[0].IsSelectable());
    CheckInvariants(pie, 0);

    sm::PieOptions options = Options();
    options.showFreeSpace = false;
    pie.Build(&root, 0, 1001, 1001, options);
    auto const items = pie.Items();
    CHECK_EQ(items.size(), std::size_t{ 2 });
    CHECK_EQ(pie.Total(), std::uint64_t{ 400 });
    CHECK(!items[0].IsSpecial() && !items[1].IsSpecial());
    CHECK(Near(items[0].sweep, 1.5 * kPi) && Near(items[1].sweep, 0.5 * kPi));
}

TEST_CASE(PieHitTestingFindsSlicesByAngleAndRing)
{
    sm::Folder root;
    sm::Folder& folder = AddFolder(root, L"folder");
    AddFile(folder, L"inner-a", 600);
    AddFile(folder, L"inner-b", 200);
    AddFile(root, L"file", 200);
    Finish(root);

    sm::PieLayout pie;
    pie.Build(&root, 0, 1001, 1001, Options());
    // Hub up to 100.1 pixels, ring 0 up to 300.3, ring 1 up to 500.5.
    sm::PiePoint const center = pie.Center();
    CHECK_EQ(pie.HitTest(center.x, center.y), -1);
    CHECK(pie.HubContains(center.x, center.y));
    CHECK(!pie.HubContains(center.x + 150.0, center.y));

    CHECK_EQ(HitAt(pie, 200.0, 0.8 * kPi), 0);      // the folder, in the ring around the hub
    CHECK_EQ(HitAt(pie, 400.0, 0.8 * kPi), 1);      // its larger file, one ring out
    CHECK_EQ(HitAt(pie, 400.0, 1.4 * kPi), 2);      // its smaller file
    CHECK_EQ(HitAt(pie, 200.0, 1.8 * kPi), 3);      // the root's file...
    CHECK_EQ(HitAt(pie, 400.0, 1.8 * kPi), -1);     // ... has nothing beyond it
    CHECK_EQ(HitAt(pie, 600.0, 0.8 * kPi), -1);     // outside the pie
    CHECK_EQ(HitAt(pie, 200.0, 0.0), 0);            // twelve o'clock starts the first slice
    CHECK_EQ(HitAt(pie, 200.0, kTwoPi - 1e-6), 3);  // ... and ends the last

    std::vector<int> chain;
    sm::PiePoint const point = At(pie, 400.0, 1.4 * kPi);
    pie.ContainersAt(point.x, point.y, chain);
    CHECK_EQ(chain.size(), std::size_t{ 2 });
    CHECK_EQ(chain[0], 0);
    CHECK_EQ(chain[1], 2);
    pie.ContainersAt(center.x, center.y, chain);
    CHECK(chain.empty());

    CHECK_EQ(pie.FindFolder(&folder), 0);
    CHECK_EQ(pie.Find(&root, pie.Items()[3].index), 3);
    CHECK_EQ(pie.Find(&folder, 99), -1);
}

TEST_CASE(InvertedPieTravelsInward)
{
    // Experimental: the folder as a rim around the edge, its contents the ring
    // inside it, each level deeper one ring further in, down to the centre.
    sm::Folder root;
    sm::Folder& folder = AddFolder(root, L"folder");
    AddFile(folder, L"inner-a", 600);
    AddFile(folder, L"inner-b", 200);
    AddFile(root, L"file", 200);
    Finish(root);

    sm::PieLayout normal;
    normal.Build(&root, 0, 1001, 1001, Options());
    sm::PieOptions options = Options();
    options.inverted = true;
    sm::PieLayout pie;
    pie.Build(&root, 0, 1001, 1001, options);

    // The same slices at the same angles: only the rings change places.
    CHECK_EQ(pie.Rings(), normal.Rings());
    CHECK_EQ(pie.Items().size(), normal.Items().size());
    for (std::size_t i = 0; i < pie.Items().size() && i < normal.Items().size(); ++i)
    {
        sm::PieSlice const& a = pie.Items()[i];
        sm::PieSlice const& b = normal.Items()[i];
        CHECK(a.ring == b.ring && a.index == b.index && Near(a.start, b.start) && Near(a.sweep, b.sweep));
    }

    // Ring 1 from the centre to 225.2 pixels, ring 0 to 450.45, the rim (the
    // folder itself, in place of the hub) to 500.5.
    CHECK(Near(pie.HubInner(), 0.9 * pie.Radius()) && Near(pie.HubOuter(), pie.Radius()));
    CHECK(Near(pie.RingsInner(), 0.0) && Near(pie.RingsOuter(), pie.HubInner()));
    CHECK(Near(pie.OuterRadius(0), pie.RingsOuter()) && Near(pie.InnerRadius(0), pie.OuterRadius(1)));
    CHECK_EQ(pie.InnerRadius(1), 0.0);           // the innermost slices meet in the centre
    CHECK_EQ(HitAt(pie, 400.0, 0.8 * kPi), 0);   // the folder, the outermost ring
    CHECK_EQ(HitAt(pie, 200.0, 0.8 * kPi), 1);   // its larger file, one ring in
    CHECK_EQ(HitAt(pie, 10.0, 0.8 * kPi), 1);    // ... right to the centre
    CHECK_EQ(HitAt(pie, 200.0, 1.4 * kPi), 2);   // its smaller file
    CHECK_EQ(HitAt(pie, 400.0, 1.8 * kPi), 3);   // the root's file...
    CHECK_EQ(HitAt(pie, 200.0, 1.8 * kPi), -1);  // ... has nothing inside it
    CHECK_EQ(HitAt(pie, 480.0, 0.8 * kPi), -1);  // the rim
    sm::PiePoint const rim = At(pie, 480.0, 0.3 * kPi);
    CHECK(pie.HubContains(rim.x, rim.y));
    CHECK(!pie.HubContains(pie.Center().x, pie.Center().y));

    std::vector<int> chain;
    sm::PiePoint const point = At(pie, 200.0, 1.4 * kPi);
    pie.ContainersAt(point.x, point.y, chain);
    CHECK(chain.size() == 2 && chain[0] == 0 && chain[1] == 2);
    CheckInvariants(pie, 0);

    // Deeper levels sit nearer the centre, where a ring is shorter: their thin slices fold sooner.
    sm::Folder deep;
    sm::Folder& sub = AddFolder(deep, L"sub");
    AddFile(sub, L"big", 980);
    AddFile(sub, L"small-a", 10);
    AddFile(sub, L"small-b", 10);
    Finish(deep);
    options.minArc = 20.0;  // the small files: about 25 pixels in ring 1 of the normal pie, 7 inverted
    pie.Build(&deep, 0, 1001, 1001, options);
    sm::PieOptions outward = options;
    outward.inverted = false;
    normal.Build(&deep, 0, 1001, 1001, outward);
    CHECK_EQ(normal.Items().size(), std::size_t{ 4 });
    CHECK_EQ(pie.Items().size(), std::size_t{ 3 });
    CHECK(pie.Items().size() == 3 && pie.Items()[2].IsFolded() && pie.Items()[2].folded == 2);
    CheckInvariants(pie, 0);
}

TEST_CASE(PieUsesOnlyTheRingsTheTreeNeeds)
{
    // root \ a \ b \ c \ file: four levels of contents.
    sm::Folder root;
    sm::Folder& a = AddFolder(root, L"a");
    sm::Folder& b = AddFolder(a, L"b");
    sm::Folder& c = AddFolder(b, L"c");
    AddFile(c, L"file", 10);
    (void)AddFolder(root, L"empty");  // an empty folder needs no ring of its own
    Finish(root);

    sm::PieOptions options = Options();
    sm::PieLayout pie;
    options.rings = 6;
    pie.Build(&root, 0, 1001, 1001, options);
    CHECK_EQ(pie.Rings(), 4);
    CHECK(Near(pie.OuterRadius(3), pie.Radius()));
    CheckInvariants(pie, 0);

    options.rings = 2;
    pie.Build(&root, 0, 1001, 1001, options);
    CHECK_EQ(pie.Rings(), 2);
    for (sm::PieSlice const& slice : pie.Items())
    {
        CHECK(slice.ring < 2);
    }
    CheckInvariants(pie, 0);

    // Folders whose contents are all too small to draw get no ring: two such
    // folders fold into one slice, and the pie keeps a single wide ring.
    sm::Folder wide;
    AddFile(wide, L"big", 1'000'000'000);
    for (std::wstring_view const name : { L"x", L"y" })
    {
        sm::Folder& sub = AddFolder(wide, name);
        for (int i = 0; i < 1000; ++i)
        {
            AddFile(sub, L"tiny", 1);
        }
    }
    Finish(wide);
    sm::PieOptions thin = Options();
    thin.minArc = 10.0;
    pie.Build(&wide, 0, 1001, 1001, thin);
    CHECK_EQ(pie.Rings(), 1);
    CHECK(Near(pie.OuterRadius(0), pie.Radius()));
    CHECK_EQ(pie.Items().size(), std::size_t{ 2 });
    CHECK_EQ(pie.Items()[1].folded, std::uint32_t{ 2 });
    CheckInvariants(pie, 0);

    // The density setting trades rings and slice size, as it trades box size in the treemap.
    for (int density = sm::Settings::kMinDensity; density < sm::Settings::kMaxDensity; ++density)
    {
        sm::PieOptions const fewer = sm::PieOptions::ForDensity(density, 1.0);
        sm::PieOptions const more = sm::PieOptions::ForDensity(density + 1, 1.0);
        CHECK(more.rings >= fewer.rings && more.minArc < fewer.minArc);
        CHECK(more.minArc > 2.0 * more.gap);  // never a slice thinner than the gaps around it
    }
}

TEST_CASE(PieOfANestedTreeKeepsItsInvariants)
{
    // The same pseudo-random tree as the treemap test, at a few sizes and densities.
    sm::Folder root;
    std::uint32_t seed = 12345;
    auto const next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    std::vector<sm::Folder*> folders{ &root };
    for (int i = 0; i < 400; ++i)
    {
        sm::Folder& parent = *folders[next() % folders.size()];
        if (next() % 4 == 0)
        {
            folders.push_back(&AddFolder(parent, L"dir"));
        }
        else
        {
            AddFile(parent, L"file", 1 + next() % 1'000'000);
        }
    }
    AddSpecial(root, sm::EntryKind::FreeSpace, 50'000'000);
    Finish(root);

    for (int density = sm::Settings::kMinDensity; density <= sm::Settings::kMaxDensity; ++density)
    {
        for (auto const [width, height] : { std::pair{ 1921, 1081 }, std::pair{ 3841, 2161 }, std::pair{ 120, 90 } })
        {
            sm::PieLayout pie;
            sm::PieOptions const options = sm::PieOptions::ForDensity(density, 1.5);
            pie.Build(&root, 2, width, height, options);
            CHECK(pie.Rings() <= options.rings);
            CheckInvariants(pie, 2);
        }
    }
}

TEST_CASE(PieOfAnEmptyFolderIsJustTheHub)
{
    sm::Folder root;
    sm::PieLayout pie;
    pie.Build(&root, 0, 800, 600, Options());
    CHECK(pie.Items().empty());
    CHECK_EQ(pie.Total(), std::uint64_t{ 0 });
    CHECK(pie.HubContains(400.0, 300.0));
    CHECK_EQ(pie.HitTest(400.0, 300.0), -1);
    CHECK_EQ(pie.HitTest(400.0, 100.0), -1);

    pie.Build(nullptr, 0, 800, 600, Options());
    CHECK(pie.Items().empty());
    CHECK(!pie.HubContains(400.0, 300.0));

    AddFile(root, L"file", 10);
    pie.Build(&root, 0, 20, 20, Options());  // no room for a pie at all
    CHECK(pie.Items().empty());
}

TEST_CASE(PieHandlesHugeSizes)
{
    sm::Folder root;
    AddFile(root, L"big", std::uint64_t{ 1 } << 61);
    AddFile(root, L"half", std::uint64_t{ 1 } << 60);
    Finish(root);

    sm::PieLayout pie;
    pie.Build(&root, 0, 1001, 1001, Options());
    CHECK_EQ(pie.Items().size(), std::size_t{ 2 });
    CHECK(Near(pie.Items()[0].sweep, kTwoPi * 2.0 / 3.0));
    CHECK(Near(pie.Items()[1].sweep, kTwoPi / 3.0));
    CheckInvariants(pie, 0);
}
