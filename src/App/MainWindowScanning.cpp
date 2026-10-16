// SpaceWindow - main window: scanning, live maps of a scan, reloads, and the unreadable folders' list.
#include "pch.h"
#include "MainWindowShared.h"

#include "../Core/ChangeJournal.h"
#include "../Core/FolderWatch.h"
#include "../Core/Format.h"

namespace winrt::SpaceWindow::implementation
{
    winrt::fire_and_forget MainWindow::StartScan(std::wstring path)
    {
        auto const strong = get_strong();
        if (scanning_)
        {
            co_return;
        }

        // Rescanning the same drive keeps the view zoomed where it was (1.4.0 zoomed all the way out).
        bool const again = tree_ && sm::EqualsIgnoringCase(sm::NormalizeScanPath(path), tree_->RootPath());
        std::vector<std::wstring> viewPath;
        if (again)
        {
            viewPath = ViewPath();
        }

        scanning_ = true;
        scanStop_ = std::stop_source{};
        auto const progress = std::make_shared<sm::ScanProgress>();
        scanProgress_ = progress;
        HideTips();
        ErrorBar().IsOpen(false);
        ShowScanOverlay(path);
        UpdateCommands();
        progressTimer_.Start();

        std::stop_token const stop = scanStop_.get_token();
        sm::ScanResult result;
        bool failed = false;
        co_await winrt::resume_background();
        try
        {
            sm::ScanOptions options;
            options.watch = true;  // reloads then list again only the folders that changed
            result = sm::ScanFolder(path, *progress, stop, options);
        }
        catch (...)
        {
            failed = true;  // e.g. out of memory
        }
        try
        {
            co_await wil::resume_foreground(dispatcher_);
        }
        catch (winrt::hresult_error const&)
        {
            co_return;  // the window has closed
        }

        progressTimer_.Stop();
        scanning_ = false;
        scanProgress_.reset();
        HideScanOverlay();
        if (!failed && result.status == sm::ScanStatus::Completed)
        {
            preview_.reset();  // SetTree draws the map anew
            if (!again)
            {
                history_.Clear();  // where the view went on another drive means nothing on this one
            }
            SetTree(std::move(result.tree), viewPath);
        }
        else
        {
            EndPreview();  // back to the tree there was, if any
            if (failed || result.status == sm::ScanStatus::NotFound)
            {
                ShowError(sm::FormatText(lang_->scanFailed, { path }));
            }
        }
        UpdateAll();
        MapFocus().Focus(mux::FocusState::Programmatic);
    }

    // Brings the tree up to date: from the drive's change journal when it was
    // read from the master file table, from what its watch saw change when it
    // was listed folder by folder, otherwise by scanning again.
    void MainWindow::Rescan()
    {
        if (!tree_)
        {
            return;
        }
        if (tree_->journal || tree_->watch)
        {
            UpdateFromChanges();
        }
        else
        {
            StartScan(lastPath_);
        }
    }

    // The folder that can be scanned again on its own: the one selected (or
    // holding the file selected), or else the one zoomed into.
    sm::Folder* MainWindow::RescanTarget() const noexcept
    {
        if (!tree_)
        {
            return nullptr;
        }
        if (sm::Entry const* const entry = SelectedEntry(); entry != nullptr)
        {
            sm::Folder* const folder = entry->child ? entry->child.get() : selection_.folder;
            if (folder != tree_->Root())
            {
                return folder;
            }
        }
        return viewRoot_ != tree_->Root() ? viewRoot_ : nullptr;
    }

    // Only what changed since the tree was read is read again and patched into
    // it: a moment, where scanning the drive takes seconds.  The files the
    // journal names are read from the master file table; the folders the
    // watch saw change are listed again.  When neither can tell (the journal
    // was reset, or too much changed at once), the drive is scanned again.
    winrt::fire_and_forget MainWindow::UpdateFromChanges()
    {
        auto const strong = get_strong();
        std::wstring const path = lastPath_;
        // Only read until the update is applied, here - but for its journal
        // index, which LargeFiles may build (nothing else uses it meanwhile).
        sm::FolderTree* const tree = tree_.get();
        std::optional<sm::JournalPosition> const journal = tree_->journal;
        sm::FolderWatch::Changes changes;
        if (!journal)
        {
            changes = tree_->watch->Take();
            if (!changes.complete)
            {
                StartScan(path);
                co_return;
            }
        }
        scanning_ = true;
        scanStop_ = std::stop_source{};
        HideTips();
        ErrorBar().IsOpen(false);
        UpdateCommands();
        // A quick update does without the progress overlay, which would only flash.
        auto const overlay = dispatcher_.CreateTimer();
        overlay.IsRepeating(false);
        overlay.Interval(kQuietUpdate);
        overlay.Tick([weak = get_weak()](auto&&, auto&&) {
            if (auto const self = weak.get(); self && self->scanning_ && !self->scanProgress_)  // still this update
            {
                self->ShowScanOverlay(self->lastPath_, false);
            }
        });
        overlay.Start();

        std::stop_token const stop = scanStop_.get_token();
        enum class Outcome
        {
            Apply,
            Scan,
            Cancelled,
        };
        Outcome outcome = Outcome::Scan;
        sm::JournalRead read;
        std::optional<sm::FolderReload> reload;
        co_await winrt::resume_background();
        try
        {
            if (journal)
            {
                // Large files may have grown unseen while held open (ChangeJournal.h): read them again too.
                std::vector<std::uint32_t> const large = sm::LargeFiles(*tree);
                read = sm::ReadJournalChanges(*journal, stop, 0, 0, large);
                outcome = read.status == sm::JournalReadStatus::Ready       ? Outcome::Apply
                          : read.status == sm::JournalReadStatus::Cancelled ? Outcome::Cancelled
                                                                            : Outcome::Scan;
            }
            else
            {
                sm::ScanProgress progress;  // counts what is found in folders new to the tree
                reload = sm::PrepareReload(*tree, changes.folders, changes.files, progress, stop);
                outcome = reload ? Outcome::Apply : Outcome::Cancelled;
            }
        }
        catch (...)
        {
            outcome = Outcome::Scan;  // e.g. out of memory: scan instead
        }
        try
        {
            co_await wil::resume_foreground(dispatcher_);
        }
        catch (winrt::hresult_error const&)
        {
            co_return;  // the window has closed
        }

        overlay.Stop();
        bool scan = outcome == Outcome::Scan;
        if (outcome == Outcome::Apply)
        {
            // The update is applied here, on the UI thread.  A large one takes
            // a moment: the progress card shows first, and gets drawn, so the
            // window does not just stop.
            bool const large =
                journal ? read.changes.files.size() >= 10'000 : reload.has_value() && reload->listings.size() >= 2'000;
            if (large && ScanOverlay().Visibility() != mux::Visibility::Visible)
            {
                ShowScanOverlay(lastPath_, false);
                try
                {
                    co_await wil::resume_foreground(dispatcher_,
                                                    Microsoft::UI::Dispatching::DispatcherQueuePriority::Low);
                }
                catch (winrt::hresult_error const&)
                {
                    co_return;  // the window has closed
                }
            }
            // Folders the view or the selection point to may go: both are found
            // again by name, as after a scan.
            std::vector<std::wstring> const viewPath = ViewPath();
            std::unique_ptr<sm::FolderTree> updated = std::move(tree_);
            try
            {
                if (journal)
                {
                    sm::ApplyJournalChanges(*updated, read.changes);
                }
                else if (reload)
                {
                    sm::ApplyReload(*updated, std::move(*reload));
                }
            }
            catch (...)
            {
                scan = true;  // half applied: the totals cannot be trusted
            }
            SetTree(std::move(updated), viewPath);
        }
        else if (outcome == Outcome::Cancelled && !journal)
        {
            tree_->watch.reset();  // the changes taken from it are not applied: the next rescan is a full one
        }
        scanning_ = false;
        HideScanOverlay();
        UpdateAll();
        MapFocus().Focus(mux::FocusState::Programmatic);
        if (scan)
        {
            StartScan(path);
        }
    }

    // Scans one folder again, everything below it - its place in the tree,
    // and everything else, stays as it is.
    winrt::fire_and_forget MainWindow::RescanFolder(sm::Folder* folder)
    {
        auto const strong = get_strong();
        if (!tree_ || folder == nullptr)
        {
            co_return;
        }
        std::wstring const path = tree_->PathOf(*folder);
        sm::FolderTree const* const tree = tree_.get();  // only read until the new contents go in, here
        scanning_ = true;
        scanStop_ = std::stop_source{};
        auto const progress = std::make_shared<sm::ScanProgress>();
        scanProgress_ = progress;
        HideTips();
        ErrorBar().IsOpen(false);
        ShowScanOverlay(path);
        UpdateCommands();
        progressTimer_.Start();

        std::stop_token const stop = scanStop_.get_token();
        std::unique_ptr<sm::Folder> fresh;
        bool failed = false;
        co_await winrt::resume_background();
        try
        {
            fresh = sm::ScanAgain(*tree, *folder, *progress, stop);
        }
        catch (...)
        {
            failed = true;  // e.g. out of memory
        }
        try
        {
            co_await wil::resume_foreground(dispatcher_);
        }
        catch (winrt::hresult_error const&)
        {
            co_return;  // the window has closed
        }

        progressTimer_.Stop();
        scanning_ = false;
        scanProgress_.reset();
        HideScanOverlay();
        bool scan = false;
        if (failed)
        {
            ShowError(sm::FormatText(lang_->scanFailed, { path }));
        }
        else if (fresh)
        {
            std::vector<std::wstring> const viewPath = ViewPath();
            std::unique_ptr<sm::FolderTree> updated = std::move(tree_);
            try
            {
                sm::ReplaceContents(*updated, *folder, std::move(fresh));
            }
            catch (...)
            {
                scan = true;  // half replaced: the totals cannot be trusted
            }
            SetTree(std::move(updated), viewPath);
        }
        UpdateAll();
        MapFocus().Focus(mux::FocusState::Programmatic);
        if (scan)
        {
            StartScan(lastPath_);
        }
    }

    void MainWindow::ShowScanOverlay(std::wstring const& path, bool counts)
    {
        sm::Lang const& lang = *lang_;
        ScanPath().Text(hstring(path));
        ScanFiles().Text(hstring(sm::Join(lang.filesFound, L" 0")));
        ScanFolders().Text(hstring(sm::Join(lang.foldersFound, L" 0")));
        mux::Visibility const shown = counts ? mux::Visibility::Visible : mux::Visibility::Collapsed;
        ScanFiles().Visibility(shown);
        ScanFolders().Visibility(shown);
        ScanProgressBar().IsIndeterminate(true);
        ScanProgressBar().Value(0);
        EmptyState().Visibility(mux::Visibility::Collapsed);
        ScanOverlay().Visibility(mux::Visibility::Visible);
        ScanCancelButton().Focus(mux::FocusState::Programmatic);
    }

    void MainWindow::HideScanOverlay()
    {
        ScanOverlay().Visibility(mux::Visibility::Collapsed);
        // Back to dimming the map, the card in the middle, for the next scan.
        ScanSmoke().Visibility(mux::Visibility::Visible);
        ScanCard().VerticalAlignment(mux::VerticalAlignment::Center);
        ScanCard().Margin({});
        EmptyState().Visibility(tree_ ? mux::Visibility::Collapsed : mux::Visibility::Visible);
    }

    // A live map of what the scan found so far: the folders listed, those
    // finished with their totals.  Taken off the UI thread; drawn in place of
    // the map while the scan runs.
    winrt::fire_and_forget MainWindow::TakeSnapshot(std::shared_ptr<sm::ScanProgress> progress)
    {
        auto const strong = get_strong();
        snapshotting_ = true;
        std::unique_ptr<sm::FolderTree> snapshot;
        co_await winrt::resume_background();
        try
        {
            snapshot = progress->Snapshot(kLiveMapEntries);
        }
        catch (...)  // NOLINT(bugprone-empty-catch): no live map this time (out of memory)
        {
        }
        try
        {
            co_await wil::resume_foreground(dispatcher_);
        }
        catch (winrt::hresult_error const&)
        {
            co_return;  // the window has closed
        }
        snapshotting_ = false;
        if (!snapshot || !scanning_ || scanProgress_ != progress)
        {
            co_return;  // nothing listed yet, or that scan is over
        }
        if (!preview_)
        {
            // The map shows through; the card makes room at the bottom.
            ScanSmoke().Visibility(mux::Visibility::Collapsed);
            ScanCard().VerticalAlignment(mux::VerticalAlignment::Bottom);
            ScanCard().Margin({ 0, 0, 0, 24 });
        }
        preview_ = std::move(snapshot);
        Relayout();
    }

    // Back to the map of the tree, if a live map was shown.
    void MainWindow::EndPreview()
    {
        if (preview_)
        {
            preview_.reset();
            Relayout();
        }
    }

    void MainWindow::OnProgressTick(Microsoft::UI::Dispatching::DispatcherQueueTimer const&, IInspectable const&)
    {
        std::shared_ptr<sm::ScanProgress> const progress = scanProgress_;
        if (!progress)
        {
            return;
        }
        sm::Lang const& lang = *lang_;
        ScanFiles().Text(hstring(
            sm::Join(lang.filesFound, L" ", sm::FormatCount(progress->files.load(std::memory_order_relaxed), lang))));
        ScanFolders().Text(hstring(sm::Join(lang.foldersFound, L" ",
                                            sm::FormatCount(progress->folders.load(std::memory_order_relaxed), lang))));
        ScanPath().Text(hstring(progress->CurrentPath()));

        // Like 1.4.0's bar: bytes found so far against the drive's used space.
        std::uint64_t const expected = progress->expectedBytes.load(std::memory_order_relaxed);
        if (expected != 0)
        {
            ScanProgressBar().IsIndeterminate(false);
            ScanProgressBar().Value(sm::PerMille(progress->bytes.load(std::memory_order_relaxed), expected));
        }

        if (++progressTicks_ % kLiveMapTicks == 0 && !snapshotting_)
        {
            TakeSnapshot(progress);
        }
    }

    void MainWindow::OnCancelScan(IInspectable const&, mux::RoutedEventArgs const&)
    {
        scanStop_.request_stop();
    }

    void MainWindow::OnShowUnreadable(IInspectable const&, mux::RoutedEventArgs const&)
    {
        ShowUnreadable();
    }

    // The folders a scan could not list: their contents are missing from the map.
    winrt::fire_and_forget MainWindow::ShowUnreadable()
    {
        auto const strong = get_strong();
        if (!tree_ || Busy())  // the status bar's link: not a command of its own
        {
            co_return;
        }
        sm::Lang const& lang = *lang_;
        std::vector<std::wstring> const paths = tree_->UnreadableFolders();
        std::wstring all;
        for (std::wstring const& path : paths)
        {
            all.append(path).append(L"\r\n");
        }
        constexpr std::size_t kListed = 1000;  // a longer list goes to the clipboard whole
        std::wstring listed;
        for (std::size_t i = 0; i < paths.size() && i < kListed; ++i)
        {
            listed.append(i == 0 ? L"" : L"\r\n").append(paths[i]);
        }
        if (paths.size() > kListed)
        {
            std::wstring const more = sm::FormatCount(paths.size() - kListed, lang);
            listed.append(L"\r\n").append(sm::FormatText(lang.moreFolders, { more }));
        }

        muxc::ContentDialog dialog = MakeDialog(lang.unreadableTitle);
        muxc::StackPanel content;
        content.Spacing(12);
        muxc::TextBlock intro;
        intro.Text(hstring(lang.unreadableText));
        intro.TextWrapping(mux::TextWrapping::Wrap);
        muxc::TextBox list;
        list.Text(hstring(listed));
        list.IsReadOnly(true);
        list.AcceptsReturn(true);
        list.TextWrapping(mux::TextWrapping::NoWrap);
        list.MaxHeight(320);
        muxc::ScrollViewer::SetHorizontalScrollBarVisibility(list, muxc::ScrollBarVisibility::Auto);
        muxc::ScrollViewer::SetVerticalScrollBarVisibility(list, muxc::ScrollBarVisibility::Auto);
        content.Children().Append(intro);
        content.Children().Append(list);
        dialog.Content(content);
        dialog.PrimaryButtonText(hstring(lang.copyList));
        dialog.CloseButtonText(hstring(lang.close));
        dialog.DefaultButton(muxc::ContentDialogButton::Close);

        dialogOpen_ = true;
        muxc::ContentDialogResult const answer = co_await dialog.ShowAsync();
        dialogOpen_ = false;
        if (answer == muxc::ContentDialogResult::Primary)
        {
            Windows::ApplicationModel::DataTransfer::DataPackage package;
            package.SetText(hstring(all));
            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
        }
    }
}
