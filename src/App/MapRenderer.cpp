// SpaceWindow - Direct2D renderer for the map: device, frames and the treemap.
//
// Box drawing follows MinimalDrawDisplayFolder() in FolderView.cpp of
// SpaceMonger 1.4.0 (Copyright (c) 1998-2020 Sean Werkema, MIT License).
// The pie chart is drawn by MapRendererPie.cpp.
#include "pch.h"
#include "MapRenderer.h"

#include "../Core/FolderTree.h"
#include "../Core/Format.h"
#include "../Core/Lang.h"
#include "../Core/Settings.h"

namespace sm::ui
{
    namespace
    {
        // 1.4.0 used 9-pixel "Small Fonts"; this is the closest legible modern size.
        constexpr float kFontSizeDips = 11.0f;

        // Segoe UI Variable's "Small" optical size is designed for text this small.
        constexpr wchar_t const* kFontCandidates[] = { L"Segoe UI Variable Small", L"Segoe UI Variable Text",
                                                       L"Segoe UI" };

        constexpr D2D1_COLOR_F kBlack{ 0.0f, 0.0f, 0.0f, 1.0f };
        constexpr D2D1_COLOR_F kTransparent{ 0.0f, 0.0f, 0.0f, 0.0f };

        D2D1_COLOR_F ToColor(sm::Rgb color) noexcept
        {
            return { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f };
        }

        bool IsDeviceLost(HRESULT hr) noexcept
        {
            return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == D2DERR_RECREATE_TARGET;
        }

        std::wstring PickFontFamily(IDWriteFactory* factory)
        {
            winrt::com_ptr<IDWriteFontCollection> fonts;
            if (SUCCEEDED(factory->GetSystemFontCollection(fonts.put(), FALSE)))
            {
                for (wchar_t const* candidate : kFontCandidates)
                {
                    UINT32 index = 0;
                    BOOL exists = FALSE;
                    if (SUCCEEDED(fonts->FindFamilyName(candidate, &index, &exists)) && exists)
                    {
                        return candidate;
                    }
                }
            }
            return L"Segoe UI";
        }
    }

    MapRenderer::MapRenderer()
    {
        D2D1_FACTORY_OPTIONS const options{};
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                               d2dFactory_.put_void()));
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                 reinterpret_cast<::IUnknown**>(dwriteFactory_.put_void())));
        fontFamily_ = PickFontFamily(dwriteFactory_.get());
        CreateTextFormats();
    }

    void MapRenderer::Attach(winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel)
    {
        panel_ = panel.as<ISwapChainPanelNative>();
        CreateDeviceResources();
    }

    void MapRenderer::CreateDeviceResources()
    {
        static constexpr D3D_FEATURE_LEVEL kLevels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
            D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1,
        };
        UINT const flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT const hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, kLevels,
                                             static_cast<UINT>(std::size(kLevels)), D3D11_SDK_VERSION,
                                             d3dDevice_.put(), nullptr, nullptr);
        if (FAILED(hr))
        {
            // No usable GPU (a virtual machine, a remote session): use the WARP rasterizer.
            winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, kLevels,
                                                   static_cast<UINT>(std::size(kLevels)), D3D11_SDK_VERSION,
                                                   d3dDevice_.put(), nullptr, nullptr));
        }

        auto const dxgiDevice = d3dDevice_.as<IDXGIDevice>();
        winrt::check_hresult(d2dFactory_->CreateDevice(dxgiDevice.get(), d2dDevice_.put()));
        winrt::check_hresult(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, context_.put()));
        context_->SetDpi(96.0f, 96.0f);  // one unit = one pixel: the layout is computed in pixels
        context_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        winrt::check_hresult(context_->CreateSolidColorBrush(kBlack, brush_.put()));

        winrt::com_ptr<IDXGIAdapter> adapter;
        winrt::check_hresult(dxgiDevice->GetAdapter(adapter.put()));
        winrt::com_ptr<IDXGIFactory2> factory;
        winrt::check_hresult(adapter->GetParent(IID_PPV_ARGS(factory.put())));

        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = static_cast<UINT>(width_);
        description.Height = static_cast<UINT>(height_);
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        winrt::check_hresult(
            factory->CreateSwapChainForComposition(d3dDevice_.get(), &description, nullptr, swapChain_.put()));
        winrt::check_hresult(panel_->SetSwapChain(swapChain_.get()));
        CreateTargets();
    }

    void MapRenderer::CreateTargets()
    {
        context_->SetTarget(nullptr);
        target_ = nullptr;
        base_ = nullptr;

        winrt::com_ptr<IDXGISurface> surface;
        winrt::check_hresult(swapChain_->GetBuffer(0, IID_PPV_ARGS(surface.put())));
        D2D1_PIXEL_FORMAT const format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        D2D1_BITMAP_PROPERTIES1 const targetProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, format, 96.0f, 96.0f);
        winrt::check_hresult(context_->CreateBitmapFromDxgiSurface(surface.get(), &targetProperties, target_.put()));

        D2D1_BITMAP_PROPERTIES1 const baseProperties =
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format, 96.0f, 96.0f);
        winrt::check_hresult(context_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(width_), static_cast<UINT32>(height_)),
                                                    nullptr, 0, &baseProperties, base_.put()));

        // The panel stretches the swap chain by the composition scale; undo
        // that so one swap-chain pixel is exactly one screen pixel.
        DXGI_MATRIX_3X2_F inverseScale{};
        inverseScale._11 = 1.0f / scaleX_;
        inverseScale._22 = 1.0f / scaleY_;
        winrt::check_hresult(swapChain_.as<IDXGISwapChain2>()->SetMatrixTransform(&inverseScale));
        baseValid_ = false;
    }

    void MapRenderer::CreateTextFormats()
    {
        float const size = kFontSizeDips * scale_;
        auto const make = [&](DWRITE_TEXT_ALIGNMENT alignment, bool trimmed) {
            winrt::com_ptr<IDWriteTextFormat> format;
            winrt::check_hresult(dwriteFactory_->CreateTextFormat(fontFamily_.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                                  size, L"", format.put()));
            winrt::check_hresult(format->SetTextAlignment(alignment));
            winrt::check_hresult(format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
            winrt::check_hresult(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
            if (trimmed)
            {
                // Names that do not fit end in "...", instead of being cut off mid-letter.
                DWRITE_TRIMMING const trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                winrt::com_ptr<IDWriteInlineObject> ellipsis;
                winrt::check_hresult(dwriteFactory_->CreateEllipsisTrimmingSign(format.get(), ellipsis.put()));
                winrt::check_hresult(format->SetTrimming(&trimming, ellipsis.get()));
            }
            return format;
        };
        leadingFormat_ = make(DWRITE_TEXT_ALIGNMENT_LEADING, false);
        centeredFormat_ = make(DWRITE_TEXT_ALIGNMENT_CENTER, true);
        labelFormat_ = make(DWRITE_TEXT_ALIGNMENT_CENTER, false);
        measureFormat_ = make(DWRITE_TEXT_ALIGNMENT_LEADING, false);

        winrt::com_ptr<IDWriteTextLayout> layout;
        winrt::check_hresult(dwriteFactory_->CreateTextLayout(L"Ag", 2, measureFormat_.get(), 1000.0f, 1000.0f, layout.put()));
        DWRITE_TEXT_METRICS metrics{};
        winrt::check_hresult(layout->GetMetrics(&metrics));
        lineHeight_ = metrics.height;
        dotsWidth_ = TextWidth(L"...");
    }

    std::wstring MapRenderer::FitName(std::wstring_view name, float width) const
    {
        constexpr std::size_t kFirstLetters = 5;    // at least this many of the name's first letters...
        constexpr std::wstring_view kDots = L"...";  // ...and these, or no name at all
        winrt::com_ptr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS metrics{};
        if (name.empty() ||
            FAILED(dwriteFactory_->CreateTextLayout(name.data(), static_cast<UINT32>(name.size()), measureFormat_.get(),
                                                    1.0e6f, 1.0e6f, layout.put())) ||
            FAILED(layout->GetMetrics(&metrics)))
        {
            return {};
        }
        if (metrics.widthIncludingTrailingWhitespace <= width)
        {
            return std::wstring(name);
        }

        // Whole letters - clusters: a letter with its accents, or both halves
        // of a surrogate pair - for as long as they fit with the dots.
        UINT32 count = 0;
        (void)layout->GetClusterMetrics(nullptr, 0, &count);  // fails, asking for room for `count`
        std::vector<DWRITE_CLUSTER_METRICS> clusters(count);
        if (count == 0 || FAILED(layout->GetClusterMetrics(clusters.data(), count, &count)))
        {
            return {};
        }
        float const room = width - dotsWidth_;
        float used = 0.0f;
        std::size_t letters = 0;
        std::size_t length = 0;  // UTF-16 code units
        for (DWRITE_CLUSTER_METRICS const& cluster : clusters)
        {
            if (used + cluster.width > room)
            {
                break;
            }
            used += cluster.width;
            length += cluster.length;
            ++letters;
        }
        if (letters < kFirstLetters)
        {
            return {};
        }
        std::wstring_view prefix = name.substr(0, length);
        while (!prefix.empty() && prefix.back() == L' ')
        {
            prefix.remove_suffix(1);  // "Program..." rather than "Program ..."
        }
        return std::wstring(prefix).append(kDots);
    }

    void MapRenderer::ReleaseDevice() noexcept
    {
        // The panel keeps the old swap chain until CreateDeviceResources() hands
        // it a new one (SetSwapChain does not accept null).
        target_ = nullptr;
        base_ = nullptr;
        brush_ = nullptr;
        context_ = nullptr;
        d2dDevice_ = nullptr;
        swapChain_ = nullptr;
        d3dDevice_ = nullptr;
        baseValid_ = false;
    }

    bool MapRenderer::SetSize(double widthDips, double heightDips, float scaleX, float scaleY)
    {
        if (!(scaleX > 0.0f) || !(scaleY > 0.0f))
        {
            scaleX = scaleY = 1.0f;
        }
        int const width = std::max(1, static_cast<int>(std::lround(widthDips * scaleX)));
        int const height = std::max(1, static_cast<int>(std::lround(heightDips * scaleY)));
        bool const scaleChanged = scaleX != scaleX_ || scaleY != scaleY_;
        if (width == width_ && height == height_ && !scaleChanged)
        {
            return false;
        }

        width_ = width;
        height_ = height;
        scaleX_ = scaleX;
        scaleY_ = scaleY;
        scale_ = std::max(scaleX, scaleY);
        // Outlines stay one pixel wide up to 175 %, like 1.4.0's at 100 %, then grow.
        lineWidth_ = std::max(1, static_cast<int>(std::floor(scale_ + 0.25f)));
        if (scaleChanged)
        {
            CreateTextFormats();
        }

        if (swapChain_)
        {
            context_->SetTarget(nullptr);
            target_ = nullptr;
            base_ = nullptr;
            HRESULT const hr =
                swapChain_->ResizeBuffers(2, static_cast<UINT>(width_), static_cast<UINT>(height_), DXGI_FORMAT_B8G8R8A8_UNORM, 0);
            if (IsDeviceLost(hr))
            {
                ReleaseDevice();
                CreateDeviceResources();
            }
            else
            {
                winrt::check_hresult(hr);
                CreateTargets();
            }
        }
        return true;
    }

    int MapRenderer::HeaderHeight() const noexcept
    {
        return static_cast<int>(std::ceil(lineHeight_)) + 2 * lineWidth_;
    }

    float MapRenderer::FontSizeDips() const noexcept
    {
        return kFontSizeDips;
    }

    sm::BoxColors MapRenderer::Colors(MapScene const& scene, bool folder, bool special, int depth, bool highlighted,
                                      bool selected) const
    {
        if (special)
        {
            return sm::FreeSpaceColors(*scene.theme, scene.darkTheme);
        }
        if (selected)
        {
            sm::Rgb const selection = scene.colors.selection;
            return { selection, selection, selection };
        }
        sm::Settings const& settings = *scene.settings;
        sm::BoxColors colors = sm::ThemeBoxColors(*scene.theme, folder ? settings.folderColor : settings.fileColor, depth,
                                                  scene.darkTheme);
        if (settings.rolloverBox)
        {
            colors = sm::ApplyRollover(colors, highlighted);
        }
        return colors;
    }

    sm::BoxColors MapRenderer::ItemColors(MapScene const& scene, sm::DisplayItem const& item, bool highlighted,
                                          bool selected) const
    {
        return Colors(scene, item.IsFolder(), item.IsSpecial(), item.depth, highlighted, selected);
    }

    float MapRenderer::TextWidth(std::wstring_view text) const
    {
        winrt::com_ptr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(dwriteFactory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), measureFormat_.get(),
                                                    1.0e6f, 1.0e6f, layout.put())) ||
            FAILED(layout->GetMetrics(&metrics)))
        {
            return std::numeric_limits<float>::infinity();  // unmeasurable text never "fits"
        }
        return metrics.widthIncludingTrailingWhitespace;
    }

    bool MapRenderer::Render(MapScene const& scene, MapOverlay const& overlay)
    {
        if (!swapChain_)
        {
            return true;
        }
        try
        {
            if (!baseValid_)
            {
                DrawBase(scene);
                baseValid_ = true;
            }

            context_->SetTarget(target_.get());
            context_->BeginDraw();
            context_->SetTransform(D2D1::Matrix3x2F::Identity());
            // Copy (not blend) the cached map: every pixel is replaced, so no clear is needed.
            context_->DrawImage(base_.get(), D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_COPY);
            if (scene.tree != nullptr && (scene.pie != nullptr || scene.layout != nullptr))
            {
                // std::cmp_* compares signed with unsigned without wrap-around.
                std::size_t const count = scene.pie != nullptr ? scene.pie->Items().size() : scene.layout->Items().size();
                auto const draw = [&](int index, State state) {
                    if (scene.pie != nullptr)
                    {
                        DrawSlice(scene, static_cast<std::size_t>(index), state);
                    }
                    else
                    {
                        DrawItem(scene, static_cast<std::size_t>(index), state);
                    }
                };
                context_->SetAntialiasMode(scene.pie != nullptr ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE
                                                                : D2D1_ANTIALIAS_MODE_ALIASED);
                for (int const index : overlay.highlighted)
                {
                    if (index >= 0 && std::cmp_less(index, count) && index != overlay.selected)
                    {
                        draw(index, State::Highlighted);
                    }
                }
                if (overlay.selected >= 0 && std::cmp_less(overlay.selected, count))
                {
                    draw(overlay.selected, State::Selected);
                }
            }
            if (overlay.zoomOutline)
            {
                context_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                brush_->SetColor(overlay.accent);
                context_->DrawRectangle(*overlay.zoomOutline, brush_.get(), 2.0f * static_cast<float>(lineWidth_));
            }
            if (overlay.zoomSector && scene.pie != nullptr)
            {
                context_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                DrawSectorOutline(scene, *overlay.zoomSector, overlay.accent);
            }
            HRESULT hr = context_->EndDraw();
            context_->SetTarget(nullptr);
            if (SUCCEEDED(hr))
            {
                hr = swapChain_->Present(1, 0);
            }
            if (IsDeviceLost(hr))
            {
                ReleaseDevice();
                CreateDeviceResources();
                return false;
            }
            winrt::check_hresult(hr);
            return true;
        }
        catch (winrt::hresult_error const& error)
        {
            if (!IsDeviceLost(error.code()))
            {
                throw;
            }
            ReleaseDevice();
            CreateDeviceResources();
            return false;
        }
    }

    void MapRenderer::DrawBase(MapScene const& scene)
    {
        context_->SetTarget(base_.get());
        context_->BeginDraw();
        context_->SetTransform(D2D1::Matrix3x2F::Identity());
        context_->Clear(scene.background ? ToColor(*scene.background) : kTransparent);
        if (scene.tree != nullptr && scene.pie != nullptr)
        {
            DrawPie(scene);
        }
        else if (scene.tree != nullptr && scene.layout != nullptr)
        {
            context_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            std::size_t const count = scene.layout->Items().size();
            for (std::size_t i = 0; i < count; ++i)
            {
                DrawItem(scene, i, State::Normal);
            }
        }
        HRESULT const hr = context_->EndDraw();
        context_->SetTarget(nullptr);
        winrt::check_hresult(hr);
    }

    void MapRenderer::Fill(float left, float top, float right, float bottom, D2D1_COLOR_F const& color)
    {
        if (right <= left || bottom <= top)
        {
            return;
        }
        brush_->SetColor(color);
        context_->FillRectangle(D2D1::RectF(left, top, right, bottom), brush_.get());
    }

    void MapRenderer::DrawItem(MapScene const& scene, std::size_t index, State state)
    {
        sm::DisplayItem const& item = scene.layout->Items()[index];
        auto const t = static_cast<float>(lineWidth_);
        auto const left = static_cast<float>(item.x);
        auto const top = static_cast<float>(item.y);
        auto const right = static_cast<float>(item.x + item.w + 1);  // boxes include their right/bottom outline
        auto const bottom = static_cast<float>(item.y + item.h + 1);
        sm::BoxColors const colors =
            ItemColors(scene, item, state == State::Highlighted, state == State::Selected);

        if (!item.IsSpecial())
        {
            // The outline (black in the classic themes), shared with the neighbouring boxes.
            D2D1_COLOR_F const outline = ToColor(scene.colors.outline);
            Fill(left, top, right, top + t, outline);
            Fill(left, bottom - t, right, bottom, outline);
            Fill(left, top, left + t, bottom, outline);
            Fill(right - t, top, right, bottom, outline);
        }
        if (right - left <= 2 * t || bottom - top <= 2 * t)
        {
            return;
        }

        // The bevel: highlight along the top and left, shadow along the bottom and right.
        D2D1_COLOR_F const face = ToColor(colors.face);
        D2D1_COLOR_F const highlight = ToColor(colors.highlight);
        D2D1_COLOR_F const shadow = ToColor(colors.shadow);
        float const l1 = left + t;
        float const t1 = top + t;
        float const r1 = right - t;
        float const b1 = bottom - t;
        Fill(l1, t1, r1, t1 + t, highlight);
        Fill(l1, b1 - t, r1, b1, shadow);
        Fill(l1, t1, l1 + t, b1, highlight);
        Fill(r1 - t, t1, r1, b1, shadow);

        float const l2 = l1 + t;
        float const t2 = t1 + t;
        float const r2 = r1 - t;
        float const b2 = b1 - t;
        if (r2 <= l2 || b2 <= t2)
        {
            return;
        }

        // A folder whose contents are on the map only paints the band around
        // them (with its title), so redrawing it never covers the contents.
        bool const hasContents = item.IsFolder() && item.subtreeEnd > index + 1;
        if (hasContents)
        {
            sm::LayoutOptions const& options = scene.layout->Options();
            float const contentLeft = left + static_cast<float>(options.border);
            float const contentTop = top + static_cast<float>(options.header);
            float const contentRight = right - static_cast<float>(options.border);
            float const contentBottom = bottom - static_cast<float>(options.border);
            Fill(l2, t2, r2, contentTop, face);
            Fill(l2, contentTop, contentLeft, contentBottom, face);
            Fill(contentRight, contentTop, r2, contentBottom, face);
            Fill(l2, contentBottom, r2, b2, face);
        }
        else
        {
            Fill(l2, t2, r2, b2, face);
        }

        DrawLabel(scene, item, D2D1::RectF(l2, t2, r2, b2), colors.face);
    }

    void MapRenderer::DrawLabel(MapScene const& scene, sm::DisplayItem const& item, D2D1_RECT_F const& inner,
                                sm::Rgb face)
    {
        sm::Entry const* const entry = item.GetEntry();
        if (entry == nullptr)
        {
            return;  // an anonymous filler box
        }
        float const width = inner.right - inner.left;
        float const height = inner.bottom - inner.top;
        if (width < lineHeight_ * 0.75f || height < lineHeight_ * 0.6f)
        {
            return;  // not even one letter fits; the name tip shows the name instead
        }

        sm::Lang const& lang = *scene.lang;
        sm::FolderTree const& tree = *scene.tree;
        std::wstring text;
        IDWriteTextFormat* format = centeredFormat_.get();
        D2D1_RECT_F rect = inner;

        switch (entry->kind)
        {
        case sm::EntryKind::FreeSpace:
        {
            sm::VolumeInfo const& volume = tree.Volume();
            sm::Folder const& root = *tree.Root();
            text.append(L"<").append(lang.freeSpace).append(L": ");
            text.append(sm::FormatPercent(volume.freeBytes, volume.totalBytes, lang)).append(L">\n");
            text.append(sm::FormatSize(volume.freeBytes, lang)).append(L" ").append(lang.free).append(L"\n");
            text.append(lang.filesTotal).append(L"  ").append(sm::FormatCount(root.fileCount, lang)).append(L"\n");
            text.append(lang.foldersTotal).append(L"  ").append(sm::FormatCount(root.folderCount, lang));
            break;
        }
        case sm::EntryKind::Unscanned:
            text.append(L"<").append(lang.unscanned).append(L": ");
            text.append(sm::FormatPercent(entry->size, tree.Volume().totalBytes, lang)).append(L">\n");
            text.append(sm::FormatSize(entry->size, lang));
            break;
        case sm::EntryKind::Folder:
            // The name sits in the title band, as in 1.4.0 - whole, or its first
            // letters and "...", or not at all: never cut off mid-letter.
            format = leadingFormat_.get();
            rect.left += static_cast<float>(lineWidth_);
            rect.bottom = std::min(inner.bottom, static_cast<float>(item.y + scene.layout->Options().header));
            text = FitName(item.source->Name(item.index), rect.right - rect.left - 1.0f);
            if (text.empty())
            {
                return;  // no room for five letters and the dots: the name tip shows the name
            }
            break;
        case sm::EntryKind::File:
            text = item.source->Name(item.index);
            // With room for three lines, add the size and date, as 1.4.0 did.
            if (height >= 3.0f * lineHeight_ + 2.0f * static_cast<float>(lineWidth_) && width >= 48.0f * scale_)
            {
                text.append(L"\n").append(sm::FormatBytes(entry->logicalSize, lang));
                text.append(L"\n").append(sm::FormatFileTime(entry->lastWriteTime, lang));
            }
            break;
        }

        brush_->SetColor(ToColor(sm::TextColor(scene.colors, face)));
        context_->DrawText(text.data(), static_cast<UINT32>(text.size()), format, rect, brush_.get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    bool MapRenderer::IsLabelTruncated(MapScene const& scene, sm::DisplayItem const& item)
    {
        sm::Entry const* const entry = item.GetEntry();
        if (entry == nullptr || item.IsSpecial() || scene.layout == nullptr)
        {
            return false;
        }
        float const textWidth = TextWidth(item.source->Name(item.index));
        auto const t = static_cast<float>(lineWidth_);
        float width = static_cast<float>(item.w + 1) - 4.0f * t;
        float const height = item.IsFolder() ? static_cast<float>(scene.layout->Options().header) - 2.0f * t
                                             : static_cast<float>(item.h + 1) - 4.0f * t;
        if (item.IsFolder())
        {
            width -= t;
        }
        return textWidth > width || lineHeight_ > height;
    }
}
