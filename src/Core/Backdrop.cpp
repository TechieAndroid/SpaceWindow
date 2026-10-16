// SpaceWindow - the background animations: what moves where.
#include "pch.h"
#include "Backdrop.h"

namespace sm
{
    namespace
    {
        // The DOOM fire's 37 colours, from no heat to white heat (as published
        // in Fabien Sanglard's "How DOOM fire was done").
        constexpr std::uint32_t kFirePalette[FireField::kHottest + 1] = {
            0x070707, 0x1F0707, 0x2F0F07, 0x470F07, 0x571707, 0x671F07, 0x771F07, 0x8F2707, 0x9F2F07, 0xAF3F07,
            0xBF4707, 0xC74707, 0xDF4F07, 0xDF5707, 0xDF5707, 0xD75F07, 0xD75F07, 0xD7670F, 0xCF6F0F, 0xCF770F,
            0xCF7F0F, 0xCF8717, 0xC78717, 0xC78F17, 0xC7971F, 0xBF9F1F, 0xBF9F1F, 0xBFA727, 0xBFA727, 0xBFAF2F,
            0xB7AF2F, 0xB7B72F, 0xB7B737, 0xCFCF6F, 0xDFDF9F, 0xEFEFC7, 0xFFFFFF,
        };

        constexpr double kPi = std::numbers::pi;
    }

    // ---------------------------------------------------------------- fire

    void FireField::Resize(int width, int height)
    {
        // Keep what burns where the old field and the new one overlap, both
        // standing on the bottom row, so the flames survive a window being resized.
        std::vector<std::uint8_t> old = std::move(heat_);
        int const oldWidth = width_;
        int const oldHeight = height_;
        width_ = std::max(width, 1);
        height_ = std::max(height, 2);
        heat_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0);
        for (int y = 1; y <= std::min(height_, oldHeight); ++y)
        {
            std::copy_n(old.begin() + static_cast<std::ptrdiff_t>(oldHeight - y) * oldWidth, std::min(width_, oldWidth),
                        heat_.begin() + static_cast<std::ptrdiff_t>(height_ - y) * width_);
        }
        std::fill(heat_.end() - width_, heat_.end(), static_cast<std::uint8_t>(kHottest));  // the bottom row burns
        // The flames reach about halfway up whatever the size: cooling all the
        // way from white heat takes kHottest levels over that many rows.
        double const reach = std::max(1.0, 0.45 * height_);
        coolOdds_ = static_cast<std::uint32_t>(std::min(1.0, kHottest / reach) * 65536.0);
    }

    void FireField::Step(Random& random)
    {
        // Top to bottom, every row from the second takes the heat of the row
        // below it - read before that row changes - one cell to either side
        // now and then, a level cooler now and then.
        for (int y = 1; y < height_; ++y)
        {
            std::uint8_t const* const from = heat_.data() + static_cast<std::size_t>(y) * width_;
            std::uint8_t* const to = heat_.data() + static_cast<std::size_t>(y - 1) * width_;
            for (int x = 0; x < width_; ++x)
            {
                std::uint32_t const r = random.Next();
                int const drift = static_cast<int>(r & 1u) - static_cast<int>((r >> 1) & 1u);  // -1, 0, 0, +1
                int const cool = ((r >> 8) & 0xFFFFu) < coolOdds_ ? 1 : 0;
                int const heat = from[x];
                to[std::clamp(x + drift, 0, width_ - 1)] = static_cast<std::uint8_t>(heat > cool ? heat - cool : 0);
            }
        }
    }

    std::uint32_t FireColor(int heat) noexcept
    {
        if (heat <= 0)
        {
            return 0;  // transparent: the canvas shows
        }
        return 0xFF000000u | kFirePalette[std::min(heat, FireField::kHottest)];
    }

    // ---------------------------------------------------------------- matrix

    void MatrixRain::Resize(int columns, int rows, Random& random)
    {
        columns_ = std::max(columns, 1);
        rows_ = std::max(rows, 1);
        glyphs_.resize(static_cast<std::size_t>(columns_) * static_cast<std::size_t>(rows_));
        for (std::uint8_t& glyph : glyphs_)
        {
            glyph = static_cast<std::uint8_t>(random.Below(kGlyphs));
        }
        drops_.assign(static_cast<std::size_t>(columns_), {});
        for (Drop& drop : drops_)
        {
            Restart(drop, random);
            // Rain from the start: some drops already on their way down.
            if (random.Below(2) == 0)
            {
                drop.wait = 0;
                drop.head = random.Below(rows_ + drop.length);
            }
        }
        steps_ = 0;
    }

    void MatrixRain::Restart(Drop& drop, Random& random) const
    {
        drop.length = 4 + random.Below(std::max(4, rows_ * 2 / 3));
        drop.head = -1;
        drop.wait = random.Below(std::max(1, rows_));
        drop.slow = random.Below(3) == 0;
    }

    void MatrixRain::Step(Random& random)
    {
        ++steps_;
        for (Drop& drop : drops_)
        {
            if (drop.wait > 0)
            {
                --drop.wait;
                continue;
            }
            if (drop.slow && (steps_ & 1u) != 0)
            {
                continue;
            }
            if (++drop.head - drop.length >= rows_)
            {
                Restart(drop, random);  // the whole trail has fallen off the bottom
            }
        }
        // The characters shimmer: a few of them change every step.
        int const changes = std::max(1, columns_ * rows_ / 40);
        for (int i = 0; i < changes; ++i)
        {
            glyphs_[static_cast<std::size_t>(random.Below(columns_ * rows_))] =
                static_cast<std::uint8_t>(random.Below(kGlyphs));
        }
    }

    float MatrixRain::Light(int column, int row) const noexcept
    {
        Drop const& drop = drops_[static_cast<std::size_t>(column)];
        int const behind = drop.head - row;
        if (behind < 0 || behind >= drop.length)
        {
            return 0.0f;
        }
        return 1.0f - static_cast<float>(behind) / static_cast<float>(drop.length);
    }

    bool MatrixRain::IsHead(int column, int row) const noexcept
    {
        return drops_[static_cast<std::size_t>(column)].head == row;
    }

    // ---------------------------------------------------------------- colour mix

    int ColorMixBand(float u, float v, float time, float phaseA, float phaseB) noexcept
    {
        // ly's colormix, three rounds of folding the plane over itself; its
        // clock runs at twice the seconds.
        float const t = 2.0f * time;
        float x = u;
        float y = v;
        float x2 = u + v;
        float y2 = u + v;
        for (int round = 0; round < 3; ++round)
        {
            float const length = std::sqrt(x * x + y * y);
            x2 += x + length;
            y2 += y + length;
            x += 0.5f * std::cos(phaseA + y2 * 0.2f + t * 0.1f);
            y += 0.5f * std::sin(phaseB + x2 - t * 0.1f);
            float const fold = std::cos(x + y) - std::sin(x * 0.7f - y);
            x -= fold;
            y -= fold;
        }
        float const length = std::sqrt(x * x + y * y);
        if (!std::isfinite(length))
        {
            return 0;
        }
        return static_cast<int>(std::fmod(std::floor(length * 5.0f), 12.0f));
    }

    // ---------------------------------------------------------------- life

    void LifeField::Resize(int width, int height, Random& random, float density)
    {
        // Cells where the old field and the new one overlap live on; the rest
        // of the new field is seeded at random.
        std::vector<std::uint8_t> const old = std::move(age_);
        int const oldWidth = width_;
        int const oldHeight = height_;
        width_ = std::max(width, 3);
        height_ = std::max(height, 3);
        std::size_t const size = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
        age_.assign(size, kLongDead);
        next_.assign(size, kLongDead);
        for (int y = 0; y < height_; ++y)
        {
            for (int x = 0; x < width_; ++x)
            {
                std::uint8_t& age = age_[static_cast<std::size_t>(y) * width_ + x];
                if (x < oldWidth && y < oldHeight)
                {
                    age = old[static_cast<std::size_t>(y) * oldWidth + x];
                }
                else if (random.Unit() < density)
                {
                    age = kAlive;
                }
            }
        }
        if (old.empty())
        {
            generation_ = 0;
        }
    }

    void LifeField::Step(Random& random, int entropyEvery)
    {
        for (int y = 0; y < height_; ++y)
        {
            int const above = (y + height_ - 1) % height_;
            int const below = (y + 1) % height_;
            for (int x = 0; x < width_; ++x)
            {
                int const left = (x + width_ - 1) % width_;
                int const right = (x + 1) % width_;
                int const neighbours = (Alive(left, above) ? 1 : 0) + (Alive(x, above) ? 1 : 0) +
                                       (Alive(right, above) ? 1 : 0) + (Alive(left, y) ? 1 : 0) +
                                       (Alive(right, y) ? 1 : 0) + (Alive(left, below) ? 1 : 0) +
                                       (Alive(x, below) ? 1 : 0) + (Alive(right, below) ? 1 : 0);
                std::uint8_t const age = Age(x, y);
                bool const lives = age == kAlive ? neighbours == 2 || neighbours == 3 : neighbours == 3;
                std::uint8_t& next = next_[static_cast<std::size_t>(y) * width_ + x];
                next = lives ? kAlive : age == kLongDead ? kLongDead : static_cast<std::uint8_t>(age + 1);
            }
        }
        age_.swap(next_);
        ++generation_;
        if (entropyEvery > 0 && generation_ % static_cast<unsigned>(entropyEvery) == 0)
        {
            // New life here and there, so the field never settles into still lifes and blinkers.
            int const sparks = std::max(1, width_ * height_ / 150);
            for (int i = 0; i < sparks; ++i)
            {
                age_[static_cast<std::size_t>(random.Below(width_ * height_))] = kAlive;
            }
        }
    }

    std::size_t LifeField::Population() const noexcept
    {
        return static_cast<std::size_t>(std::count(age_.begin(), age_.end(), kAlive));
    }

    // ---------------------------------------------------------------- tunnel

    namespace
    {
        constexpr double kSpeed = 1.6;      // tube radii a second
        constexpr double kSpacing = 0.3;    // between rings
        constexpr double kNear = 0.3;       // the nearest ring drawn, ...
        constexpr double kFar = 12.0;       // ... and the farthest
        constexpr double kLookAhead = 3.0;  // the camera looks at the centre line this far ahead

        struct Point
        {
            double x = 0.0;
            double y = 0.0;
        };

        // The tube's centre line: slow bends both ways, never repeating quite
        // the same (the tube's radius is 1).
        Point CentreLine(double z) noexcept
        {
            return { 0.9 * std::sin(0.31 * z) + 0.55 * std::sin(0.13 * z + 1.7),
                     0.7 * std::sin(0.23 * z + 0.6) + 0.45 * std::sin(0.11 * z + 0.3) };
        }
    }

    void TunnelRings(double time, float width, float height, std::vector<TunnelRing>& rings)
    {
        rings.clear();
        if (!(width > 0.0f) || !(height > 0.0f) || !std::isfinite(time))  // NaN sizes too
        {
            return;
        }
        double const travelled = time * kSpeed;
        Point const eye = CentreLine(travelled);
        Point const ahead = CentreLine(travelled + kLookAhead);
        // Turning to look along the tube keeps where it leads near the middle;
        // the bends nearer by swing past.
        Point const slope{ (ahead.x - eye.x) / kLookAhead, (ahead.y - eye.y) / kLookAhead };
        // A wide tube, so that plenty of it shows around whatever is in front of it.
        double const focal = 1.1 * std::min(width, height);
        auto const first = static_cast<long long>(std::ceil((travelled + kNear) / kSpacing));
        auto const last = static_cast<long long>(std::floor((travelled + kFar) / kSpacing));
        for (long long k = last; k >= first; --k)
        {
            double const z = static_cast<double>(k) * kSpacing;
            double const distance = z - travelled;
            Point const centre = CentreLine(z);
            double const dx = centre.x - eye.x - slope.x * distance;
            double const dy = centre.y - eye.y - slope.y * distance;
            TunnelRing ring;
            ring.x = static_cast<float>(width / 2.0 + focal * dx / distance);
            ring.y = static_cast<float>(height / 2.0 + focal * dy / distance);
            ring.radius = static_cast<float>(focal / distance);
            ring.depth = static_cast<float>((distance - kNear) / (kFar - kNear));
            ring.twist = static_cast<float>(std::fmod(0.22 * z, 2.0 * kPi));
            ring.index = static_cast<int>(k % 1'000'000);
            rings.push_back(ring);
        }
    }
}
