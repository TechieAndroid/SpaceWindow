// SpaceWindow - main window: the drive dialog, Settings and About.
#include "pch.h"
#include "MainWindowShared.h"

#include "../Core/Format.h"

namespace winrt::SpaceWindow::implementation
{
    namespace
    {
        bool Checked(muxc::CheckBox const& box)
        {
            return unbox_value_or<bool>(box.IsChecked(), false);
        }

        int Delay(muxc::NumberBox const& box, int fallback)
        {
            double const value = box.Value();
            return std::isnan(value) ? fallback : static_cast<int>(std::lround(value));
        }

        // Takes an element out of its panel so a dialog can host it.
        mux::FrameworkElement Detach(mux::FrameworkElement const& element)
        {
            if (auto const panel = element.Parent().try_as<muxc::Panel>())
            {
                uint32_t index = 0;
                if (panel.Children().IndexOf(element, index))
                {
                    panel.Children().RemoveAt(index);
                }
            }
            return element;
        }
    }

    muxc::ContentDialog MainWindow::MakeDialog(std::wstring_view title)
    {
        // As in the WinUI Gallery: a desktop app must give its dialogs a XamlRoot.
        muxc::ContentDialog dialog;
        dialog.XamlRoot(RootGrid().XamlRoot());
        dialog.Style(
            mux::Application::Current().Resources().Lookup(box_value(L"DefaultContentDialogStyle")).as<mux::Style>());
        dialog.Title(box_value(hstring(title)));
        dialog.RequestedTheme(RootGrid().ActualTheme());
        return dialog;
    }

    muxc::ContentDialog MainWindow::HostDialog(muxc::ContentDialog& slot, mux::FrameworkElement const& content)
    {
        if (!slot)
        {
            slot = MakeDialog({});
            slot.Content(Detach(content));
        }
        slot.RequestedTheme(RootGrid().ActualTheme());
        return slot;
    }

    winrt::fire_and_forget MainWindow::OpenDriveOrFolder()
    {
        auto const strong = get_strong();
        dialogOpen_ = true;

        // Network and removable drives can be slow to answer - their icons
        // too - so ask off the UI thread.
        int const iconSize = static_cast<int>(std::lround(32 * DpiScale()));
        co_await winrt::resume_background();
        std::vector<sm::shell::DriveInfo> const drives = sm::shell::EnumerateDrives(iconSize);
        try
        {
            co_await wil::resume_foreground(dispatcher_);
        }
        catch (winrt::hresult_error const&)
        {
            co_return;  // the window has closed
        }

        sm::Lang const& lang = *lang_;
        FillDriveList(drives);
        muxc::ContentDialog dialog = HostDialog(driveDialog_, DriveContent());
        dialog.Title(box_value(hstring(lang.selectDrive)));
        dialog.PrimaryButtonText(hstring(lang.ok));
        dialog.CloseButtonText(hstring(lang.cancel));
        dialog.DefaultButton(muxc::ContentDialogButton::Primary);
        dialog.IsPrimaryButtonEnabled(DriveList().SelectedIndex() >= 0);
        BrowseButton().Content(box_value(hstring(lang.browseFolder)));
        driveAccepted_ = false;
        browseRequested_ = false;

        muxc::ContentDialogResult const result = co_await dialog.ShowAsync();
        std::wstring path;
        if ((result == muxc::ContentDialogResult::Primary || driveAccepted_) && DriveList().SelectedItem())
        {
            path = unbox_value<hstring>(DriveList().SelectedItem().as<muxc::ListViewItem>().Tag());
        }
        else if (browseRequested_)
        {
            try
            {
                // The Windows App SDK picker also works when the app runs elevated,
                // where the older Windows.Storage pickers fail.
                Microsoft::Windows::Storage::Pickers::FolderPicker picker(AppWindow().Id());
                auto const picked = co_await picker.PickSingleFolderAsync();
                if (picked)
                {
                    path = picked.Path();
                }
            }
            catch (winrt::hresult_error const&)
            {
                if (std::optional<std::wstring> const folder = sm::shell::PickFolder(WindowHandle()))
                {
                    path = *folder;
                }
            }
        }
        dialogOpen_ = false;
        if (!path.empty())
        {
            StartScan(std::move(path));
        }
    }

    void MainWindow::FillDriveList(std::vector<sm::shell::DriveInfo> const& drives)
    {
        sm::Lang const& lang = *lang_;
        auto const items = DriveList().Items();
        items.Clear();
        std::wstring const current = tree_ ? tree_->Volume().root : std::wstring();
        int select = drives.empty() ? -1 : 0;

        for (std::size_t i = 0; i < drives.size(); ++i)
        {
            sm::shell::DriveInfo const& drive = drives[i];
            muxc::Grid row;
            row.ColumnSpacing(12);
            row.Padding({ 0, 6, 0, 6 });
            muxc::ColumnDefinition iconColumn;
            iconColumn.Width(mux::GridLengthHelper::Auto());
            row.ColumnDefinitions().Append(iconColumn);
            row.ColumnDefinitions().Append(muxc::ColumnDefinition());

            muxc::Image icon;
            icon.Width(32);
            icon.Height(32);
            icon.VerticalAlignment(mux::VerticalAlignment::Center);
            icon.Source(ToImageSource(drive.icon));
            row.Children().Append(icon);

            muxc::StackPanel text;
            text.Spacing(4);
            text.VerticalAlignment(mux::VerticalAlignment::Center);
            muxc::Grid::SetColumn(text, 1);
            muxc::TextBlock name;
            name.Text(hstring(drive.displayName));
            text.Children().Append(name);
            if (drive.ready && drive.totalBytes != 0)
            {
                // A capacity bar like File Explorer's, red when the drive is nearly full.
                std::uint32_t const used = sm::PerMille(drive.totalBytes - drive.freeBytes, drive.totalBytes);
                muxc::ProgressBar bar;
                bar.Maximum(1000);
                bar.Value(used);
                bar.ShowError(used >= 900);
                text.Children().Append(bar);
                muxc::TextBlock capacity;
                capacity.Style(mux::Application::Current()
                                   .Resources()
                                   .Lookup(box_value(L"CaptionTextBlockStyle"))
                                   .as<mux::Style>());
                capacity.Text(hstring(sm::Join(sm::FormatSize(drive.freeBytes, lang), L" ", lang.free, L"  /  ",
                                               sm::FormatSize(drive.totalBytes, lang), L" ", lang.total)));
                text.Children().Append(capacity);
            }
            row.Children().Append(text);

            muxc::ListViewItem item;
            item.Content(row);
            item.Tag(box_value(hstring(drive.root)));
            mux::Automation::AutomationProperties::SetName(item, hstring(drive.displayName));
            items.Append(item);
            if (!current.empty() && sm::EqualsIgnoringCase(drive.root, current))
            {
                select = static_cast<int>(i);
            }
        }
        DriveList().SelectedIndex(select);
    }

    void MainWindow::OnDriveDoubleTapped(IInspectable const&, muxi::DoubleTappedRoutedEventArgs const&)
    {
        // As in 1.4.0: double-clicking a drive opens it.
        if (driveDialog_ && DriveList().SelectedIndex() >= 0)
        {
            driveAccepted_ = true;
            driveDialog_.Hide();
        }
    }

    void MainWindow::OnDriveSelectionChanged(IInspectable const&, muxc::SelectionChangedEventArgs const&)
    {
        if (driveDialog_)
        {
            driveDialog_.IsPrimaryButtonEnabled(DriveList().SelectedIndex() >= 0);
        }
    }

    void MainWindow::OnBrowseFolder(IInspectable const&, mux::RoutedEventArgs const&)
    {
        if (driveDialog_)
        {
            browseRequested_ = true;
            driveDialog_.Hide();
        }
    }

    winrt::fire_and_forget MainWindow::ShowSettings()
    {
        auto const strong = get_strong();
        dialogOpen_ = true;
        settingsOpen_ = true;
        sm::Lang const* const originalLanguage = lang_;
        LoadThemes();  // theme files may have come and gone
        muxc::ContentDialog dialog = HostDialog(settingsDialog_, SettingsContent());
        PopulateSettings();
        dialog.DefaultButton(muxc::ContentDialogButton::Primary);

        muxc::ContentDialogResult const result = co_await dialog.ShowAsync();
        dialogOpen_ = false;
        settingsOpen_ = false;
        if (result == muxc::ContentDialogResult::Primary)
        {
            ReadSettings();
            settings_.Validate();
            sm::SaveSettings(settings_);
            Relayout();
        }
        else
        {
            lang_ = originalLanguage;  // the language was only being previewed
        }
        backdropPreview_.reset();  // the animation picked, or back from a preview
        renderer_.InvalidateBase();
        RequestRender();
        ApplyTheme(settings_.theme);  // the chosen theme, or back from a preview
        if (themeErrors_.empty())
        {
            ThemeBar().IsOpen(false);  // the theme files have been put right
        }
        else
        {
            ThemeBar().Message(hstring(JoinLines(themeErrors_)));
        }
        ApplyLanguage();
        UpdateAll();
    }

    void MainWindow::PopulateSettings()
    {
        populatingSettings_ = true;
        auto const done = wil::scope_exit([this] { populatingSettings_ = false; });

        auto const languages = LanguageBox().Items();
        languages.Clear();
        int index = 0;
        for (sm::Lang const& lang : sm::Languages())
        {
            languages.Append(box_value(hstring(lang.name)));
            if (&lang == lang_)
            {
                LanguageBox().SelectedIndex(index);
            }
            ++index;
        }
        FillSettingsLists(settings_.theme, settings_.density - sm::Settings::kMinDensity, settings_.fileColor,
                          settings_.folderColor, static_cast<int>(settings_.backdrop));
        ApplySettingsText();
        ShowThemeErrors();
        std::vector<std::wstring> const folders = sm::shell::ThemeFolders();
        muxc::ToolTipService::SetToolTip(ThemesFolderButton(),
                                         folders.empty() ? nullptr : box_value(hstring(folders.front())));

        BiasSlider().Value(settings_.bias);
        NameTipsBox().IsChecked(settings_.showNameTips);
        NameTipDelayBox().Value(settings_.nameTipDelay);
        InfoTipsBox().IsChecked(settings_.showInfoTips);
        InfoTipDelayBox().Value(settings_.infoTipDelay);
        TipPathBox().IsChecked((settings_.infoTipFlags & sm::TipPath) != 0);
        TipNameBox().IsChecked((settings_.infoTipFlags & sm::TipName) != 0);
        TipIconBox().IsChecked((settings_.infoTipFlags & sm::TipIcon) != 0);
        TipDateBox().IsChecked((settings_.infoTipFlags & sm::TipDate) != 0);
        TipSizeBox().IsChecked((settings_.infoTipFlags & sm::TipSize) != 0);
        TipAttribBox().IsChecked((settings_.infoTipFlags & sm::TipAttrib) != 0);
        RolloverBox().IsChecked(settings_.rolloverBox);
        AutoRescanBox().IsChecked(settings_.autoRescan);
        DisableDeleteBox().IsChecked(settings_.disableDelete);
        AnimatedZoomBox().IsChecked(settings_.animatedZoom);
        SavePositionBox().IsChecked(settings_.savePosition);
        UpdateTipControls();
    }

    void MainWindow::FillSettingsLists(std::wstring_view theme, int density, int fileColor, int folderColor,
                                       int backdrop)
    {
        sm::Lang const& lang = *lang_;
        auto const fill = [](muxc::ComboBox const& box, std::span<std::wstring_view const> names, int selected) {
            auto const items = box.Items();
            items.Clear();
            for (std::wstring_view const name : names)
            {
                items.Append(box_value(hstring(name)));
            }
            box.SelectedIndex(selected);
        };

        // The classic themes have translated names; the others' names are their own.
        std::vector<std::wstring_view> themeNames;
        sm::Theme const& picked = sm::FindTheme(themes_, theme);
        int pickedIndex = 0;
        for (sm::Theme const& candidate : themes_)
        {
            if (&candidate == &picked)
            {
                pickedIndex = static_cast<int>(themeNames.size());
            }
            std::size_t const classic = candidate.base == sm::ThemeBase::System  ? 0
                                        : candidate.base == sm::ThemeBase::Light ? 1
                                                                                 : 2;
            themeNames.push_back(candidate.classic ? lang.themeNames.at(classic) : std::wstring_view(candidate.name));
        }
        fill(ThemeBox(), themeNames, pickedIndex);
        fill(DensityBox(), lang.densityNames, density);
        fill(FileColorBox(), lang.colorNames, fileColor);
        fill(FolderColorBox(), lang.colorNames, folderColor);
        fill(BackdropBox(), lang.backdropNames, backdrop);
    }

    void MainWindow::ApplySettingsText()
    {
        sm::Lang const& lang = *lang_;
        auto const text = [](std::wstring_view value) { return box_value(hstring(value)); };
        if (settingsDialog_)
        {
            settingsDialog_.Title(text(lang.settings));
            settingsDialog_.PrimaryButtonText(hstring(lang.ok));
            settingsDialog_.CloseButtonText(hstring(lang.cancel));
        }
        AppearanceHeader().Text(hstring(lang.appearance));
        LanguageBox().Header(text(lang.language));
        ThemeBox().Header(text(lang.theme));
        ThemesFolderButton().Content(text(lang.themesFolder));
        ThemeErrorBar().Title(hstring(lang.themeErrors));
        BackdropBox().Header(text(lang.backgroundAnimation));
        BackdropHint().Text(hstring(lang.backgroundAnimationHint));
        LayoutHeader().Text(hstring(lang.layout));
        DensityBox().Header(text(lang.density));
        BiasSlider().Header(text(lang.bias));
        BiasHorz().Text(hstring(lang.horz));
        BiasEqual().Text(hstring(lang.equal));
        BiasVert().Text(hstring(lang.vert));
        ColorsHeader().Text(hstring(lang.displayColors));
        FileColorBox().Header(text(lang.files));
        FolderColorBox().Header(text(lang.folders));
        TipsHeader().Text(hstring(lang.toolTips));
        NameTipsBox().Content(text(lang.showNameTips));
        NameTipDelayBox().Header(text(lang.delay));
        InfoTipsBox().Content(text(lang.showInfoTips));
        InfoTipDelayBox().Header(text(lang.delay));
        TipPathBox().Content(text(lang.fullPath));
        TipNameBox().Content(text(lang.fileName));
        TipIconBox().Content(text(lang.icon));
        TipDateBox().Content(text(lang.dateTime));
        TipSizeBox().Content(text(lang.fileSize));
        TipAttribBox().Content(text(lang.attrib));
        RolloverBox().Content(text(lang.showRolloverBox));
        MiscHeader().Text(hstring(lang.miscOptions));
        AutoRescanBox().Content(text(lang.autoRescan));
        DisableDeleteBox().Content(text(lang.disableDelete));
        AnimatedZoomBox().Content(text(lang.animatedZoom));
        SavePositionBox().Content(text(lang.savePos));
        ShortcutsHeader().Text(hstring(lang.keyboardShortcuts));
        FillShortcuts();
    }

    // The keyboard shortcuts, as Settings lists them - from the table the
    // window's keys come from (src/Core/Commands.cpp): their keys as key
    // caps, in the language, then what they do.
    void MainWindow::FillShortcuts()
    {
        namespace mua = mux::Automation;
        mux::ResourceDictionary const resources = mux::Application::Current().Resources();
        auto const style = [&resources](wchar_t const* name) {
            return resources.Lookup(box_value(name)).as<mux::Style>();
        };
        mux::Style const keyCap = style(L"KeyCapStyle");
        mux::Style const caption = style(L"CaptionTextBlockStyle");
        auto const label = [&caption](std::wstring_view text) {
            muxc::TextBlock block;
            block.Style(caption);
            block.Text(hstring(text));
            block.HorizontalAlignment(mux::HorizontalAlignment::Center);
            block.VerticalAlignment(mux::VerticalAlignment::Center);
            mua::AutomationProperties::SetAccessibilityView(
                block, mua::Peers::AccessibilityView::Raw);  // read with the action
            return block;
        };

        muxc::Grid const list = ShortcutsList();
        list.Children().Clear();
        list.RowDefinitions().Clear();
        sm::Lang const& lang = *lang_;
        for (sm::Command const command : sm::CommandsWithKeys())
        {
            int const row = static_cast<int>(list.RowDefinitions().Size());
            muxc::RowDefinition definition;
            definition.Height({ 1.0, mux::GridUnitType::Auto });
            list.RowDefinitions().Append(definition);

            muxc::StackPanel keys;
            keys.Orientation(muxc::Orientation::Horizontal);
            keys.Spacing(4);
            for (sm::KeyBinding const& binding : sm::KeyBindings())
            {
                if (binding.command != command)
                {
                    continue;
                }
                if (keys.Children().Size() != 0)
                {
                    muxc::TextBlock const slash = label(L"/");
                    slash.Opacity(0.6);
                    keys.Children().Append(slash);
                }
                for (std::wstring const& key : sm::KeysOf(binding, lang))
                {
                    muxc::Border cap;
                    cap.Style(keyCap);
                    cap.Child(label(key));
                    keys.Children().Append(cap);
                }
            }
            muxc::Grid::SetRow(keys, row);
            list.Children().Append(keys);

            std::wstring_view const does = sm::ShortcutAction(command, lang);
            muxc::TextBlock action;
            action.Text(hstring(does));
            action.TextWrapping(mux::TextWrapping::Wrap);
            action.VerticalAlignment(mux::VerticalAlignment::Center);
            mua::AutomationProperties::SetName(action, hstring(sm::Join(sm::CommandKeys(command, lang), L": ", does)));
            muxc::Grid::SetRow(action, row);
            muxc::Grid::SetColumn(action, 1);
            list.Children().Append(action);
        }
    }

    void MainWindow::OnLanguagePreview(IInspectable const&, muxc::SelectionChangedEventArgs const&)
    {
        if (populatingSettings_)
        {
            return;
        }
        auto const languages = sm::Languages();
        int const index = LanguageBox().SelectedIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= languages.size())
        {
            return;
        }

        // Like 1.4.0, the dialog switches language as soon as one is picked;
        // the main window follows, and Cancel switches both back.
        lang_ = &languages[static_cast<std::size_t>(index)];
        populatingSettings_ = true;
        FillSettingsLists(PickedTheme(), DensityBox().SelectedIndex(), FileColorBox().SelectedIndex(),
                          FolderColorBox().SelectedIndex(), BackdropBox().SelectedIndex());
        ApplySettingsText();
        populatingSettings_ = false;
        ApplyLanguage();
    }

    void MainWindow::OnBackdropPreview(IInspectable const&, muxc::SelectionChangedEventArgs const&)
    {
        // Like the theme, an animation shows as soon as it is picked; Cancel switches back.
        int const index = BackdropBox().SelectedIndex();
        if (!populatingSettings_ && index >= 0 && index < sm::kBackdropCount)
        {
            backdropPreview_ = static_cast<sm::Backdrop>(index);
            renderer_.InvalidateBase();
            RequestRender();
        }
    }

    void MainWindow::OnThemePreview(IInspectable const&, muxc::SelectionChangedEventArgs const&)
    {
        // Like the language, a theme shows as soon as it is picked; Cancel switches back.
        if (!populatingSettings_ && ThemeBox().SelectedIndex() >= 0)
        {
            ApplyTheme(PickedTheme());
        }
    }

    // The id of the theme picked in Settings.
    std::wstring MainWindow::PickedTheme()
    {
        int const index = ThemeBox().SelectedIndex();
        return index >= 0 && std::cmp_less(index, themes_.size()) ? themes_[static_cast<std::size_t>(index)].id
                                                                  : settings_.theme;
    }

    // Reads the theme files again while Settings is open, and shows the
    // picked theme as its file now has it.
    void MainWindow::RefreshThemes()
    {
        std::wstring const picked = PickedTheme();
        LoadThemes();
        populatingSettings_ = true;
        FillSettingsLists(picked, DensityBox().SelectedIndex(), FileColorBox().SelectedIndex(),
                          FolderColorBox().SelectedIndex(), BackdropBox().SelectedIndex());
        populatingSettings_ = false;
        ShowThemeErrors();
        ApplyTheme(PickedTheme());
    }

    void MainWindow::ShowThemeErrors()
    {
        ThemeErrorBar().Message(hstring(JoinLines(themeErrors_)));
        ThemeErrorBar().IsOpen(!themeErrors_.empty());
    }

    void MainWindow::OnOpenThemesFolder(IInspectable const&, mux::RoutedEventArgs const&)
    {
        std::vector<std::wstring> const folders = sm::shell::ThemeFolders();
        if (!folders.empty())
        {
            (void)sm::shell::OpenFolder(folders.front());
        }
    }

    void MainWindow::OnTipOptionChanged(IInspectable const&, mux::RoutedEventArgs const&)
    {
        UpdateTipControls();
    }

    void MainWindow::UpdateTipControls()
    {
        bool const names = Checked(NameTipsBox());
        NameTipDelayBox().IsEnabled(names);
        bool const info = Checked(InfoTipsBox());
        for (muxc::Control const& control :
             { muxc::Control(InfoTipDelayBox()), muxc::Control(TipPathBox()), muxc::Control(TipNameBox()),
               muxc::Control(TipIconBox()), muxc::Control(TipDateBox()), muxc::Control(TipSizeBox()),
               muxc::Control(TipAttribBox()) })
        {
            control.IsEnabled(info);
        }
    }

    void MainWindow::ReadSettings()
    {
        // Out-of-range picks (-1: nothing selected) are clamped by Validate();
        // 1.4.0 stored CB_ERR - 3 as the density and indexed its tables with it.
        settings_.language = std::wstring(lang_->code);
        settings_.theme = PickedTheme();
        settings_.density = DensityBox().SelectedIndex() + sm::Settings::kMinDensity;
        settings_.fileColor = FileColorBox().SelectedIndex();
        settings_.folderColor = FolderColorBox().SelectedIndex();
        settings_.bias = static_cast<int>(std::lround(BiasSlider().Value()));
        settings_.showNameTips = Checked(NameTipsBox());
        settings_.nameTipDelay = Delay(NameTipDelayBox(), settings_.nameTipDelay);
        settings_.showInfoTips = Checked(InfoTipsBox());
        settings_.infoTipDelay = Delay(InfoTipDelayBox(), settings_.infoTipDelay);
        std::uint32_t flags = 0;
        flags |= Checked(TipPathBox()) ? sm::TipPath : 0u;
        flags |= Checked(TipNameBox()) ? sm::TipName : 0u;
        flags |= Checked(TipIconBox()) ? sm::TipIcon : 0u;
        flags |= Checked(TipDateBox()) ? sm::TipDate : 0u;
        flags |= Checked(TipSizeBox()) ? sm::TipSize : 0u;
        flags |= Checked(TipAttribBox()) ? sm::TipAttrib : 0u;
        settings_.infoTipFlags = flags;
        settings_.rolloverBox = Checked(RolloverBox());
        settings_.autoRescan = Checked(AutoRescanBox());
        settings_.disableDelete = Checked(DisableDeleteBox());
        settings_.animatedZoom = Checked(AnimatedZoomBox());
        settings_.savePosition = Checked(SavePositionBox());
        settings_.backdrop = static_cast<sm::Backdrop>(BackdropBox().SelectedIndex());  // -1: Validate() makes it None
    }

    winrt::fire_and_forget MainWindow::ShowAbout()
    {
        auto const strong = get_strong();
        dialogOpen_ = true;
        sm::Lang const& lang = *lang_;
        muxc::ContentDialog dialog = HostDialog(aboutDialog_, AboutContent());
        dialog.Title(box_value(hstring(lang.aboutApp)));
        dialog.CloseButtonText(hstring(lang.ok));
        dialog.DefaultButton(muxc::ContentDialogButton::Close);

        // The runtime is bundled with the app (self-contained), so report the
        // release it was built against; RuntimeInfo describes framework packages only.
        std::wstring const version = sm::Join(
            L"SpaceWindow ", kVersion, L"  ·  WinUI 3  ·  Windows App SDK ", std::to_wstring(SPACEWINDOW_WASDK_MAJOR),
            L".", std::to_wstring(SPACEWINDOW_WASDK_MINOR), L".", std::to_wstring(SPACEWINDOW_WASDK_PATCH));
        AboutVersion().Text(hstring(version));
        AboutFreeware().Text(hstring(lang.freeware));
        AboutWarranty().Text(hstring(lang.warranty));
        AboutCredits().Text(hstring(lang.credits));

        co_await dialog.ShowAsync();
        dialogOpen_ = false;
    }
}
