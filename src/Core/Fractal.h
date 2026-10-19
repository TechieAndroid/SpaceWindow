// SpaceWindow - the fractal background animations: an endless flight into the
// Mandelbrot set or a Julia set, as XaoS's autopilot flies it.
//
// Ported from XaoS, a fast portable realtime fractal zoomer
//   Copyright (C) 1996,1997 by Jan Hubicka and Thomas Marsh, and the XaoS
//   developers (https://github.com/xaos-project/XaoS)
// and, as XaoS, free software under the GNU General Public License, version 2
// or (at your option) any later version.  See LICENSE.txt.
//
// What comes from where in XaoS:
//  * The zooming engine (src/engine/zoom.cpp): each frame keeps the rows and
//    columns of the last one that lie near enough where the new frame wants
//    them - which to keep is a dynamic programme over every row, then every
//    column (mkrealloc_table) - moves their pixels, and calculates only the
//    rest: the most telling lines first, guessing pixels whose neighbours all
//    agree ("solid guessing", zoomd.h), within a budget a frame; the lines the
//    budget does not reach copy their nearest neighbour until a later frame
//    calculates them.
//  * The formulas (src/engine/formulas.cpp, docalc.h): z = z^2 + c, with
//    periodicity checking for the Mandelbrot set and XaoS's smooth colouring.
//  * The palettes (src/util/palette.cpp): the default one and the three kinds
//    of random ones, from XaoS's own random numbers.
//  * The autopilot (src/ui-hlp/autopilot.cpp): it picks a spot on the edge of
//    the set, or where the colours vary most, and zooms in on it, wandering a
//    little; when it finds nothing it zooms out, and when it is stuck or has
//    reached the end of the numbers' precision, it starts over.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "Backdrop.h"

namespace sm
{
    enum class FractalSet : std::uint8_t
    {
        Mandelbrot,
        Julia,
    };

    // A palette as XaoS makes them: entry 0 for the points in the set, the
    // others cycled through by the points outside it.  0x00RRGGBB.
    class FractalPalette
    {
    public:
        FractalPalette() { Default(); }
        void Default();                                     // mkdefaultpalette
        void Randomize(std::uint32_t seed, int algorithm);  // mkpalette: algorithm 0, 1 or 2
        std::span<std::uint32_t const> Colors() const noexcept { return colors_; }

    private:
        std::vector<std::uint32_t> colors_;
    };

    class FractalZoom
    {
    public:
        static constexpr int kMaxIterations = 1000;      // XaoS's DEFAULT_MAX_ITER
        static constexpr double kStepsPerSecond = 25.0;  // the autopilot's ticks (one per Step)

        // A new flight over `set`: its whole view, a new palette (the default
        // one for the very first flight) and, for a Julia set, a new seed.
        void Start(FractalSet set, Random& random);
        // Keeps the view; everything is calculated again.
        void Resize(int width, int height);
        // One tick: the autopilot steers, the view zooms, and the frame is
        // calculated as far as the budget goes.
        void Step(Random& random);
        // The frame in colours: premultiplied 0xAARRGGBB, Width() x Height().
        // `time` (seconds) cycles the palette.
        void Paint(std::span<std::uint32_t> pixels, double time) const;

        int Width() const noexcept { return width_; }
        int Height() const noexcept { return height_; }
        FractalSet Set() const noexcept { return set_; }
        // What a point of the frame shows: 0 inside the set, else 1 + its
        // smooth iteration count in 256ths.
        std::uint32_t Value(int x, int y) const noexcept
        {
            return values_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                           static_cast<std::size_t>(x)];
        }
        // The view: its centre and size in the complex plane.
        double CenterX() const noexcept { return centerX_; }
        double CenterY() const noexcept { return centerY_; }
        double ViewWidth() const noexcept { return spanX_; }
        double ViewHeight() const noexcept { return spanY_; }
        bool Complete() const noexcept { return !incomplete_; }  // every line of the frame calculated
        unsigned Flights() const noexcept { return flights_; }   // Start()s, including the autopilot's own
        double JuliaRe() const noexcept { return seedRe_; }
        double JuliaIm() const noexcept { return seedIm_; }

        // The calculation of one point (XaoS's mand_calc, mand_peri and
        // mand_julia, smooth): 0 inside, else 1 + the smooth count in 256ths.
        // `iterations` gets the iterations it took.
        static std::uint32_t Calculate(double zre, double zim, double pre, double pim, double periodicityLimit,
                                       unsigned& iterations) noexcept;

        // One axis of XaoS's realloc table: what each row or column of the
        // new frame is made from.
        struct Line
        {
            double position = 0.0;  // where in the plane
            double price = 0.0;     // how much calculating it matters (the order lines are calculated in)
            int plus = 0;           // the old line it is moved from, when it is kept
            bool recalculate = false;
            bool dirty = false;  // not calculated yet in this frame
        };
        // mkrealloc_table and newpositions: the lines of the new frame, from
        // `begin` to `end` in `size` steps, given where the old frame's lines
        // were (`old`).  Kept lines cost the square of how far they are from
        // where they should be, new ones as much as one four lines away.
        static void MakeTable(std::span<double const> old, std::span<Line> lines, double begin, double end);

    private:
        enum class Button : std::uint8_t
        {
            None,
            ZoomIn,   // BUTTON1
            ZoomOut,  // BUTTON3
        };

        void Again();  // the autopilot starts over: XaoS's again()
        void CleanAutopilot() noexcept;
        void Autopilot(Random& random);
        bool LookForEdge(Random& random, int x, int y, int range, int tries);    // look1
        bool LookForDetail(Random& random, int x, int y, int range, int tries);  // look2
        void PickSpot(Random& random, int x, int y, int range);
        void Zoom();
        void Render();
        void Fresh();
        void Move();
        void CalculateNew();
        void CalculateLine(int y);    // calcline: a row
        void CalculateColumn(int x);  // calccolumn
        void Fill();
        std::uint32_t Point(double x, double y, bool periodicity);
        void ViewEdges(double& left, double& top, double& right, double& bottom) const noexcept;
        std::uint32_t& At(std::vector<std::uint32_t>& buffer, int x, int y) const noexcept
        {
            return buffer[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
        }

        FractalSet set_ = FractalSet::Mandelbrot;
        FractalPalette palette_;
        int width_ = 0;
        int height_ = 0;
        double centerX_ = -0.75;
        double centerY_ = 0.0;
        double spanX_ = 2.5;
        double spanY_ = 2.5;
        double seedRe_ = 0.0;  // a Julia set's c
        double seedIm_ = 0.0;
        unsigned flights_ = 0;

        std::vector<std::uint32_t> values_;  // the frame
        std::vector<std::uint32_t> old_;     // the frame before
        std::vector<double> xpos_;           // where the frame's columns are in the plane
        std::vector<double> ypos_;           // and its rows
        std::vector<Line> columns_;
        std::vector<Line> rows_;
        bool fresh_ = true;       // nothing of the frame before can be kept
        bool incomplete_ = true;  // lines were left to copy their neighbours
        bool moved_ = true;       // the view changed since the last frame
        double periodicityLimit_ = 0.0;
        std::uint64_t spent_ = 0;  // iterations this frame

        // The autopilot (XaoS's uih_context): where it is heading, and how.
        double step_ = 0.0;   // how fast the view zooms: 1 - its scale a tick
        double zoomX_ = 0.0;  // the point zoomed into
        double zoomY_ = 0.0;
        int targetX_ = 0;  // ... as a point of the frame
        int targetY_ = 0;
        bool hasTarget_ = false;
        Button button_ = Button::None;
        double minSize_ = 0.0;
        double maxSize_ = 0.0;
        int autime_ = 0;
        int minLong_ = 0;
    };
}
