// SpaceWindow - the background animations: what moves where, without drawing.
// New in SpaceWindow.  The renderer (src/App/MapRendererBackdrop.cpp) draws them
// in the space the map leaves free.
//
//  * Fire: the PSX DOOM fire (Fabien Sanglard, "How DOOM fire was done"), as
//    kiedtl/fire and the ly display manager play it in a terminal.
//  * Matrix: falling characters, as CMatrix and ly.
//  * Colour mix: the swirling three-colour pattern of ly's "colormix".
//  * Life: Conway's Game of Life with a pinch of new life now and then (ly's
//    "entropy"), so that it never settles.
//  * Tunnel: an endless, curving neon tube flown through, in the style of a
//    year-2000 screen saver.
//  * Mandelbrot and Julia: flights into the sets, ported from XaoS (Fractal.h).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace sm
{
    // Cheap, reproducible randomness (xorshift32): the animations need a lot of it.
    class Random
    {
    public:
        Random() noexcept = default;
        explicit Random(std::uint32_t seed) noexcept : state_(seed != 0 ? seed : 1u) {}
        std::uint32_t Next() noexcept
        {
            state_ ^= state_ << 13;
            state_ ^= state_ >> 17;
            state_ ^= state_ << 5;
            return state_;
        }
        int Below(int count) noexcept
        {
            return count > 0 ? static_cast<int>(Next() % static_cast<std::uint32_t>(count)) : 0;
        }
        float Unit() noexcept { return static_cast<float>(Next() >> 8) * (1.0f / 16777216.0f); }  // [0, 1)

    private:
        std::uint32_t state_ = 0x9E3779B9u;
    };

    // The DOOM fire: a field of heat, the bottom row as hot as it gets.  Every
    // step each cell's heat moves one row up, drifting a cell sideways and
    // cooling a level now and then, so flames lick upwards and die away.
    class FireField
    {
    public:
        static constexpr int kHottest = 36;  // heat 0 (none) .. 36, one palette colour each

        void Resize(int width, int height);  // keeps the flames it can
        void Step(Random& random);
        int Width() const noexcept { return width_; }
        int Height() const noexcept { return height_; }
        std::uint8_t At(int x, int y) const noexcept { return heat_[static_cast<std::size_t>(y) * width_ + x]; }
        std::span<std::uint8_t const> Heat() const noexcept { return heat_; }

    private:
        int width_ = 0;
        int height_ = 0;
        std::uint32_t coolOdds_ = 0;  // out of 2^16: the chance a cell cools a level on its way up
        std::vector<std::uint8_t> heat_;
    };

    // The fire's colours, from no heat (transparent black) to white heat.
    std::uint32_t FireColor(int heat) noexcept;  // 0xAARRGGBB, premultiplied

    // Falling characters.  Each column has a drop - a bright head and a trail
    // fading behind it - that falls a row per step (slow columns every other
    // step) and starts again at the top a while after leaving the bottom.
    class MatrixRain
    {
    public:
        static constexpr int kGlyphs = 64;  // glyph numbers 0 .. 63, chosen by the renderer

        void Resize(int columns, int rows, Random& random);
        void Step(Random& random);
        int Columns() const noexcept { return columns_; }
        int Rows() const noexcept { return rows_; }
        int Glyph(int column, int row) const noexcept
        {
            return glyphs_[static_cast<std::size_t>(row) * columns_ + column];
        }

        // How lit a cell is: 1 at a drop's head, fading along its trail, 0 elsewhere.
        float Light(int column, int row) const noexcept;
        bool IsHead(int column, int row) const noexcept;

    private:
        struct Drop
        {
            int head = 0;    // row of the head (it may be below the last row, the trail still showing)
            int length = 0;  // rows of trail
            int wait = 0;    // steps before the next drop starts, while the column is dark
            bool slow = false;
        };
        void Restart(Drop& drop, Random& random) const;

        int columns_ = 0;
        int rows_ = 0;
        unsigned steps_ = 0;
        std::vector<Drop> drops_;
        std::vector<std::uint8_t> glyphs_;
    };

    // ly's colour mix: a smooth, slowly swirling field, banded into twelve
    // steps between three colours.  `u` and `v` run from -1 to 1 across the
    // height (u further, by the aspect ratio, across the width); `time` in
    // seconds.  Returns the band, 0 .. 11.
    int ColorMixBand(float u, float v, float time, float phaseA, float phaseB) noexcept;

    // Conway's Game of Life on a torus: a dead cell with three live neighbours
    // is born, a live one with two or three survives.  Every `entropyEvery`
    // generations a few random cells come alive.  Cells remember how long
    // ago they died, so they can fade.
    class LifeField
    {
    public:
        static constexpr std::uint8_t kAlive = 0;  // Age() of a live cell; dead ones count up to kLongDead
        static constexpr std::uint8_t kLongDead = 255;

        void Resize(int width, int height, Random& random, float density = 0.3f);  // keeps the cells it can
        void Step(Random& random, int entropyEvery = 10);
        int Width() const noexcept { return width_; }
        int Height() const noexcept { return height_; }
        bool Alive(int x, int y) const noexcept { return Age(x, y) == kAlive; }
        void SetAlive(int x, int y) noexcept { age_[static_cast<std::size_t>(y) * width_ + x] = kAlive; }
        std::uint8_t Age(int x, int y) const noexcept { return age_[static_cast<std::size_t>(y) * width_ + x]; }
        unsigned Generation() const noexcept { return generation_; }
        std::size_t Population() const noexcept;

    private:
        int width_ = 0;
        int height_ = 0;
        unsigned generation_ = 0;
        std::vector<std::uint8_t> age_;
        std::vector<std::uint8_t> next_;
    };

    // The tunnel: rings of a tube along a curving centre line, seen from a
    // camera flying along it.  `Rings` lists the rings in view for `time`
    // seconds into the flight, farthest first, in pixels for a width x height
    // area; their places follow the bends, their sizes shrink with distance.
    struct TunnelRing
    {
        float x = 0.0f;  // centre, pixels
        float y = 0.0f;
        float radius = 0.0f;  // pixels
        float depth = 0.0f;   // 0 (at the camera) .. 1 (as far as can be seen)
        float twist = 0.0f;   // radians: where the tube's lengthwise lines cross this ring
        int index = 0;        // the ring's number along the tube, for its colour
    };
    void TunnelRings(double time, float width, float height, std::vector<TunnelRing>& rings);
}
