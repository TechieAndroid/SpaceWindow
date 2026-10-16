// SpaceWindow - the window's commands and the keys that run them.
//
// New in SpaceWindow (1.4.0 had no keyboard support beyond its menus' access
// keys).  One table, KeyBindings(), says which keys run which command: the
// window makes its keyboard accelerators from it, the map takes its own keys
// from it, the toolbar shows them, and Settings lists them - each in the
// window's language.  README.md lists them too, and a unit test checks that
// it lists the same.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sm
{
    struct Lang;

    enum class Command : std::uint8_t
    {
        Open,
        Reload,
        FullRescan,
        RescanFolder,
        ZoomFull,
        ZoomIn,
        ZoomOut,
        GoDown,
        GoBack,
        GoForward,
        FreeSpace,
        PieChart,
        InvertedPie,
        Run,
        Delete,
        Settings,
        About,
        Properties,
        ShowInExplorer,
        CopyPath,
        // The map's own, while it has the keyboard focus.
        SelectNearest,  // an arrow key: the nearest box or slice that way
        Activate,       // zoom into the selected folder, or run the selected file
        ClearSelection,
        ContextMenu,
    };

    // Modifier keys held with a key, combined.
    enum KeyModifier : std::uint8_t
    {
        kCtrl = 1,
        kShift = 2,
        kAlt = 4,
    };

    struct KeyBinding
    {
        Command command{};
        std::uint16_t key = 0;       // a virtual-key code, VK_* (WinRT's VirtualKey has the same values)
        std::uint8_t modifiers = 0;  // KeyModifier
        bool map = false;            // only while the map has the focus; the others work anywhere in the window
    };

    // Every key binding, in the order Settings lists them.
    std::span<KeyBinding const> KeyBindings() noexcept;

    // The command `key` runs with `modifiers` held: in the map (`map`), or
    // anywhere in the window.
    std::optional<Command> CommandForKey(std::uint16_t key, std::uint8_t modifiers, bool map) noexcept;

    // The commands that have keys, in the order Settings lists them.
    std::vector<Command> CommandsWithKeys();

    // The keys of a binding as they are labelled in the language, modifiers
    // first: { L"Ctrl", L"Maj", L"P" }.
    std::vector<std::wstring> KeysOf(KeyBinding const& binding, Lang const& lang);

    // Every binding of a command, as Settings and the toolbar show them:
    // L"Alt+↑ / Backspace".  Empty for a command without keys.
    std::wstring CommandKeys(Command command, Lang const& lang);

    // What a command with keys does, as Settings lists it.  Empty for the others.
    std::wstring_view ShortcutAction(Command command, Lang const& lang) noexcept;
}
