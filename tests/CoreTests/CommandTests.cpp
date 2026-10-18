// Tests for the window's commands and the keys that run them.
#include "pch.h"
#include "TestHarness.h"

#ifndef _WIN32
#include <fstream>
#endif

#include "../../src/Core/Commands.h"
#include "../../src/Core/Lang.h"

namespace
{
    sm::Lang const& Us()
    {
        return sm::FindLanguage(L"us");
    }
    sm::Lang const& Fr()
    {
        return sm::FindLanguage(L"fr");
    }

    // A file of the repository, found from the test program:
    // bin\<platform>\<configuration>\CoreTests\CoreTests.exe (CMake says where it is).
    std::filesystem::path RepositoryFile(std::wstring_view name)
    {
#ifdef SPACEWINDOW_SOURCE_DIR
        return std::filesystem::path(SPACEWINDOW_SOURCE_DIR) / sm::posix::ToNative(name);
#else
        std::wstring program(MAX_PATH, L'\0');
        DWORD length = 0;
        while ((length = GetModuleFileNameW(nullptr, program.data(), static_cast<DWORD>(program.size()))) ==
               program.size())
        {
            program.resize(program.size() * 2);
        }
        program.resize(length);
        std::filesystem::path root = std::filesystem::path(program).parent_path();
        for (int level = 0; level < 4; ++level)
        {
            root = root.parent_path();
        }
        return root / name;
#endif
    }

    std::wstring ReadUtf8(std::filesystem::path const& path)
    {
#ifndef _WIN32
        std::ifstream file(path, std::ios::binary);
        std::string const bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return sm::posix::DecodeUtf8(bytes).value_or(std::wstring());
#else
        std::string bytes;
        {
            wil::unique_hfile const file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            LARGE_INTEGER size{};
            if (!file || !GetFileSizeEx(file.get(), &size))
            {
                return {};
            }
            bytes.resize(static_cast<std::size_t>(size.QuadPart));
            DWORD read = 0;
            if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
            {
                return {};
            }
            bytes.resize(read);
        }
        int const length = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        std::wstring text(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), length);
        return text;
#endif
    }
}

TEST_CASE(ShortcutsReadAsSettingsListedThem)
{
    // The keys of every command, in the order Settings lists them, as the
    // language tables spelt them out before the command table made them.
    using sm::Command;
    std::vector<Command> const commands = sm::CommandsWithKeys();
    // clang-format off: laid out by hand
    std::vector<std::wstring_view> const us = { L"← / ↑ / → / ↓", L"Enter",    L"Alt+↑ / Backspace", L"Alt+↓",
                                                L"Alt+←",           L"Alt+→",    L"Home",              L"Esc",
                                                L"Ctrl+O",          L"F5",       L"Shift+F5",          L"Ctrl+P",
                                                L"Ctrl+Shift+P",    L"Del",      L"Alt+Enter",         L"Ctrl+Shift+C",
                                                L"Menu",            L"F1" };
    // clang-format on
    // clang-format off: laid out by hand
    std::vector<std::wstring_view> const fr = { L"← / ↑ / → / ↓", L"Entrée",   L"Alt+↑ / Retour arrière", L"Alt+↓",
                                                L"Alt+←",           L"Alt+→",    L"Origine",               L"Échap",
                                                L"Ctrl+O",          L"F5",       L"Maj+F5",                L"Ctrl+P",
                                                L"Ctrl+Maj+P",      L"Suppr",    L"Alt+Entrée",            L"Ctrl+Maj+C",
                                                L"Menu",            L"F1" };
    // clang-format on
    CHECK_EQ(commands.size(), us.size());
    for (std::size_t i = 0; i < commands.size(); ++i)
    {
        CHECK_EQ(sm::CommandKeys(commands[i], Us()), std::wstring(us[i]));
        CHECK_EQ(sm::CommandKeys(commands[i], Fr()), std::wstring(fr[i]));
    }
    CHECK(commands.front() == Command::SelectNearest && commands.back() == Command::About);
    CHECK(sm::CommandKeys(Command::Settings, Us()).empty());  // the toolbar's, without keys
    CHECK(sm::ShortcutAction(Command::Settings, Us()).empty());
}

TEST_CASE(KeysRunTheirCommands)
{
    using sm::Command;
    auto const run = [](std::uint16_t key, std::uint8_t modifiers, bool map) {
        return sm::CommandForKey(key, modifiers, map);
    };
    CHECK(run('O', sm::kCtrl, false) == Command::Open);
    CHECK(run(VK_F5, 0, false) == Command::Reload);
    CHECK(run(VK_F5, sm::kShift, false) == Command::FullRescan);
    CHECK(run('P', sm::kCtrl, false) == Command::PieChart);
    CHECK(run('P', sm::kCtrl | sm::kShift, false) == Command::InvertedPie);
    CHECK(run(VK_UP, sm::kAlt, false) == Command::ZoomOut);
    CHECK(run(VK_BACK, 0, false) == Command::ZoomOut);
    CHECK(run(VK_RETURN, sm::kAlt, false) == Command::Properties);
    // The map's own keys, only while it has the focus.
    CHECK(run(VK_UP, 0, true) == Command::SelectNearest);
    CHECK(run(VK_RETURN, 0, true) == Command::Activate);
    CHECK(run(VK_APPS, 0, true) == Command::ContextMenu);
    CHECK(!run(VK_UP, 0, false).has_value());
    CHECK(!run(VK_UP, sm::kAlt, true).has_value());  // Alt+arrows are the window's: the map lets them through
    CHECK(!run('O', 0, false).has_value());
    CHECK(!run('Q', sm::kCtrl, false).has_value());

    // No key runs two commands.
    std::set<std::tuple<std::uint16_t, std::uint8_t, bool>> keys;
    for (sm::KeyBinding const& binding : sm::KeyBindings())
    {
        CHECK(keys.emplace(binding.key, binding.modifiers, binding.map).second);
    }
}

TEST_CASE(ShortcutKeysAreSpelledOutPerLanguage)
{
    using Keys = std::vector<std::wstring>;
    auto const binding = [](sm::Command command) {
        return *std::ranges::find(sm::KeyBindings(), command, &sm::KeyBinding::command);
    };
    CHECK(sm::KeysOf(binding(sm::Command::InvertedPie), Us()) == (Keys{ L"Ctrl", L"Shift", L"P" }));
    CHECK(sm::KeysOf(binding(sm::Command::InvertedPie), Fr()) == (Keys{ L"Ctrl", L"Maj", L"P" }));
    CHECK(sm::KeysOf(binding(sm::Command::ZoomOut), Fr()) == (Keys{ L"Alt", L"↑" }));
    CHECK(sm::KeysOf(binding(sm::Command::About), Us()) == (Keys{ L"F1" }));
    CHECK(sm::KeysOf(binding(sm::Command::ClearSelection), Fr()) == (Keys{ L"Échap" }));
}

TEST_CASE(ReadmeListsTheShortcutsSettingsLists)
{
    // README.md's keyboard table lists the same keys as Settings, in the same
    // order (it may say more about each, and add rows of its own).
    std::wstring const readme = ReadUtf8(RepositoryFile(L"README.md"));
    CHECK(!readme.empty());
    std::size_t at = readme.find(L"\n| Keyboard | |");
    CHECK(at != std::wstring::npos);
    std::vector<std::wstring> listed;
    for (at = readme.find(L'\n', at + 1); at != std::wstring::npos && readme.compare(at + 1, 1, L"|") == 0;
         at = readme.find(L'\n', at + 1))
    {
        std::size_t const end = readme.find(L" |", at + 3);
        listed.emplace_back(readme.substr(at + 3, end - at - 3));  // "| keys | what they do |"
    }
    std::vector<std::wstring> expected;
    for (sm::Command const command : sm::CommandsWithKeys())
    {
        expected.push_back(sm::CommandKeys(command, Us()));
    }
    std::vector<std::wstring> keysListed;
    std::ranges::copy_if(listed, std::back_inserter(keysListed), [&expected](std::wstring const& keys) {
        return std::ranges::find(expected, keys) != expected.end();
    });
    for (std::wstring const& keys : expected)
    {
        if (std::ranges::find(keysListed, keys) == keysListed.end())
        {
            std::printf("      README.md does not list %s\n", test::ToUtf8(keys).c_str());
        }
    }
    CHECK(keysListed == expected);
}
