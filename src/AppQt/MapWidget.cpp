// SpaceWindow (Qt) - the map's widget.
#include "MapWidget.h"

#include <cmath>

#include <QContextMenuEvent>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

#include "QtCompat.h"

namespace sm::qt
{
    namespace
    {
        constexpr auto kZoomDuration = std::chrono::milliseconds(280);
    }

    MapWidget::MapWidget(QWidget* parent) : QWidget(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setMinimumSize(200, 150);

        animation_.setInterval(std::chrono::milliseconds(15));
        connect(&animation_, &QTimer::timeout, this, [this] {
            if (zoom_ && std::chrono::steady_clock::now() - zoomStart_ >= kZoomDuration)
            {
                StopZoom();
            }
            update();
        });
        // A frame whenever the animation moved: at its own pace (Life's 9 steps a
        // second) up to 60 a second (the tunnel).
        frames_.setInterval(std::chrono::milliseconds(16));
        frames_.setTimerType(Qt::PreciseTimer);
        connect(&frames_, &QTimer::timeout, this, [this] {
            if (scene_ && renderer_.BackdropMoved(scene_(), pixelWidth_, pixelHeight_))
            {
                update();
            }
        });

        // The tips: a name in its box's colours, and a card of details.
        nameTip_ = new QLabel(this);
        nameTip_->setAttribute(Qt::WA_TransparentForMouseEvents);
        nameTip_->hide();

        infoTip_ = new QFrame(this);
        infoTip_->setObjectName(QStringLiteral("InfoTip"));
        infoTip_->setAttribute(Qt::WA_TransparentForMouseEvents);
        infoTip_->setAutoFillBackground(true);
        infoTip_->setBackgroundRole(QPalette::ToolTipBase);
        infoTip_->setForegroundRole(QPalette::ToolTipText);
        infoTip_->setFrameShape(QFrame::StyledPanel);
        infoTip_->setMaximumWidth(560);
        auto* const grid = new QGridLayout(infoTip_);
        grid->setContentsMargins(10, 8, 10, 8);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(2);
        infoIcon_ = new QLabel(infoTip_);
        infoIcon_->setFixedSize(32, 32);
        grid->addWidget(infoIcon_, 0, 0, 3, 1, Qt::AlignTop);
        infoName_ = new QLabel(infoTip_);
        QFont bold = infoName_->font();
        bold.setBold(true);
        infoName_->setFont(bold);
        infoName_->setWordWrap(true);
        infoPath_ = new QLabel(infoTip_);
        infoPath_->setWordWrap(true);
        infoDetails_ = new QLabel(infoTip_);
        for (QLabel* const label : { infoName_, infoPath_, infoDetails_ })
        {
            label->setForegroundRole(QPalette::ToolTipText);
            label->setTextFormat(Qt::PlainText);
        }
        QFont small = infoPath_->font();
        small.setPointSizeF(small.pointSizeF() * 0.9);
        infoPath_->setFont(small);
        infoDetails_->setFont(small);
        grid->addWidget(infoName_, 0, 1);
        grid->addWidget(infoPath_, 1, 1);
        grid->addWidget(infoDetails_, 2, 1);
        infoTip_->hide();
    }

    void MapWidget::SetSource(std::function<MapScene()> scene, std::function<MapOverlay()> overlay)
    {
        scene_ = std::move(scene);
        overlay_ = std::move(overlay);
        InvalidateBase();
    }

    int MapWidget::PixelWidth() const noexcept
    {
        return pixelWidth_;
    }

    int MapWidget::PixelHeight() const noexcept
    {
        return pixelHeight_;
    }

    double MapWidget::Scale() const noexcept
    {
        return scale_ > 0.0 ? scale_ : devicePixelRatioF();
    }

    void MapWidget::InvalidateBase()
    {
        baseValid_ = false;
        update();
    }

    void MapWidget::Refresh()
    {
        update();
    }

    // The device pixels the widget covers in its window.  Qt rounds each of its
    // edges to a pixel on its own: at a fractional scale the widget can cover
    // a row or a column more or less than its size times the scale, and its
    // first one is where its origin rounds to.
    QRect MapWidget::DevicePixels(double scale) const
    {
        QPoint const origin = mapTo(window(), QPoint(0, 0));
        int const left = qRound(origin.x() * scale);
        int const top = qRound(origin.y() * scale);
        return { left, top, std::max(1, qRound((origin.x() + width()) * scale) - left),
                 std::max(1, qRound((origin.y() + height()) * scale) - top) };
    }

    // Called when the size, the screen's scale or the font may have changed -
    // never while painting: the window lays the map out again on the signal.
    void MapWidget::CheckGeometry()
    {
        double const scale = devicePixelRatioF();
        QSize const pixels = WantedPixels(scale);
        bool const metrics = renderer_.SetScale(scale);  // the scale, or the application's font
        if (!metrics && scale == scale_ && pixels == QSize(pixelWidth_, pixelHeight_))
        {
            return;
        }
        pixelWidth_ = pixels.width();
        pixelHeight_ = pixels.height();
        scale_ = scale;
        baseValid_ = false;
        Q_EMIT GeometryChanged();
    }

    void MapWidget::resizeEvent(QResizeEvent* event)
    {
        QWidget::resizeEvent(event);
        CheckGeometry();
    }

    void MapWidget::moveEvent(QMoveEvent* event)
    {
        QWidget::moveEvent(event);
        CheckGeometry();  // its edges may round to other pixels
    }

    void MapWidget::changeEvent(QEvent* event)
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::PaletteChange)
        {
            InvalidateBase();
        }
        else if (event->type() == QEvent::FontChange)
        {
            CheckGeometry();  // the map's letters follow the application's font
        }
    }

    bool MapWidget::event(QEvent* event)
    {
        bool const moved = event->type() == QEvent::ScreenChangeInternal;
#if SPACEWINDOW_QT_API >= QT_VERSION_CHECK(6, 6, 0)
        bool const rescaled = event->type() == QEvent::DevicePixelRatioChange;
#else
        bool const rescaled = false;
#endif
        bool const handled = QWidget::event(event);
        if (moved || rescaled)
        {
            CheckGeometry();
        }
        return handled;
    }

    void MapWidget::paintEvent(QPaintEvent*)
    {
        double const scale = devicePixelRatioF();
        QRect const device = DevicePixels(scale);
        if (scale != scale_ || device.size() != QSize(pixelWidth_, pixelHeight_))
        {
            // Qt before 6.6 tells no one when only the screen's scale changed, and
            // nobody tells a widget its window moved what holds it: catch it here,
            // and lay the map out again once this frame is drawn.
            QMetaObject::invokeMethod(this, &MapWidget::CheckGeometry, Qt::QueuedConnection);
        }
        if (!scene_ || pixelWidth_ == 0)
        {
            QPainter(this).fillRect(rect(), palette().window());
            return;
        }
        MapScene const scene = scene_();
        bool const animated = scene.backdrop != Backdrop::None;
        if (!baseValid_ || baseClear_ != animated || base_.width() != pixelWidth_ || base_.height() != pixelHeight_)
        {
            if (base_.width() != pixelWidth_ || base_.height() != pixelHeight_)
            {
                base_ = QImage(pixelWidth_, pixelHeight_, QImage::Format_ARGB32_Premultiplied);
            }
            renderer_.DrawBase(base_, scene);
            baseValid_ = true;
            baseClear_ = animated;
        }
        if (animated)
        {
            renderer_.PrepareBackdrop(scene, pixelWidth_, pixelHeight_);
        }
        else
        {
            renderer_.ReleaseBackdrop();
        }
        MapOverlay overlay = overlay_ ? overlay_() : MapOverlay{};
        if (zoom_)
        {
            double const t = std::chrono::duration<double>(std::chrono::steady_clock::now() - zoomStart_) /
                             std::chrono::duration<double>(kZoomDuration);
            auto const eased = static_cast<float>(1.0 - std::pow(1.0 - std::min(t, 1.0), 3.0));  // ease-out
            if (zoom_->pie)
            {
                overlay.zoomSector = zoom_->SectorAt(eased);
            }
            else
            {
                overlay.zoomOutline = zoom_->RectAt(eased);
            }
        }
        // In the map's pixels, from the first device pixel the widget covers: the
        // image goes to the screen pixel for pixel, and the overlay is drawn over
        // it there - no copy of the map per frame.
        QPainter painter(this);
        QPointF const origin = QPointF(mapTo(window(), QPoint(0, 0))) * scale;  // in device pixels
        painter.setWorldTransform(QTransform(1.0 / scale_, 0.0, 0.0, 1.0 / scale_, (device.left() - origin.x()) / scale,
                                             (device.top() - origin.y()) / scale));
        if (animated)
        {
            renderer_.DrawBackdrop(painter, scene);  // every pixel
            painter.drawImage(QPointF(0.0, 0.0), base_);
        }
        else
        {
            painter.setCompositionMode(QPainter::CompositionMode_Source);  // opaque: copied, not blended
            painter.drawImage(QPointF(0.0, 0.0), base_);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        }
        renderer_.DrawOverlay(painter, scene, overlay);
    }

    void MapWidget::SetAnimating(bool animating)
    {
        if (animating == frames_.isActive())
        {
            return;
        }
        if (animating)
        {
            frames_.start();
        }
        else
        {
            frames_.stop();
        }
        update();
    }

    void MapWidget::StartZoom(ZoomMotion const& motion)
    {
        zoom_ = motion;
        zoomStart_ = std::chrono::steady_clock::now();
        animation_.start();
        update();
    }

    void MapWidget::StopZoom()
    {
        zoom_.reset();
        animation_.stop();
        update();
    }

    QPointF MapWidget::ToPixel(QPointF position) const
    {
        double const scale = Scale();
        QPointF const origin = QPointF(mapTo(window(), QPoint(0, 0))) * scale;
        QRect const device = DevicePixels(scale);
        return position * scale + origin - QPointF(device.topLeft());
    }

    void MapWidget::mouseMoveEvent(QMouseEvent* event)
    {
        Q_EMIT PointerMoved(ToPixel(event->position()));
    }

    void MapWidget::leaveEvent(QEvent* event)
    {
        QWidget::leaveEvent(event);
        Q_EMIT PointerLeft();
    }

    void MapWidget::mousePressEvent(QMouseEvent* event)
    {
        setFocus(Qt::MouseFocusReason);  // a click gives the map the keyboard
        if (event->button() == Qt::LeftButton)
        {
            Q_EMIT Pressed(ToPixel(event->position()));
        }
    }

    void MapWidget::mouseDoubleClickEvent(QMouseEvent* event)
    {
        if (event->button() == Qt::LeftButton)
        {
            Q_EMIT DoubleClicked(ToPixel(event->position()));
        }
    }

    void MapWidget::contextMenuEvent(QContextMenuEvent* event)
    {
        QPointF const position = event->reason() == QContextMenuEvent::Mouse ? QPointF(event->pos()) : QPointF(-1, -1);
        Q_EMIT ContextMenuAt(position.x() < 0 ? position : ToPixel(position), event->globalPos());
    }

    void MapWidget::keyPressEvent(QKeyEvent* event)
    {
        bool handled = false;
        Q_EMIT KeyPressed(event->key(), event->modifiers(), handled);
        if (!handled)
        {
            QWidget::keyPressEvent(event);
        }
    }

    // ---------------------------------------------------------------- tips

    void MapWidget::PlaceTip(QWidget* tip, QPointF at, bool centered)
    {
        tip->adjustSize();
        QSize const size = tip->sizeHint();
        tip->resize(size);
        double x = at.x();
        double y = at.y();
        if (centered)
        {
            x -= size.width() / 2.0;
            y -= size.height() / 2.0;
        }
        x = std::clamp(x, 0.0, std::max(0.0, static_cast<double>(width() - size.width())));
        y = std::clamp(y, 0.0, std::max(0.0, static_cast<double>(height() - size.height())));
        tip->move(static_cast<int>(x), static_cast<int>(y));
        tip->show();
        tip->raise();
    }

    void MapWidget::ShowNameTip(QString const& text, QColor const& face, QColor const& border, QColor const& textColor,
                                QPointF at, bool centered)
    {
        QFont font = renderer_.Font();
        font.setPixelSize(std::max(6, static_cast<int>(std::lround(font.pixelSize() / Scale()))));
        nameTip_->setFont(font);
        nameTip_->setText(text);
        nameTip_->setStyleSheet(QStringLiteral("QLabel { background: %1; color: %2; border: 1px solid %3; "
                                               "padding: 0px 3px 1px 3px; }")
                                    .arg(face.name(), textColor.name(), border.name()));
        PlaceTip(nameTip_, at, centered);
    }

    void MapWidget::ShowInfoTip(QString const& name, QString const& path, QString const& details, QIcon const& icon,
                                QPointF at)
    {
        infoName_->setText(name);
        infoName_->setVisible(!name.isEmpty());
        infoPath_->setText(path);
        infoPath_->setVisible(!path.isEmpty());
        infoDetails_->setText(details);
        infoDetails_->setVisible(!details.isEmpty());
        infoIcon_->setVisible(!icon.isNull());
        if (!icon.isNull())
        {
            infoIcon_->setPixmap(icon.pixmap(QSize(32, 32), devicePixelRatioF()));
        }
        PlaceTip(infoTip_, at, false);
    }

    void MapWidget::HideTips()
    {
        nameTip_->hide();
        infoTip_->hide();
    }
}
