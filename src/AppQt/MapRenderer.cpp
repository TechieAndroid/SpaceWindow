// SpaceWindow (Qt) - drawing the map: the treemap and the pie chart.
//
// Box drawing follows MinimalDrawDisplayFolder() in FolderView.cpp of
// SpaceMonger 1.4.0 (Copyright (c) 1998-2020 Sean Werkema, MIT License), as
// src/App/MapRenderer.cpp does; the pie chart follows src/App/MapRendererPie.cpp.
#include "MapRenderer.h"

#include <cmath>
#include <numbers>

#include <QApplication>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QTextBoundaryFinder>

#include "FolderTree.h"
#include "Format.h"
#include "Lang.h"
#include "QtText.h"
#include "Settings.h"

namespace sm::qt
{
    namespace
    {
        // 1.4.0 used 9-pixel "Small Fonts"; this is the closest legible modern size (in DIPs).
        constexpr double kFontSize = 11.0;

        constexpr double kPi = std::numbers::pi;
        constexpr double kTwoPi = 2.0 * std::numbers::pi;
        constexpr double kFullTurn = kTwoPi - 1e-4;  // a sweep this close to a full turn is a whole ring

        double Degrees(double radians) noexcept
        {
            return radians * 180.0 / kPi;
        }

        // The slice standing for a folder's smaller entries is the theme's
        // de-emphasised "grouped" colour, a step lighter under the mouse.
        Rgb GroupColor(MapScene const& scene, bool highlighted) noexcept
        {
            MapColors const& colors = scene.colors;
            return highlighted ? Mix(colors.grouped, colors.lightText, 0.2) : colors.grouped;
        }

        // Free space under the mouse: one step towards the text colour.
        Rgb Lift(MapScene const& scene, Rgb color) noexcept
        {
            return scene.darkTheme ? Mix(color, scene.colors.lightText, 0.18) : Mix(color, scene.colors.darkText, 0.1);
        }

        void Fill(QPainter& painter, float left, float top, float right, float bottom, QColor const& color)
        {
            if (right <= left || bottom <= top)
            {
                return;
            }
            painter.fillRect(QRectF(left, top, right - left, bottom - top), color);
        }
    }

    bool MapRenderer::SetScale(double scale)
    {
        if (!(scale > 0.0))
        {
            scale = 1.0;
        }
        QFont font = QApplication::font();
        font.setPixelSize(std::max(6, static_cast<int>(std::lround(kFontSize * scale))));
        font.setHintingPreference(QFont::PreferNoHinting);
        // Grey-scale smoothing everywhere: the cached map is an image, which gets no
        // sub-pixel text, and the overlay drawn on the screen must look the same.
        font.setStyleStrategy(QFont::StyleStrategy(QFont::PreferDefault | QFont::NoSubpixelAntialias));
        if (scale == scale_ && font == font_)
        {
            return false;
        }
        scale_ = scale;
        // Outlines stay one pixel wide up to 175 %, like 1.4.0's at 100 %, then grow.
        lineWidth_ = std::max(1, static_cast<int>(std::floor(scale + 0.25)));
        font_ = font;
        QFontMetricsF const metrics(font_);
        lineHeight_ = static_cast<float>(metrics.height());
        widths_.clear();  // measured in the old size
        dotsWidth_ = TextWidth(L"...");
        return true;
    }

    int MapRenderer::HeaderHeight() const noexcept
    {
        return static_cast<int>(std::ceil(lineHeight_)) + 2 * lineWidth_;
    }

    float MapRenderer::TextWidth(std::wstring_view text) const
    {
        if (auto const known = widths_.find(text); known != widths_.end())
        {
            return known->second;
        }
        auto const width = static_cast<float>(QFontMetricsF(font_).horizontalAdvance(Q(text)));
        constexpr std::size_t kRemembered = 8192;  // a few screens' worth of labels
        if (widths_.size() >= kRemembered)
        {
            widths_.clear();
        }
        widths_.emplace(text, width);
        return width;
    }

    std::wstring MapRenderer::FitName(std::wstring_view name, float width) const
    {
        if (name.empty())
        {
            return {};
        }
        if (TextWidth(name) <= width)
        {
            return std::wstring(name);  // most names: whole, and measured before
        }
        // Its letters - a letter with its accents, an emoji with its modifiers -
        // for sm::FitName to keep as many as fit.
        QString const text = Q(name);
        QFontMetricsF const metrics(font_);
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
        std::vector<Cluster> clusters;
        qsizetype start = 0;
        for (qsizetype end = finder.toNextBoundary(); end > 0; end = finder.toNextBoundary())
        {
            QString const letter = text.mid(start, end - start);
            // Its length in wide characters: code points, a surrogate pair counting once.
            std::uint16_t length = 0;
            for (QChar const unit : letter)
            {
                length += unit.isLowSurrogate() ? 0 : 1;
            }
            clusters.push_back({ static_cast<float>(metrics.horizontalAdvance(letter)), length });
            start = end;
        }
        return sm::FitName(name, clusters, width, dotsWidth_);
    }

    BoxColors MapRenderer::Colors(MapScene const& scene, bool folder, bool special, int depth, bool highlighted,
                                  bool selected) const
    {
        if (special)
        {
            return FreeSpaceColors(*scene.theme, scene.darkTheme);
        }
        if (selected)
        {
            Rgb const selection = scene.colors.selection;
            return { selection, selection, selection };
        }
        Settings const& settings = *scene.settings;
        BoxColors colors =
            ThemeBoxColors(*scene.theme, folder ? settings.folderColor : settings.fileColor, depth, scene.darkTheme);
        if (settings.rolloverBox)
        {
            colors = ApplyRollover(colors, highlighted);
        }
        return colors;
    }

    BoxColors MapRenderer::ItemColors(MapScene const& scene, DisplayItem const& item, bool highlighted,
                                      bool selected) const
    {
        return Colors(scene, item.IsFolder(), item.IsSpecial(), item.depth, highlighted, selected);
    }

    void MapRenderer::DrawBase(QImage& image, MapScene const& scene)
    {
        image.fill(scene.backdrop != Backdrop::None ? QColor(Qt::transparent) : ToQColor(scene.background));
        QPainter painter(&image);
        painter.setFont(font_);
        if (scene.tree != nullptr && scene.pie != nullptr)
        {
            DrawPie(painter, scene);
        }
        else if (scene.tree != nullptr && scene.layout != nullptr)
        {
            painter.setRenderHint(QPainter::Antialiasing, false);
            std::size_t const count = scene.layout->Items().size();
            for (std::size_t i = 0; i < count; ++i)
            {
                DrawItem(painter, scene, i, State::Normal);
            }
        }
    }

    void MapRenderer::DrawOverlay(QPainter& painter, MapScene const& scene, MapOverlay const& overlay)
    {
        painter.save();
        painter.setFont(font_);
        if (scene.tree != nullptr && (scene.pie != nullptr || scene.layout != nullptr))
        {
            std::size_t const count = scene.pie != nullptr ? scene.pie->Items().size() : scene.layout->Items().size();
            auto const draw = [&](int index, State state) {
                if (scene.pie != nullptr)
                {
                    DrawSlice(painter, scene, static_cast<std::size_t>(index), state);
                }
                else
                {
                    DrawItem(painter, scene, static_cast<std::size_t>(index), state);
                }
            };
            painter.setRenderHint(QPainter::Antialiasing, scene.pie != nullptr);
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
            painter.setRenderHint(QPainter::Antialiasing, true);
            RectF const& r = *overlay.zoomOutline;
            painter.setPen(QPen(overlay.accent, 2.0 * lineWidth_));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(r.left, r.top, r.right - r.left, r.bottom - r.top));
        }
        if (overlay.zoomSector && scene.pie != nullptr)
        {
            painter.setRenderHint(QPainter::Antialiasing, true);
            Sector const& s = *overlay.zoomSector;
            QPainterPath const shape = SectorShape(scene.pie->Center(), s.inner, s.outer, s.start, s.sweep, 0.0);
            painter.setPen(QPen(overlay.accent, 2.0 * lineWidth_));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(shape);
        }
        painter.restore();
    }

    // ---------------------------------------------------------------- the treemap

    void MapRenderer::DrawItem(QPainter& painter, MapScene const& scene, std::size_t index, State state)
    {
        DisplayItem const& item = scene.layout->Items()[index];
        auto const t = static_cast<float>(lineWidth_);
        auto const left = static_cast<float>(item.x);
        auto const top = static_cast<float>(item.y);
        auto const right = static_cast<float>(item.x + item.w + 1);  // boxes include their right/bottom outline
        auto const bottom = static_cast<float>(item.y + item.h + 1);
        BoxColors const colors = ItemColors(scene, item, state == State::Highlighted, state == State::Selected);

        if (!item.IsSpecial())
        {
            // The outline (black in the classic themes), shared with the neighbouring boxes.
            QColor const outline = ToQColor(scene.colors.outline);
            Fill(painter, left, top, right, top + t, outline);
            Fill(painter, left, bottom - t, right, bottom, outline);
            Fill(painter, left, top, left + t, bottom, outline);
            Fill(painter, right - t, top, right, bottom, outline);
        }
        if (right - left <= 2 * t || bottom - top <= 2 * t)
        {
            return;
        }

        // The bevel: highlight along the top and left, shadow along the bottom and right.
        QColor const face = ToQColor(colors.face);
        QColor const highlight = ToQColor(colors.highlight);
        QColor const shadow = ToQColor(colors.shadow);
        float const l1 = left + t;
        float const t1 = top + t;
        float const r1 = right - t;
        float const b1 = bottom - t;
        Fill(painter, l1, t1, r1, t1 + t, highlight);
        Fill(painter, l1, b1 - t, r1, b1, shadow);
        Fill(painter, l1, t1, l1 + t, b1, highlight);
        Fill(painter, r1 - t, t1, r1, b1, shadow);

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
            LayoutOptions const& options = scene.layout->Options();
            float const contentLeft = left + static_cast<float>(options.border);
            float const contentTop = top + static_cast<float>(options.header);
            float const contentRight = right - static_cast<float>(options.border);
            float const contentBottom = bottom - static_cast<float>(options.border);
            Fill(painter, l2, t2, r2, contentTop, face);
            Fill(painter, l2, contentTop, contentLeft, contentBottom, face);
            Fill(painter, contentRight, contentTop, r2, contentBottom, face);
            Fill(painter, l2, contentBottom, r2, b2, face);
        }
        else
        {
            Fill(painter, l2, t2, r2, b2, face);
        }
        DrawLabel(painter, scene, item, QRectF(l2, t2, r2 - l2, b2 - t2), colors.face);
    }

    void MapRenderer::DrawText(QPainter& painter, QRectF const& rect, std::wstring const& text, Qt::Alignment alignment,
                               bool elide, QColor const& color)
    {
        QString shown = Q(text);
        if (elide)
        {
            // Names that do not fit end in "...", instead of being cut off mid-letter.
            QFontMetricsF const metrics(font_);
            QStringList lines = shown.split(QLatin1Char('\n'));
            for (QString& line : lines)
            {
                line = metrics.elidedText(line, Qt::ElideRight, rect.width());
            }
            shown = lines.join(QLatin1Char('\n'));
        }
        painter.save();
        painter.setClipRect(rect);
        painter.setPen(color);
        painter.drawText(rect, static_cast<int>(alignment | Qt::AlignVCenter), shown);
        painter.restore();
    }

    void MapRenderer::DrawLabel(QPainter& painter, MapScene const& scene, DisplayItem const& item, QRectF const& inner,
                                Rgb face)
    {
        Entry const* const entry = item.GetEntry();
        if (entry == nullptr)
        {
            return;  // an anonymous filler box
        }
        auto const width = static_cast<float>(inner.width());
        auto const height = static_cast<float>(inner.height());
        if (width < lineHeight_ * 0.75f || height < lineHeight_ * 0.6f)
        {
            return;  // not even one letter fits; the name tip shows the name instead
        }

        Lang const& lang = *scene.lang;
        FolderTree const& tree = *scene.tree;
        std::wstring text;
        Qt::Alignment alignment = Qt::AlignHCenter;
        bool elide = true;
        QRectF rect = inner;
        switch (entry->kind)
        {
        case EntryKind::FreeSpace:
        {
            VolumeInfo const& volume = tree.Volume();
            Folder const& root = *tree.Root();
            text.append(L"<").append(lang.freeSpace).append(L": ");
            text.append(FormatPercent(volume.freeBytes, volume.totalBytes, lang)).append(L">\n");
            text.append(FormatSize(volume.freeBytes, lang)).append(L" ").append(lang.free).append(L"\n");
            text.append(lang.filesTotal).append(L"  ").append(FormatCount(root.fileCount, lang)).append(L"\n");
            text.append(lang.foldersTotal).append(L"  ").append(FormatCount(root.folderCount, lang));
            break;
        }
        case EntryKind::Unscanned:
            text.append(L"<").append(lang.unscanned).append(L": ");
            text.append(FormatPercent(entry->size, tree.Volume().totalBytes, lang)).append(L">\n");
            text.append(FormatSize(entry->size, lang));
            break;
        case EntryKind::Folder:
            // The name sits in the title band, as in 1.4.0 - whole, or its first
            // letters and "...", or not at all: never cut off mid-letter.
            alignment = Qt::AlignLeft;
            elide = false;
            rect.setLeft(rect.left() + lineWidth_);
            rect.setBottom(std::min(inner.bottom(), static_cast<qreal>(item.y + scene.layout->Options().header)));
            text = FitName(item.source->Name(item.index), static_cast<float>(rect.width()) - 1.0f);
            if (text.empty())
            {
                return;  // no room for five letters and the dots: the name tip shows the name
            }
            break;
        case EntryKind::File:
            text = item.source->Name(item.index);
            // With room for three lines, add the size and date, as 1.4.0 did.
            if (height >= 3.0f * lineHeight_ + 2.0f * static_cast<float>(lineWidth_) &&
                width >= 48.0f * static_cast<float>(scale_))
            {
                text.append(L"\n").append(FormatBytes(entry->logicalSize, lang));
                text.append(L"\n").append(FormatFileTime(entry->lastWriteTime, lang));
            }
            break;
        }
        DrawText(painter, rect, text, alignment, elide, ToQColor(TextColor(scene.colors, face)));
    }

    bool MapRenderer::IsLabelTruncated(MapScene const& scene, DisplayItem const& item)
    {
        Entry const* const entry = item.GetEntry();
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

    // ---------------------------------------------------------------- the pie chart

    BoxColors MapRenderer::SliceColors(MapScene const& scene, PieSlice const& slice, bool highlighted,
                                       bool selected) const
    {
        if (slice.IsFolded())
        {
            Rgb const grouped = GroupColor(scene, highlighted);
            return { grouped, grouped, grouped };
        }
        BoxColors colors = Colors(scene, slice.IsFolder(), slice.IsSpecial(), slice.depth, highlighted, selected);
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

    bool MapRenderer::IsSliceLabelTruncated(MapScene const& scene, PieSlice const& slice)
    {
        if (scene.pie == nullptr || !slice.IsSelectable())
        {
            return false;  // grouped entries and free space have info tips instead
        }
        std::optional<PieLabel> const label = SliceLabel(scene, slice);
        return !label || label->trimmed;
    }

    QPainterPath MapRenderer::SectorShape(PiePoint center, double inner, double outer, double start, double sweep,
                                          double inset) const
    {
        QPainterPath path;
        double const a = inner > 0.0 ? inner + inset : 0.0;
        double const b = outer - inset;
        if (b <= a || sweep <= 0.0)
        {
            return path;
        }
        auto const point = [center](double radius, double angle) {
            return QPointF(center.x + radius * std::sin(angle), center.y - radius * std::cos(angle));
        };
        auto const box = [center](double radius) {
            return QRectF(center.x - radius, center.y - radius, 2.0 * radius, 2.0 * radius);
        };
        // Angles here run clockwise from twelve o'clock; Qt's run anticlockwise from three.
        auto const qtAngle = [](double angle) { return 90.0 - Degrees(angle); };

        if (sweep >= kFullTurn)
        {
            // A whole ring: the outer circle with the inner one cut out.
            path.setFillRule(Qt::OddEvenFill);
            path.addEllipse(box(b));
            if (a > 0.0)
            {
                path.addEllipse(box(a));
            }
            return path;
        }
        // The straight edges move in by `inset`, staying parallel to
        // themselves: at radius r that is asin(inset / r) radians.
        auto const turn = [inset](double radius) {
            return inset > 0.0 && radius > 0.0 ? std::asin(std::min(1.0, inset / radius)) : 0.0;
        };
        double const outerFrom = start + turn(b);
        double const outerTo = start + sweep - turn(b);
        if (outerTo <= outerFrom)
        {
            return path;  // narrower than the gap: nothing of it shows
        }
        path.moveTo(point(b, outerFrom));
        path.arcTo(box(b), qtAngle(outerFrom), -Degrees(outerTo - outerFrom));
        // A narrow slice can come to a point before it reaches its inner radius.
        double const apex = sweep < kPi && inset > 0.0 ? inset / std::sin(sweep / 2.0) : 0.0;
        if (apex >= a)
        {
            path.lineTo(point(apex, start + sweep / 2.0));
        }
        else
        {
            double const innerFrom = start + turn(a);
            double const innerTo = start + sweep - turn(a);
            path.lineTo(point(a, innerTo));
            path.arcTo(box(a), qtAngle(innerTo), Degrees(innerTo - innerFrom));
        }
        path.closeSubpath();
        return path;
    }

    void MapRenderer::DrawPie(QPainter& painter, MapScene const& scene)
    {
        painter.setRenderHint(QPainter::Antialiasing, true);
        std::size_t const count = scene.pie->Items().size();
        for (std::size_t i = 0; i < count; ++i)
        {
            DrawSlice(painter, scene, i, State::Normal);
        }
        DrawHub(painter, scene);
    }

    void MapRenderer::DrawSlice(QPainter& painter, MapScene const& scene, std::size_t index, State state)
    {
        PieLayout const& pie = *scene.pie;
        PieSlice const& slice = pie.Items()[index];
        QPainterPath const shape = SectorShape(pie.Center(), pie.InnerRadius(slice.ring), pie.OuterRadius(slice.ring),
                                               slice.start, slice.sweep, pie.Options().gap / 2.0);
        if (shape.isEmpty())
        {
            return;  // narrower than the gap; it can still be pointed at for its tip
        }
        BoxColors const colors = SliceColors(scene, slice, state == State::Highlighted, state == State::Selected);
        painter.fillPath(shape, ToQColor(colors.face));
        if (slice.IsSpecial())
        {
            // Free space is drawn "empty": a plain fill with a hairline edge.
            painter.strokePath(shape, QPen(ToQColor(GroupColor(scene, false)), lineWidth_));
        }
        if (std::optional<PieLabel> const label = SliceLabel(scene, slice))
        {
            DrawPieLabel(painter, scene, *label, colors.face);
        }
    }

    void MapRenderer::DrawHub(QPainter& painter, MapScene const& scene)
    {
        PieLayout const& pie = *scene.pie;
        Folder const* const root = pie.Root();
        if (root == nullptr)
        {
            return;
        }
        PiePoint const center = pie.Center();
        double const inset = pie.Options().gap / 2.0;
        QPainterPath const shape = SectorShape(center, pie.HubInner(), pie.HubOuter(), 0.0, kTwoPi, inset);
        if (shape.isEmpty())
        {
            return;
        }
        // The hub - or, in the inverted pie, the rim - stands for the current
        // folder: its name and the space the pie shows.  The rim is open.
        bool const rim = pie.HubInner() > 0.0;
        Rgb const face = rim ? scene.background : scene.colors.freeSpace;
        if (!rim)
        {
            painter.fillPath(shape, ToQColor(face));
            painter.strokePath(shape, QPen(ToQColor(GroupColor(scene, false)), lineWidth_));
        }

        std::wstring name;
        if (root->parent != nullptr)
        {
            name = root->parent->Name(root->indexInParent);
        }
        else
        {
            // The scanned folder's own name ("/home/ann/" -> "ann"); a whole drive keeps its root ("/").
            std::wstring_view path = scene.tree->RootPath();
            if (path.size() > 1 && path.ends_with(kPathSeparator))
            {
                path.remove_suffix(1);
                path.remove_prefix(path.find_last_of(kPathSeparator) + 1);
            }
            name = path;
        }
        std::wstring const total = FormatSize(pie.Total(), *scene.lang);
        double const inner = pie.HubInner() > 0.0 ? pie.HubInner() + inset : 0.0;
        double const outer = pie.HubOuter() - inset;
        RingSegment const segment{ center, inner, outer, 0.0, kTwoPi };
        PiePoint const anchor = inner > 0.0 ? PiePoint{ center.x, center.y - (inner + outer) / 2.0 } : center;
        if (std::optional<FittedLabel> label =
                FitLabel(segment, anchor, 2.0 * lineWidth_, lineHeight_, name, total, true, true, Measure(), Shorten()))
        {
            PieLabel const shown{
                std::move(label->text),
                QRectF(label->left, label->top, label->right - label->left, label->bottom - label->top), label->trimmed
            };
            DrawPieLabel(painter, scene, shown, face);
        }
    }

    std::optional<MapRenderer::PieLabel> MapRenderer::SliceLabel(MapScene const& scene, PieSlice const& slice) const
    {
        PieLayout const& pie = *scene.pie;
        RingSegment const segment{ pie.Center(), pie.InnerRadius(slice.ring), pie.OuterRadius(slice.ring), slice.start,
                                   slice.sweep };
        double const pad = pie.Options().gap / 2.0 + 2.0 * lineWidth_;
        // Most slices are too small for any text: rule them out before measuring.
        double const thickness = segment.outer - segment.inner;
        double const length = slice.sweep * (segment.outer - pad);
        if (std::min(thickness, length) < lineHeight_ + 2.0 * pad || std::max(thickness, length) < 3.0 * lineHeight_)
        {
            return std::nullopt;
        }

        Lang const& lang = *scene.lang;
        std::wstring name;
        if (slice.IsFolded())
        {
            std::wstring const count = FormatCount(slice.folded, lang);
            name = FormatText(lang.smallerItems, { count });
        }
        else if (slice.IsSpecial())
        {
            Entry const* const entry = slice.GetEntry();
            name = entry != nullptr && entry->kind == EntryKind::FreeSpace ? lang.freeSpace : lang.unscanned;
        }
        else
        {
            name = slice.source->Name(slice.index);
        }
        PiePoint const middle = pie.Centroid(slice);
        std::wstring const size = FormatSize(slice.bytes, lang);
        std::optional<FittedLabel> label =
            FitLabel(segment, middle, pad, lineHeight_, name, size, slice.IsFolder(), false, Measure(), Shorten());
        if (!label)
        {
            return std::nullopt;
        }
        return PieLabel{ std::move(label->text),
                         QRectF(label->left, label->top, label->right - label->left, label->bottom - label->top),
                         label->trimmed };
    }

    MeasureText MapRenderer::Measure() const
    {
        return [this](std::wstring_view text) { return static_cast<double>(TextWidth(text)); };
    }

    ShortenName MapRenderer::Shorten() const
    {
        return [this](std::wstring_view text, double width) { return FitName(text, static_cast<float>(width)); };
    }

    void MapRenderer::DrawPieLabel(QPainter& painter, MapScene const& scene, PieLabel const& label, Rgb face)
    {
        painter.save();
        painter.setPen(ToQColor(TextColor(scene.colors, face)));
        painter.drawText(label.rect, Qt::AlignCenter, Q(label.text));
        painter.restore();
    }
}
