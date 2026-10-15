// SpaceWindow - application object.
//
// Replaces CSpaceMonger::InitInstance() of SpaceMonger 1.4.0
// (Copyright (c) 1998-2020 Sean Werkema, MIT License).
#include "pch.h"
#include "App.xaml.h"

#include "MainWindow.xaml.h"
#include "../Core/FolderTree.h"

namespace winrt::SpaceWindow::implementation
{
    App::App()
    {
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, Microsoft::UI::Xaml::UnhandledExceptionEventArgs const& e) {
            if (IsDebuggerPresent())
            {
                auto const message = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched([[maybe_unused]] Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args)
    {
        // When the user runs SpaceWindow elevated, let the scanner list every
        // folder (a no-op otherwise).
        sm::TryEnableBackupPrivilege();

        // "SpaceWindow.exe <path>" opens and scans a drive or folder, so the app
        // can be used from "Open with" and the command line.
        std::wstring initialPath;
        int argc = 0;
        wil::unique_hlocal_ptr<PWSTR[]> const argv(CommandLineToArgvW(GetCommandLineW(), &argc));
        if (argv && argc > 1)
        {
            initialPath = argv[1];
        }

        window_ = make<MainWindow>();
        window_.Activate();
        if (!initialPath.empty())
        {
            get_self<implementation::MainWindow>(window_)->StartScan(std::move(initialPath));
        }
    }
}
