// SpaceWindow - small text helpers shared by the library and the app.
#pragma once

#include <string>
#include <string_view>

namespace sm
{
    // Compare as Windows compares file names: code unit by code unit,
    // ignoring case.
    bool EqualsIgnoringCase(std::wstring_view a, std::wstring_view b) noexcept;
    bool LessIgnoringCase(std::wstring_view a, std::wstring_view b) noexcept;

    // Strings, views and literals, one after another.  The parts bind by
    // reference, so no view outlives a temporary it was made from.
    template <typename... Parts>
    std::wstring Join(Parts const&... parts)
    {
        std::wstring result;
        (result.append(parts), ...);
        return result;
    }
}
