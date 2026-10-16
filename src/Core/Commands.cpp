// SpaceWindow - the window's commands and the keys that run them.
#include "pch.h"
#include "Commands.h"

#include "Lang.h"

namespace sm
{
    namespace
    {
        constexpr KeyBinding kBindings[] = {
            { Command::SelectNearest, VK_LEFT, 0, true },
            { Command::SelectNearest, VK_UP, 0, true },
            { Command::SelectNearest, VK_RIGHT, 0, true },
            { Command::SelectNearest, VK_DOWN, 0, true },
            { Command::Activate, VK_RETURN, 0, true },
            { Command::ZoomOut, VK_UP, kAlt },  // as in File Explorer
            { Command::ZoomOut, VK_BACK },
            { Command::GoDown, VK_DOWN, kAlt },
            { Command::GoBack, VK_LEFT, kAlt },
            { Command::GoForward, VK_RIGHT, kAlt },
            { Command::ZoomFull, VK_HOME },
            { Command::ClearSelection, VK_ESCAPE, 0, true },
            { Command::Open, 'O', kCtrl },
            { Command::Reload, VK_F5 },
            { Command::FullRescan, VK_F5, kShift },
            { Command::PieChart, 'P', kCtrl },
            { Command::InvertedPie, 'P', kCtrl | kShift },
            { Command::Delete, VK_DELETE },
            { Command::Properties, VK_RETURN, kAlt },
            { Command::CopyPath, 'C', kCtrl | kShift },
            { Command::ContextMenu, VK_APPS, 0, true },
            { Command::About, VK_F1 },
        };

        std::wstring KeyName(std::uint16_t key, KeyNames const& names)
        {
            switch (key)
            {
            case VK_LEFT: return L"←";
            case VK_UP: return L"↑";
            case VK_RIGHT: return L"→";
            case VK_DOWN: return L"↓";
            case VK_RETURN: return std::wstring(names.enter);
            case VK_BACK: return std::wstring(names.backspace);
            case VK_HOME: return std::wstring(names.home);
            case VK_ESCAPE: return std::wstring(names.escape);
            case VK_DELETE: return std::wstring(names.del);
            case VK_APPS: return std::wstring(names.menu);
            default: break;
            }
            if (key >= VK_F1 && key <= VK_F24)
            {
                return L"F" + std::to_wstring(key - VK_F1 + 1);
            }
            if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
            {
                wchar_t const letter[] = { static_cast<wchar_t>(key), L'\0' };
                return letter;
            }
            return L"?";
        }
    }

    std::span<KeyBinding const> KeyBindings() noexcept
    {
        return kBindings;
    }

    std::optional<Command> CommandForKey(std::uint16_t key, std::uint8_t modifiers, bool map) noexcept
    {
        for (KeyBinding const& binding : kBindings)
        {
            if (binding.key == key && binding.modifiers == modifiers && binding.map == map)
            {
                return binding.command;
            }
        }
        return std::nullopt;
    }

    std::vector<Command> CommandsWithKeys()
    {
        std::vector<Command> commands;
        for (KeyBinding const& binding : kBindings)
        {
            if (std::ranges::find(commands, binding.command) == commands.end())
            {
                commands.push_back(binding.command);
            }
        }
        return commands;
    }

    std::vector<std::wstring> KeysOf(KeyBinding const& binding, Lang const& lang)
    {
        KeyNames const& names = lang.keyNames;
        std::vector<std::wstring> keys;
        if ((binding.modifiers & kCtrl) != 0)
        {
            keys.emplace_back(names.ctrl);
        }
        if ((binding.modifiers & kShift) != 0)
        {
            keys.emplace_back(names.shift);
        }
        if ((binding.modifiers & kAlt) != 0)
        {
            keys.emplace_back(names.alt);
        }
        keys.push_back(KeyName(binding.key, names));
        return keys;
    }

    std::wstring CommandKeys(Command command, Lang const& lang)
    {
        std::wstring text;
        for (KeyBinding const& binding : kBindings)
        {
            if (binding.command != command)
            {
                continue;
            }
            if (!text.empty())
            {
                text.append(L" / ");
            }
            std::vector<std::wstring> const keys = KeysOf(binding, lang);
            for (std::size_t i = 0; i < keys.size(); ++i)
            {
                text.append(i == 0 ? L"" : L"+").append(keys[i]);
            }
        }
        return text;
    }

    std::wstring_view ShortcutAction(Command command, Lang const& lang) noexcept
    {
        ShortcutHelp const& help = lang.shortcutHelp;
        switch (command)
        {
        case Command::SelectNearest: return help.selectNearest;
        case Command::Activate: return help.activate;
        case Command::ZoomOut: return help.zoomOut;
        case Command::GoDown: return help.goDown;
        case Command::GoBack: return help.goBack;
        case Command::GoForward: return help.goForward;
        case Command::ZoomFull: return help.zoomFull;
        case Command::ClearSelection: return help.clearSelection;
        case Command::Open: return help.open;
        case Command::Reload: return help.reload;
        case Command::FullRescan: return help.fullRescan;
        case Command::PieChart: return help.pieChart;
        case Command::InvertedPie: return help.invertedPie;
        case Command::Delete: return help.del;
        case Command::Properties: return help.properties;
        case Command::CopyPath: return help.copyPath;
        case Command::ContextMenu: return help.contextMenu;
        case Command::About: return help.about;
        case Command::RescanFolder:
        case Command::ZoomIn:
        case Command::FreeSpace:
        case Command::Run:
        case Command::Settings:
        case Command::ShowInExplorer: break;
        }
        return {};
    }
}
