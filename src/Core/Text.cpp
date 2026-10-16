// SpaceWindow - small text helpers shared by the library and the app.
#include "pch.h"
#include "Text.h"

namespace sm
{
    namespace
    {
        // CSTR_LESS_THAN, CSTR_EQUAL or CSTR_GREATER_THAN.
        int Compare(std::wstring_view a, std::wstring_view b) noexcept
        {
            if (a.empty() || b.empty())
            {
                // An empty view may have no characters to point to at all.
                return a.size() == b.size() ? CSTR_EQUAL : (a.empty() ? CSTR_LESS_THAN : CSTR_GREATER_THAN);
            }
            return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()),
                                        TRUE);
        }
    }

    bool EqualsIgnoringCase(std::wstring_view a, std::wstring_view b) noexcept
    {
        return Compare(a, b) == CSTR_EQUAL;
    }

    bool LessIgnoringCase(std::wstring_view a, std::wstring_view b) noexcept
    {
        return Compare(a, b) == CSTR_LESS_THAN;
    }
}
