// SpaceWindow - small text helpers shared by the library and the app.
#include "pch.h"
#include "Text.h"

namespace sm
{
    namespace
    {
#ifndef _WIN32
        constexpr int CSTR_LESS_THAN = 1;
        constexpr int CSTR_EQUAL = 2;
        constexpr int CSTR_GREATER_THAN = 3;

        // A letter in upper case, as Windows' file-name comparison has it
        // (simple one-to-one mappings): Latin, Greek, Cyrillic, Armenian and
        // full-width Latin, which covers the names people give files.
        constexpr char32_t Upper(char32_t c) noexcept
        {
            if (c < 0x80)
            {
                return c >= U'a' && c <= U'z' ? c - 0x20 : c;
            }
            if ((c >= 0xE0 && c <= 0xFE && c != 0xF7) || (c >= 0x3B1 && c <= 0x3CB && c != 0x3C2) ||
                (c >= 0x430 && c <= 0x44F) || (c >= 0xFF41 && c <= 0xFF5A))
            {
                return c - 0x20;
            }
            if (c == 0xFF)
            {
                return 0x178;
            }
            if (c == 0x3C2)
            {
                return 0x3A3;  // final sigma
            }
            if (c >= 0x450 && c <= 0x45F)
            {
                return c - 0x50;
            }
            if (c >= 0x561 && c <= 0x586)
            {
                return c - 0x30;  // Armenian
            }
            // Pairs of capital and small letter, the small one odd: most of Latin
            // Extended-A, Latin Extended Additional, Greek and Cyrillic extensions.
            bool const oddSmall = (c >= 0x100 && c <= 0x12F) || (c >= 0x132 && c <= 0x137) ||
                                  (c >= 0x14A && c <= 0x177) || (c >= 0x1E00 && c <= 0x1EFF) ||
                                  (c >= 0x3D8 && c <= 0x3EF) || (c >= 0x460 && c <= 0x481) ||
                                  (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F);
            if (oddSmall && (c & 1) != 0)
            {
                return c - 1;
            }
            // ... and those whose small one is even.
            bool const evenSmall =
                (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E) || (c >= 0x4C1 && c <= 0x4CE);
            if (evenSmall && (c & 1) == 0)
            {
                return c - 1;
            }
            return c;
        }

        int CompareStringOrdinal(wchar_t const* a, int aLength, wchar_t const* b, int bLength, bool ignoreCase) noexcept
        {
            int const common = std::min(aLength, bLength);
            for (int i = 0; i < common; ++i)
            {
                auto x = static_cast<char32_t>(a[i]);
                auto y = static_cast<char32_t>(b[i]);
                if (ignoreCase)
                {
                    x = Upper(x);
                    y = Upper(y);
                }
                if (x != y)
                {
                    return x < y ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
                }
            }
            return aLength == bLength ? CSTR_EQUAL : (aLength < bLength ? CSTR_LESS_THAN : CSTR_GREATER_THAN);
        }

        constexpr bool TRUE = true;
#endif

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
