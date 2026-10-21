// SpaceWindow - the fractal background animations (Fractal.h).
//
// Ported from XaoS, a fast portable realtime fractal zoomer
//   Copyright (C) 1996,1997 by Jan Hubicka and Thomas Marsh, and the XaoS
//   developers (https://github.com/xaos-project/XaoS)
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 2 of the License, or (at your option)
// any later version.  It is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
// Public License (LICENSE.txt) for more details.
#include "pch.h"
#include "Fractal.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace sm
{
    namespace
    {
        // The zooming engine's fixed-point steps (zoom.cpp): a line FPMUL
        // units wide; old lines up to RANGE lines away may be kept.
        constexpr int kFpMul = 64;
        constexpr int kRange = 4;
        constexpr int kDSize = 2 * kRange;  // an old line is near at most this many new ones in a row
        constexpr int kDSizes = 3;          // log2(kDSize)
        constexpr int kDSizeMask = kDSize - 1;
        constexpr int kIRange = kFpMul * kRange;
        constexpr long long kNewPrice = static_cast<long long>(kFpMul) * kFpMul * kRange * kRange;
        constexpr long long Price(int a, int b) noexcept
        {
            return static_cast<long long>(a - b) * (a - b);
        }

        constexpr int kGuessRange = 3;                // solid guessing over this many lines (XaoS's default range)
        constexpr double kBailout = 4.0;              // DEFAULT_BAILOUT, of |z|^2
        constexpr std::uint64_t kBudget = 3'000'000;  // iterations a frame; the rest waits for later frames

        // The autopilot (config.h, autopilot.cpp, ui_helper.cpp).
        constexpr int kLookSize = 2;  // the square around a spot it looks at: 2 * 2 + 1 points a side
        constexpr int kRange1 = 30;   // how far it wanders from the last spot
        constexpr int kGuesses = kRange1 * kRange1 / 2;
        constexpr int kGuesses1 = 10;
        constexpr int kMaxTime = 10;  // ticks before it picks again, at most
        constexpr int kMinCount = 5;  // decisions without getting deeper before it starts over
        // XaoS's acceleration and top speed, halved for a background: about
        // 1.27x a second at full speed.
        constexpr double kSpeedup = 0.0006 * 3 / 2;
        constexpr double kMaxStep = 0.008 * 3 / 2;
        constexpr double kMul = (1.0 / FractalZoom::kStepsPerSecond) * 20.0;  // XaoS's frames are 1/20 s

        constexpr double kCycleSpeed = 6.0;  // palette entries a second
        constexpr unsigned kBrightness = 4;  // fifths: a little darker, so the map stays easy to read

        // Where the flights start (formulas.cpp's mandel entry; Julia sets
        // fill a little more), and the Julia sets flown into: each c is near
        // the Mandelbrot set's edge, where they are most intricate.
        constexpr double kMandelbrotX = -0.75;
        constexpr double kMandelbrotSize = 2.5;
        constexpr double kJuliaSize = 3.0;
        constexpr std::array<std::array<double, 2>, 12> kJuliaSeeds = { {
            { -0.8, 0.156 },
            { -0.7269, 0.1889 },
            { 0.285, 0.01 },
            { -0.4, 0.6 },
            { -0.835, -0.2321 },
            { -0.70176, -0.3842 },
            { 0.355, 0.355 },
            { -0.54, 0.54 },
            { -0.123, 0.745 },   // Douady's rabbit
            { -0.391, -0.587 },  // a Siegel disk
            { -1.0, 0.0 },       // the basilica
            { -0.75, 0.11 },
        } };

        // XaoS's own random numbers (util/random.cpp), so its palettes come out as its do.
        class XaosRandom
        {
        public:
            explicit XaosRandom(unsigned seed) noexcept : state_(seed) {}
            int Next() noexcept
            {
                state_ = ((state_ * 1103515245u) + 12345u) & kMax;
                return static_cast<int>(state_);
            }
            // rrandom: "do not use modulo type random since it should bring very
            // different results for slightly different sizes"
            int Scaled(int range) noexcept { return static_cast<int>((Next() / static_cast<double>(kMax)) * range); }

        private:
            static constexpr unsigned kMax = 0xFFFFFF;
            unsigned state_;
        };

        using Rgb3 = std::array<int, 3>;

        // The default palette's 31 colours (palette.cpp, colors1).
        constexpr std::array<Rgb3, 31> kDefaultColors = { {
            { 0, 0, 0 },    { 120, 119, 238 }, { 24, 7, 25 },  { 197, 66, 28 },   { 29, 18, 11 }, { 135, 46, 71 },
            { 24, 27, 13 }, { 241, 230, 128 }, { 17, 31, 24 }, { 240, 162, 139 }, { 11, 4, 30 },  { 106, 87, 189 },
            { 29, 21, 14 }, { 12, 140, 118 },  { 10, 6, 29 },  { 50, 144, 77 },   { 22, 0, 24 },  { 148, 188, 243 },
            { 4, 32, 7 },   { 231, 146, 14 },  { 10, 13, 20 }, { 184, 147, 68 },  { 13, 28, 3 },  { 169, 248, 152 },
            { 4, 0, 34 },   { 62, 83, 48 },    { 7, 21, 22 },  { 152, 97, 184 },  { 8, 3, 12 },   { 247, 92, 235 },
            { 31, 32, 16 },
        } };

        constexpr int kMaxEntries = 256;  // a 256-colour screen's palette, as XaoS made them first

        // mksmooth: `setsegments` gradients of `segmentSize` entries between
        // the colours, the last one filling what is left.
        std::vector<std::uint32_t> Smooth(std::span<Rgb3 const> colors, int setsegments, int segmentSize)
        {
            std::vector<std::uint32_t> entries;
            int const nsegments = static_cast<int>(colors.size());
            int segmentSize1 = segmentSize;
            for (int i = 0; i < setsegments; ++i)
            {
                if (i == setsegments - 1)
                {
                    segmentSize1 = kMaxEntries - static_cast<int>(entries.size()) - 2;
                }
                if (segmentSize1 <= 0)
                {
                    break;
                }
                Rgb3 const& from = colors[static_cast<std::size_t>(i % nsegments)];
                Rgb3 const& to = colors[static_cast<std::size_t>((i + 1) % setsegments % nsegments)];
                std::array<float, 3> value{};
                std::array<float, 3> add{};
                for (std::size_t c = 0; c < 3; ++c)
                {
                    value[c] = static_cast<float>(from[c]);
                    add[c] = static_cast<float>(to[c] - from[c]) / static_cast<float>(segmentSize1);
                }
                for (int y = 0; y < segmentSize1; ++y)
                {
                    if (static_cast<int>(entries.size()) == kMaxEntries)
                    {
                        return entries;
                    }
                    auto const channel = [&value](std::size_t c) {
                        return static_cast<std::uint32_t>(std::clamp(static_cast<int>(value[c]), 0, 255));
                    };
                    entries.push_back((channel(0) << 16) | (channel(1) << 8) | channel(2));
                    for (std::size_t c = 0; c < 3; ++c)
                    {
                        value[c] += add[c];
                    }
                }
            }
            return entries;
        }

        // palette.cpp's hsv_to_rgb: h, s and v from 0 to 255.
        Rgb3 HsvToRgb(int h, int s, int v) noexcept
        {
            h = ((h + 256) % 256 + 256) % 256;
            if (s == 0)
            {
                return { v, v, v };
            }
            int const hue = h * 6;
            int const f = hue & 255;
            int const p = v * (256 - s) / 256;
            int const q = (v * (256 - (s * f) / 256)) >> 8;
            int const t = (v * (256 * 256 - (s * (256 - f)))) >> 16;
            switch (hue / 256)
            {
            case 0: return { v, t, p };
            case 1: return { q, v, p };
            case 2: return { p, v, t };
            case 3: return { p, q, v };
            case 4: return { t, p, v };
            default: return { v, p, q };
            }
        }

        // The three ways XaoS colours a random palette's segments.
        void RandomizeSegments(std::vector<Rgb3>& colors, int nsegments, bool whitemode, XaosRandom& random)
        {
            int i = 0;
            if (whitemode)
            {
                colors[0] = { 255, 255, 255 };
                for (i = 0; i < nsegments; i += 2)
                {
                    if (i != 0)
                    {
                        colors[static_cast<std::size_t>(i)] = { random.Next() % 256, random.Next() % 256,
                                                                random.Next() % 256 };
                    }
                    if (i + 1 < nsegments)
                    {
                        colors[static_cast<std::size_t>(i) + 1] = { random.Next() % 35, random.Next() % 35,
                                                                    random.Next() % 35 };
                    }
                }
            }
            else
            {
                for (i = 0; i < nsegments; i += 2)
                {
                    colors[static_cast<std::size_t>(i)] = { random.Next() % 35, random.Next() % 35,
                                                            random.Next() % 35 };
                    if (i + 1 < nsegments)
                    {
                        colors[static_cast<std::size_t>(i) + 1] = { random.Next() % 256, random.Next() % 256,
                                                                    random.Next() % 256 };
                    }
                }
            }
            colors[static_cast<std::size_t>(i) - 1] = colors[0];
        }

        void RandomizeSegments2(std::vector<Rgb3>& colors, int nsegments, bool whitemode, XaosRandom& random)
        {
            int const white = whitemode ? 255 : 0;
            int i = 0;
            for (i = 0; i < nsegments; ++i)
            {
                if (i % 3 == 2)
                {
                    colors[static_cast<std::size_t>(i)] = { white, white, white };
                }
                else if (i % 3 == 0)
                {
                    colors[static_cast<std::size_t>(i)] = { 255 - white, 255 - white, 255 - white };
                }
                else
                {
                    colors[static_cast<std::size_t>(i)] = { random.Next() % 256, random.Next() % 256,
                                                            random.Next() % 256 };
                }
            }
            colors[static_cast<std::size_t>(i) - 1] = colors[0];
        }

        void RandomizeSegments3(std::vector<Rgb3>& colors, int nsegments, XaosRandom& random)
        {
            int i = 0;
            for (i = 0; i < nsegments; ++i)
            {
                if (i % 3 == 0)
                {
                    int const level = i % 6 != 0 ? 255 : 0;
                    colors[static_cast<std::size_t>(i)] = { level, level, level };
                }
                else
                {
                    int const s = random.Next() % 256;
                    int h = random.Next() % (128 - 32);
                    int v = random.Next() % 128;
                    if ((i % 6 > 3) != (i % 3 == 1))
                    {
                        h += 42 + 16;
                    }
                    else
                    {
                        h += 42 + 128 + 16;
                        v += 128 + 64;
                    }
                    colors[static_cast<std::size_t>(i)] = HsvToRgb(h, s, v);
                }
            }
            colors[static_cast<std::size_t>(i) - 1] = colors[0];
        }

        // addprices: the lines in the middle of a run of new ones matter most,
        // then the middles of the halves, and so on.
        void AddPrices(std::span<FractalZoom::Line> lines, int r, int r2)
        {
            while (r < r2)
            {
                int const r3 = r + ((r2 - r) >> 1);
                auto& line = lines[static_cast<std::size_t>(r3)];
                line.price = (lines[static_cast<std::size_t>(r2)].position - line.position) * line.price;
                AddPrices(lines, r, r3);
                r = r3 + 1;
            }
        }

        // The point of a line the fill copies from: the calculated line
        // nearest it, before or after.
        template <typename Lines>
        int NearestCalculated(Lines const& lines, int index)
        {
            int before = index - 1;
            while (before >= 0 && lines[static_cast<std::size_t>(before)].dirty)
            {
                --before;
            }
            int after = index + 1;
            int const size = static_cast<int>(lines.size());
            while (after < size && lines[static_cast<std::size_t>(after)].dirty)
            {
                ++after;
            }
            double const here = lines[static_cast<std::size_t>(index)].position;
            if (before < 0)
            {
                return after < size ? after : -1;
            }
            if (after >= size)
            {
                return before;
            }
            return here - lines[static_cast<std::size_t>(before)].position >
                           lines[static_cast<std::size_t>(after)].position - here
                       ? after
                       : before;
        }
    }

    void FractalPalette::Default()
    {
        // segmentsize 8, (maxentries + 3) / 8 segments, the last one shorter.
        colors_ = Smooth(kDefaultColors, (kMaxEntries + 3) / 8, 8);
    }

    void FractalPalette::Randomize(std::uint32_t seed, int algorithm)
    {
        XaosRandom random(seed);
        auto const next = static_cast<unsigned>(random.Next());
        bool const whitemode = random.Next() % 2 != 0;

        constexpr int qq = 255;
        int segmentSize = 0;
        for (int i = 0; i < 4; ++i)
        {
            segmentSize += random.Scaled(qq / 3 + 4);  // smaller segments, more likely
        }
        segmentSize = std::abs(segmentSize / 2 - qq / 3 + 3);
        segmentSize = std::clamp(segmentSize, 8, kMaxEntries / 3);
        int const segments = std::max(1, (kMaxEntries + segmentSize - 5) / segmentSize);

        XaosRandom colorsRandom(next);
        std::vector<Rgb3> colors(static_cast<std::size_t>(segments) + 2);
        switch (algorithm)
        {
        case 2: RandomizeSegments3(colors, segments, colorsRandom); break;
        case 1: RandomizeSegments2(colors, segments, whitemode, colorsRandom); break;
        default: RandomizeSegments(colors, segments, whitemode, colorsRandom); break;
        }
        colors.resize(static_cast<std::size_t>(segments));
        colors_ = Smooth(colors, segments, segmentSize);
        if (colors_.size() < 2)
        {
            Default();
        }
    }

    std::uint32_t FractalZoom::Calculate(double zre, double zim, double pre, double pim, double periodicityLimit,
                                         unsigned& iterations) noexcept
    {
        // docalc.h's CALC and PERI, with mand's FORMULA, SMOOTH: iter counts down.
        unsigned iter = kMaxIterations;
        double rp = zre * zre;
        double ip = zim * zim;
        double szmag = 0.0;
        bool periodic = false;
        auto const formula = [&]() noexcept {
            szmag = rp + ip;
            zim = (zim * zre) * 2 + pim;
            zre = rp - ip + pre;
            ip = zim * zim;
            rp = zre * zre;
        };
        if (periodicityLimit <= 0.0)
        {
            while (rp + ip < kBailout && iter != 0)
            {
                formula();
                --iter;
            }
        }
        else
        {
            // The first few iterations without checking.
            unsigned iter1 = 8;
            while (rp + ip < kBailout && iter1 != 0)
            {
                formula();
                --iter1;
            }
            if (iter1 != 0)
            {
                iter -= 8 - iter1;
            }
            else
            {
                // Save z now and then and see whether it comes back: a cycle,
                // inside the set.  The saves grow further apart, to catch long cycles.
                iter -= 8;
                double r1 = zre;
                double s1 = zim;
                unsigned whenSaveNew = 3;  // 2^n - 1: a mask
                int whenIncSave = 10;
                while (rp + ip < kBailout && iter != 0)
                {
                    formula();
                    if ((iter & whenSaveNew) == 0)
                    {
                        r1 = zre;
                        s1 = zim;
                        if (--whenIncSave == 0)
                        {
                            whenSaveNew = ((whenSaveNew + 1) << 1) - 1;
                            whenIncSave = 10;
                        }
                    }
                    else if (std::abs(r1 - zre) < periodicityLimit && std::abs(s1 - zim) < periodicityLimit)
                    {
                        periodic = true;
                        break;
                    }
                    --iter;
                }
            }
        }
        iterations = kMaxIterations - iter;
        if (periodic || iter == 0)
        {
            return 0;
        }
        // SMOOTHOUTPUT: the iterations, and how far past the bailout the last one went.
        double const magnitude = rp + ip + 0.000001;
        szmag += 0.000001;
        double const smooth = static_cast<double>(kMaxIterations - iter) * 256.0 +
                              std::log(kBailout / szmag) / std::log(magnitude / szmag) * 256.0;
        return 1u + static_cast<std::uint32_t>(std::clamp(smooth, 0.0, 16'000'000.0));
    }

    void FractalZoom::MakeTable(std::span<double const> fpos, std::span<Line> realloc, double begin, double end)
    {
        int const size = static_cast<int>(realloc.size());
        if (realloc.empty() || fpos.size() != realloc.size())
        {
            return;
        }

        // The dynamic programme's states: for each old line p kept as new
        // line i (the last kDSize i's), and for each i calculated anew, the
        // cheapest way there and where it came from.
        struct Dyn
        {
            long long price = 0;
            int previous = -1;   // the state before; -1: none (the first line)
            bool fresh = false;  // this line is calculated after all ("previous + nosetadd")
        };
        std::vector<Dyn> dyn(static_cast<std::size_t>(size) * (kDSize + 1));
        int const freshBase = size << kDSizes;
        auto const added = [](int p, int i) { return (p << kDSizes) + (i & kDSizeMask); };
        auto const fresh = [freshBase](int i) { return freshBase + i; };
        auto const set = [&dyn](int state, long long price, int previous, bool isFresh = false) {
            Dyn& d = dyn[static_cast<std::size_t>(state)];
            d.price = price;
            d.previous = previous;
            d.fresh = isFresh;
        };
        auto const priceOf = [&dyn](int state) { return dyn[static_cast<std::size_t>(state)].price; };
        std::vector<int> best(static_cast<std::size_t>(size));
        std::vector<int> best1(static_cast<std::size_t>(size));
        auto const at = [](std::vector<int>& v, int p) -> int& { return v[static_cast<std::size_t>(p)]; };

        // The old lines in the new frame's fixed point: 0 at `begin`, size * FPMUL at `end`.
        std::vector<int> posStore(static_cast<std::size_t>(size) + 1);
        int* const pos = posStore.data();
        double const tofix = static_cast<double>(size) * kFpMul / (end - begin);
        for (int counter = size - 1; counter >= 0; --counter)
        {
            double const fixed = (fpos[static_cast<std::size_t>(counter)] - begin) * tofix;
            pos[counter] = std::isnan(fixed) ? INT_MAX / 2 : static_cast<int>(std::clamp(fixed, -1e9, 1e9));
            if (counter < size - 1 && pos[counter] > pos[counter + 1])
            {
                pos[counter] = pos[counter + 1];
            }
        }
        pos[size] = INT_MAX;

        int ps = 0;  // the old lines near the last new line: ps .. pe
        int ps1 = 0;
        int pe = 0;
        long long bestprice = LLONG_MAX;
        int bestdata = -1;
        int y = 0;
        for (int i = 0; i < size; ++i, y += kFpMul)
        {
            bestprice = LLONG_MAX;
            int p = ps;
            std::swap(best, best1);

            // Old lines further than kIRange away are dearer than a new one.
            int yend = std::max(y - kIRange, -kFpMul);
            while (pos[p] <= yend)
            {
                ++p;
            }
            ps1 = p;
            yend = y + kIRange;

            // First choice: calculate line i anew, after the best way to i - 1.
            int previous = -1;
            long long myprice = 0;
            if (ps != pe && p > ps)
            {
                previous = p < pe ? at(best, p - 1) : at(best, pe - 1);
                myprice = priceOf(previous);
            }
            else if (i > 0)
            {
                previous = fresh(i - 1);
                myprice = priceOf(previous);
            }
            int data = fresh(i);
            myprice += kNewPrice;
            bestdata = data;
            bestprice = myprice;
            set(data, myprice, previous);
            data = added(p, i);

            // Then: keep one of the old lines in range, never one an earlier
            // new line kept, nor one before it.
            if (ps != pe)
            {
                long long price1 = 0;
                if (p == ps)
                {
                    if (pos[p] != pos[p + 1])
                    {
                        previous = fresh(i - 1);
                        myprice = priceOf(previous) + Price(pos[p], y);
                        if (myprice < bestprice)
                        {
                            bestprice = myprice;
                            bestdata = data;
                            set(data, myprice, previous);
                        }
                    }
                    at(best1, p) = bestdata;
                    data += kDSize;
                    ++p;
                }

                previous = -2;  // none yet
                price1 = myprice;
                while (p < pe)
                {
                    if (pos[p] != pos[p + 1])
                    {
                        if (previous != at(best, p - 1))
                        {
                            previous = at(best, p - 1);
                            price1 = myprice = priceOf(previous);
                            // A cheaper way to i - 1 than before: line i - 1 might
                            // rather be calculated anew, after it.
                            if (myprice + kNewPrice < bestprice)
                            {
                                bestprice = myprice + kNewPrice;
                                bestdata = data - kDSize;
                                set(bestdata, bestprice, previous, true);
                                at(best1, p - 1) = bestdata;
                            }
                        }
                        else
                        {
                            myprice = price1;
                        }
                        myprice += Price(pos[p], y);
                        if (myprice < bestprice)
                        {
                            bestprice = myprice;
                            bestdata = data;
                            set(data, myprice, previous);
                        }
                        else if (pos[p] > y)
                        {
                            at(best1, p) = bestdata;
                            data += kDSize;
                            ++p;
                            break;
                        }
                    }
                    at(best1, p) = bestdata;
                    data += kDSize;
                    ++p;
                }
                while (p < pe)
                {
                    at(best1, p) = bestdata;
                    data += kDSize;
                    ++p;
                }

                // The old lines only line i has in range.
                previous = p > ps ? at(best, p - 1) : fresh(i - 1);
                price1 = priceOf(previous);
                if (price1 + kNewPrice < bestprice && p > ps1)
                {
                    myprice = price1 + kNewPrice;
                    bestprice = myprice;
                    bestdata = data - kDSize;
                    set(bestdata, myprice, previous, true);
                    at(best1, p - 1) = bestdata;
                }
                while (pos[p] < yend)
                {
                    if (pos[p] != pos[p + 1])
                    {
                        myprice = price1 + Price(pos[p], y);
                        if (myprice < bestprice)
                        {
                            bestprice = myprice;
                            bestdata = data;
                            set(data, myprice, previous);
                        }
                        else if (pos[p] > y)
                        {
                            break;
                        }
                    }
                    at(best1, p) = bestdata;
                    data += kDSize;
                    ++p;
                }
                while (pos[p] < yend)
                {
                    at(best1, p) = bestdata;
                    ++p;
                }
            }
            else if (pos[p] < yend)
            {
                // Line i - 1 was calculated anew: no overlap to mind.
                long long myprice1 = 0;
                if (i > 0)
                {
                    previous = fresh(i - 1);
                    myprice1 = priceOf(previous);
                }
                else
                {
                    previous = -1;
                }
                while (pos[p] < yend)
                {
                    if (pos[p] != pos[p + 1])
                    {
                        myprice = myprice1 + Price(pos[p], y);
                        if (myprice < bestprice)
                        {
                            set(data, myprice, previous);
                            bestprice = myprice;
                            bestdata = data;
                        }
                        else if (pos[p] > y)
                        {
                            break;
                        }
                    }
                    at(best1, p) = bestdata;
                    ++p;
                    data += kDSize;
                }
                while (pos[p] < yend)
                {
                    at(best1, p) = bestdata;
                    ++p;
                }
            }
            ps = ps1;
            ps1 = pe;
            pe = p;
        }

        // Back from the cheapest way to the last line: what each line is made from.
        int mode = (begin > fpos[0] && end < fpos[static_cast<std::size_t>(size) - 1]) ? 1 : 0;  // zooming in
        if (pos[0] > 0 && pos[size - 1] < size * kFpMul)
        {
            mode = 2;  // zooming out
        }
        int lastplus = 0;
        int state = bestdata;
        for (int i = size; i > 0 && state >= 0;)
        {
            --i;
            Line& line = realloc[static_cast<std::size_t>(i)];
            Dyn const& d = dyn[static_cast<std::size_t>(state)];
            if (d.fresh || state >= freshBase)
            {
                line.recalculate = true;
                line.dirty = true;
                if (++lastplus >= size)
                {
                    lastplus = 0;
                }
                line.plus = lastplus;
            }
            else
            {
                int const p = state >> kDSizes;
                line.position = fpos[static_cast<std::size_t>(p)];
                line.plus = p;
                line.dirty = false;
                line.recalculate = false;
            }
            state = d.previous;
        }

        // newpositions: the new lines evenly between the kept ones around them.
        double const step = static_cast<double>(size) / (end - begin);
        int rs = -1;
        while (rs < size - 1)
        {
            int re = rs + 1;
            if (realloc[static_cast<std::size_t>(re)].recalculate)
            {
                while (re < size && realloc[static_cast<std::size_t>(re)].recalculate)
                {
                    ++re;
                }
                double runEnd = re == size ? end : realloc[static_cast<std::size_t>(re)].position;
                double start = 0.0;
                if (rs == -1)
                {
                    start = std::min(begin, runEnd);
                }
                else
                {
                    start = realloc[static_cast<std::size_t>(rs)].position;
                }
                if (re == size && start > runEnd)
                {
                    runEnd = start;
                }
                double const add = (runEnd - start) / (re - rs);
                for (++rs; rs < re; ++rs)
                {
                    start += add;
                    Line& line = realloc[static_cast<std::size_t>(rs)];
                    line.position = start;
                    double const off = std::abs(fpos[static_cast<std::size_t>(rs)] - start) * step;
                    switch (mode)
                    {
                    case 1: line.price = 1.0 / (1.0 + off); break;
                    case 2: line.price = off * (rs == 0 || rs == size - 1 ? 500.0 : 1.0); break;
                    default: line.price = 1.0; break;
                    }
                }
            }
            rs = re;
        }
    }

    void FractalZoom::Start(FractalSet set, Random& random)
    {
        set_ = set;
        if (flights_ == 0)
        {
            palette_.Default();
        }
        else
        {
            palette_.Randomize(random.Next(), random.Below(3));
        }
        auto const& seed = kJuliaSeeds[static_cast<std::size_t>(random.Below(static_cast<int>(kJuliaSeeds.size())))];
        seedRe_ = seed[0];
        seedIm_ = seed[1];
        centerX_ = set == FractalSet::Mandelbrot ? kMandelbrotX : 0.0;
        centerY_ = 0.0;
        double const size = set == FractalSet::Mandelbrot ? kMandelbrotSize : kJuliaSize;
        // Square points: the larger side shows more.
        spanX_ = size;
        spanY_ = size;
        if (width_ > 0 && height_ > 0)
        {
            if (width_ > height_)
            {
                spanX_ = size * width_ / height_;
            }
            else
            {
                spanY_ = size * height_ / width_;
            }
        }
        step_ = 0.0;
        button_ = Button::None;
        hasTarget_ = false;
        fresh_ = true;
        ++flights_;
        CleanAutopilot();
    }

    void FractalZoom::Resize(int width, int height)
    {
        width = std::max(width, 0);
        height = std::max(height, 0);
        if (width == width_ && height == height_)
        {
            return;
        }
        // The same plane under the smaller side, square points.
        double const size = std::min(spanX_, spanY_);
        width_ = width;
        height_ = height;
        if (width_ > 0 && height_ > 0)
        {
            spanX_ = width_ >= height_ ? size * width_ / height_ : size;
            spanY_ = width_ >= height_ ? size : size * height_ / width_;
        }
        auto const pixels = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
        values_.assign(pixels, 0);
        old_.assign(pixels, 0);
        xpos_.assign(static_cast<std::size_t>(width_), 0.0);
        ypos_.assign(static_cast<std::size_t>(height_), 0.0);
        columns_.assign(static_cast<std::size_t>(width_), Line{});
        rows_.assign(static_cast<std::size_t>(height_), Line{});
        hasTarget_ = false;
        fresh_ = true;
        incomplete_ = true;
    }

    void FractalZoom::CleanAutopilot() noexcept
    {
        minSize_ = 1000.0;
        maxSize_ = 0.0;
        autime_ = 0;
        minLong_ = 0;
        hasTarget_ = false;
    }

    void FractalZoom::ViewEdges(double& left, double& top, double& right, double& bottom) const noexcept
    {
        left = centerX_ - spanX_ / 2;
        right = centerX_ + spanX_ / 2;
        top = centerY_ - spanY_ / 2;
        bottom = centerY_ + spanY_ / 2;
    }

    void FractalZoom::Step(Random& random)
    {
        if (width_ <= 0 || height_ <= 0)
        {
            return;
        }
        if (flights_ == 0)
        {
            Start(set_, random);
        }
        Autopilot(random);
        Zoom();
        if (fresh_ || moved_ || incomplete_)
        {
            Render();
        }
    }

    // do_autopilot, a tick at a time.
    void FractalZoom::Autopilot(Random& random)
    {
        double left = 0.0;
        double top = 0.0;
        double right = 0.0;
        double bottom = 0.0;
        ViewEdges(left, top, right, bottom);
        // Out of precision: a tenth of a point no longer moves the edge.
        double const step = spanX_ / width_ / 10;
        double const ystep = spanY_ / height_ / 10;
        double const pos = right + step;
        double const pos1 = right - step;
        double const ypos = bottom + ystep;
        double const ypos1 = bottom - ystep;
        if ((minLong_ > kMinCount && button_ == Button::ZoomOut) || !(pos > right) || !(ypos > bottom) ||
            pos1 >= right || ypos1 >= bottom || spanX_ > 100.0 || std::isnan(spanX_ * spanY_ * centerX_ * centerY_))
        {
            Start(set_, random);
        }
        // Nothing to look at yet, or waiting for a better picture?
        if (fresh_ || (button_ == Button::None && incomplete_))
        {
            return;
        }
        if (spanX_ < minSize_)
        {
            minSize_ = spanX_;
            minLong_ = 0;
        }
        if (spanX_ > maxSize_)  // no oscillating
        {
            minSize_ = spanX_;
            maxSize_ = spanX_;
            minLong_ = 0;
        }
        if (autime_ <= 0)
        {
            ++minLong_;
            autime_ = random.Below(kMaxTime);
            int const x = hasTarget_ ? targetX_ : INT_MAX;
            int const y = hasTarget_ ? targetY_ : INT_MAX;
            bool found = LookForEdge(random, x, y, kRange1, kGuesses);
            if (!found)
            {
                found = LookForDetail(random, x, y, kRange1, kGuesses);
            }
            if (random.Below(30) == 0)
            {
                found = false;  // now and then, somewhere else altogether
            }
            if (!found)
            {
                found = LookForEdge(random, x, y, 10000, kGuesses1);
            }
            if (!found)
            {
                found = LookForDetail(random, x, y, 10000, kGuesses1);
            }
            if (!found)
            {
                if (incomplete_)
                {
                    button_ = Button::None;
                }
                else
                {
                    button_ = Button::ZoomOut;
                    autime_ >>= 1;
                }
            }
        }
        --autime_;
    }

    void FractalZoom::PickSpot(Random& random, int x, int y, int range)
    {
        hasTarget_ = true;
        if (range > width_ / 2)
        {
            targetX_ = random.Below(width_ - 2 * kLookSize - 1) + kLookSize;
            targetY_ = random.Below(height_ - 2 * kLookSize - 1) + kLookSize;
        }
        else
        {
            targetX_ = std::clamp(random.Below(range) - (range >> 1) + x, kLookSize, width_ - 2 - kLookSize);
            targetY_ = std::clamp(random.Below(range) - (range >> 1) + y, kLookSize, height_ - 2 - kLookSize);
        }
    }

    // look1: a spot on the set's edge - a few of the points around it inside.
    bool FractalZoom::LookForEdge(Random& random, int x, int y, int range, int tries)
    {
        if (width_ < 2 * kLookSize + 4 || height_ < 2 * kLookSize + 4 ||
            (range < width_ / 2 && (x < 0 || x > width_ || y < 0 || y > height_)))
        {
            return false;
        }
        int inside = 0;
        do
        {
            --tries;
            inside = 0;
            PickSpot(random, x, y, range);
            for (int j = targetY_ - kLookSize; j <= targetY_ + kLookSize; ++j)
            {
                for (int i = targetX_ - kLookSize; i <= targetX_ + kLookSize; ++i)
                {
                    inside += Value(i, j) == 0 ? 1 : 0;
                }
            }
        } while ((inside == 0 || inside > kLookSize * kLookSize) && tries > 0);
        if (tries > 0)
        {
            button_ = Button::ZoomIn;
            return true;
        }
        return false;
    }

    // look2: a spot where the colours vary - few pairs of points the same,
    // compared by palette entry, as on XaoS's 256-colour screens.
    bool FractalZoom::LookForDetail(Random& random, int x, int y, int range, int tries)
    {
        if (width_ < 2 * kLookSize + 4 || height_ < 2 * kLookSize + 4 ||
            (range < width_ / 2 && (x < 0 || x > width_ || y < 0 || y > height_)))
        {
            return false;
        }
        auto const entry = [this](int i, int j) {
            std::uint32_t const value = Value(i, j);
            return value == 0 ? UINT32_MAX : (value - 1) >> 8;
        };
        int same = 0;
        do
        {
            --tries;
            same = 0;
            PickSpot(random, x, y, range);
            for (int j = targetY_ - kLookSize; j < targetY_ + kLookSize; ++j)
            {
                for (int i = targetX_ - kLookSize; i <= targetX_ + kLookSize; ++i)
                {
                    for (int j1 = j + 1; j1 < targetY_ + kLookSize; ++j1)
                    {
                        for (int i1 = i + 1; i1 < targetX_ + kLookSize; ++i1)
                        {
                            same += entry(i, j) == entry(i1, j1) ? 1 : 0;
                        }
                    }
                }
            }
        } while (same > kLookSize * kLookSize / 2 && tries > 0);
        if (tries > 0)
        {
            button_ = Button::ZoomIn;
            return true;
        }
        return false;
    }

    // uih_update's zooming: toward the spot, speeding up to the top speed, or
    // away from it, or slowing down.
    void FractalZoom::Zoom()
    {
        if (step_ != 0.0)
        {
            if (hasTarget_)
            {
                zoomX_ = centerX_ - spanX_ / 2 + targetX_ * spanX_ / width_;
                zoomY_ = centerY_ - spanY_ / 2 + targetY_ * spanY_ / height_;
            }
            double const scale = std::pow(1.0 - step_, kMul);
            centerX_ = zoomX_ + (centerX_ - zoomX_) * scale;
            centerY_ = zoomY_ + (centerY_ - zoomY_) * scale;
            spanX_ *= scale;
            spanY_ *= scale;
            moved_ = true;
        }
        switch (button_)
        {
        case Button::ZoomIn: step_ = std::min(step_ + kSpeedup * 2 * kMul, kMaxStep); break;
        case Button::ZoomOut: step_ = std::max(step_ - kSpeedup * 2 * kMul, -kMaxStep); break;
        default:
            if (step_ > 0.0)
            {
                step_ = step_ < kSpeedup * kMul ? 0.0 : step_ - kSpeedup * kMul;
            }
            else if (step_ < 0.0)
            {
                step_ = step_ > -kSpeedup * kMul ? 0.0 : step_ + kSpeedup * kMul;
            }
            break;
        }
    }

    // do_fractal: the new frame from the old one.
    void FractalZoom::Render()
    {
        double left = 0.0;
        double top = 0.0;
        double right = 0.0;
        double bottom = 0.0;
        ViewEdges(left, top, right, bottom);
        periodicityLimit_ = spanX_ / width_;
        spent_ = 0;
        if (fresh_)
        {
            Fresh();
        }
        else
        {
            MakeTable(xpos_, columns_, left, right);
            MakeTable(ypos_, rows_, top, bottom);
            Move();
        }
        CalculateNew();
        Fill();
        for (int x = 0; x < width_; ++x)
        {
            xpos_[static_cast<std::size_t>(x)] = columns_[static_cast<std::size_t>(x)].position;
        }
        for (int y = 0; y < height_; ++y)
        {
            ypos_[static_cast<std::size_t>(y)] = rows_[static_cast<std::size_t>(y)].position;
        }
        fresh_ = false;
        moved_ = false;
    }

    void FractalZoom::Fresh()
    {
        double left = 0.0;
        double top = 0.0;
        double right = 0.0;
        double bottom = 0.0;
        ViewEdges(left, top, right, bottom);
        auto const init = [](std::vector<Line>& lines, double begin, double end) {
            double const step = (end - begin) / static_cast<double>(lines.size());
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                lines[i] = Line{ begin + static_cast<double>(i) * step, 1.0, 0, true, true };
            }
        };
        init(columns_, left, right);
        init(rows_, top, bottom);
        std::ranges::fill(values_, 0u);
    }

    // moveoldpoints: the kept rows' kept columns, from the old frame.
    void FractalZoom::Move()
    {
        old_.swap(values_);
        for (int y = 0; y < height_; ++y)
        {
            Line const& row = rows_[static_cast<std::size_t>(y)];
            if (row.dirty)
            {
                continue;
            }
            for (int x = 0; x < width_; ++x)
            {
                Line const& column = columns_[static_cast<std::size_t>(x)];
                if (!column.dirty)
                {
                    At(values_, x, y) = At(old_, column.plus, row.plus);
                }
            }
        }
    }

    // calculatenewinterruptible: the new lines, the most telling first, as
    // far as the budget goes.
    void FractalZoom::CalculateNew()
    {
        std::vector<std::pair<double, int>> queue;  // price, line (columns 0 .., then rows width_ ..)
        auto const enqueue = [&queue](std::vector<Line>& lines, int base) {
            int const size = static_cast<int>(lines.size());
            for (int r = 0; r < size; ++r)
            {
                if (!lines[static_cast<std::size_t>(r)].recalculate)
                {
                    continue;
                }
                int r2 = r;
                while (r2 < size && lines[static_cast<std::size_t>(r2)].recalculate)
                {
                    queue.emplace_back(0.0, base + r2);
                    ++r2;
                }
                if (r2 == size)
                {
                    --r2;
                }
                AddPrices(lines, r, r2);
                r = r2;
            }
        };
        enqueue(columns_, 0);
        enqueue(rows_, width_);
        for (auto& [price, line] : queue)
        {
            price = line < width_ ? columns_[static_cast<std::size_t>(line)].price
                                  : rows_[static_cast<std::size_t>(line - width_)].price;
        }
        std::ranges::stable_sort(queue, [](auto const& a, auto const& b) { return a.first > b.first; });

        bool column = false;
        bool row = false;
        for (auto const& [price, line] : queue)
        {
            if (spent_ >= kBudget && column && row)
            {
                break;
            }
            if (line < width_)
            {
                CalculateColumn(line);
                column = true;
            }
            else
            {
                CalculateLine(line - width_);
                row = true;
            }
        }
    }

    std::uint32_t FractalZoom::Point(double x, double y, bool periodicity)
    {
        unsigned iterations = 0;
        double const limit = periodicity ? periodicityLimit_ : 0.0;
        std::uint32_t const value = set_ == FractalSet::Mandelbrot
                                        ? Calculate(x, y, x, y, limit, iterations)
                                        : Calculate(x, y, seedRe_, seedIm_, limit, iterations);
        spent_ += iterations + 1;
        return value;
    }

    // calcline: a row, guessing points whose calculated neighbours - the
    // rows above and below it within range, the columns either side - all agree.
    void FractalZoom::CalculateLine(int ry)
    {
        Line& line = rows_[static_cast<std::size_t>(ry)];
        line.dirty = false;
        line.recalculate = false;
        double const y = line.position;
        auto const dirtyRow = [this](int r) { return rows_[static_cast<std::size_t>(r)].dirty; };
        auto const dirtyColumn = [this](int c) { return columns_[static_cast<std::size_t>(c)].dirty; };
        auto const columnAt = [this](int c) { return columns_[static_cast<std::size_t>(c)].position; };

        int rend = std::max(ry - kGuessRange - 1, 0);
        int ryl = ry - 1;
        while (rend <= ryl && dirtyRow(ryl))
        {
            --ryl;
        }
        rend = std::min(ry + kGuessRange, height_);
        int ryr = ry + 1;
        while (rend > ryr && dirtyRow(ryr))
        {
            ++ryr;
        }
        std::uint32_t* const current = &At(values_, 0, ry);
        if (ryr >= height_ || ryl < 0 || dirtyRow(ryr) || dirtyRow(ryl))
        {
            for (int x = 0; x < width_; ++x)
            {
                if (!dirtyColumn(x))
                {
                    current[x] = Point(columnAt(x), y, true);
                }
            }
            return;
        }
        std::uint32_t const* const up = &At(values_, 0, ryl);
        std::uint32_t const* const down = &At(values_, 0, ryr);
        constexpr int kFar = INT_MAX / 2;
        int distup = kFar;
        int distdown = 0;
        for (int x = 0; x < width_; ++x, --distdown, ++distup)
        {
            if (dirtyColumn(x))
            {
                continue;
            }
            if (distdown <= 0)
            {
                int next = x + 1;
                while (next < width_ && dirtyColumn(next))
                {
                    ++next;
                }
                distdown = next == width_ ? kFar : next - x;
            }
            bool const nearby = distdown < INT_MAX / 4 && distup < INT_MAX / 4;
            std::uint32_t const c = down[x];
            if (nearby && up[x] == c && c == current[x - distup] && c == up[x - distup] && c == up[x + distdown] &&
                c == down[x + distdown] && c == down[x - distup])
            {
                current[x] = c;
            }
            else
            {
                // All of them outside the set: no cycle to look for.
                bool const outside = nearby && up[x] != 0 && c != 0 && current[x - distup] != 0 &&
                                     up[x - distup] != 0 && up[x + distdown] != 0 && down[x - distup] != 0 &&
                                     down[x + distdown] != 0;
                current[x] = Point(columnAt(x), y, !outside);
            }
            distup = 0;
        }
    }

    // calccolumn: the same, down a column.
    void FractalZoom::CalculateColumn(int rx)
    {
        Line& line = columns_[static_cast<std::size_t>(rx)];
        line.dirty = false;
        line.recalculate = false;
        double const x = line.position;
        auto const dirtyRow = [this](int r) { return rows_[static_cast<std::size_t>(r)].dirty; };
        auto const dirtyColumn = [this](int c) { return columns_[static_cast<std::size_t>(c)].dirty; };
        auto const rowAt = [this](int r) { return rows_[static_cast<std::size_t>(r)].position; };

        int rend = std::max(rx - kGuessRange + 1, 0);
        int rxl = rx - 1;
        while (rend <= rxl && dirtyColumn(rxl))
        {
            --rxl;
        }
        rend = std::min(rx + kGuessRange, width_);
        int rxr = rx + 1;
        while (rxr < rend && dirtyColumn(rxr))
        {
            ++rxr;
        }
        if (rxr >= width_ || rxl < 0 || dirtyColumn(rxr) || dirtyColumn(rxl))
        {
            for (int y = 0; y < height_; ++y)
            {
                if (!dirtyRow(y))
                {
                    At(values_, rx, y) = Point(x, rowAt(y), true);
                }
            }
            return;
        }
        auto const value = [this](int column, int row) { return At(values_, column, row); };
        constexpr int kFar = INT_MAX / 2;
        int distup = kFar;
        int distdown = 0;
        for (int y = 0; y < height_; ++y, --distdown, ++distup)
        {
            if (dirtyRow(y))
            {
                continue;
            }
            if (distdown <= 0)
            {
                int next = y + 1;
                while (next < height_ && dirtyRow(next))
                {
                    ++next;
                }
                distdown = next == height_ ? kFar : next - y;
            }
            bool const nearby = distdown < INT_MAX / 4 && distup < INT_MAX / 4;
            std::uint32_t const c = value(rxr, y);
            if (nearby && value(rxl, y) == c && value(rxl, y - distup) == c && value(rxr, y - distup) == c &&
                value(rx, y - distup) == c && value(rxr, y + distdown) == c && value(rxl, y + distdown) == c)
            {
                At(values_, rx, y) = c;
            }
            else
            {
                bool const outside = nearby && value(rxl, y) != 0 && c != 0 && value(rxr, y + distdown) != 0 &&
                                     value(rxl, y + distdown) != 0 && value(rxl, y - distup) != 0 &&
                                     value(rx, y - distup) != 0 && value(rxr, y - distup) != 0;
                At(values_, rx, y) = Point(x, rowAt(y), !outside);
            }
            distup = 0;
        }
    }

    // fill: the lines the budget did not reach copy the nearest calculated
    // one, and take its place in the plane, so a later frame calculates
    // where they should be.
    void FractalZoom::Fill()
    {
        int left = 0;
        for (int x = 0; x < width_; ++x)
        {
            if (!columns_[static_cast<std::size_t>(x)].dirty)
            {
                continue;
            }
            int const from = NearestCalculated(columns_, x);
            ++left;
            if (from >= 0)
            {
                for (int y = 0; y < height_; ++y)
                {
                    if (!rows_[static_cast<std::size_t>(y)].dirty)
                    {
                        At(values_, x, y) = At(values_, from, y);
                    }
                }
                columns_[static_cast<std::size_t>(x)].position = columns_[static_cast<std::size_t>(from)].position;
            }
        }
        for (auto& column : columns_)
        {
            column.dirty = false;
        }
        for (int y = 0; y < height_; ++y)
        {
            if (!rows_[static_cast<std::size_t>(y)].dirty)
            {
                continue;
            }
            int const from = NearestCalculated(rows_, y);
            ++left;
            if (from >= 0)
            {
                std::copy_n(&At(values_, 0, from), width_, &At(values_, 0, y));
                rows_[static_cast<std::size_t>(y)].position = rows_[static_cast<std::size_t>(from)].position;
            }
        }
        for (auto& row : rows_)
        {
            row.dirty = false;
        }
        incomplete_ = left > 0;
    }

    void FractalZoom::Paint(std::span<std::uint32_t> pixels, double time) const
    {
        std::span<std::uint32_t const> const colors = palette_.Colors();
        auto const count = static_cast<std::uint32_t>(colors.size());
        auto const pixelCount = std::min(pixels.size(), values_.size());
        if (count < 2)
        {
            std::fill_n(pixels.begin(), pixelCount, 0xFF000000u);
            return;
        }
        auto const dim = [](std::uint32_t rgb) {
            std::uint32_t result = 0xFF000000u;
            for (int shift = 0; shift <= 16; shift += 8)
            {
                result |= (((rgb >> shift) & 0xFFu) * kBrightness / 5u) << shift;
            }
            return result;
        };
        std::uint32_t const inside = dim(colors[0]);
        std::uint64_t const period = static_cast<std::uint64_t>(count - 1) << 8;
        auto const cycle =
            static_cast<std::uint64_t>(std::fmod(time * kCycleSpeed * 256.0, static_cast<double>(period)));
        for (std::size_t i = 0; i < pixelCount; ++i)
        {
            std::uint32_t const value = values_[i];
            if (value == 0)
            {
                pixels[i] = inside;
                continue;
            }
            // SMOOTHOUTPUT's palette: between two entries, after the first.
            std::uint64_t const at = (value - 1u + cycle) % period;
            auto const entry = static_cast<std::uint32_t>(at >> 8);
            auto const weight = static_cast<std::uint32_t>(at & 255u);
            std::uint32_t const a = colors[1 + entry];
            std::uint32_t const b = entry == count - 2 ? colors[1] : colors[2 + entry];
            std::uint32_t mixed = 0;
            for (int shift = 0; shift <= 16; shift += 8)
            {
                std::uint32_t const ca = (a >> shift) & 0xFFu;
                std::uint32_t const cb = (b >> shift) & 0xFFu;
                mixed |= ((cb * weight + ca * (256u - weight)) >> 8) << shift;
            }
            pixels[i] = dim(mixed);
        }
    }
}
