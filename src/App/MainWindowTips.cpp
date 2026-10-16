// SpaceWindow - main window: the name tips and the info tips.
#include "pch.h"
#include "MainWindowShared.h"

#include "../Core/Format.h"

namespace winrt::SpaceWindow::implementation
{
    void MainWindow::HideTips()
    {
        ++tips_;  // an icon still on its way is not for the next tip
        if (nameTipTimer_)
        {
            nameTipTimer_.Stop();
            infoTipTimer_.Stop();
        }
        NameTip().Visibility(mux::Visibility::Collapsed);
        InfoTip().Visibility(mux::Visibility::Collapsed);
    }

    void MainWindow::RestartTipTimers()
    {
        if (settings_.showNameTips)
        {
            nameTipTimer_.Interval(std::chrono::milliseconds(std::max(1, settings_.nameTipDelay)));
            nameTipTimer_.Start();
        }
        if (settings_.showInfoTips)
        {
            infoTipTimer_.Interval(std::chrono::milliseconds(std::max(1, settings_.infoTipDelay)));
            infoTipTimer_.Start();
        }
    }

    void MainWindow::PlaceTip(mux::FrameworkElement const& tip, double x, double y, bool centered)
    {
        tip.Visibility(mux::Visibility::Visible);
        constexpr float unbounded = std::numeric_limits<float>::infinity();
        tip.Measure({ unbounded, unbounded });
        auto const size = tip.DesiredSize();
        if (centered)
        {
            x -= size.Width / 2.0;
            y -= size.Height / 2.0;
        }
        double const maxX = std::max(0.0, MapArea().ActualWidth() - size.Width);
        double const maxY = std::max(0.0, MapArea().ActualHeight() - size.Height);
        muxc::Canvas::SetLeft(tip, std::clamp(x, 0.0, maxX));
        muxc::Canvas::SetTop(tip, std::clamp(y, 0.0, maxY));
    }

    // A name that does not fit its box pops up over the box (1.4.0's "name
    // tips"); in the pie chart, over the middle of the slice.
    void MainWindow::ShowNameTip()
    {
        if (!tree_ || hoverItem_ < 0 || !std::cmp_less(hoverItem_, ItemCount()) || !Item(hoverItem_).IsSelectable())
        {
            return;
        }
        sm::ui::MapScene const scene = Scene();
        bool const selected = hoverItem_ == selectedItem_;
        bool const highlighted = std::ranges::find(hoverChain_, hoverItem_) != hoverChain_.end();
        float const scale = renderer_.Scale();
        sm::BoxColors colors;
        double x = 0.0;
        double y = 0.0;
        if (PieMode())
        {
            sm::PieSlice const& slice = pie_.Items()[static_cast<std::size_t>(hoverItem_)];
            if (!renderer_.IsSliceLabelTruncated(scene, slice))
            {
                return;
            }
            colors = renderer_.SliceColors(scene, slice, highlighted, selected);
            sm::PiePoint const anchor = pie_.Centroid(slice);
            x = anchor.x / scale;
            y = anchor.y / scale;
        }
        else
        {
            sm::DisplayItem const& item = layout_.Items()[static_cast<std::size_t>(hoverItem_)];
            if (!renderer_.IsLabelTruncated(scene, item))
            {
                return;
            }
            colors = renderer_.ItemColors(scene, item, highlighted, selected);
            x = (item.x + renderer_.LineWidth()) / scale;
            y = item.IsFolder() ? (item.y + renderer_.LineWidth()) / scale
                                : (item.y + item.h / 2.0) / scale - renderer_.FontSizeDips() * 0.7;
        }

        sm::MapItem const& item = Item(hoverItem_);
        NameTip().Background(muxm::SolidColorBrush(ToColor(colors.face)));
        NameTip().BorderBrush(muxm::SolidColorBrush(ToColor(scene.colors.outline)));
        NameTipText().Foreground(muxm::SolidColorBrush(ToColor(sm::TextColor(scene.colors, colors.face))));
        NameTipText().FontFamily(muxm::FontFamily(hstring(renderer_.FontFamily())));
        NameTipText().FontSize(renderer_.FontSizeDips());
        NameTipText().Text(hstring(item.source->Name(item.index)));
        PlaceTip(NameTip(), x, y, PieMode());
    }

    // Details of the file under the mouse (1.4.0's "info tips").
    void MainWindow::ShowInfoTip()
    {
        if (!tree_ || hoverItem_ < 0 || !std::cmp_less(hoverItem_, ItemCount()))
        {
            return;
        }
        sm::MapItem const& item = Item(hoverItem_);
        sm::Lang const& lang = *lang_;
        std::uint32_t const flags = settings_.infoTipFlags;
        std::wstring name;
        std::wstring folderPath;
        std::wstring details;
        auto const line = [&details](std::wstring const& text) {
            if (!text.empty())
            {
                if (!details.empty())
                {
                    details.push_back(L'\n');
                }
                details.append(text);
            }
        };
        std::wstring iconPath;  // the icon is fetched off the UI thread, then put in

        if (sm::Entry const* const entry = item.IsSelectable() ? item.GetEntry() : nullptr)
        {
            folderPath = tree_->PathOf(*item.source);
            name = item.source->Name(item.index);
            if (flags & sm::TipSize)
            {
                // 1.4.0 showed only the file length; the space it takes on disk is new.
                std::wstring const onDisk = sm::FormatSize(entry->size, lang);
                line(sm::Join(sm::FormatBytes(entry->logicalSize, lang), L"  (",
                              sm::FormatText(lang.onDisk, { onDisk }), L")"));
                if (entry->child)
                {
                    line(sm::Join(lang.filesTotal, L" ", sm::FormatCount(entry->child->fileCount, lang), L"   ",
                                  lang.foldersTotal, L" ", sm::FormatCount(entry->child->folderCount, lang)));
                }
                if (entry->countedElsewhere)
                {
                    line(std::wstring(lang.hardLink));
                }
            }
            if (entry->child && entry->child->unreadable)
            {
                line(std::wstring(lang.unreadableFolder));
            }
            if (flags & sm::TipAttrib)
            {
                line(sm::FormatAttributes(entry->attributes, lang));
            }
            if (flags & sm::TipDate)
            {
                line(sm::FormatFileTime(entry->lastWriteTime, lang));
            }
            if (flags & sm::TipIcon)
            {
                iconPath = folderPath + name;
            }
        }
        else if (PieMode())
        {
            // A slice of grouped small entries, or of free space: what it is and how much.
            sm::PieSlice const& slice = pie_.Items()[static_cast<std::size_t>(hoverItem_)];
            if (slice.IsFolded())
            {
                std::wstring const count = sm::FormatCount(slice.folded, lang);
                name = sm::FormatText(lang.smallerItems, { count });
                folderPath = tree_->PathOf(*slice.source);
            }
            else
            {
                sm::Entry const* const special = slice.GetEntry();
                name =
                    special != nullptr && special->kind == sm::EntryKind::FreeSpace ? lang.freeSpace : lang.unscanned;
                folderPath = tree_->RootPath();
            }
            if (flags & sm::TipSize)
            {
                line(sm::Join(sm::FormatSize(slice.bytes, lang), L"  (",
                              sm::FormatPercent(slice.bytes, PercentBase(), lang), L")"));
            }
        }
        else
        {
            return;  // treemap filler boxes and free space have no tips, as in 1.4.0
        }

        // A grouped or free-space slice has no other way to say what it is, so its name always shows.
        bool const showName = (flags & sm::TipName) != 0 || !item.IsSelectable();
        InfoTipName().Text(hstring(name));
        InfoTipName().Visibility(showName ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        InfoTipPath().Text(hstring(folderPath));
        InfoTipPath().Visibility((flags & sm::TipPath) ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        InfoTipDetails().Text(hstring(details));
        InfoTipDetails().Visibility(details.empty() ? mux::Visibility::Collapsed : mux::Visibility::Visible);
        InfoTipIcon().Source(nullptr);
        InfoTipIcon().Visibility(mux::Visibility::Collapsed);

        // The tip shows at once; the icon follows when it is read - the shell
        // can take its time with a large program, or a file on a network share.
        infoTipAt_ = { lastPointerDips_.X + 16.0f, lastPointerDips_.Y + 16.0f };
        std::uint64_t const tip = ++tips_;
        if (showName || (flags & sm::TipPath) != 0 || !details.empty())
        {
            PlaceTip(InfoTip(), infoTipAt_.X, infoTipAt_.Y);
        }
        if (!iconPath.empty())
        {
            LoadTipIcon(std::move(iconPath), static_cast<int>(std::lround(32 * renderer_.Scale())), tip);
        }
    }

    winrt::fire_and_forget MainWindow::LoadTipIcon(std::wstring path, int size, std::uint64_t tip)
    {
        auto const strong = get_strong();
        co_await winrt::resume_background();
        sm::shell::IconPixels const icon = sm::shell::GetIcon(path, size);
        try
        {
            co_await wil::resume_foreground(dispatcher_);
        }
        catch (winrt::hresult_error const&)
        {
            co_return;  // the window has closed
        }
        if (tip != tips_ || icon.bgra.empty())
        {
            co_return;  // the mouse has moved on, or there is no icon
        }
        InfoTipIcon().Source(ToImageSource(icon));
        InfoTipIcon().Visibility(mux::Visibility::Visible);
        PlaceTip(InfoTip(), infoTipAt_.X, infoTipAt_.Y);  // bigger now: kept inside the map
    }
}
