// SpaceWindow - fitting the map's labels into their boxes and slices.
#include "pch.h"
#include "LabelFit.h"

namespace sm
{
    namespace
    {
        constexpr double kTwoPi = 2.0 * std::numbers::pi;

        // A sweep this close to a full turn is a whole ring.
        constexpr double kFullTurn = kTwoPi - 1e-4;
    }

    std::wstring FitName(std::wstring_view name, std::span<Cluster const> clusters, float room, float dotsWidth)
    {
        constexpr std::size_t kFirstLetters = 5;     // at least this many of the name's first letters...
        constexpr std::wstring_view kDots = L"...";  // ...and these, or no name at all
        if (name.empty())
        {
            return {};
        }
        float total = 0.0f;
        for (Cluster const& cluster : clusters)
        {
            total += cluster.width;
        }
        if (total <= room)
        {
            return std::wstring(name);
        }

        // Whole letters, for as long as they fit with the dots.
        float const space = room - dotsWidth;
        float used = 0.0f;
        std::size_t letters = 0;
        std::size_t length = 0;  // wide characters
        for (Cluster const& cluster : clusters)
        {
            if (used + cluster.width > space)
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

    bool Inside(RingSegment const& segment, double x, double y, double pad) noexcept
    {
        double const dx = x - segment.center.x;
        double const dy = y - segment.center.y;
        double const radius = std::hypot(dx, dy);
        if ((segment.inner > 0.0 && radius < segment.inner + pad) || radius > segment.outer - pad)
        {
            return false;
        }
        if (segment.sweep >= kFullTurn)
        {
            return true;
        }
        double angle = std::atan2(dx, -dy);
        if (angle < 0.0)
        {
            angle += kTwoPi;
        }
        double const margin = pad / std::max(radius, 1.0);  // `pad` pixels, as an angle at this radius
        return angle >= segment.start + margin && angle <= segment.start + segment.sweep - margin;
    }

    double Room(RingSegment const& segment, PiePoint anchor, double height, double pad)
    {
        auto const fits = [&](double width) {
            double const halfWidth = width / 2.0;
            double const halfHeight = height / 2.0;
            for (double const dx : { -halfWidth, 0.0, halfWidth })
            {
                for (double const dy : { -halfHeight, 0.0, halfHeight })
                {
                    if (!Inside(segment, anchor.x + dx, anchor.y + dy, pad))
                    {
                        return false;
                    }
                }
            }
            if (segment.inner <= 0.0)
            {
                return true;
            }
            // Corners and edge midpoints are in; no part of the block may dip into
            // the ring's hole either.  Find its point nearest the centre.
            double const dx = segment.center.x - anchor.x;
            double const dy = segment.center.y - anchor.y;
            return std::hypot(dx - std::clamp(dx, -halfWidth, halfWidth),
                              dy - std::clamp(dy, -halfHeight, halfHeight)) >= segment.inner + pad;
        };
        if (!fits(0.0))
        {
            return 0.0;
        }
        double fitting = 0.0;
        double failing = 2.0 * segment.outer;
        while (failing - fitting > 0.5)
        {
            double const width = (fitting + failing) / 2.0;
            (fits(width) ? fitting : failing) = width;
        }
        return fitting;
    }

    std::optional<FittedLabel> FitLabel(RingSegment const& segment, PiePoint anchor, double pad, double lineHeight,
                                        std::wstring const& name, std::wstring const& detail, bool mayTrim,
                                        bool sideBySide, MeasureText const& measure, ShortenName const& shorten)
    {
        auto const label = [anchor, lineHeight](std::wstring text, double width, double lines, bool trimmed) {
            double const height = lines * lineHeight;
            return FittedLabel{ std::move(text),        anchor.x - width / 2.0,  anchor.y - height / 2.0,
                                anchor.x + width / 2.0, anchor.y + height / 2.0, trimmed };
        };
        constexpr double kSlack = 2.0;  // DirectWrite trims text set in a box exactly its own width
        double const nameWidth = measure(name) + kSlack;
        double const detailWidth = measure(detail) + kSlack;
        double const both = std::max(nameWidth, detailWidth);
        if (Room(segment, anchor, 2.0 * lineHeight, pad) >= both)
        {
            std::wstring text = name;
            text.append(L"\n").append(detail);
            return label(std::move(text), both, 2.0, false);
        }
        double const room = Room(segment, anchor, lineHeight, pad);
        if (sideBySide)
        {
            std::wstring text = name;
            text.append(L"  ·  ").append(detail);
            if (double const width = measure(text) + kSlack; room >= width)
            {
                return label(std::move(text), width, 1.0, false);
            }
        }
        if (room >= nameWidth)
        {
            return label(name, nameWidth, 1.0, false);
        }
        if (mayTrim && shorten)
        {
            if (std::wstring shortened = shorten(name, room - kSlack); !shortened.empty())
            {
                return label(std::move(shortened), room, 1.0, true);
            }
        }
        return std::nullopt;
    }
}
