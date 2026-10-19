// Tests for the fractal background animations (the port of XaoS's zooming
// engine, formulas, palettes and autopilot).
#include "pch.h"
#include "TestHarness.h"

#include <cmath>
#include <vector>

#include "../../src/Core/Fractal.h"

namespace
{
    std::uint32_t Mandelbrot(double x, double y, double limit = 0.0)
    {
        unsigned iterations = 0;
        return sm::FractalZoom::Calculate(x, y, x, y, limit, iterations);
    }
}

TEST_CASE(FractalPointsInsideAndOutsideTheSet)
{
    // Inside: the centre of the main cardioid, a cycle of two (-1), the
    // real axis' edge (-2); with and without looking for cycles.
    for (double const limit : { 0.0, 1e-3 })
    {
        CHECK_EQ(Mandelbrot(0.0, 0.0, limit), 0u);
        CHECK_EQ(Mandelbrot(-1.0, 0.0, limit), 0u);
        CHECK_EQ(Mandelbrot(-0.1, 0.1, limit), 0u);
    }
    // Looking for cycles is quicker inside, and the same outside.
    unsigned slow = 0;
    unsigned quick = 0;
    sm::FractalZoom::Calculate(-1.0, 0.0, -1.0, 0.0, 0.0, slow);
    sm::FractalZoom::Calculate(-1.0, 0.0, -1.0, 0.0, 1e-3, quick);
    CHECK_EQ(slow, static_cast<unsigned>(sm::FractalZoom::kMaxIterations));
    CHECK(quick < slow / 10);
    CHECK_EQ(Mandelbrot(0.3, 0.6, 1e-3), Mandelbrot(0.3, 0.6));

    // Outside, the smooth count grows towards the set and never jumps by more
    // than about an iteration between near points.
    std::uint32_t const far = Mandelbrot(1.0, 1.0);
    std::uint32_t const nearer = Mandelbrot(0.3, 0.6);
    CHECK(far > 0u && nearer > far);
    std::uint32_t last = Mandelbrot(-2.5, 0.0);
    for (int i = 1; i <= 50; ++i)
    {
        std::uint32_t const value = Mandelbrot(-2.5 + i * 0.01, 0.0);
        CHECK(value > 0u);
        CHECK(std::abs(static_cast<long long>(value) - static_cast<long long>(last)) < 512LL);  // two iterations
        last = value;
    }
    // A Julia set: z starts at the point, c is fixed.
    unsigned iterations = 0;
    CHECK_EQ(sm::FractalZoom::Calculate(0.0, 0.0, -1.0, 0.0, 0.0, iterations), 0u);  // the basilica's centre
    CHECK(sm::FractalZoom::Calculate(1.5, 1.5, -1.0, 0.0, 0.0, iterations) > 0u);
}

TEST_CASE(FractalTableKeepsLinesThatStayNear)
{
    constexpr int kSize = 200;
    std::vector<double> old(kSize);
    for (int i = 0; i < kSize; ++i)
    {
        old[static_cast<std::size_t>(i)] = -1.0 + 2.0 * i / kSize;
    }
    std::vector<sm::FractalZoom::Line> lines(kSize);

    // The same view: every line kept, in place.
    sm::FractalZoom::MakeTable(old, lines, -1.0, 1.0);
    for (int i = 0; i < kSize; ++i)
    {
        auto const& line = lines[static_cast<std::size_t>(i)];
        CHECK(!line.recalculate && !line.dirty);
        CHECK_EQ(line.plus, i);
    }

    // Zoomed in by 2 %: most lines kept, none twice, in order, each near where
    // it should be; the gaps get new lines between their neighbours.
    double const begin = -0.98;
    double const end = 0.98;
    double const step = (end - begin) / kSize;
    sm::FractalZoom::MakeTable(old, lines, begin, end);
    int kept = 0;
    int lastPlus = -1;
    double lastPosition = -2.0;
    for (int i = 0; i < kSize; ++i)
    {
        auto const& line = lines[static_cast<std::size_t>(i)];
        CHECK(line.position >= lastPosition);
        lastPosition = line.position;
        if (!line.recalculate)
        {
            ++kept;
            CHECK(line.plus > lastPlus);
            lastPlus = line.plus;
            CHECK_EQ(line.position, old[static_cast<std::size_t>(line.plus)]);
            CHECK(std::abs(line.position - (begin + i * step)) < 4.0 * step);
        }
        else
        {
            CHECK(line.dirty);
        }
    }
    CHECK(kept > kSize * 9 / 10);
    CHECK(kept < kSize);

    // Somewhere else altogether: everything new.
    sm::FractalZoom::MakeTable(old, lines, 10.0, 12.0);
    for (auto const& line : lines)
    {
        CHECK(line.recalculate);
        CHECK(line.position >= 10.0 && line.position <= 12.0);
    }
}

TEST_CASE(FractalFrameShowsTheSet)
{
    sm::Random random(7);
    sm::FractalZoom zoom;
    zoom.Resize(160, 100);
    zoom.Start(sm::FractalSet::Mandelbrot, random);
    CHECK_EQ(zoom.Flights(), 1u);
    // The whole set, square points: 2.5 high, wider across.
    CHECK(std::abs(zoom.ViewHeight() - 2.5) < 1e-12);
    CHECK(std::abs(zoom.ViewWidth() - 4.0) < 1e-12);

    // The first frame comes in over a few steps, within the budget, then is
    // complete: every point as calculated where its row and column are
    // (solid guessing may take a few for their neighbours).
    int steps = 0;
    while (!zoom.Complete() && steps < 100)
    {
        zoom.Step(random);
        ++steps;
    }
    CHECK(zoom.Complete());
    double const left = zoom.CenterX() - zoom.ViewWidth() / 2;
    double const top = zoom.CenterY() - zoom.ViewHeight() / 2;
    int inside = 0;
    int same = 0;
    for (int y = 0; y < zoom.Height(); ++y)
    {
        for (int x = 0; x < zoom.Width(); ++x)
        {
            std::uint32_t const value = zoom.Value(x, y);
            inside += value == 0 ? 1 : 0;
            // The frame may have zoomed a little since: compare with its own view.
            double const px = left + x * zoom.ViewWidth() / zoom.Width();
            double const py = top + y * zoom.ViewHeight() / zoom.Height();
            same += (value == 0) == (Mandelbrot(px, py) == 0) ? 1 : 0;
        }
    }
    int const points = zoom.Width() * zoom.Height();
    CHECK(inside > points / 20 && inside < points / 2);  // the set covers part of the view
    CHECK(same > points * 9 / 10);

    // Painted: every point opaque.
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(points));
    zoom.Paint(pixels, 1.5);
    for (std::uint32_t const pixel : pixels)
    {
        CHECK_EQ(pixel >> 24, 0xFFu);
    }
}

TEST_CASE(FractalAutopilotFliesIn)
{
    for (sm::FractalSet const set : { sm::FractalSet::Mandelbrot, sm::FractalSet::Julia })
    {
        sm::Random random(set == sm::FractalSet::Mandelbrot ? 99u : 1234u);
        sm::FractalZoom zoom;
        zoom.Resize(120, 80);
        zoom.Start(set, random);
        double const start = zoom.ViewWidth();
        double smallest = start;
        // Two minutes of flight.
        for (int i = 0; i < 120 * 25; ++i)
        {
            zoom.Step(random);
            CHECK(std::isfinite(zoom.ViewWidth()) && zoom.ViewWidth() > 0.0);
            CHECK(std::isfinite(zoom.CenterX()) && std::isfinite(zoom.CenterY()));
            CHECK(zoom.ViewWidth() <= 100.0);
            smallest = std::min(smallest, zoom.ViewWidth());
        }
        CHECK(smallest < start / 100);  // it went deep (or hit the end of precision and started over)
        CHECK(zoom.Flights() >= 1u);
    }
}

TEST_CASE(FractalAutopilotStartsOverAtTheEndOfPrecision)
{
    sm::Random random(5);
    sm::FractalZoom zoom;
    zoom.Resize(64, 48);
    zoom.Start(sm::FractalSet::Mandelbrot, random);
    unsigned const flights = zoom.Flights();
    // Fly until the doubles run out: it starts over rather than show noise.
    for (int i = 0; i < 25 * 60 * 20 && zoom.Flights() == flights; ++i)
    {
        zoom.Step(random);
        CHECK(zoom.ViewWidth() > 1e-15);
    }
    CHECK(zoom.Flights() > flights);
}

TEST_CASE(FractalPalettesAsXaosMakesThem)
{
    sm::FractalPalette const palette;
    auto const colors = palette.Colors();
    // The default palette: 31 colours, 8 entries apart, from black.
    CHECK_EQ(colors.size(), std::size_t{ 254 });
    CHECK_EQ(colors[0], 0x000000u);
    CHECK_EQ(colors[8], 0x7877EEu);   // {120, 119, 238}
    CHECK_EQ(colors[16], 0x180719u);  // {24, 7, 25}

    // Random ones: the same seed, the same palette; another seed, another one.
    for (int algorithm = 0; algorithm < 3; ++algorithm)
    {
        sm::FractalPalette a;
        sm::FractalPalette b;
        sm::FractalPalette c;
        a.Randomize(42, algorithm);
        b.Randomize(42, algorithm);
        c.Randomize(43, algorithm);
        CHECK(a.Colors().size() >= 2u && a.Colors().size() <= 256u);
        CHECK(std::equal(a.Colors().begin(), a.Colors().end(), b.Colors().begin(), b.Colors().end()));
        CHECK(!std::equal(a.Colors().begin(), a.Colors().end(), c.Colors().begin(), c.Colors().end()));
    }
}
