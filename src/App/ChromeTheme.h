// SpaceWindow - colours of the window's controls.
//
// New in SpaceWindow.  WinUI's controls take their colours from a few dozen
// shared brushes in the theme dictionaries of XamlControlsResources
// ("TextFillColorPrimaryBrush", "AccentFillColorDefaultBrush", ...), which
// they mostly know by names of their own ("ButtonForeground", ...).  A theme
// puts brushes of its colours into the application's resources under all
// those names (ChromeThemeKeys.h lists them): WinUI looks there first - the
// "lightweight styling" of the WinUI documentation, for every control at
// once.  The controls' translucent fills (hover, pressed) stay as they are:
// they suit any background.  The classic themes take the brushes out again.
//
// WinUI's own brushes cannot simply be recoloured: it hands out copies, and
// locks the one it draws plain text with.
#pragma once

#include "../Core/Theme.h"

namespace sm::ui
{
    class ChromeTheme
    {
    public:
        // Puts the controls' colours for `theme` into the application's
        // resources.  Returns true if they changed: WinUI looks resources up
        // again only when an element's theme changes, so elements already
        // shown need their RequestedTheme switched away and back.
        bool Apply(sm::Theme const& theme);

    private:
        std::vector<winrt::hstring> names_;  // the resources Apply added
    };
}
