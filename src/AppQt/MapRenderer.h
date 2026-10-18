// SpaceWindow (Qt) - drawing the map: the treemap and the pie chart.
//
// A QPainter port of src/App/MapRenderer*.cpp (Direct2D).  The map is drawn
// in the screen's physical pixels - outlines stay one pixel wide and crisp at
// any scale - into an image that is drawn once per layout; hover, selection
// and zoom frames draw only what changes, over it, straight on the screen.
// A background animation (MapRendererBackdrop.cpp) plays behind the map
// where it leaves room: the cached map then keeps its background clear.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainterPath>

#include "Backdrop.h"
#include "LabelFit.h"
#include "MapView.h"
#include "Palette.h"
#include "Theme.h"

class QImage;
class QPainter;

namespace sm
{
    class FolderTree;
    struct Lang;
}

namespace sm::qt
{
    // What to draw.
    struct MapScene
    {
        FolderTree const* tree = nullptr;
        TreemapLayout const* layout = nullptr;  // the treemap ...
        PieLayout const* pie = nullptr;         // ... or, when set, the pie chart
        Settings const* settings = nullptr;
        Lang const* lang = nullptr;
        Theme const* theme = nullptr;        // required
        MapColors colors;                    // the theme's map colours in the window's mode
        bool darkTheme = false;              // the window is in dark mode
        Rgb background;                      // behind the map: the window's colour
        Backdrop backdrop = Backdrop::None;  // the animation behind the map, if one plays
        double time = 0.0;                   // seconds into it
    };

    // Transient decorations drawn over the cached map.
    struct MapOverlay
    {
        int selected = -1;
        std::span<int const> highlighted;
        std::optional<RectF> zoomOutline;  // treemap zoom animation frame, in pixels
        std::optional<Sector> zoomSector;  // pie chart zoom animation frame
        QColor accent{ 0, 120, 215 };
    };

    class MapRenderer
    {
    public:
        // The screen's scale: one map pixel per physical pixel.  Takes the
        // application's font again, so call it after the font changed too.
        // True if the metrics below changed.
        bool SetScale(double scale);
        double Scale() const noexcept { return scale_; }

        // Metrics the layout needs so boxes fit their labels at this scale.
        int LineWidth() const noexcept { return lineWidth_; }
        int BorderWidth() const noexcept { return 3 * lineWidth_; }
        int HeaderHeight() const noexcept;
        QFont const& Font() const noexcept { return font_; }

        // The whole map, into an image of the map's size in pixels: on the
        // window's colour, or on nothing while an animation plays behind it.
        void DrawBase(QImage& image, MapScene const& scene);

        // The background animation for a width x height pixel panel: stepped
        // at its own pace, and drawn off-screen as far as it can be ...
        void PrepareBackdrop(MapScene const& scene, int width, int height);
        // ... then drawn under the map, covering every pixel.
        void DrawBackdrop(QPainter& painter, MapScene const& scene) const;
        // True if the animation would look different drawn now.
        bool BackdropMoved(MapScene const& scene, int width, int height) const noexcept;
        // No animation: what it kept goes.
        void ReleaseBackdrop() noexcept;

        // The selection, the highlighted items and the zoom outline, over the base.  The
        // painter's coordinates must be the map's pixels.
        void DrawOverlay(QPainter& painter, MapScene const& scene, MapOverlay const& overlay);

        // True if an item's name does not fit in its box (it gets a name tip).
        bool IsLabelTruncated(MapScene const& scene, DisplayItem const& item);
        BoxColors ItemColors(MapScene const& scene, DisplayItem const& item, bool highlighted, bool selected) const;
        bool IsSliceLabelTruncated(MapScene const& scene, PieSlice const& slice);
        BoxColors SliceColors(MapScene const& scene, PieSlice const& slice, bool highlighted, bool selected) const;

    private:
        enum class State
        {
            Normal,
            Highlighted,
            Selected,
        };

        struct PieLabel
        {
            std::wstring text;
            QRectF rect;
            bool trimmed = false;
        };

        void DrawItem(QPainter& painter, MapScene const& scene, std::size_t index, State state);
        void DrawLabel(QPainter& painter, MapScene const& scene, DisplayItem const& item, QRectF const& inner,
                       Rgb face);
        void DrawText(QPainter& painter, QRectF const& rect, std::wstring const& text, Qt::Alignment alignment,
                      bool elide, QColor const& color);
        BoxColors Colors(MapScene const& scene, bool folder, bool special, int depth, bool highlighted,
                         bool selected) const;
        float TextWidth(std::wstring_view text) const;
        std::wstring FitName(std::wstring_view name, float width) const;

        void DrawPie(QPainter& painter, MapScene const& scene);
        void DrawSlice(QPainter& painter, MapScene const& scene, std::size_t index, State state);
        void DrawHub(QPainter& painter, MapScene const& scene);
        void DrawPieLabel(QPainter& painter, MapScene const& scene, PieLabel const& label, Rgb face);
        QPainterPath SectorShape(PiePoint center, double inner, double outer, double start, double sweep,
                                 double inset) const;
        std::optional<PieLabel> SliceLabel(MapScene const& scene, PieSlice const& slice) const;
        MeasureText Measure() const;
        ShortenName Shorten() const;

        void PrepareGrid(int cell, double rate, double time, bool smooth);
        void PrepareRain(double time);
        void DrawTunnel(QPainter& painter, double time) const;

        double scale_ = 0.0;
        int lineWidth_ = 1;
        QFont font_;
        float lineHeight_ = 12.0f;
        float dotsWidth_ = 0.0f;

        struct TextHash
        {
            using is_transparent = void;
            std::size_t operator()(std::wstring_view text) const noexcept
            {
                return std::hash<std::wstring_view>{}(text);
            }
        };
        mutable std::unordered_map<std::wstring, float, TextHash, std::equal_to<>> widths_;

        struct BackdropState
        {
            Backdrop kind = Backdrop::None;
            Random random;
            double next = 0.0;  // seconds: when the next step is due
            float phaseA = 0.0f;
            float phaseB = 0.0f;
            int width = 0;  // the panel, in pixels
            int height = 0;
            // Fire, colour mix and Life: a pixel per cell, stretched over the panel.
            FireField fire;
            LifeField life;
            std::vector<std::uint32_t> pixels;  // premultiplied 0xAARRGGBB
            QImage grid;                        // over `pixels`, not a copy
            int columns = 0;
            int rows = 0;
            int cell = 0;
            bool smooth = false;
            // Falling characters: the glyphs drawn once, in the head's and the
            // trail's colours, and stamped into a layer whenever the rain moves.
            MatrixRain rain;
            QImage headGlyphs;
            QImage trailGlyphs;
            int glyphWidth = 0;
            int glyphHeight = 0;
            QImage layer;
            // The tunnel: drawn afresh every frame.
            std::vector<TunnelRing> rings;
        };
        BackdropState backdrop_;
    };
}
