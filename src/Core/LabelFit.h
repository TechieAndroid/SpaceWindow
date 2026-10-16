// SpaceWindow - fitting the map's labels into their boxes and slices.
//
// New in SpaceWindow.  What a label says and where it goes is worked out here
// from the widths of its text; the renderer measures text (DirectWrite) and
// draws what comes out.  So the rules - a folder's name shortened to its
// first letters and "...", five letters at least; a pie slice's label kept
// level and whole inside the slice - are tested without a screen.
#pragma once

#include <cstdint>
#include <functional>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "PieLayout.h"

namespace sm
{
    // A letter as drawn: its width in pixels, and how many UTF-16 code units
    // make it (a letter with its accents, both halves of a surrogate pair).
    struct Cluster
    {
        float width = 0.0f;
        std::uint16_t length = 0;
    };

    // A folder's name as it fits `room` pixels, `clusters` being its letters:
    // whole; else its first letters - five at least - and "..." (`dotsWidth`
    // wide), as many letters as fit with the dots; else nothing.  The name
    // never ends in a space before the dots ("Program..." for "Program Files").
    std::wstring FitName(std::wstring_view name, std::span<Cluster const> clusters, float room, float dotsWidth);

    // A part of a ring: [inner, outer] pixels from the centre, [start, start
    // + sweep] radians clockwise from twelve o'clock.  An inner radius of 0
    // is a whole disc.
    struct RingSegment
    {
        PiePoint center;
        double inner = 0.0;
        double outer = 0.0;
        double start = 0.0;
        double sweep = 2.0 * std::numbers::pi;
    };

    // Is the point inside the segment, at least `pad` pixels from its edges?
    bool Inside(RingSegment const& segment, double x, double y, double pad) noexcept;

    // The widest level block of text `height` pixels tall, centred on
    // `anchor`, that fits inside the segment with `pad` pixels to spare (0 if none does).
    double Room(RingSegment const& segment, PiePoint anchor, double height, double pad);

    // A label placed in a segment: what it says, and the box it is drawn in.
    struct FittedLabel
    {
        std::wstring text;
        double left = 0.0;
        double top = 0.0;
        double right = 0.0;
        double bottom = 0.0;
        bool trimmed = false;  // a name shortened with "..."
    };

    // How wide a text is drawn, in pixels.
    using MeasureText = std::function<double(std::wstring_view text)>;
    // A name shortened to fit a width (FitName), or nothing.
    using ShortenName = std::function<std::wstring(std::wstring_view name, double width)>;

    // Fits `name` - and `detail` on a second line, when both fit whole (or,
    // if `sideBySide`, after the name on the same line) - level and centred
    // on `anchor`, `lineHeight` pixels a line.  If `mayTrim`, a name too long
    // for the room is shortened, if it can be.  Nothing if it does not fit.
    std::optional<FittedLabel> FitLabel(RingSegment const& segment, PiePoint anchor, double pad, double lineHeight,
                                        std::wstring const& name, std::wstring const& detail, bool mayTrim,
                                        bool sideBySide, MeasureText const& measure, ShortenName const& shorten);
}
