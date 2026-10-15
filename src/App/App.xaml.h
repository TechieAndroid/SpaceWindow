// SpaceWindow - application object.
//
// Replaces CSpaceMonger::InitInstance() of SpaceMonger 1.4.0
// (Copyright (c) 1998-2020 Sean Werkema, MIT License).
#pragma once

#include "App.xaml.g.h"
#include "MainWindow.g.h"

namespace winrt::SpaceWindow::implementation
{
    struct App : AppT<App>
    {
        App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

    private:
        SpaceWindow::MainWindow window_{ nullptr };
    };
}
