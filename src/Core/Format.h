// SpaceWindow - number, size, date and attribute formatting.
//
// Modernized from PrintFileSize(), PrintDate() and GetSizeString() in
// FolderView.cpp of SpaceMonger 1.4.0 (Copyright (c) 1998-2020 Sean Werkema,
// MIT License).
#pragma once

#include <cstdint>
#include <string>

namespace sm
{
    struct Lang;

    // 1234567 -> "1,234,567" (separator from the language table).
    std::wstring FormatCount(std::uint64_t value, Lang const& lang);

    // 1234567 -> "1,234,567 bytes".  1.4.0 printed nothing at all for 0.
    std::wstring FormatBytes(std::uint64_t bytes, Lang const& lang);

    // 1234567 -> "1.1 MB".  Binary units, one truncated decimal, as in 1.4.0,
    // but with TB and PB added so a 4 TB drive no longer reads "4096.0 Gb".
    std::wstring FormatSize(std::uint64_t bytes, Lang const& lang);

    // part/whole -> "12.3%" (truncated to a tenth of a percent).  Safe for
    // sizes where the original part * 1000 overflowed 64 bits.
    std::wstring FormatPercent(std::uint64_t part, std::uint64_t whole, Lang const& lang);

    // Tenths of a percent, truncated; 0 if whole is 0.
    std::uint32_t PerMille(std::uint64_t part, std::uint64_t whole) noexcept;

    // FILETIME (UTC) -> "05 Mar 1999   14:03:22" in *local* time.  1.4.0
    // showed UTC because it skipped the time-zone conversion.
    std::wstring FormatFileTime(std::uint64_t fileTimeUtc, Lang const& lang);

    // FILE_ATTRIBUTE_* -> "Archive Hidden System" using the language's names.
    std::wstring FormatAttributes(std::uint32_t attributes, Lang const& lang);
}
