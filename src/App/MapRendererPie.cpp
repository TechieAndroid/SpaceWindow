// SpaceWindow - Direct2D renderer for the map: the pie chart.
//
// New in SpaceWindow.  Every slice is drawn inset by half the gap on all sides, so
// neighbouring slices and rings are separated by the background itself (a
// "surface gap") rather than by drawn outlines, and edges are antialiased.
// A slice's label is level text centred in the middle of the slice - a name
// and its size when both fit whole, else the name alone - and only where it
// fits: never tilted or shifted.  A folder's name too long for its slice is
// shortened to its first letters and "..." (five letters at least); other
// names show whole or not at all.  Zooming in makes room; until then the name
// tip and the info tip show what a slice has no room for.  The rules are in
// src/Core/LabelFit.cpp (tested there); this file measures and draws.
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
        constexpr double kPi = std::numbers::pi;
        constexpr double kTwoPi = 2.0 * std::numbers::pi;

        // A sweep this close to a full turn is drawn as a whole ring: an arc
        // from a point back to (almost) the same point is not well defined.
        constexpr double kFullTurn = kTwoPi - 1e-4;

        D2D1_COLOR_F ToColor(sm::Rgb color) noexcept
        {
            return { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f };
        }

        // The slice standing for a folder's smaller entries is the theme's
        // de-emphasised "grouped" colour (a grey about 3:1 against the window in
        // the classic themes), a step lighter under the mouse.  Free space and
        // the hub are outlined in the same colour.
        sm::Rgb GroupColor(MapScene const& scene, bool highlighted) noexcept
        {
            sm::MapColors const& colors = scene.colors;
            return highlighted ? sm::Mix(colors.grouped, colors.lightText, 0.2) : colors.grouped;
        }

        // What shows behind the map, for text drawn straight on it: the
        // animation's dark canvas, the window's colour, or Mica.
        sm::Rgb Behind(MapScene const& scene) noexcept
        {
            if (scene.backdrop != sm::Backdrop::None)
            {
                return { 0x04, 0x04, 0x06 };
            }
            if (scene.background)
            {
                return *scene.background;
            }
            return scene.darkTheme ? sm::Rgb{ 0x20, 0x20, 0x20 } : sm::Rgb{ 0xF3, 0xF3, 0xF3 };
        }

        // Free space under the mouse: one step towards the text colour.
        sm::Rgb Lift(MapScene const& scene, sm::Rgb color) noexcept
        {
            return scene.darkTheme ? sm::Mix(color, scene.colors.lightText, 0.18)
                                   : sm::Mix(color, scene.colors.darkText, 0.1);
        }

        // A fitted label (sm::FitLabel) as the renderer draws it.
        PieLabel ToPieLabel(sm::FittedLabel label) noexcept
        {
            return PieLabel{ std::move(label.text),
                             D2D1::RectF(static_cast<float>(label.left), static_cast<float>(label.top),
                                         static_cast<float>(label.right), static_cast<float>(label.bottom)),
                             label.trimmed };
        }
    }

    sm::BoxColors MapRenderer::SliceColors(MapScene const& scene, sm::PieSlice const& slice, bool highlighted,
                                           bool selected) const
    {
        if (slice.IsFolded())
        {
            sm::Rgb const grouped = GroupColor(scene, highlighted);
            return { grouped, grouped, grouped };
        }
        sm::BoxColors colors = Colors(scene, slice.IsFolder(), slice.IsSpecial(), slice.depth, highlighted, selected);
        if (highlighted && !selected)
        {
            // The slice under the mouse lifts.  With rollover boxes on, the
            // scheme's rollover shading has already done that.
            if (slice.IsSpecial())
            {
                colors.face = Lift(scene, colors.face);
            }
            else if (!scene.settings->rolloverBox)
            {
                colors.face = colors.highlight;
            }
        }
        return colors;
    }

    bool MapRenderer::IsSliceLabelTruncated(MapScene const& scene, sm::PieSlice const& slice)
    {
        if (scene.pie == nullptr || !slice.IsSelectable())
        {
            return false;  // grouped entries and free space have info tips instead
        }
        std::optional<PieLabel> const label = SliceLabel(scene, slice);
        return !label || label->trimmed;
    }

    winrt::com_ptr<ID2D1PathGeometry1> MapRenderer::SectorShape(sm::PiePoint center, double inner, double outer,
                                                                double start, double sweep, double inset) const
    {
        double const a = inner > 0.0 ? inner + inset : 0.0;
        double const b = outer - inset;
        if (b <= a || sweep <= 0.0)
        {
            return nullptr;
        }
        auto const point = [center](double radius, double angle) {
            return D2D1::Point2F(static_cast<float>(center.x + radius * std::sin(angle)),
                                 static_cast<float>(center.y - radius * std::cos(angle)));
        };
        auto const arcSize = [](double angle) { return angle > kPi ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL; };

        winrt::com_ptr<ID2D1PathGeometry1> path;
        winrt::check_hresult(d2dFactory_->CreatePathGeometry(path.put()));
        winrt::com_ptr<ID2D1GeometrySink> sink;
        winrt::check_hresult(path->Open(sink.put()));
        if (sweep >= kFullTurn)
        {
            // A whole ring: the outer circle with the inner one cut out.
            sink->SetFillMode(D2D1_FILL_MODE_ALTERNATE);
            for (double const radius : { b, a })
            {
                if (radius <= 0.0)
                {
                    continue;
                }
                auto const size = D2D1::SizeF(static_cast<float>(radius), static_cast<float>(radius));
                sink->BeginFigure(point(radius, 0.0), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddArc(D2D1::ArcSegment(point(radius, kPi), size, 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                              D2D1_ARC_SIZE_SMALL));
                sink->AddArc(D2D1::ArcSegment(point(radius, 0.0), size, 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                              D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            }
        }
        else
        {
            // The straight edges move in by `inset`, staying parallel to
            // themselves: at radius r that is asin(inset / r) radians.
            auto const turn = [inset](double radius) {
                return inset > 0.0 && radius > 0.0 ? std::asin(std::min(1.0, inset / radius)) : 0.0;
            };
            double const outerFrom = start + turn(b);
            double const outerTo = start + sweep - turn(b);
            if (outerTo <= outerFrom)
            {
                return nullptr;  // narrower than the gap: nothing of it shows
            }
            sink->BeginFigure(point(b, outerFrom), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddArc(D2D1::ArcSegment(point(b, outerTo), D2D1::SizeF(static_cast<float>(b), static_cast<float>(b)),
                                          0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, arcSize(outerTo - outerFrom)));
            // A narrow slice can come to a point before it reaches its inner radius.
            double const apex = sweep < kPi && inset > 0.0 ? inset / std::sin(sweep / 2.0) : 0.0;
            if (apex >= a)
            {
                sink->AddLine(point(apex, start + sweep / 2.0));
            }
            else
            {
                double const innerFrom = start + turn(a);
                double const innerTo = start + sweep - turn(a);
                sink->AddLine(point(a, innerTo));
                sink->AddArc(D2D1::ArcSegment(point(a, innerFrom),
                                              D2D1::SizeF(static_cast<float>(a), static_cast<float>(a)), 0.0f,
                                              D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, arcSize(innerTo - innerFrom)));
            }
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        }
        winrt::check_hresult(sink->Close());
        return path;
    }

    void MapRenderer::DrawPie(MapScene const& scene)
    {
        context_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        std::size_t const count = scene.pie->Items().size();
        for (std::size_t i = 0; i < count; ++i)
        {
            DrawSlice(scene, i, State::Normal);
        }
        DrawHub(scene);
    }

    void MapRenderer::DrawSlice(MapScene const& scene, std::size_t index, State state)
    {
        sm::PieLayout const& pie = *scene.pie;
        sm::PieSlice const& slice = pie.Items()[index];
        winrt::com_ptr<ID2D1PathGeometry1> const shape =
            SectorShape(pie.Center(), pie.InnerRadius(slice.ring), pie.OuterRadius(slice.ring), slice.start,
                        slice.sweep, pie.Options().gap / 2.0);
        if (!shape)
        {
            return;  // narrower than the gap; it can still be pointed at for its tip
        }
        bool const selected = state == State::Selected;
        sm::BoxColors const colors = SliceColors(scene, slice, state == State::Highlighted, selected);
        brush_->SetColor(ToColor(colors.face));
        context_->FillGeometry(shape.get(), brush_.get());
        if (slice.IsSpecial())
        {
            // Free space is drawn "empty": a plain fill with a hairline edge.
            brush_->SetColor(ToColor(GroupColor(scene, false)));
            context_->DrawGeometry(shape.get(), brush_.get(), static_cast<float>(lineWidth_));
        }
        if (std::optional<PieLabel> const label = SliceLabel(scene, slice))
        {
            DrawPieLabel(scene, *label, colors.face);
        }
    }

    void MapRenderer::DrawHub(MapScene const& scene)
    {
        sm::PieLayout const& pie = *scene.pie;
        sm::Folder const* const root = pie.Root();
        if (root == nullptr)
        {
            return;
        }
        sm::PiePoint const center = pie.Center();
        double const inset = pie.Options().gap / 2.0;
        winrt::com_ptr<ID2D1PathGeometry1> const shape =
            SectorShape(center, pie.HubInner(), pie.HubOuter(), 0.0, kTwoPi, inset);
        if (!shape)
        {
            return;
        }

        // The hub - or, in the inverted pie, the rim - stands for the current
        // folder: its name and the space the pie shows.  The rim is open: no
        // fill and no edge, just the name over whatever is behind the map.
        bool const rim = pie.HubInner() > 0.0;
        sm::Rgb const face = rim ? Behind(scene) : scene.colors.freeSpace;
        if (!rim)
        {
            brush_->SetColor(ToColor(face));
            context_->FillGeometry(shape.get(), brush_.get());
            brush_->SetColor(ToColor(GroupColor(scene, false)));
            context_->DrawGeometry(shape.get(), brush_.get(), static_cast<float>(lineWidth_));
        }

        std::wstring name;
        if (root->parent != nullptr)
        {
            name = root->parent->Name(root->indexInParent);
        }
        else
        {
            // The scanned folder's own name ("C:\Windows\" -> "Windows"); a drive keeps its root ("C:\").
            std::wstring_view path = scene.tree->RootPath();
            if (path.size() > 3 && path.ends_with(L'\\'))
            {
                path.remove_suffix(1);
                path.remove_prefix(path.find_last_of(L'\\') + 1);
            }
            name = path;
        }
        std::wstring const total = sm::FormatSize(pie.Total(), *scene.lang);
        double const inner = pie.HubInner() > 0.0 ? pie.HubInner() + inset : 0.0;
        double const outer = pie.HubOuter() - inset;
        sm::RingSegment const segment{ center, inner, outer, 0.0, kTwoPi };
        // The hub's label sits in its middle; the rim's at its top, like a title.
        sm::PiePoint const anchor = inner > 0.0 ? sm::PiePoint{ center.x, center.y - (inner + outer) / 2.0 } : center;
        // The hub does not grow when zooming, so its name is shortened rather
        // than left out.  A thin rim has room for one line: the size goes beside the name.
        if (std::optional<sm::FittedLabel> label = sm::FitLabel(segment, anchor, 2.0 * lineWidth_, lineHeight_, name,
                                                                total, true, true, Measure(), Shorten()))
        {
            DrawPieLabel(scene, ToPieLabel(std::move(*label)), face);
        }
    }

    std::optional<PieLabel> MapRenderer::SliceLabel(MapScene const& scene, sm::PieSlice const& slice) const
    {
        sm::PieLayout const& pie = *scene.pie;
        sm::RingSegment const segment{ pie.Center(), pie.InnerRadius(slice.ring), pie.OuterRadius(slice.ring),
                                       slice.start, slice.sweep };
        double const pad = pie.Options().gap / 2.0 + 2.0 * lineWidth_;
        // Most slices are too small for any text: rule them out before measuring.
        // A line of text needs its height one way and a few letters the other,
        // across the ring or along it.
        double const thickness = segment.outer - segment.inner;
        double const length = slice.sweep * (segment.outer - pad);
        if (std::min(thickness, length) < lineHeight_ + 2.0 * pad || std::max(thickness, length) < 3.0 * lineHeight_)
        {
            return std::nullopt;
        }

        sm::Lang const& lang = *scene.lang;
        std::wstring name;
        if (slice.IsFolded())
        {
            std::wstring const count = sm::FormatCount(slice.folded, lang);
            name = sm::FormatText(lang.smallerItems, { count });
        }
        else if (slice.IsSpecial())
        {
            sm::Entry const* const entry = slice.GetEntry();
            name = entry != nullptr && entry->kind == sm::EntryKind::FreeSpace ? lang.freeSpace : lang.unscanned;
        }
        else
        {
            name = slice.source->Name(slice.index);
        }
        // Level text in the middle of the slice - halfway round it and halfway
        // through the ring, or further out in a slice that comes to a point in
        // the centre (PieLayout::Centroid) - or nothing: a name that does not
        // fit there (a folder's, shortened to its first letters, at least)
        // waits for a zoom to give it room.
        sm::PiePoint const middle = pie.Centroid(slice);
        std::wstring const size = sm::FormatSize(slice.bytes, lang);
        std::optional<sm::FittedLabel> label =
            sm::FitLabel(segment, middle, pad, lineHeight_, name, size, slice.IsFolder(), false, Measure(), Shorten());
        return label ? std::optional<PieLabel>(ToPieLabel(std::move(*label))) : std::nullopt;
    }

    sm::MeasureText MapRenderer::Measure() const
    {
        return [this](std::wstring_view text) { return static_cast<double>(TextWidth(text)); };
    }

    sm::ShortenName MapRenderer::Shorten() const
    {
        return [this](std::wstring_view text, double width) { return FitName(text, static_cast<float>(width)); };
    }

    void MapRenderer::DrawPieLabel(MapScene const& scene, PieLabel const& label, sm::Rgb face)
    {
        brush_->SetColor(ToColor(sm::TextColor(scene.colors, face)));
        context_->DrawText(label.text.data(), static_cast<UINT32>(label.text.size()), labelFormat_.get(), label.rect,
                           brush_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void MapRenderer::DrawSectorOutline(MapScene const& scene, Sector const& sector, D2D1_COLOR_F const& color)
    {
        winrt::com_ptr<ID2D1PathGeometry1> const shape =
            SectorShape(scene.pie->Center(), sector.inner, sector.outer, sector.start, sector.sweep, 0.0);
        if (shape)
        {
            brush_->SetColor(color);
            context_->DrawGeometry(shape.get(), brush_.get(), 2.0f * static_cast<float>(lineWidth_));
        }
    }
}
