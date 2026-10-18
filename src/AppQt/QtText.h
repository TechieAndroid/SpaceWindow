// SpaceWindow (Qt) - text and colours between the core library and Qt.
#pragma once

#include <string>
#include <string_view>

#include <QColor>
#include <QString>

#include "Lang.h"
#include "Palette.h"

namespace sm::qt
{
    // Wide strings hold whole code points here (UTF-32); QString is UTF-16.
    inline QString Q(std::wstring_view text)
    {
        return QString::fromWCharArray(text.data(), static_cast<qsizetype>(text.size()));
    }

    inline std::wstring W(QString const& text)
    {
        return text.toStdWString();
    }

    inline QColor ToQColor(Rgb color)
    {
        return { color.r, color.g, color.b };
    }

    // The language tables speak of Windows' Recycle Bin and File Explorer:
    // the words a Linux desktop uses instead, in each language.
    struct Wording
    {
        std::wstring_view showInFileManager;  // replaces Lang::showInExplorer ('&' marks the access key)
        std::wstring_view confirmDeleteTitle;
        std::wstring_view confirmDelete;  // "{0}" = name, "{1}" = size
        std::wstring_view deleteFailed;   // "{0}" = name
        std::wstring_view deleteHelp;     // Settings' list of shortcuts
        std::wstring_view unreadableText;
        std::wstring_view credits;
        std::wstring_view windowsColors;  // the "Windows Colors" scheme
        std::wstring_view mapName;        // the map, to screen readers
    };

    Wording const& WordingFor(Lang const& lang) noexcept;
}
