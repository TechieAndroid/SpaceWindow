// SpaceWindow - number, size, date and attribute formatting.
//
// Modernized from FolderView.cpp of SpaceMonger 1.4.0
// (Copyright (c) 1998-2020 Sean Werkema, MIT License).
#include "pch.h"
#include "Format.h"

#include "Lang.h"

namespace sm
{
    namespace
    {
        void AppendTwoDigits(std::wstring& out, unsigned value)
        {
            out.push_back(static_cast<wchar_t>(L'0' + (value / 10) % 10));
            out.push_back(static_cast<wchar_t>(L'0' + value % 10));
        }
    }

    std::wstring FormatCount(std::uint64_t value, Lang const& lang)
    {
        // Build the digits right to left, inserting a separator every three.
        wchar_t buffer[40];
        wchar_t* end = std::end(buffer);
        wchar_t* p = end;
        int digits = 0;
        do
        {
            if (digits != 0 && digits % 3 == 0)
            {
                *--p = lang.thousandsSeparator;
            }
            *--p = static_cast<wchar_t>(L'0' + value % 10);
            value /= 10;
            ++digits;
        } while (value != 0);
        return { p, end };
    }

    std::wstring FormatBytes(std::uint64_t bytes, Lang const& lang)
    {
        std::wstring result = FormatCount(bytes, lang);
        result.push_back(L' ');
        result.append(lang.bytes);
        return result;
    }

    std::wstring FormatSize(std::uint64_t bytes, Lang const& lang)
    {
        if (bytes < 1024)
        {
            return FormatBytes(bytes, lang);
        }

        std::wstring_view const units[] = { lang.kb, lang.mb, lang.gb, lang.tb, lang.pb };
        std::uint64_t unit = 1024;
        std::size_t index = 0;
        while (index + 1 < std::size(units) && bytes / unit >= 1024)
        {
            unit *= 1024;
            ++index;
        }

        std::uint64_t const whole = bytes / unit;
        // unit <= 2^50, so remainder * 10 cannot overflow.
        auto const tenth = static_cast<unsigned>((bytes % unit) * 10 / unit);

        std::wstring result = FormatCount(whole, lang);
        result.push_back(lang.decimalPoint);
        result.push_back(static_cast<wchar_t>(L'0' + tenth));
        result.push_back(L' ');
        result.append(units[index]);
        return result;
    }

    std::uint32_t PerMille(std::uint64_t part, std::uint64_t whole) noexcept
    {
        if (whole == 0)
        {
            return 0;
        }
        if (part >= whole)
        {
            return 1000;
        }
        if (part <= UINT64_MAX / 1000)
        {
            return static_cast<std::uint32_t>(part * 1000 / whole);
        }
        // part * 1000 would overflow 64 bits (sizes above ~16 PB).  A double keeps
        // 53 bits of precision, far more than a tenth of a percent needs.
        double const ratio = static_cast<double>(part) / static_cast<double>(whole);
        return std::min<std::uint32_t>(static_cast<std::uint32_t>(ratio * 1000.0), 999);
    }

    std::wstring FormatPercent(std::uint64_t part, std::uint64_t whole, Lang const& lang)
    {
        std::uint32_t const value = PerMille(part, whole);
        std::wstring result = std::to_wstring(value / 10);
        result.push_back(lang.decimalPoint);
        result.push_back(static_cast<wchar_t>(L'0' + value % 10));
        result.append(lang.percent);
        return result;
    }

    std::wstring FormatFileTime(std::uint64_t fileTimeUtc, Lang const& lang)
    {
        FILETIME utc{};
        utc.dwLowDateTime = static_cast<DWORD>(fileTimeUtc);
        utc.dwHighDateTime = static_cast<DWORD>(fileTimeUtc >> 32);

        SYSTEMTIME universal{};
        SYSTEMTIME local{};
        if (!FileTimeToSystemTime(&utc, &universal) ||
            !SystemTimeToTzSpecificLocalTimeEx(nullptr, &universal, &local))
        {
            return {};
        }

        // Same layout as 1.4.0: "05 Mar 1999   14:03:22".
        std::wstring result;
        result.reserve(32);
        AppendTwoDigits(result, local.wDay);
        result.push_back(L' ');
        result.append(lang.monthNames[(local.wMonth - 1u) % 12u]);
        result.push_back(L' ');
        result.append(std::to_wstring(local.wYear));
        result.append(L"   ");
        result.append(std::to_wstring(local.wHour));
        result.push_back(L':');
        AppendTwoDigits(result, local.wMinute);
        result.push_back(L':');
        AppendTwoDigits(result, local.wSecond);
        return result;
    }

    std::wstring FormatAttributes(std::uint32_t attributes, Lang const& lang)
    {
        // Same order as SetupInfoTip() in 1.4.0.
        static constexpr std::uint32_t kFlags[] = {
            FILE_ATTRIBUTE_ARCHIVE,   FILE_ATTRIBUTE_COMPRESSED,    FILE_ATTRIBUTE_DIRECTORY,
            FILE_ATTRIBUTE_ENCRYPTED, FILE_ATTRIBUTE_HIDDEN,        FILE_ATTRIBUTE_OFFLINE,
            FILE_ATTRIBUTE_READONLY,  FILE_ATTRIBUTE_REPARSE_POINT, FILE_ATTRIBUTE_SPARSE_FILE,
            FILE_ATTRIBUTE_SYSTEM,    FILE_ATTRIBUTE_TEMPORARY,
        };
        static_assert(std::size(kFlags) == std::tuple_size_v<decltype(Lang::attributeNames)>);

        std::wstring result;
        for (std::size_t i = 0; i < std::size(kFlags); ++i)
        {
            if (attributes & kFlags[i])
            {
                if (!result.empty())
                {
                    result.push_back(L' ');
                }
                result.append(lang.attributeNames[i]);
            }
        }
        return result;
    }
}
