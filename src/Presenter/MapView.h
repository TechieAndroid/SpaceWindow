// SpaceWindow - the map a window shows, and how it moves: no toolkit in sight.
//
// New in the Linux port.  What the WinUI window (src/App/MainWindow*.cpp)
// decides for itself without touching WinUI - the tree shown and the folder
// zoomed into, the treemap or pie chart laid out for it, the selection and
// the box under the mouse, zooming, Back and Forward, which commands can run,
// and the text of the status bar, the title and the tips - in one class a
// front end drives: the Qt one (src/AppQt) today, the WinUI one to follow.
//
// Lengths are in pixels of the map: front ends lay the map out at the
// screen's real resolution and convert the mouse's position.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Commands.h"
#include "FolderTree.h"
#include "PieLayout.h"
#include "Settings.h"
#include "TreemapLayout.h"
#include "ViewHistory.h"

namespace sm
{
    struct Lang;

    struct RectF
    {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
    };

    // A segment of the pie chart's rings.
    struct Sector
    {
        float start = 0.0f;  // radians, clockwise from twelve o'clock
        float sweep = 0.0f;
        float inner = 0.0f;  // pixels from the centre
        float outer = 0.0f;
    };

    // An outline flying from the item zoomed into (or out of) to the whole map:
    // a rectangle over the treemap, a ring segment over the pie chart.
    struct ZoomMotion
    {
        bool pie = false;
        RectF from;
        RectF to;
        Sector fromSector;
        Sector toSector;

        RectF RectAt(float t) const noexcept;  // t: 0 .. 1, eased by the caller
        Sector SectorAt(float t) const noexcept;
    };

    // What the map is laid out in: its size in pixels and the screen's scale,
    // and the treemap's border and title band at that scale (the renderer's).
    struct MapGeometry
    {
        int width = 1;
        int height = 1;
        double scale = 1.0;
        int border = 3;
        int header = 15;
    };

    // What the window is doing, for which commands can run.
    struct CommandContext
    {
        bool scanning = false;
    };

    // The info tip of an item.
    struct InfoTip
    {
        std::wstring name;
        std::wstring folderPath;
        std::wstring details;   // lines
        std::wstring iconPath;  // the file whose icon it shows; empty for none
        bool showName = true;
        bool showPath = true;
    };

    enum class Direction
    {
        Left,
        Up,
        Right,
        Down,
    };

    class MapView
    {
    public:
        // The selected entry, by identity, so it survives a new layout.
        struct Selection
        {
            Folder* folder = nullptr;
            std::uint32_t index = kNoEntry;
        };

        // ---------------------------------------------------------------- the tree

        // Shows a tree, zoomed into the folder at `viewPath` (names from the
        // root) - or as far down it as the tree goes.  Lays the map out.
        void SetTree(std::unique_ptr<FolderTree> tree, std::vector<std::wstring> const& viewPath);

        // Takes the tree out, to update it elsewhere (SetTree puts it back).
        std::unique_ptr<FolderTree> TakeTree() noexcept;

        // A live map of a scan running: shown whole, in place of the tree, until cleared.
        void SetPreview(std::unique_ptr<FolderTree> preview);
        void ClearPreview();
        bool HasPreview() const noexcept { return preview_ != nullptr; }

        FolderTree* Tree() const noexcept { return tree_.get(); }
        FolderTree const* Shown() const noexcept { return preview_ ? preview_.get() : tree_.get(); }
        Folder* ViewRoot() const noexcept { return viewRoot_; }
        ViewHistory& History() noexcept { return history_; }

        // Names of the folders from the root down to the one zoomed into.
        std::vector<std::wstring> ViewPath() const;

        // ---------------------------------------------------------------- layout

        // How to lay the map out; lays it out again.
        void Configure(Settings const& settings, MapGeometry const& geometry);
        void SetShowFreeSpace(bool show);
        bool ShowFreeSpace() const noexcept { return showFreeSpace_; }
        void Relayout();

        Settings const& CurrentSettings() const noexcept { return settings_; }
        MapGeometry const& Geometry() const noexcept { return geometry_; }
        bool PieMode() const noexcept { return settings_.viewMode != ViewMode::Treemap; }
        TreemapLayout const& Treemap() const noexcept { return layout_; }
        PieLayout const& Pie() const noexcept { return pie_; }

        // The items of whichever view is showing: treemap boxes or pie slices.
        std::size_t ItemCount() const noexcept;
        MapItem const& Item(int item) const;
        int FindItem(Folder const* folder, std::uint32_t index) const noexcept;
        int FindFolderItem(Folder const* folder) const noexcept;
        PiePoint ItemAnchor(int item) const;  // a box's middle, a slice's label point

        // The selectable item at a point, or -1.
        int ItemAt(double x, double y) const;

        // ---------------------------------------------------------------- selection and mouse

        // Selects an item (or nothing, for -1 and items that cannot be selected).
        void Select(int item);
        int SelectedItem() const noexcept { return selectedItem_; }
        Selection CurrentSelection() const noexcept { return selection_; }
        Entry const* SelectedEntry() const noexcept;
        std::wstring SelectedPath() const;

        // The nearest selectable item that way (the first one if nothing is selected).
        void MoveSelection(Direction direction);

        // The mouse moved to a point: the item under it, and - with rollover
        // boxes, or in the pie chart - the items highlighted.  True if those changed.
        bool Hover(double x, double y);
        void EndHover() noexcept;
        int HoverItem() const noexcept { return hoverItem_; }
        std::span<int const> Highlighted() const noexcept;

        // ---------------------------------------------------------------- moving

        // Each returns the zoom animation to play, if the way is on the map.
        std::optional<ZoomMotion> ZoomIn(int item);
        std::optional<ZoomMotion> ZoomIntoSelection();
        std::optional<ZoomMotion> ZoomOut();
        std::optional<ZoomMotion> ZoomFull();
        std::optional<ZoomMotion> GoDown();
        std::optional<ZoomMotion> GoBack();
        std::optional<ZoomMotion> GoForward();

        // The folder that can be scanned again on its own: the one selected
        // (or holding the file selected), or else the one zoomed into.
        Folder* RescanTarget() const noexcept;

        // The selected entry went (moved to the trash): out of the tree, with
        // the totals above it and the hard links it shared brought up to date.
        void RemoveSelected();

        // ---------------------------------------------------------------- commands and text

        bool CanRun(Command command, CommandContext const& context) const noexcept;

        // What the status bar says: the path, and its sizes and counts.
        std::wstring StatusPath() const;
        std::wstring StatusInfo(Lang const& lang) const;

        // The window's title (1.4.0's UpdateTitleBar()).
        std::wstring Title(Lang const& lang) const;

        // The info tip of an item; nothing for those without one.
        std::optional<InfoTip> InfoTipOf(int item, Lang const& lang) const;

        std::uint64_t PercentBase() const noexcept;
        std::uint64_t ViewSize() const noexcept;

        // Zoom geometry.
        RectF FullRect() const noexcept;
        RectF CenterRect() const noexcept;
        RectF ItemRect(int item) const;
        Sector SliceSector(int item) const;
        Sector FullSector(float middle) const;
        Sector HubSector() const;

    private:
        void SetViewRoot(Folder* folder);
        Folder* FindView(std::vector<std::wstring> const& path) const;
        bool StaysInView(ViewHistory::Path const& path) const;
        Folder* TrailFolder() const;
        std::optional<ZoomMotion> ShowFolder(Folder* target, Selection selection);
        std::optional<ZoomMotion> GoTo(std::optional<ViewHistory::Path> const& path);
        std::optional<ZoomMotion> Motion(RectF from, RectF to) const;
        std::optional<ZoomMotion> Motion(Sector from, Sector to) const;

        std::unique_ptr<FolderTree> tree_;
        std::unique_ptr<FolderTree> preview_;
        Folder* viewRoot_ = nullptr;
        int viewDepth_ = 0;
        ViewHistory history_;
        Settings settings_;
        MapGeometry geometry_;
        bool showFreeSpace_ = true;

        TreemapLayout layout_;
        PieLayout pie_;
        Selection selection_;
        int selectedItem_ = -1;
        int hoverItem_ = -1;
        std::vector<int> hoverChain_;
        std::vector<int> chainScratch_;
    };
}
