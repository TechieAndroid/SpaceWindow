// SpaceWindow - Direct2D renderer for the map: the background animations.
//
// New in SpaceWindow.  The animations (src/Core/Backdrop.h) play on a dark
// canvas of their own behind the map, wherever the map leaves space: around a
// pie chart, through the gaps between its slices, and behind the welcome
// screen.  Before each frame an animation steps at its own pace and draws what
// it can off-screen; the frame then shows it under the cached map.
//  * Fire, colour mix and Life are grids of cells, a pixel each in a small
//    bitmap stretched over the panel: smoothly for the fire and the colour
//    mix, as crisp squares for Life.
//  * The falling characters come from a sheet of glyphs drawn once, tinted
//    as they are stamped into a layer whenever the rain moves.
//  * The tunnel is drawn afresh every frame: neon rings with a soft glow, and
//    lines along the tube that twist as it goes.
#include "pch.h"
#include "MapRenderer.h"

namespace sm::ui
{
    namespace
    {
        constexpr double kTwoPi = 2.0 * std::numbers::pi;
        constexpr D2D1_COLOR_F kTransparent{ 0.0f, 0.0f, 0.0f, 0.0f };
        constexpr D2D1_COLOR_F kCanvas{ 0.015f, 0.015f, 0.025f, 1.0f };     // a terminal's black
        constexpr D2D1_COLOR_F kTunnelCanvas{ 0.025f, 0.0f, 0.07f, 1.0f };  // deep violet

        constexpr double kFireRate = 30.0;  // steps a second: the PSX fire ran at 30
        constexpr double kMixRate = 20.0;   // its bands drift slowly: 20 looks as smooth as 30, for a third less work
        constexpr double kLifeRate = 9.0;   // generations a second
        constexpr double kRainRate = 18.0;

        // The characters that rain: half-width katakana, then a few digits.
        constexpr wchar_t kFirstKatakana = 0xFF66;
        constexpr int kKatakana = 56;
        constexpr wchar_t kDigits[] = L"01234579";
        static_assert(kKatakana + static_cast<int>(std::size(kDigits)) - 1 == sm::MatrixRain::kGlyphs);

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
        constexpr std::uint32_t kMixBands[12] = {
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
            float const strength =
                age == sm::LifeField::kAlive ? 1.0f : 0.55f * (1.0f - static_cast<float>(age) / kFade);
            auto const channel = [strength](float value) {
                return static_cast<std::uint32_t>(std::lround(value * strength * 255.0f));
            };
            auto const alpha = static_cast<std::uint32_t>(std::lround(strength * 255.0f));
            return (alpha << 24) | (channel(0.35f) << 16) | (channel(1.0f) << 8) | channel(0.45f);  // premultiplied
        }

        // The tunnel's neon, flowing along the tube: cyan, violet, magenta, electric blue.
        D2D1_COLOR_F Neon(int index, double time, float alpha) noexcept
        {
            constexpr D2D1_COLOR_F kStops[] = {
                { 0.0f, 0.95f, 1.0f, 1.0f },
                { 0.6f, 0.25f, 1.0f, 1.0f },
                { 1.0f, 0.2f, 0.75f, 1.0f },
                { 0.2f, 0.5f, 1.0f, 1.0f },
            };
            double at = std::fmod(index * 0.09 + time * 0.25, 4.0);
            if (at < 0.0)
            {
                at += 4.0;
            }
            auto const i = static_cast<std::size_t>(at) % 4;
            auto const f = static_cast<float>(at - std::floor(at));
            D2D1_COLOR_F const& a = kStops[i];
            D2D1_COLOR_F const& b = kStops[(i + 1) % 4];
            return { a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, alpha };
        }

        // How clearly a ring shows: fading into the distance, and in as it
        // comes out of it - and out again just before the camera passes through it.
        float RingFade(float depth) noexcept
        {
            float const distance = std::pow(std::clamp(1.0f - depth, 0.0f, 1.0f), 1.6f);
            float const arrival = std::clamp(depth / 0.05f, 0.0f, 1.0f);  // windows.h defines `near` and `far` away
            return distance * arrival * arrival * (3.0f - 2.0f * arrival);
        }
    }

    void MapRenderer::ReleaseBackdrop() noexcept
    {
        backdrop_ = BackdropState{};
    }

    void MapRenderer::PrepareBackdrop(MapScene const& scene)
    {
        BackdropState& b = backdrop_;
        if (b.kind != scene.backdrop)
        {
            ReleaseBackdrop();
            b.kind = scene.backdrop;
            b.random = sm::Random(static_cast<std::uint32_t>(GetTickCount64()));
            b.next = scene.time;
            b.phaseA = b.random.Unit() * static_cast<float>(kTwoPi);
            b.phaseB = b.random.Unit() * static_cast<float>(kTwoPi);
        }
        switch (b.kind)
        {
        case sm::Backdrop::Fire:
            PrepareGrid(std::max(2, static_cast<int>(std::lround(4.0f * scale_))), kFireRate, scene.time, true);
            break;
        case sm::Backdrop::ColorMix:
            PrepareGrid(std::max(4, static_cast<int>(std::lround(9.0f * scale_))), kMixRate, scene.time, true);
            break;
        case sm::Backdrop::Life:
            PrepareGrid(std::max(3, static_cast<int>(std::lround(7.0f * scale_))), kLifeRate, scene.time, false);
            break;
        case sm::Backdrop::Mandelbrot:
        case sm::Backdrop::Julia:
            PrepareGrid(std::max(1, static_cast<int>(std::lround(2.0f * scale_))), sm::FractalZoom::kStepsPerSecond,
                        scene.time, true);
            break;
        case sm::Backdrop::Matrix: PrepareRain(scene.time); break;
        case sm::Backdrop::Tunnel:
            sm::TunnelRings(scene.time, static_cast<float>(width_), static_cast<float>(height_), b.rings);
            break;
        default: break;
        }
    }

    void MapRenderer::PrepareGrid(int cell, double rate, double time, bool smooth)
    {
        BackdropState& b = backdrop_;
        int const columns = std::max(1, (width_ + cell - 1) / cell);
        int const rows = std::max(2, (height_ + cell - 1) / cell);
        bool stepped = false;
        if (columns != b.columns || rows != b.rows || cell != b.cell || !b.grid)
        {
            b.columns = columns;
            b.rows = rows;
            b.cell = cell;
            b.smooth = smooth;
            if (b.kind == sm::Backdrop::Fire)
            {
                b.fire.Resize(columns, rows);
            }
            else if (b.kind == sm::Backdrop::Life)
            {
                b.life.Resize(columns, rows, b.random);
            }
            else if (b.kind == sm::Backdrop::Mandelbrot || b.kind == sm::Backdrop::Julia)
            {
                if (b.fractal.Flights() == 0)
                {
                    b.fractal.Start(b.kind == sm::Backdrop::Julia ? sm::FractalSet::Julia : sm::FractalSet::Mandelbrot,
                                    b.random);
                }
                b.fractal.Resize(columns, rows);
            }
            b.pixels.assign(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows), 0);
            b.grid = nullptr;
            stepped = true;
        }
        if (time - b.next > 1.0)
        {
            b.next = time;  // after a pause, carry on from now rather than catch up
        }
        while (b.next <= time)
        {
            if (b.kind == sm::Backdrop::Fire)
            {
                b.fire.Step(b.random);
            }
            else if (b.kind == sm::Backdrop::Life)
            {
                b.life.Step(b.random);
            }
            else if (b.kind == sm::Backdrop::Mandelbrot || b.kind == sm::Backdrop::Julia)
            {
                b.fractal.Step(b.random);
            }
            b.next += 1.0 / rate;
            stepped = true;
        }
        if (!stepped)
        {
            return;
        }

        if (b.kind == sm::Backdrop::Mandelbrot || b.kind == sm::Backdrop::Julia)
        {
            b.fractal.Paint(b.pixels, time);
        }
        else
        {
            std::uint32_t* pixel = b.pixels.data();
            for (int y = 0; y < rows; ++y)
            {
                for (int x = 0; x < columns; ++x, ++pixel)
                {
                    switch (b.kind)
                    {
                    case sm::Backdrop::Fire: *pixel = sm::FireColor(b.fire.At(x, y)); break;
                    case sm::Backdrop::Life: *pixel = LifeColor(b.life.Age(x, y)); break;
                    default:
                    {
                        // The pattern's units: -1 .. 1 down the height, square cells.
                        float const u = static_cast<float>(2 * x + 1 - columns) / static_cast<float>(rows);
                        float const v = static_cast<float>(2 * y + 1 - rows) / static_cast<float>(rows);
                        *pixel = kMixBands[sm::ColorMixBand(u, v, static_cast<float>(time), b.phaseA, b.phaseB)];
                        break;
                    }
                    }
                }
            }
        }
        auto const pitch = static_cast<UINT32>(columns) * 4u;
        if (!b.grid)
        {
            D2D1_BITMAP_PROPERTIES1 const properties = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::check_hresult(
                context_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(columns), static_cast<UINT32>(rows)),
                                       b.pixels.data(), pitch, &properties, b.grid.put()));
        }
        else
        {
            winrt::check_hresult(b.grid->CopyFromMemory(nullptr, b.pixels.data(), pitch));
        }
    }

    void MapRenderer::PrepareRain(double time)
    {
        BackdropState& b = backdrop_;
        if (!b.glyphs)
        {
            // The characters, white on nothing, side by side: tinted as they are stamped.
            float const size = 15.0f * scale_;
            winrt::check_hresult(dwriteFactory_->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                                  size, L"", b.glyphFormat.put()));
            b.glyphFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            b.glyphFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            b.glyphFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            b.glyphWidth = std::round(0.75f * size);
            b.glyphHeight = std::round(1.2f * size);
            D2D1_BITMAP_PROPERTIES1 const properties =
                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::check_hresult(
                context_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(b.glyphWidth * sm::MatrixRain::kGlyphs),
                                                   static_cast<UINT32>(b.glyphHeight)),
                                       nullptr, 0, &properties, b.glyphs.put()));
            context_->SetTarget(b.glyphs.get());
            context_->BeginDraw();
            context_->SetTransform(D2D1::Matrix3x2F::Identity());
            context_->Clear(kTransparent);
            D2D1_TEXT_ANTIALIAS_MODE const textMode = context_->GetTextAntialiasMode();
            context_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // the coverage becomes the mask
            brush_->SetColor({ 1.0f, 1.0f, 1.0f, 1.0f });
            for (int i = 0; i < sm::MatrixRain::kGlyphs; ++i)
            {
                wchar_t const glyph = i < kKatakana ? static_cast<wchar_t>(kFirstKatakana + i) : kDigits[i - kKatakana];
                float const left = static_cast<float>(i) * b.glyphWidth;
                context_->DrawText(&glyph, 1, b.glyphFormat.get(),
                                   D2D1::RectF(left, 0.0f, left + b.glyphWidth, b.glyphHeight), brush_.get(),
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            context_->SetTextAntialiasMode(textMode);
            HRESULT const hr = context_->EndDraw();
            context_->SetTarget(nullptr);
            winrt::check_hresult(hr);
        }

        int const columns = std::max(1, static_cast<int>(static_cast<float>(width_) / b.glyphWidth));
        int const rows = std::max(1, static_cast<int>(static_cast<float>(height_) / b.glyphHeight) + 1);
        bool stepped = false;
        if (columns != b.rain.Columns() || rows != b.rain.Rows() || !b.layer)
        {
            b.rain.Resize(columns, rows, b.random);
            D2D1_BITMAP_PROPERTIES1 const properties =
                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            b.layer = nullptr;
            winrt::check_hresult(
                context_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(width_), static_cast<UINT32>(height_)), nullptr,
                                       0, &properties, b.layer.put()));
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

        context_->SetTarget(b.layer.get());
        context_->BeginDraw();
        context_->SetTransform(D2D1::Matrix3x2F::Identity());
        context_->Clear(kTransparent);
        context_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);  // FillOpacityMask needs it
        // A centred margin: the columns sit evenly across the width.
        float const offset =
            std::floor((static_cast<float>(width_) - static_cast<float>(columns) * b.glyphWidth) / 2.0f);
        for (int column = 0; column < columns; ++column)
        {
            float const left = offset + static_cast<float>(column) * b.glyphWidth;
            for (int row = 0; row < rows; ++row)
            {
                float const light = b.rain.Light(column, row);
                if (light < 0.03f)
                {
                    continue;
                }
                // The head shines white-green; the trail is green, fading.
                brush_->SetColor(b.rain.IsHead(column, row)
                                     ? D2D1_COLOR_F{ 0.85f, 1.0f, 0.88f, 1.0f }
                                     : D2D1_COLOR_F{ 0.1f, 1.0f, 0.35f, 0.12f + 0.88f * std::pow(light, 1.3f) });
                float const top = static_cast<float>(row) * b.glyphHeight;
                auto const glyph = static_cast<float>(b.rain.Glyph(column, row));
                D2D1_RECT_F const destination = D2D1::RectF(left, top, left + b.glyphWidth, top + b.glyphHeight);
                D2D1_RECT_F const source =
                    D2D1::RectF(glyph * b.glyphWidth, 0.0f, (glyph + 1.0f) * b.glyphWidth, b.glyphHeight);
                context_->FillOpacityMask(b.glyphs.get(), brush_.get(), &destination, &source);
            }
        }
        HRESULT const hr = context_->EndDraw();
        context_->SetTarget(nullptr);
        winrt::check_hresult(hr);
    }

    void MapRenderer::DrawBackdrop(MapScene const& scene)
    {
        BackdropState const& b = backdrop_;
        context_->Clear(b.kind == sm::Backdrop::Tunnel ? kTunnelCanvas : kCanvas);
        switch (b.kind)
        {
        case sm::Backdrop::Fire:
        case sm::Backdrop::ColorMix:
        case sm::Backdrop::Life:
        case sm::Backdrop::Mandelbrot:
        case sm::Backdrop::Julia:
            if (b.grid)
            {
                auto const right = static_cast<float>(b.columns * b.cell);
                auto const bottom = static_cast<float>(b.rows * b.cell);
                context_->DrawBitmap(b.grid.get(), D2D1::RectF(0.0f, 0.0f, right, bottom), 1.0f,
                                     b.smooth ? D2D1_INTERPOLATION_MODE_LINEAR
                                              : D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
            break;
        case sm::Backdrop::Matrix:
            if (b.layer)
            {
                context_->DrawImage(b.layer.get());
            }
            break;
        case sm::Backdrop::Tunnel: DrawTunnel(scene.time); break;
        default: break;
        }
    }

    void MapRenderer::DrawTunnel(double time)
    {
        BackdropState& b = backdrop_;
        if (b.rings.empty())
        {
            return;
        }
        context_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        // A violet glow where the tube leads.
        sm::TunnelRing const& farthest = b.rings.front();
        float const glowRadius = 0.55f * static_cast<float>(std::min(width_, height_));
        if (!b.glow)
        {
            D2D1_GRADIENT_STOP const stops[] = {
                { 0.0f, { 0.55f, 0.15f, 0.85f, 0.45f } },
                { 0.45f, { 0.3f, 0.05f, 0.6f, 0.18f } },
                { 1.0f, { 0.1f, 0.0f, 0.25f, 0.0f } },
            };
            winrt::com_ptr<ID2D1GradientStopCollection> collection;
            winrt::check_hresult(context_->CreateGradientStopCollection(stops, static_cast<UINT32>(std::size(stops)),
                                                                        D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
                                                                        collection.put()));
            winrt::check_hresult(context_->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(D2D1::Point2F(), D2D1::Point2F(), glowRadius, glowRadius),
                collection.get(), b.glow.put()));
        }
        b.glow->SetCenter(D2D1::Point2F(farthest.x, farthest.y));
        b.glow->SetRadiusX(glowRadius);
        b.glow->SetRadiusY(glowRadius);
        context_->FillRectangle(D2D1::RectF(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_)),
                                b.glow.get());

        // Farthest first, so nearer rings are drawn over them; each ring with
        // its lines along the tube to the next one nearer.
        constexpr int kLines = 14;
        for (std::size_t i = 0; i < b.rings.size(); ++i)
        {
            sm::TunnelRing const& ring = b.rings[i];
            float const fade = RingFade(ring.depth);
            if (fade <= 0.01f)
            {
                continue;
            }
            float const width = std::max(1.0f, 2.4f * scale_ * std::pow(1.0f - ring.depth, 1.5f));
            D2D1_ELLIPSE const circle = D2D1::Ellipse(D2D1::Point2F(ring.x, ring.y), ring.radius, ring.radius);
            brush_->SetColor(Neon(ring.index, time, 0.16f * fade));
            context_->DrawEllipse(circle, brush_.get(), 4.0f * width);  // the glow
            brush_->SetColor(Neon(ring.index, time, fade));
            context_->DrawEllipse(circle, brush_.get(), width);

            if (i + 1 < b.rings.size())
            {
                sm::TunnelRing const& nearer = b.rings[i + 1];
                float const nearerFade = RingFade(nearer.depth);
                brush_->SetColor(Neon(ring.index, time, 0.7f * std::min(fade, nearerFade)));
                for (int line = 0; line < kLines; ++line)
                {
                    double const angle = line * kTwoPi / kLines;
                    double const from = angle + ring.twist;
                    double const to = angle + nearer.twist;
                    D2D1_POINT_2F const a = D2D1::Point2F(ring.x + ring.radius * static_cast<float>(std::cos(from)),
                                                          ring.y + ring.radius * static_cast<float>(std::sin(from)));
                    D2D1_POINT_2F const c = D2D1::Point2F(nearer.x + nearer.radius * static_cast<float>(std::cos(to)),
                                                          nearer.y + nearer.radius * static_cast<float>(std::sin(to)));
                    context_->DrawLine(a, c, brush_.get(), std::max(0.75f, 0.6f * width));
                }
            }
        }
    }
}
