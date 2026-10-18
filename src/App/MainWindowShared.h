// SpaceWindow - main window: what its source files share.
//
// MainWindow's members are spread over several files, by what they do:
//   MainWindow.xaml.cpp     the window, its chrome, the toolbar and the status bar
//   MainWindowCommands.cpp  commands, keyboard shortcuts, the context menu, drag and drop
//   MainWindowDialogs.cpp   the drive dialog, Settings and About
//   MainWindowMap.cpp       the map: its layout, drawing, selection and zoom, the mouse and the keys
//   MainWindowScanning.cpp  scanning, live maps of a scan, reloads, the unreadable folders' list
//   MainWindowTips.cpp      the name tips and the info tips
#pragma once

#include "MainWindow.xaml.h"
#include "../Core/Text.h"

namespace winrt::SpaceWindow::implementation
{
    using namespace std::chrono_literals;
    using Windows::Foundation::IInspectable;
    using Windows::Foundation::Point;
    namespace mux = Microsoft::UI::Xaml;
    namespace muxc = Microsoft::UI::Xaml::Controls;
    namespace muxi = Microsoft::UI::Xaml::Input;
    namespace muxm = Microsoft::UI::Xaml::Media;
    namespace muw = Microsoft::UI::Windowing;

    inline constexpr std::chrono::milliseconds kZoomDuration = 280ms;
    inline constexpr std::chrono::milliseconds kProgressInterval = 100ms;
    inline constexpr std::chrono::milliseconds kQuietUpdate = 400ms;  // a quicker update shows no progress overlay
    inline constexpr unsigned kLiveMapTicks = 5;                      // progress ticks between two live maps of a scan
    inline constexpr std::size_t kLiveMapEntries = 30'000;            // more than a map can show anyway
    inline constexpr double kDefaultWidth = 1100.0;                   // DIPs
    inline constexpr double kDefaultHeight = 740.0;
    inline constexpr double kMinimumWidth = 520.0;
    inline constexpr double kMinimumHeight = 400.0;
    inline constexpr wchar_t kVersion[] = L"1.1.0";
    inline constexpr wchar_t kSeparator[] = L"   ·   ";

    inline Windows::UI::Color ToColor(sm::Rgb color) noexcept
    {
        return { 0xFF, color.r, color.g, color.b };
    }

    inline D2D1_COLOR_F ToD2D(Windows::UI::Color color) noexcept
    {
        return { color.R / 255.0f, color.G / 255.0f, color.B / 255.0f, color.A / 255.0f };
    }

    inline muxm::ImageSource ToImageSource(sm::shell::IconPixels const& icon)
    {
        if (icon.bgra.empty())
        {
            return nullptr;
        }
        muxm::Imaging::WriteableBitmap bitmap(icon.width, icon.height);
        auto const buffer = bitmap.PixelBuffer();
        std::memcpy(buffer.data(), icon.bgra.data(), std::min<std::size_t>(buffer.Capacity(), icon.bgra.size()));
        bitmap.Invalidate();
        return bitmap;
    }

    // A command's caption in a language, '&' before its access key as in the
    // original menus, and its label on the toolbar (MainWindowCommands.cpp).
    struct CommandText
    {
        std::wstring_view caption;
        std::wstring label;
    };
    CommandText TextOf(sm::Command command, sm::Lang const& lang);

    // The command table's modifier keys as WinUI's.
    Windows::System::VirtualKeyModifiers ToVirtualKeyModifiers(std::uint8_t modifiers) noexcept;

    inline std::wstring JoinLines(std::vector<std::wstring> const& lines)
    {
        std::wstring text;
        for (std::wstring const& line : lines)
        {
            if (!text.empty())
            {
                text.push_back(L'\n');
            }
            text.append(line);
        }
        return text;
    }
}
