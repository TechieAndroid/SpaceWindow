// Tests for the background animations' simulations.
#include "pch.h"
#include "TestHarness.h"

#include "../../src/Core/Backdrop.h"

TEST_CASE(FireBurnsFromTheBottomUp)
{
    constexpr int kWidth = 40;
    constexpr int kHeight = 60;
    sm::Random random(12345);
    sm::FireField fire;
    fire.Resize(kWidth, kHeight);
    for (int i = 0; i < 300; ++i)
    {
        fire.Step(random);
    }
    // The bottom row stays white-hot, heat stays in range, and the fire is
    // hottest low down and cools as it rises.
    long long low = 0;
    long long high = 0;
    for (int y = 0; y < kHeight; ++y)
    {
        for (int x = 0; x < kWidth; ++x)
        {
            int const heat = fire.At(x, y);
            CHECK(heat >= 0 && heat <= sm::FireField::kHottest);
            if (y == kHeight - 1)
            {
                CHECK_EQ(heat, sm::FireField::kHottest);
            }
            (y >= kHeight / 2 ? low : high) += heat;
        }
    }
    CHECK(low > 2 * high);
    CHECK(high > 0);  // the flames do rise

    // Resizing keeps the flames, standing on the bottom row.
    int const before = fire.At(10, kHeight - 10);
    fire.Resize(30, 40);
    CHECK_EQ(fire.At(10, 40 - 10), before);
    CHECK_EQ(fire.At(0, 39), sm::FireField::kHottest);
}

TEST_CASE(FireColoursRunFromNothingToWhiteHeat)
{
    CHECK_EQ(sm::FireColor(0), 0u);  // transparent
    CHECK_EQ(sm::FireColor(sm::FireField::kHottest), 0xFFFFFFFFu);
    CHECK_EQ(sm::FireColor(1000), 0xFFFFFFFFu);
    CHECK_EQ(sm::FireColor(1) >> 24, 0xFFu);
}

TEST_CASE(MatrixDropsFallAndStartAgain)
{
    constexpr int kColumns = 20;
    constexpr int kRows = 30;
    sm::Random random(7);
    sm::MatrixRain rain;
    rain.Resize(kColumns, kRows, random);
    std::vector<bool> lit(kColumns, false);
    for (int step = 0; step < 600; ++step)
    {
        rain.Step(random);
        for (int column = 0; column < kColumns; ++column)
        {
            int heads = 0;
            for (int row = 0; row < kRows; ++row)
            {
                float const light = rain.Light(column, row);
                CHECK(light >= 0.0f && light <= 1.0f);
                CHECK(rain.Glyph(column, row) >= 0 && rain.Glyph(column, row) < sm::MatrixRain::kGlyphs);
                heads += rain.IsHead(column, row) ? 1 : 0;
                if (light > 0.0f)
                {
                    lit[static_cast<std::size_t>(column)] = true;
                }
                if (rain.IsHead(column, row))
                {
                    CHECK_EQ(light, 1.0f);  // the head is the brightest
                }
            }
            CHECK(heads <= 1);
        }
    }
    // Every column has rained, and drops keep coming after the first ones fell off.
    CHECK(std::ranges::all_of(lit, [](bool on) { return on; }));
}

TEST_CASE(ColourMixStaysInItsTwelveBands)
{
    bool changes = false;
    for (float const time : { 0.0f, 1.5f, 120.0f })
    {
        for (int across = -6; across <= 6; ++across)
        {
            for (int down = -4; down <= 4; ++down)
            {
                float const u = 0.3f * static_cast<float>(across);
                float const v = 0.25f * static_cast<float>(down);
                int const band = sm::ColorMixBand(u, v, time, 1.0f, 2.0f);
                CHECK(band >= 0 && band < 12);
                changes = changes || band != sm::ColorMixBand(u, v, time + 20.0f, 1.0f, 2.0f);
            }
        }
    }
    CHECK(changes);  // it moves
}

TEST_CASE(LifeFollowsConwaysRules)
{
    sm::Random random(1);
    sm::LifeField life;
    life.Resize(8, 8, random, 0.0f);  // empty
    CHECK_EQ(life.Population(), std::size_t{ 0 });

    // A blinker turns from upright to flat and back.
    life.SetAlive(3, 2);
    life.SetAlive(3, 3);
    life.SetAlive(3, 4);
    life.Step(random, 0);
    CHECK(life.Alive(2, 3) && life.Alive(3, 3) && life.Alive(4, 3));
    CHECK(!life.Alive(3, 2) && !life.Alive(3, 4));
    CHECK_EQ(life.Age(3, 2), std::uint8_t{ 1 });  // died a generation ago: fading
    CHECK_EQ(life.Population(), std::size_t{ 3 });
    life.Step(random, 0);
    CHECK(life.Alive(3, 2) && life.Alive(3, 3) && life.Alive(3, 4));
    CHECK_EQ(life.Generation(), 2u);

    // The field is a torus: a blinker across the edge works the same.
    sm::LifeField edge;
    edge.Resize(6, 6, random, 0.0f);
    edge.SetAlive(0, 5);
    edge.SetAlive(0, 0);
    edge.SetAlive(0, 1);
    edge.Step(random, 0);
    CHECK(edge.Alive(5, 0) && edge.Alive(0, 0) && edge.Alive(1, 0));

    // Without entropy an empty field stays empty; with it, life comes back.
    sm::LifeField empty;
    empty.Resize(30, 20, random, 0.0f);
    for (int i = 0; i < 9; ++i)
    {
        empty.Step(random, 10);
    }
    CHECK_EQ(empty.Population(), std::size_t{ 0 });
    empty.Step(random, 10);
    CHECK(empty.Population() > 0);
}

TEST_CASE(TunnelRingsShrinkIntoTheDistance)
{
    std::vector<sm::TunnelRing> rings;
    sm::TunnelRings(10.0, 1000.0f, 700.0f, rings);
    CHECK(rings.size() > 10);
    for (std::size_t i = 0; i < rings.size(); ++i)
    {
        sm::TunnelRing const& ring = rings[i];
        CHECK(std::isfinite(ring.x) && std::isfinite(ring.y) && ring.radius > 0.0f);
        CHECK(ring.depth >= 0.0f && ring.depth <= 1.0f);
        if (i > 0)
        {
            // Farthest first: each ring nearer and bigger than the one before.
            CHECK(ring.depth < rings[i - 1].depth && ring.radius > rings[i - 1].radius);
        }
    }

    // Endless: an hour on, as many rings in view.
    std::vector<sm::TunnelRing> later;
    sm::TunnelRings(3600.0, 1000.0f, 700.0f, later);
    CHECK(later.size() + 1 >= rings.size() && later.size() <= rings.size() + 1);

    // It curves: where the tube leads moves about.
    sm::TunnelRings(14.0, 1000.0f, 700.0f, later);
    CHECK(std::abs(later.front().x - rings.front().x) > 1.0f || std::abs(later.front().y - rings.front().y) > 1.0f);

    sm::TunnelRings(10.0, 0.0f, 700.0f, later);
    CHECK(later.empty());
}
