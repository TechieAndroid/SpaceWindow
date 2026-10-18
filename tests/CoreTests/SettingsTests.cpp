// Tests for settings validation, persistence and migration from SpaceMonger 1.x.
#include "pch.h"
#include "TestHarness.h"

#ifndef _WIN32
#include <fstream>
#endif

#include "../../src/Core/Settings.h"

namespace
{
#ifdef _WIN32
    // A scratch registry key under HKCU\Software\SpaceWindowTests, deleted afterwards (not under
    // the app's own key: an empty one left behind would stop the 1.x import).
    class ScratchKey
    {
    public:
        ScratchKey()
        {
            GUID guid{};
            (void)CoCreateGuid(&guid);
            wchar_t text[40]{};
            (void)StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
            path_ = std::wstring(L"Software\\SpaceWindowTests\\") + text;
        }
        ScratchKey(ScratchKey const&) = delete;
        ScratchKey& operator=(ScratchKey const&) = delete;
        ~ScratchKey()
        {
            (void)RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
            // Leave no trace: remove the parent too once it is empty (this fails,
            // harmlessly, while another test's key is still there).
            (void)RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\SpaceWindowTests");
        }

        std::wstring const& Path() const noexcept { return path_; }
        std::wstring Sub(std::wstring_view name) const { return path_ + L"\\" + std::wstring(name); }

    private:
        std::wstring path_;
    };

    void WriteDword(std::wstring const& key, wchar_t const* name, DWORD value)
    {
        CHECK(RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS);
    }

    void WriteString(std::wstring const& key, char const* name, char const* value)
    {
        // 1.x was an ANSI program; RegSetValueExA stores REG_SZ as UTF-16 anyway.
        CHECK(RegSetKeyValueA(HKEY_CURRENT_USER, test::ToUtf8(key).c_str(), name, REG_SZ, value,
                              static_cast<DWORD>(std::strlen(value) + 1)) == ERROR_SUCCESS);
    }
#else
    using DWORD = std::uint32_t;
    constexpr int SW_SHOWMAXIMIZED = 3;

    // Scratch settings files in a folder of their own in $TMPDIR, deleted afterwards.
    class ScratchKey
    {
    public:
        ScratchKey()
        {
            static std::atomic<unsigned> serial = 0;
            folder_ = std::filesystem::temp_directory_path() /
                      ("SpaceWindowSettings-" + std::to_string(getpid()) + "-" + std::to_string(serial++));
            CHECK(std::filesystem::create_directory(folder_));
        }
        ScratchKey(ScratchKey const&) = delete;
        ScratchKey& operator=(ScratchKey const&) = delete;
        ~ScratchKey()
        {
            std::error_code error;
            std::filesystem::remove_all(folder_, error);
        }

        std::wstring Path() const { return Sub(L"settings.ini"); }
        std::wstring Sub(std::wstring_view name) const
        {
            return sm::posix::FromNative((folder_ / sm::posix::ToNative(name)).string());
        }

    private:
        std::filesystem::path folder_;
    };

    void WriteLine(std::wstring const& key, std::string const& line)
    {
        std::ofstream file(sm::posix::ToNative(key), std::ios::binary | std::ios::app);
        CHECK(static_cast<bool>(file << line << '\n'));
    }

    void WriteDword(std::wstring const& key, wchar_t const* name, DWORD value)
    {
        WriteLine(key, sm::posix::ToNative(name) + " = " + std::to_string(value));
    }

    void WriteString(std::wstring const& key, char const* name, char const* value)
    {
        WriteLine(key, std::string(name) + " = " + value);
    }
#endif
}

TEST_CASE(ValidateClampsOutOfRangeValues)
{
    sm::Settings s;
    s.density = 3;     // 1.4.0 accepted 3, which matched no entry in its density list
    s.fileColor = 12;  // ... and colour 12, which read past the end of its palette
    s.folderColor = -1;
    s.bias = 100;
    s.nameTipDelay = -5;
    s.infoTipDelay = 1'000'000;
    s.infoTipFlags = 0xFFFF;
    s.language = L"FR";
    s.theme = std::wstring(sm::Settings::kMaxThemeId + 1, L'x');
    s.viewMode = static_cast<sm::ViewMode>(-1);
    s.backdrop = static_cast<sm::Backdrop>(sm::kBackdropCount);
    s.window = { INT_MIN, 0, 800, 600, false };
    s.Validate();

    CHECK_EQ(s.density, sm::Settings::kMaxDensity);
    CHECK_EQ(s.fileColor, 0);
    CHECK_EQ(s.folderColor, 0);
    CHECK_EQ(s.bias, sm::Settings::kMaxBias);
    CHECK_EQ(s.nameTipDelay, 0);
    CHECK_EQ(s.infoTipDelay, sm::Settings::kMaxDelay);
    CHECK_EQ(s.infoTipFlags, static_cast<std::uint32_t>(sm::TipAll));
    CHECK_EQ(s.language, std::wstring(L"fr"));
    CHECK_EQ(s.theme, std::wstring(L"system"));
    CHECK(s.viewMode == sm::ViewMode::Treemap);
    CHECK(s.backdrop == sm::Backdrop::None);
    CHECK(!s.window.IsValid());

    sm::Settings inverted;
    inverted.viewMode = sm::ViewMode::InvertedPie;
    inverted.backdrop = sm::Backdrop::Tunnel;
    inverted.Validate();
    CHECK(inverted.viewMode == sm::ViewMode::InvertedPie && inverted.backdrop == sm::Backdrop::Tunnel);

    sm::Settings defaults;
    defaults.Validate();
    CHECK(defaults == sm::Settings{});

    s.language = L"english";
    s.theme.clear();
    s.Validate();
    CHECK(s.language.empty());
    CHECK_EQ(s.theme, std::wstring(L"system"));

    // Any other id is kept: theme files come and go.
    s.theme = L"My Theme";
    s.Validate();
    CHECK_EQ(s.theme, std::wstring(L"My Theme"));
}

TEST_CASE(SettingsRoundTripThroughTheRegistry)
{
    ScratchKey const scratch;
    sm::Settings saved;
    saved.density = -2;
    saved.fileColor = 5;
    saved.folderColor = 11;
    saved.autoRescan = true;
    saved.animatedZoom = false;
    saved.disableDelete = true;
    saved.rolloverBox = true;
    saved.bias = -7;
    saved.savePosition = true;
    saved.window = { -1200, 40, 1024, 700, true };
    saved.showNameTips = false;
    saved.nameTipDelay = 400;
    saved.showInfoTips = false;
    saved.infoTipFlags = sm::TipPath | sm::TipAttrib;
    saved.infoTipDelay = 900;
    saved.language = L"uk";
    saved.theme = L"dracula";
    saved.viewMode = sm::ViewMode::InvertedPie;
    saved.backdrop = sm::Backdrop::Tunnel;

    CHECK(sm::SaveSettings(saved, scratch.Path()));
    sm::Settings const loaded = sm::LoadSettings(scratch.Path(), scratch.Sub(L"none"));
    CHECK(loaded == saved);
}

#ifdef _WIN32  // (SpaceWindow 1.0 ran on Windows only)
TEST_CASE(NumericThemeOfVersion1IsConverted)
{
    // SpaceWindow 1.0 stored 0, 1 or 2 for "Use system setting", "Light" and "Dark".
    ScratchKey const scratch;
    std::wstring const key = scratch.Sub(L"current");
    WriteDword(key, L"theme", 2);
    CHECK_EQ(sm::LoadSettings(key, scratch.Sub(L"none")).theme, std::wstring(L"dark"));
    WriteDword(key, L"theme", 1);
    CHECK_EQ(sm::LoadSettings(key, scratch.Sub(L"none")).theme, std::wstring(L"light"));
    WriteDword(key, L"theme", 0);
    CHECK_EQ(sm::LoadSettings(key, scratch.Sub(L"none")).theme, std::wstring(L"system"));
}
#endif

TEST_CASE(MissingKeyGivesDefaults)
{
    ScratchKey const scratch;
    CHECK(sm::LoadSettings(scratch.Sub(L"none"), scratch.Sub(L"none-either")) == sm::Settings{});
}

TEST_CASE(LegacySettingsAreImported)
{
    ScratchKey const scratch;
    std::wstring const legacy = scratch.Sub(L"legacy");
    WriteDword(legacy, L"density", static_cast<DWORD>(-1));
    WriteDword(legacy, L"folder_color", 3);
    WriteDword(legacy, L"bias", static_cast<DWORD>(-20));
    WriteDword(legacy, L"show_rollover_box", 1);
    WriteDword(legacy, L"infotip_flags", sm::TipName | sm::TipSize);
    WriteDword(legacy, L"window_rect.left", 100);
    WriteDword(legacy, L"window_rect.top", 50);
    WriteDword(legacy, L"window_rect.right", 900);
    WriteDword(legacy, L"window_rect.bottom", 650);
    WriteDword(legacy, L"window_showcmd", SW_SHOWMAXIMIZED);
    WriteString(legacy, "lang", "fr");

    sm::Settings const s = sm::LoadSettings(scratch.Sub(L"new"), legacy);
    CHECK_EQ(s.density, -1);
    CHECK_EQ(s.folderColor, 3);
    CHECK_EQ(s.bias, -20);
    CHECK(s.rolloverBox);
    CHECK_EQ(s.infoTipFlags, static_cast<std::uint32_t>(sm::TipName | sm::TipSize));
    CHECK_EQ(s.language, std::wstring(L"fr"));
    CHECK_EQ(s.window.x, 100);
    CHECK_EQ(s.window.y, 50);
    CHECK_EQ(s.window.width, 800);
    CHECK_EQ(s.window.height, 600);
    CHECK(s.window.maximized);
}
