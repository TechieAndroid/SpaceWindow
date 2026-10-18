// SpaceWindow (Qt) - the map's widget: draws it, plays the zoom animation,
// shows the tips, and reports the mouse and the keys.
//
// The Qt counterpart of the WinUI window's SwapChainPanel.  Positions it
// reports are in the map's pixels (the screen's physical pixels).
#pragma once

#include <chrono>
#include <functional>
#include <optional>

#include <QImage>
#include <QTimer>
#include <QWidget>

#include "MapRenderer.h"

class QFrame;
class QLabel;

namespace sm::qt
{
    class MapWidget : public QWidget
    {
        Q_OBJECT

    public:
        explicit MapWidget(QWidget* parent = nullptr);

        // What to draw, asked for whenever the map is drawn.
        void SetSource(std::function<MapScene()> scene, std::function<MapOverlay()> overlay);

        MapRenderer& Renderer() noexcept { return renderer_; }

        // For tests: the map as last drawn, without the overlay, in physical pixels.
        QImage const& Base() const noexcept { return base_; }

        // The map's size in pixels and the screen's scale.
        int PixelWidth() const noexcept;
        int PixelHeight() const noexcept;
        double Scale() const noexcept;

        // The cached map must be drawn again (new layout, colours or language).
        void InvalidateBase();
        // The overlay changed (selection, hover).
        void Refresh();

        void StartZoom(ZoomMotion const& motion);
        void StopZoom();

        // Plays the background animation the scene asks for (frames at up to
        // 60 a second), or holds it still on its last frame.
        void SetAnimating(bool animating);
        bool IsAnimating() const noexcept { return frames_.isActive(); }

        // Tips, placed in the widget's own coordinates (DIPs).
        void ShowNameTip(QString const& text, QColor const& face, QColor const& border, QColor const& textColor,
                         QPointF at, bool centered);
        void ShowInfoTip(QString const& name, QString const& path, QString const& details, QIcon const& icon,
                         QPointF at);
        void HideTips();

    Q_SIGNALS:
        void GeometryChanged();  // the size in pixels or the scale
        void PointerMoved(QPointF pixel);
        void PointerLeft();
        void Pressed(QPointF pixel);
        void DoubleClicked(QPointF pixel);
        void ContextMenuAt(QPointF pixel, QPoint global);
        void KeyPressed(int key, Qt::KeyboardModifiers modifiers, bool& handled);

    protected:
        void paintEvent(QPaintEvent* event) override;
        void resizeEvent(QResizeEvent* event) override;
        void moveEvent(QMoveEvent* event) override;
        void changeEvent(QEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void leaveEvent(QEvent* event) override;
        void mousePressEvent(QMouseEvent* event) override;
        void mouseDoubleClickEvent(QMouseEvent* event) override;
        void contextMenuEvent(QContextMenuEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;
        bool event(QEvent* event) override;

    private:
        QPointF ToPixel(QPointF position) const;
        QRect DevicePixels(double scale) const;
        QSize WantedPixels(double scale) const { return DevicePixels(scale).size(); }
        void CheckGeometry();
        void PlaceTip(QWidget* tip, QPointF at, bool centered);

        MapRenderer renderer_;
        std::function<MapScene()> scene_;
        std::function<MapOverlay()> overlay_;
        QImage base_;  // the map, in physical pixels; the overlay is drawn over it on the screen
        bool baseValid_ = false;
        bool baseClear_ = false;  // drawn without its background, for an animation to show through
        int pixelWidth_ = 0;
        int pixelHeight_ = 0;
        double scale_ = 0.0;

        std::optional<ZoomMotion> zoom_;
        std::chrono::steady_clock::time_point zoomStart_;
        QTimer animation_;
        QTimer frames_;  // the background animation's

        QLabel* nameTip_ = nullptr;
        QFrame* infoTip_ = nullptr;
        QLabel* infoIcon_ = nullptr;
        QLabel* infoName_ = nullptr;
        QLabel* infoPath_ = nullptr;
        QLabel* infoDetails_ = nullptr;
    };
}
