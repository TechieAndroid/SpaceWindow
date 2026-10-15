// SpaceWindow - user settings.
//
// Modernized from CCurrentSettings::Load/Save/Reset in SpaceMonger.cpp of
// SpaceMonger 1.4.0 (Copyright (c) 1998-2020 Sean Werkema, MIT License).
#include "pch.h"
#include "Settings.h"

#include "Palette.h"

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
        constexpr wchar_t kWindowX[] = L"window_x";
        constexpr wchar_t kWindowY[] = L"window_y";
        constexpr wchar_t kWindowWidth[] = L"window_width";
        constexpr wchar_t kWindowHeight[] = L"window_height";
        constexpr wchar_t kWindowMaximized[] = L"window_maximized";

        class Key
        {
        public:
            static Key Open(std::wstring_view path)
            {
                Key key;
                std::wstring const name(path);
                if (RegOpenKeyExW(HKEY_CURRENT_USER, name.c_str(), 0, KEY_QUERY_VALUE, key.handle_.put()) != ERROR_SUCCESS)
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
                if (RegGetValueW(handle_.get(), nullptr, name, RRF_RT_REG_DWORD, nullptr, &data, &size) == ERROR_SUCCESS)
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
        if (viewMode != ViewMode::Pie)
        {
            viewMode = ViewMode::Treemap;
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

    bool SaveSettings(Settings const& s, std::wstring_view key)
    {
        wil::unique_hkey handle;
        std::wstring const name(key);
        if (RegCreateKeyExW(HKEY_CURRENT_USER, name.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                            nullptr, handle.put(), nullptr) != ERROR_SUCCESS)
        {
            return false;
        }

        HKEY const k = handle.get();
        bool ok = true;
        ok &= Write(k, kDensity, s.density);
        ok &= Write(k, kFileColor, s.fileColor);
        ok &= Write(k, kFolderColor, s.folderColor);
        ok &= Write(k, kAutoRescan, s.autoRescan);
        ok &= Write(k, kAnimatedZoom, s.animatedZoom);
        ok &= Write(k, kDisableDelete, s.disableDelete);
        ok &= Write(k, kSavePos, s.savePosition);
        ok &= Write(k, kBias, s.bias);
        ok &= Write(k, kShowNameTips, s.showNameTips);
        ok &= Write(k, kRolloverBox, s.rolloverBox);
        ok &= Write(k, kShowInfoTips, s.showInfoTips);
        ok &= Write(k, kNameTipDelay, s.nameTipDelay);
        ok &= Write(k, kInfoTipFlags, static_cast<DWORD>(s.infoTipFlags));
        ok &= Write(k, kInfoTipDelay, s.infoTipDelay);
        ok &= Write(k, kLang, s.language);
        ok &= Write(k, kTheme, s.theme);
        ok &= Write(k, kViewMode, static_cast<int>(s.viewMode));
        ok &= Write(k, kWindowX, s.window.x);
        ok &= Write(k, kWindowY, s.window.y);
        ok &= Write(k, kWindowWidth, s.window.width);
        ok &= Write(k, kWindowHeight, s.window.height);
        ok &= Write(k, kWindowMaximized, s.window.maximized);
        return ok;
    }
}
