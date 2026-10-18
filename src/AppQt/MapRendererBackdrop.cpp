// SpaceWindow (Qt) - drawing the map: the background animations.
//
// A QPainter port of src/App/MapRendererBackdrop.cpp (Direct2D).  The
// animations (src/Core/Backdrop.h) play on a dark canvas of their own behind
// the map, wherever the map leaves space: around a pie chart, through the
// gaps between its slices, and behind the welcome screen.  Before each frame
// an animation steps at its own pace and draws what it can off-screen; the
// frame then shows it under the cached map.
//  * Fire, colour mix and Life are grids of cells, a pixel each in a small
//    image stretched over the panel: smoothly for the fire and the colour
//    mix, as crisp squares for Life.
//  * The falling characters come from sheets of glyphs drawn once, stamped
//    into a layer whenever the rain moves.
//  * The tunnel is drawn afresh every frame: neon rings with a soft glow, and
//    lines along the tube that twist as it goes.
#include "MapRenderer.h"

#include <chrono>
#include <cmath>
#include <numbers>

#include <QFontDatabase>
#include <QPainter>
#include <QRadialGradient>

namespace sm::qt
{
    namespace
    {
        constexpr double kTwoPi = 2.0 * std::numbers::pi;

        constexpr QColor kCanvas(4, 4, 6);         // a terminal's black
        constexpr QColor kTunnelCanvas(6, 0, 18);  // deep violet

        constexpr double kFireRate = 30.0;  // steps a second: the PSX fire ran at 30
        constexpr double kMixRate = 20.0;   // its bands drift slowly: 20 looks as smooth as 30, for a third less work
        constexpr double kLifeRate = 9.0;   // generations a second
        constexpr double kRainRate = 18.0;

        // The characters that rain: half-width katakana, then a few digits -
        // or, where no font has katakana, Latin letters.
        constexpr char32_t kFirstKatakana = 0xFF66;
        constexpr int kKatakana = 56;
        constexpr char16_t kDigits[] = u"01234579";
        constexpr char16_t kLatin[] = u"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789#$%&";
        static_assert(kKatakana + static_cast<int>(std::size(kDigits)) - 1 == MatrixRain::kGlyphs);
        static_assert(static_cast<int>(std::size(kLatin)) - 1 >= MatrixRain::kGlyphs);

        // The colour mix's twelve bands: ly's red, blue and black, each over
        // the next in four strengths (its block characters, from full to
        // light shade), a little darker so the map stays easy to read.
        constexpr std::uint32_t Mix(std::uint32_t over, std::uint32_t under, int quarters) noexcept
        {
            std::uint32_t result = 0xFF000000u;
            for (int shift = 0; shift <= 16; shift += 8)
            {
                std::uint32_t const a = (over >> shift) & 0xFFu;
                std::uint32_t const b = (under >> shift) & 0xFFu;
                std::uint32_t const value =
                    (a * static_cast<std::uint32_t>(quarters) + b * static_cast<std::uint32_t>(4 - quarters)) / 4u;
                result |= (value * 4u / 5u) << shift;
            }
            return result;
        }
        constexpr std::uint32_t kRed = 0xFF0000;
        constexpr std::uint32_t kBlue = 0x0000FF;
        constexpr std::uint32_t kBlack = 0x000000;
        constexpr std::array<std::uint32_t, 12> kMixBands = {
            Mix(kRed, kBlue, 4),   Mix(kRed, kBlue, 3),   Mix(kRed, kBlue, 2),   Mix(kRed, kBlue, 1),
            Mix(kBlue, kBlack, 4), Mix(kBlue, kBlack, 3), Mix(kBlue, kBlack, 2), Mix(kBlue, kBlack, 1),
            Mix(kBlack, kRed, 4),  Mix(kBlack, kRed, 3),  Mix(kBlack, kRed, 2),  Mix(kBlack, kRed, 1),
        };

        // A Life cell: bright green while alive, fading for a few generations after.
        std::uint32_t LifeColor(std::uint8_t age) noexcept
        {
            constexpr int kFade = 7;
            if (age >= kFade)
            {
                return 0;
            }
            float const strength = age == LifeField::kAlive ? 1.0f : 0.55f * (1.0f - static_cast<float>(age) / kFade);
            auto const channel = [strength](float value) {
                return static_cast<std::uint32_t>(std::lround(value * strength * 255.0f));
            };
            auto const alpha = static_cast<std::uint32_t>(std::lround(strength * 255.0f));
            return (alpha << 24) | (channel(0.35f) << 16) | (channel(1.0f) << 8) | channel(0.45f);  // premultiplied
        }

        // The tunnel's neon, flowing along the tube: cyan, violet, magenta, electric blue.
        QColor Neon(int index, double time, float alpha) noexcept
        {
            constexpr std::array<std::array<float, 3>, 4> kStops = { {
                { 0.0f, 0.95f, 1.0f },
                { 0.6f, 0.25f, 1.0f },
                { 1.0f, 0.2f, 0.75f },
                { 0.2f, 0.5f, 1.0f },
            } };
            double at = std::fmod(index * 0.09 + time * 0.25, 4.0);
            if (at < 0.0)
            {
                at += 4.0;
            }
            auto const i = static_cast<std::size_t>(at) % 4;
            auto const f = static_cast<float>(at - std::floor(at));
            auto const& a = kStops[i];
            auto const& b = kStops[(i + 1) % 4];
            return QColor::fromRgbF(a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f,
                                    alpha);
        }

        // How clearly a ring shows: fading into the distance, and in as it
        // comes out of it - and out again just before the camera passes through it.
        float RingFade(float depth) noexcept
        {
            float const distance = std::pow(std::clamp(1.0f - depth, 0.0f, 1.0f), 1.6f);
            float const arrival = std::clamp(depth / 0.05f, 0.0f, 1.0f);
            return distance * arrival * arrival * (3.0f - 2.0f * arrival);
        }
    }

    void MapRenderer::ReleaseBackdrop() noexcept
    {
        backdrop_ = BackdropState{};
    }

    bool MapRenderer::BackdropMoved(MapScene const& scene, int width, int height) const noexcept
    {
        BackdropState const& b = backdrop_;
        return scene.backdrop != b.kind || width != b.width || height != b.height ||
               scene.backdrop == Backdrop::Tunnel || scene.time >= b.next || scene.time + 1.0 < b.next;
    }

    void MapRenderer::PrepareBackdrop(MapScene const& scene, int width, int height)
    {
        BackdropState& b = backdrop_;
        if (b.kind != scene.backdrop)
        {
            ReleaseBackdrop();
            b.kind = scene.backdrop;
            auto const now = std::chrono::steady_clock::now().time_since_epoch().count();
            b.random = Random(static_cast<std::uint32_t>(now) ^ static_cast<std::uint32_t>(now >> 32));
            b.next = scene.time;
            b.phaseA = b.random.Unit() * static_cast<float>(kTwoPi);
            b.phaseB = b.random.Unit() * static_cast<float>(kTwoPi);
        }
        b.width = std::max(1, width);
        b.height = std::max(1, height);
        auto const scaled = [this](float pixels, int least) {
            return std::max(least, static_cast<int>(std::lround(pixels * static_cast<float>(scale_))));
        };
        switch (b.kind)
        {
        case Backdrop::Fire: PrepareGrid(scaled(4.0f, 2), kFireRate, scene.time, true); break;
        case Backdrop::ColorMix: PrepareGrid(scaled(9.0f, 4), kMixRate, scene.time, true); break;
        case Backdrop::Life: PrepareGrid(scaled(7.0f, 3), kLifeRate, scene.time, false); break;
        case Backdrop::Matrix: PrepareRain(scene.time); break;
        case Backdrop::Tunnel:
            TunnelRings(scene.time, static_cast<float>(b.width), static_cast<float>(b.height), b.rings);
            b.next = scene.time;
            break;
        default: break;
        }
    }

    void MapRenderer::PrepareGrid(int cell, double rate, double time, bool smooth)
    {
        BackdropState& b = backdrop_;
        int const columns = std::max(1, (b.width + cell - 1) / cell);
        int const rows = std::max(2, (b.height + cell - 1) / cell);
        bool stepped = false;
        if (columns != b.columns || rows != b.rows || cell != b.cell || b.grid.isNull())
        {
            b.columns = columns;
            b.rows = rows;
            b.cell = cell;
            b.smooth = smooth;
            if (b.kind == Backdrop::Fire)
            {
                b.fire.Resize(columns, rows);
            }
            else if (b.kind == Backdrop::Life)
            {
                b.life.Resize(columns, rows, b.random);
            }
            b.pixels.assign(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows), 0);
            b.grid = QImage(reinterpret_cast<uchar*>(b.pixels.data()), columns, rows,
                            static_cast<qsizetype>(columns) * 4, QImage::Format_ARGB32_Premultiplied);
            stepped = true;
        }
        if (time - b.next > 1.0)
        {
            b.next = time;  // after a pause, carry on from now rather than catch up
        }
        while (b.next <= time)
        {
            if (b.kind == Backdrop::Fire)
            {
                b.fire.Step(b.random);
            }
            else if (b.kind == Backdrop::Life)
            {
                b.life.Step(b.random);
            }
            b.next += 1.0 / rate;
            stepped = true;
        }
        if (!stepped)
        {
            return;
        }

        std::uint32_t* pixel = b.pixels.data();
        for (int y = 0; y < rows; ++y)
        {
            for (int x = 0; x < columns; ++x, ++pixel)
            {
                switch (b.kind)
                {
                case Backdrop::Fire: *pixel = FireColor(b.fire.At(x, y)); break;
                case Backdrop::Life: *pixel = LifeColor(b.life.Age(x, y)); break;
                default:
                {
                    // The pattern's units: -1 .. 1 down the height, square cells.
                    float const u = static_cast<float>(2 * x + 1 - columns) / static_cast<float>(rows);
                    float const v = static_cast<float>(2 * y + 1 - rows) / static_cast<float>(rows);
                    auto const band =
                        static_cast<std::size_t>(ColorMixBand(u, v, static_cast<float>(time), b.phaseA, b.phaseB));
                    *pixel = kMixBands[band % kMixBands.size()];
                    break;
                }
                }
            }
        }
    }

    void MapRenderer::PrepareRain(double time)
    {
        BackdropState& b = backdrop_;
        if (b.headGlyphs.isNull())
        {
            // The characters side by side, in the head's colour and the
            // trail's, on nothing: stamped with the strength of their light.
            QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            int const size = std::max(6, static_cast<int>(std::lround(15.0 * scale_)));
            font.setPixelSize(size);
            font.setStyleStrategy(QFont::StyleStrategy(QFont::PreferDefault | QFont::NoSubpixelAntialias));
            b.glyphWidth = std::max(1, static_cast<int>(std::lround(0.75 * size)));
            b.glyphHeight = std::max(1, static_cast<int>(std::lround(1.2 * size)));
            bool const katakana = !QFontDatabase::families(QFontDatabase::Japanese).isEmpty();
            auto const sheet = [&](QColor const& color) {
                QImage glyphs(b.glyphWidth * MatrixRain::kGlyphs, b.glyphHeight, QImage::Format_ARGB32_Premultiplied);
                glyphs.fill(Qt::transparent);
                QPainter painter(&glyphs);
                painter.setFont(font);
                painter.setPen(color);
                for (int i = 0; i < MatrixRain::kGlyphs; ++i)
                {
                    QString const glyph =
                        !katakana ? QString(QChar(kLatin[i]))
                        : i < kKatakana
                            ? QString::fromUcs4(
                                  std::array<char32_t, 1>{ kFirstKatakana + static_cast<char32_t>(i) }.data(), 1)
                            : QString(QChar(kDigits[i - kKatakana]));
                    painter.drawText(QRect(i * b.glyphWidth, 0, b.glyphWidth, b.glyphHeight), Qt::AlignCenter, glyph);
                }
                return glyphs;
            };
            b.headGlyphs = sheet(QColor::fromRgbF(0.85f, 1.0f, 0.88f));  // the head shines white-green
            b.trailGlyphs = sheet(QColor::fromRgbF(0.1f, 1.0f, 0.35f));  // the trail is green, fading
        }

        int const columns = std::max(1, b.width / b.glyphWidth);
        int const rows = std::max(1, b.height / b.glyphHeight + 1);
        bool stepped = false;
        if (columns != b.rain.Columns() || rows != b.rain.Rows() || b.layer.size() != QSize(b.width, b.height))
        {
            b.rain.Resize(columns, rows, b.random);
            b.layer = QImage(b.width, b.height, QImage::Format_ARGB32_Premultiplied);
            stepped = true;
        }
        if (time - b.next > 1.0)
        {
            b.next = time;
        }
        while (b.next <= time)
        {
            b.rain.Step(b.random);
            b.next += 1.0 / kRainRate;
            stepped = true;
        }
        if (!stepped)
        {
            return;
        }

        b.layer.fill(Qt::transparent);
        QPainter painter(&b.layer);
        // A centred margin: the columns sit evenly across the width.
        int const offset = (b.width - columns * b.glyphWidth) / 2;
        for (int column = 0; column < columns; ++column)
        {
            int const left = offset + column * b.glyphWidth;
            for (int row = 0; row < rows; ++row)
            {
                float const light = b.rain.Light(column, row);
                if (light < 0.03f)
                {
                    continue;
                }
                bool const head = b.rain.IsHead(column, row);
                painter.setOpacity(head ? 1.0 : 0.12 + 0.88 * std::pow(light, 1.3));
                QRect const source(b.rain.Glyph(column, row) * b.glyphWidth, 0, b.glyphWidth, b.glyphHeight);
                painter.drawImage(QPoint(left, row * b.glyphHeight), head ? b.headGlyphs : b.trailGlyphs, source);
            }
        }
    }

    void MapRenderer::DrawBackdrop(QPainter& painter, MapScene const& scene) const
    {
        BackdropState const& b = backdrop_;
        QRectF const panel(0.0, 0.0, b.width, b.height);
        painter.save();
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(panel, b.kind == Backdrop::Tunnel ? kTunnelCanvas : kCanvas);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        switch (b.kind)
        {
        case Backdrop::Fire:
        case Backdrop::ColorMix:
        case Backdrop::Life:
            if (!b.grid.isNull())
            {
                painter.setRenderHint(QPainter::SmoothPixmapTransform, b.smooth);
                painter.drawImage(QRectF(0.0, 0.0, b.columns * b.cell, b.rows * b.cell), b.grid);
            }
            break;
        case Backdrop::Matrix:
            if (!b.layer.isNull())
            {
                painter.drawImage(QPointF(0.0, 0.0), b.layer);
            }
            break;
        case Backdrop::Tunnel: DrawTunnel(painter, scene.time); break;
        default: break;
        }
        painter.restore();
    }

    void MapRenderer::DrawTunnel(QPainter& painter, double time) const
    {
        BackdropState const& b = backdrop_;
        if (b.rings.empty())
        {
            return;
        }
        painter.setRenderHint(QPainter::Antialiasing, true);

        // A violet glow where the tube leads.
        TunnelRing const& farthest = b.rings.front();
        double const glowRadius = 0.55 * std::min(b.width, b.height);
        QRadialGradient glow(QPointF(farthest.x, farthest.y), glowRadius);
        glow.setColorAt(0.0, QColor::fromRgbF(0.55f, 0.15f, 0.85f, 0.45f));
        glow.setColorAt(0.45, QColor::fromRgbF(0.3f, 0.05f, 0.6f, 0.18f));
        glow.setColorAt(1.0, QColor::fromRgbF(0.1f, 0.0f, 0.25f, 0.0f));
        painter.fillRect(QRectF(0.0, 0.0, b.width, b.height), glow);

        // Farthest first, so nearer rings are drawn over them; each ring with
        // its lines along the tube to the next one nearer.
        constexpr int kLines = 14;
        painter.setBrush(Qt::NoBrush);
        for (std::size_t i = 0; i < b.rings.size(); ++i)
        {
            TunnelRing const& ring = b.rings[i];
            float const fade = RingFade(ring.depth);
            if (fade <= 0.01f)
            {
                continue;
            }
            double const width = std::max(1.0, 2.4 * scale_ * std::pow(1.0 - ring.depth, 1.5));
            QPointF const center(ring.x, ring.y);
            painter.setPen(QPen(Neon(ring.index, time, 0.16f * fade), 4.0 * width));  // the glow
            painter.drawEllipse(center, ring.radius, ring.radius);
            painter.setPen(QPen(Neon(ring.index, time, fade), width));
            painter.drawEllipse(center, ring.radius, ring.radius);

            if (i + 1 < b.rings.size())
            {
                TunnelRing const& nearer = b.rings[i + 1];
                float const nearerFade = RingFade(nearer.depth);
                painter.setPen(
                    QPen(Neon(ring.index, time, 0.7f * std::min(fade, nearerFade)), std::max(0.75, 0.6 * width)));
                for (int line = 0; line < kLines; ++line)
                {
                    double const angle = line * kTwoPi / kLines;
                    double const from = angle + ring.twist;
                    double const to = angle + nearer.twist;
                    painter.drawLine(
                        QPointF(ring.x + ring.radius * std::cos(from), ring.y + ring.radius * std::sin(from)),
                        QPointF(nearer.x + nearer.radius * std::cos(to), nearer.y + nearer.radius * std::sin(to)));
                }
            }
        }
    }
}
