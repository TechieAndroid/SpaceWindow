// SpaceWindow - Direct2D renderer for the map: the treemap and the pie chart.
//
// Replaces the GDI drawing code of FolderView.cpp in SpaceMonger 1.4.0
// (Copyright (c) 1998-2020 Sean Werkema, MIT License): OnDraw(),
// MinimalDrawDisplayFolder() and the helpers in xapp.h.  The pie chart is new.
//
//  * Draws with Direct2D into a SwapChainPanel at the panel's physical pixel
//    size, so outlines stay one pixel and crisp at every scale factor while
//    the DirectWrite labels scale with DPI.  1.4.0 drew 9-pixel "Small Fonts"
//    text that Windows bitmap-stretched on high-DPI screens.
//  * The whole map is drawn once into an off-screen bitmap.  Hover, selection
//    and zoom-animation frames copy that bitmap and redraw only the boxes that
//    changed - the GPU version of 1.4.0's "minimal redraw" - so the map
//    follows the mouse at the display's refresh rate even with thousands of boxes.
//  * The pie chart (MapRendererPie.cpp) is antialiased, separates its slices
//    with a gap of background rather than outlines, and labels a slice only
//    where the label fits; the name and info tips carry the rest.
//  * Survives GPU resets (device-lost handling).
#pragma once

#include "../Core/Palette.h"
#include "../Core/PieLayout.h"
#include "../Core/Theme.h"
#include "../Core/TreemapLayout.h"

namespace sm
{
    class FolderTree;
    struct Lang;
    struct Settings;
}

namespace sm::ui
{
    // What to draw.
    struct MapScene
    {
        sm::FolderTree const* tree = nullptr;
        sm::TreemapLayout const* layout = nullptr;  // the treemap ...
        sm::PieLayout const* pie = nullptr;         // ... or, when set, the pie chart
        sm::Settings const* settings = nullptr;
        sm::Lang const* lang = nullptr;
        sm::Theme const* theme = nullptr;  // required
        sm::MapColors colors;              // the theme's map colours in the window's mode
        bool darkTheme = false;            // the window is in dark mode
        // Behind the map: the window's colour, or nothing so that Mica shows
        // through (the swap chain shows nothing else of what is behind it).
        std::optional<sm::Rgb> background;
    };

    // A segment of the pie chart's rings (for its zoom animation).
    struct Sector
    {
        float start = 0.0f;  // radians, clockwise from twelve o'clock
        float sweep = 0.0f;
        float inner = 0.0f;  // pixels from the centre
        float outer = 0.0f;
    };

    // Text placed inside a pie slice or the hub: level, centred in it.
    struct PieLabel
    {
        std::wstring text;
        D2D1_RECT_F rect{};
        bool trimmed = false;  // a folder's name shortened with "..."
    };

    // Transient decorations drawn over the cached map.
    struct MapOverlay
    {
        int selected = -1;                       // layout item drawn as selected
        std::span<int const> highlighted;        // items drawn highlighted (rollover chain, slice under the mouse)
        std::optional<D2D1_RECT_F> zoomOutline;  // treemap zoom animation frame, in pixels
        std::optional<Sector> zoomSector;        // pie chart zoom animation frame
        D2D1_COLOR_F accent{ 0.0f, 0.47f, 0.84f, 1.0f };
    };

    class MapRenderer
    {
    public:
        MapRenderer();
        MapRenderer(MapRenderer const&) = delete;
        MapRenderer& operator=(MapRenderer const&) = delete;

        void Attach(winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel);

        // Sets the panel's size in DIPs and its composition scale.  Returns
        // true if the pixel size or the scale changed (the map needs a new layout).
        bool SetSize(double widthDips, double heightDips, float scaleX, float scaleY);

        int PixelWidth() const noexcept { return width_; }
        int PixelHeight() const noexcept { return height_; }
        float Scale() const noexcept { return scale_; }

        // Metrics the layout needs so boxes fit their labels at this scale.
        int LineWidth() const noexcept { return lineWidth_; }
        int BorderWidth() const noexcept { return 3 * lineWidth_; }
        int HeaderHeight() const noexcept;
        float FontSizeDips() const noexcept;
        std::wstring_view FontFamily() const noexcept { return fontFamily_; }

        // The cached map must be redrawn (new layout, colours or language).
        void InvalidateBase() noexcept { baseValid_ = false; }

        // Draws a frame.  Returns false if the GPU was reset and the frame
        // should be retried.
        bool Render(MapScene const& scene, MapOverlay const& overlay);

        // True if an item's name does not fit in its box (it gets a name tip).
        bool IsLabelTruncated(MapScene const& scene, sm::DisplayItem const& item);

        // The colours an item is drawn with (also used for its name tip).
        sm::BoxColors ItemColors(MapScene const& scene, sm::DisplayItem const& item, bool highlighted,
                                 bool selected) const;

        // The same for a pie slice.
        bool IsSliceLabelTruncated(MapScene const& scene, sm::PieSlice const& slice);
        sm::BoxColors SliceColors(MapScene const& scene, sm::PieSlice const& slice, bool highlighted,
                                  bool selected) const;

    private:
        enum class State
        {
            Normal,
            Highlighted,
            Selected,
        };

        void CreateDeviceResources();
        void CreateTargets();
        void CreateTextFormats();
        void ReleaseDevice() noexcept;
        void DrawBase(MapScene const& scene);
        void DrawItem(MapScene const& scene, std::size_t index, State state);
        void DrawLabel(MapScene const& scene, sm::DisplayItem const& item, D2D1_RECT_F const& inner, sm::Rgb face);
        void Fill(float left, float top, float right, float bottom, D2D1_COLOR_F const& color);
        sm::BoxColors Colors(MapScene const& scene, bool folder, bool special, int depth, bool highlighted,
                             bool selected) const;
        float TextWidth(std::wstring_view text) const;

        // A folder's name as it fits `width` pixels: whole; else its first
        // letters - five at least - followed by "...", as many as fit with the
        // dots; else nothing.  Measured, so the name never runs off its box.
        std::wstring FitName(std::wstring_view name, float width) const;

        // The pie chart (MapRendererPie.cpp).
        void DrawPie(MapScene const& scene);
        void DrawSlice(MapScene const& scene, std::size_t index, State state);
        void DrawHub(MapScene const& scene);
        void DrawSectorOutline(MapScene const& scene, Sector const& sector, D2D1_COLOR_F const& color);
        void DrawPieLabel(MapScene const& scene, PieLabel const& label, sm::Rgb face);
        winrt::com_ptr<ID2D1PathGeometry1> SectorShape(sm::PiePoint center, double inner, double outer, double start,
                                                       double sweep, double inset) const;
        std::optional<PieLabel> SliceLabel(MapScene const& scene, sm::PieSlice const& slice) const;

        winrt::com_ptr<ID2D1Factory1> d2dFactory_;
        winrt::com_ptr<IDWriteFactory> dwriteFactory_;
        winrt::com_ptr<ISwapChainPanelNative> panel_;

        winrt::com_ptr<ID3D11Device> d3dDevice_;
        winrt::com_ptr<ID2D1Device> d2dDevice_;
        winrt::com_ptr<ID2D1DeviceContext> context_;
        winrt::com_ptr<IDXGISwapChain1> swapChain_;
        winrt::com_ptr<ID2D1Bitmap1> target_;  // the swap chain's back buffer
        winrt::com_ptr<ID2D1Bitmap1> base_;    // the cached map
        winrt::com_ptr<ID2D1SolidColorBrush> brush_;

        winrt::com_ptr<IDWriteTextFormat> leadingFormat_;   // folder titles (fitted by FitName)
        winrt::com_ptr<IDWriteTextFormat> centeredFormat_;  // file names and the free-space block
        winrt::com_ptr<IDWriteTextFormat> labelFormat_;     // pie labels (fitted beforehand)
        winrt::com_ptr<IDWriteTextFormat> measureFormat_;   // untrimmed, for measuring
        std::wstring fontFamily_;
        float lineHeight_ = 0.0f;
        float dotsWidth_ = 0.0f;  // of the "..." that ends a shortened name

        int width_ = 1;
        int height_ = 1;
        float scaleX_ = 1.0f;
        float scaleY_ = 1.0f;
        float scale_ = 1.0f;
        int lineWidth_ = 1;
        bool baseValid_ = false;
    };
}
