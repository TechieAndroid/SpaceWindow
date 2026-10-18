// SpaceWindow - user settings.
//
// Modernized from CCurrentSettings::Load/Save/Reset in SpaceMonger.cpp of
// SpaceMonger 1.4.0 (Copyright (c) 1998-2020 Sean Werkema, MIT License).
#include "pch.h"
#include "Settings.h"

#include "Palette.h"
#ifndef _WIN32
#include "Posix.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#endif

namespace sm
{
    namespace
    {
        // Value names shared with the 1.x registry layout.
        constexpr wchar_t kDensity[] = L"density";
        constexpr wchar_t kFileColor[] = L"file_color";
        constexpr wchar_t kFolderColor[] = L"folder_color";
        constexpr wchar_t kAutoRescan[] = L"auto_rescan";
        constexpr wchar_t kAnimatedZoom[] = L"animated_zoom";
        constexpr wchar_t kDisableDelete[] = L"disable_delete";
        constexpr wchar_t kSavePos[] = L"save_pos";
        constexpr wchar_t kBias[] = L"bias";
        constexpr wchar_t kShowNameTips[] = L"show_name_tips";
        constexpr wchar_t kRolloverBox[] = L"show_rollover_box";
        constexpr wchar_t kShowInfoTips[] = L"show_info_tips";
        constexpr wchar_t kNameTipDelay[] = L"nametip_delay";
        constexpr wchar_t kInfoTipFlags[] = L"infotip_flags";
        constexpr wchar_t kInfoTipDelay[] = L"infotip_delay";
        constexpr wchar_t kLang[] = L"lang";

        // Values new in SpaceWindow.
        constexpr wchar_t kTheme[] = L"theme";
        constexpr wchar_t kViewMode[] = L"view_mode";
        constexpr wchar_t kBackdrop[] = L"background_animation";
        constexpr wchar_t kWindowX[] = L"window_x";
        constexpr wchar_t kWindowY[] = L"window_y";
        constexpr wchar_t kWindowWidth[] = L"window_width";
        constexpr wchar_t kWindowHeight[] = L"window_height";
        constexpr wchar_t kWindowMaximized[] = L"window_maximized";

#ifdef _WIN32
        class Key
        {
        public:
            static Key Open(std::wstring_view path)
            {
                Key key;
                std::wstring const name(path);
                if (RegOpenKeyExW(HKEY_CURRENT_USER, name.c_str(), 0, KEY_QUERY_VALUE, key.handle_.put()) !=
                    ERROR_SUCCESS)
                {
                    key.handle_.reset();
                }
                return key;
            }

            explicit operator bool() const noexcept { return static_cast<bool>(handle_); }

            // DWORDs carry signed values too (bias, density): 1.x wrote its ints as REG_DWORD.
            void Read(wchar_t const* name, int& value) const noexcept
            {
                DWORD data = 0;
                DWORD size = sizeof(data);
                if (RegGetValueW(handle_.get(), nullptr, name, RRF_RT_REG_DWORD, nullptr, &data, &size) ==
                    ERROR_SUCCESS)
                {
                    value = static_cast<int>(data);
                }
            }

            void Read(wchar_t const* name, bool& value) const noexcept
            {
                int number = value ? 1 : 0;
                Read(name, number);
                value = number != 0;
            }

            void Read(wchar_t const* name, std::uint32_t& value) const noexcept
            {
                int number = static_cast<int>(value);
                Read(name, number);
                value = static_cast<std::uint32_t>(number);
            }

            void Read(wchar_t const* name, std::wstring& value) const
            {
                wchar_t buffer[MAX_PATH]{};
                DWORD size = sizeof(buffer);
                if (RegGetValueW(handle_.get(), nullptr, name, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS)
                {
                    value.assign(buffer);
                }
            }

        private:
            wil::unique_hkey handle_;
        };
#else
        // ShowWindow's and CreateWindow's values, as SpaceMonger 1.x stored them.
        constexpr int SW_SHOWNORMAL = 1;
        constexpr int SW_SHOWMAXIMIZED = 3;
        constexpr int CW_USEDEFAULT = INT_MIN;

        // Where a settings file is: a relative path is below $XDG_CONFIG_HOME.
        std::filesystem::path SettingsFile(std::wstring_view key)
        {
            if (key.empty())
            {
                return {};
            }
            std::filesystem::path path(posix::ToNative(key));
            if (path.is_absolute())
            {
                return path;
            }
            if (char const* const config = std::getenv("XDG_CONFIG_HOME"); config != nullptr && config[0] == '/')
            {
                return std::filesystem::path(config) / path;
            }
            if (char const* const home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
            {
                return std::filesystem::path(home) / ".config" / path;
            }
            return {};
        }

        std::wstring_view TrimSpaces(std::wstring_view text) noexcept
        {
            std::size_t const first = text.find_first_not_of(L" \t\r");
            if (first == std::wstring_view::npos)
            {
                return {};
            }
            return text.substr(first, text.find_last_not_of(L" \t\r") - first + 1);
        }

        // The settings file, read: "name = value" lines; '#' and ';' start comment lines.
        class Key
        {
        public:
            static Key Open(std::wstring_view path)
            {
                Key key;
                std::filesystem::path const file = SettingsFile(path);
                std::ifstream stream;
                if (!file.empty())
                {
                    stream.open(file, std::ios::binary);
                }
                if (!stream)
                {
                    return key;
                }
                key.open_ = true;
                std::string line;
                while (std::getline(stream, line))
                {
                    std::wstring const text = posix::FromNative(line);
                    std::wstring_view const trimmed = TrimSpaces(text);
                    std::size_t const equals = trimmed.find(L'=');
                    if (trimmed.empty() || trimmed.front() == L'#' || trimmed.front() == L';' ||
                        equals == std::wstring_view::npos)
                    {
                        continue;
                    }
                    key.values_[std::wstring(TrimSpaces(trimmed.substr(0, equals)))] =
                        std::wstring(TrimSpaces(trimmed.substr(equals + 1)));
                }
                return key;
            }

            explicit operator bool() const noexcept { return open_; }

            // Numbers as the registry's DWORDs held them: signed, or up to 2^32 - 1.
            void Read(wchar_t const* name, int& value) const noexcept
            {
                auto const found = values_.find(name);
                if (found == values_.end() || found->second.empty())
                {
                    return;
                }
                wchar_t* end = nullptr;
                errno = 0;
                long long const number = std::wcstoll(found->second.c_str(), &end, 10);
                if (errno == 0 && end != nullptr && *end == L'\0' && std::cmp_greater_equal(number, INT_MIN) &&
                    std::cmp_less_equal(number, UINT32_MAX))
                {
                    value = static_cast<int>(static_cast<std::uint32_t>(number));
                }
            }

            void Read(wchar_t const* name, bool& value) const noexcept
            {
                int number = value ? 1 : 0;
                Read(name, number);
                value = number != 0;
            }

            void Read(wchar_t const* name, std::uint32_t& value) const noexcept
            {
                int number = static_cast<int>(value);
                Read(name, number);
                value = static_cast<std::uint32_t>(number);
            }

            void Read(wchar_t const* name, std::wstring& value) const
            {
                if (auto const found = values_.find(name); found != values_.end())
                {
                    value = found->second;
                }
            }

        private:
            bool open_ = false;
            std::map<std::wstring, std::wstring, std::less<>> values_;
        };
#endif

        void ReadCommon(Key const& key, Settings& s)
        {
            key.Read(kDensity, s.density);
            key.Read(kFileColor, s.fileColor);
            key.Read(kFolderColor, s.folderColor);
            key.Read(kAutoRescan, s.autoRescan);
            key.Read(kAnimatedZoom, s.animatedZoom);
            key.Read(kDisableDelete, s.disableDelete);
            key.Read(kSavePos, s.savePosition);
            key.Read(kBias, s.bias);
            key.Read(kShowNameTips, s.showNameTips);
            key.Read(kRolloverBox, s.rolloverBox);
            key.Read(kShowInfoTips, s.showInfoTips);
            key.Read(kNameTipDelay, s.nameTipDelay);
            key.Read(kInfoTipFlags, s.infoTipFlags);
            key.Read(kInfoTipDelay, s.infoTipDelay);
            key.Read(kLang, s.language);
        }

        void ReadLegacyPlacement(Key const& key, Settings& s)
        {
            int left = 0;
            int top = 0;
            int right = 0;
            int bottom = 0;
            int showCommand = SW_SHOWNORMAL;
            key.Read(L"window_rect.left", left);
            key.Read(L"window_rect.top", top);
            key.Read(L"window_rect.right", right);
            key.Read(L"window_rect.bottom", bottom);
            key.Read(L"window_showcmd", showCommand);
            // 1.x stored CW_USEDEFAULT (0x80000000) when nothing was saved.
            if (left != CW_USEDEFAULT && top != CW_USEDEFAULT && right > left && bottom > top)
            {
                s.window = { left, top, right - left, bottom - top, showCommand == SW_SHOWMAXIMIZED };
            }
        }

#ifdef _WIN32
        bool Write(HKEY key, wchar_t const* name, DWORD value) noexcept
        {
            return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<BYTE const*>(&value), sizeof(value)) ==
                   ERROR_SUCCESS;
        }

        bool Write(HKEY key, wchar_t const* name, int value) noexcept
        {
            return Write(key, name, static_cast<DWORD>(value));
        }

        bool Write(HKEY key, wchar_t const* name, bool value) noexcept
        {
            return Write(key, name, static_cast<DWORD>(value ? 1 : 0));
        }

        bool Write(HKEY key, wchar_t const* name, std::wstring const& value) noexcept
        {
            auto const bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
            return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<BYTE const*>(value.c_str()), bytes) ==
                   ERROR_SUCCESS;
        }

        // Where SaveSettings writes: a registry key.
        struct Sink
        {
            HKEY key;
            bool Write(wchar_t const* name, int value) const noexcept { return sm::Write(key, name, value); }
            bool Write(wchar_t const* name, bool value) const noexcept { return sm::Write(key, name, value); }
            bool Write(wchar_t const* name, std::uint32_t value) const noexcept
            {
                return sm::Write(key, name, static_cast<DWORD>(value));
            }
            bool Write(wchar_t const* name, std::wstring const& value) const noexcept
            {
                return sm::Write(key, name, value);
            }
        };
#else
        // Where SaveSettings writes: the lines of the settings file.
        struct Sink
        {
            std::string text = "# SpaceWindow settings\n";

            bool Line(wchar_t const* name, std::wstring_view value)
            {
                std::wstring line(name);
                line.append(L" = ").append(value);
                std::ranges::replace(line, L'\n', L' ');
                std::ranges::replace(line, L'\r', L' ');
                text.append(posix::ToNative(line)).push_back('\n');
                return true;
            }
            bool Write(wchar_t const* name, int value) { return Line(name, std::to_wstring(value)); }
            bool Write(wchar_t const* name, bool value) { return Line(name, value ? L"1" : L"0"); }
            bool Write(wchar_t const* name, std::uint32_t value) { return Line(name, std::to_wstring(value)); }
            bool Write(wchar_t const* name, std::wstring const& value) { return Line(name, value); }
        };
#endif

        template <typename Out>
        bool WriteAll(Out& out, Settings const& s)
        {
            bool ok = true;
            ok &= out.Write(kDensity, s.density);
            ok &= out.Write(kFileColor, s.fileColor);
            ok &= out.Write(kFolderColor, s.folderColor);
            ok &= out.Write(kAutoRescan, s.autoRescan);
            ok &= out.Write(kAnimatedZoom, s.animatedZoom);
            ok &= out.Write(kDisableDelete, s.disableDelete);
            ok &= out.Write(kSavePos, s.savePosition);
            ok &= out.Write(kBias, s.bias);
            ok &= out.Write(kShowNameTips, s.showNameTips);
            ok &= out.Write(kRolloverBox, s.rolloverBox);
            ok &= out.Write(kShowInfoTips, s.showInfoTips);
            ok &= out.Write(kNameTipDelay, s.nameTipDelay);
            ok &= out.Write(kInfoTipFlags, s.infoTipFlags);
            ok &= out.Write(kInfoTipDelay, s.infoTipDelay);
            ok &= out.Write(kLang, s.language);
            ok &= out.Write(kTheme, s.theme);
            ok &= out.Write(kViewMode, static_cast<int>(s.viewMode));
            ok &= out.Write(kBackdrop, static_cast<int>(s.backdrop));
            ok &= out.Write(kWindowX, s.window.x);
            ok &= out.Write(kWindowY, s.window.y);
            ok &= out.Write(kWindowWidth, s.window.width);
            ok &= out.Write(kWindowHeight, s.window.height);
            ok &= out.Write(kWindowMaximized, s.window.maximized);
            return ok;
        }
    }

    void Settings::Validate()
    {
        density = std::clamp(density, kMinDensity, kMaxDensity);
        if (fileColor < 0 || fileColor >= kSchemeCount)
        {
            fileColor = 0;
        }
        if (folderColor < 0 || folderColor >= kSchemeCount)
        {
            folderColor = 0;
        }
        bias = std::clamp(bias, kMinBias, kMaxBias);
        nameTipDelay = std::clamp(nameTipDelay, 0, kMaxDelay);
        infoTipDelay = std::clamp(infoTipDelay, 0, kMaxDelay);
        infoTipFlags &= TipAll;

        // Whether a theme of that name exists is up to the UI (theme files come and go).
        if (theme.empty() || theme.size() > kMaxThemeId)
        {
            theme = L"system";
        }
        if (viewMode != ViewMode::Pie && viewMode != ViewMode::InvertedPie)
        {
            viewMode = ViewMode::Treemap;
        }
        if (int const kind = static_cast<int>(backdrop); kind < 0 || kind >= kBackdropCount)
        {
            backdrop = Backdrop::None;
        }

        // Language codes are two lower-case letters; anything else means "follow Windows".
        for (wchar_t& c : language)
        {
            if (c >= L'A' && c <= L'Z')
            {
                c = static_cast<wchar_t>(c - L'A' + L'a');
            }
        }
        if (language.size() != 2)
        {
            language.clear();
        }

        // Window geometry is checked against the monitors by the UI; here only
        // reject sizes no monitor could show.
        // (No std::abs: a corrupt value of INT_MIN - CW_USEDEFAULT - would overflow it.)
        constexpr int kMaxCoordinate = 1 << 16;
        auto const inRange = [](int value, int low, int high) noexcept { return value >= low && value <= high; };
        if (!inRange(window.width, 1, kMaxCoordinate) || !inRange(window.height, 1, kMaxCoordinate) ||
            !inRange(window.x, -kMaxCoordinate, kMaxCoordinate) || !inRange(window.y, -kMaxCoordinate, kMaxCoordinate))
        {
            window = {};
        }
    }

    Settings LoadSettings(std::wstring_view key, std::wstring_view legacyKey)
    {
        Settings settings;
        if (Key const current = Key::Open(key))
        {
            ReadCommon(current, settings);
            std::wstring theme;
            current.Read(kTheme, theme);
            if (theme.empty())
            {
                // SpaceWindow 1.0 stored the theme as a number.
                int number = 0;
                current.Read(kTheme, number);
                theme = number == 1 ? L"light" : number == 2 ? L"dark" : L"system";
            }
            settings.theme = std::move(theme);
            int viewMode = static_cast<int>(settings.viewMode);
            current.Read(kViewMode, viewMode);
            settings.viewMode = static_cast<ViewMode>(viewMode);
            int backdrop = static_cast<int>(settings.backdrop);
            current.Read(kBackdrop, backdrop);
            settings.backdrop = static_cast<Backdrop>(backdrop);
            current.Read(kWindowX, settings.window.x);
            current.Read(kWindowY, settings.window.y);
            current.Read(kWindowWidth, settings.window.width);
            current.Read(kWindowHeight, settings.window.height);
            current.Read(kWindowMaximized, settings.window.maximized);
        }
        else if (Key const legacy = Key::Open(legacyKey))
        {
            ReadCommon(legacy, settings);
            ReadLegacyPlacement(legacy, settings);
        }
        settings.Validate();
        return settings;
    }

#ifdef _WIN32
    bool SaveSettings(Settings const& s, std::wstring_view key)
    {
        wil::unique_hkey handle;
        std::wstring const name(key);
        if (RegCreateKeyExW(HKEY_CURRENT_USER, name.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                            nullptr, handle.put(), nullptr) != ERROR_SUCCESS)
        {
            return false;
        }

        Sink const sink{ handle.get() };
        return WriteAll(sink, s);
    }
#else
    bool SaveSettings(Settings const& s, std::wstring_view key)
    {
        std::filesystem::path const file = SettingsFile(key);
        if (file.empty())
        {
            return false;
        }
        Sink sink;
        WriteAll(sink, s);
        // Written whole beside it, then put in its place: a crash never leaves half a file.
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        std::filesystem::path temporary = file;
        temporary += ".new";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream << sink.text;
            if (!stream.flush())
            {
                return false;
            }
        }
        std::filesystem::rename(temporary, file, error);
        return !error;
    }
#endif
}
