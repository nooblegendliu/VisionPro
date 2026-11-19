#pragma once

#include <visionpro/core/geometry.hpp>

#include <QGraphicsView>
#include <QImage>
#include <QPolygonF>

#include <array>
#include <cstdint>

class QGraphicsPathItem;
class QGraphicsPixmapItem;
class QGraphicsRectItem;

namespace visionpro::ui {

struct BlobAnnotation {
    core::AxisAlignedRect bounds;
    core::Point2D centroid;
    std::int64_t area_pixels;
};

struct ImagePixel {
    std::size_t x;
    std::size_t y;
    std::size_t root_x;
    std::size_t root_y;
    core::ChannelOrder order;
    std::array<std::uint8_t, 3> samples;
};

// Copies each visible row into Qt-owned storage in source channel order.
[[nodiscard]] core::Result<QImage> to_display_image(const core::ImageHandle& image);

class ImageView final : public QGraphicsView {
    Q_OBJECT

public:
    enum class DrawingMode { Off, Rectangle, Vertex };
    explicit ImageView(QWidget* parent = nullptr);

    [[nodiscard]] core::Result<std::monostate> set_image(const core::ImageHandle& image, const QList<QRgb>& palette = {});
    [[nodiscard]] core::Result<std::monostate> recolor_labels(const QList<QRgb>& palette);
    [[nodiscard]] core::Result<std::monostate> set_overlays(
        const std::optional<core::RegionDefinition>& effective_region,
        const std::optional<core::RasterMask>& member_mask,
        std::span<const BlobAnnotation> blobs = {});
    void clear_image();
    void fit_image();
    void set_drawing_mode(DrawingMode mode);
    void set_rectangle_drawing(bool enabled) { set_drawing_mode(enabled ? DrawingMode::Rectangle : DrawingMode::Off); }

    // Scene coordinates coincide with local Pixel coordinates. Integer positions are pixel centers.
    [[nodiscard]] std::optional<QPointF> image_coordinates(const QPoint& viewport_position) const;
    [[nodiscard]] std::optional<ImagePixel> pixel_at(const QPoint& viewport_position) const;

signals:
    void pixel_changed(const QString& pixel);
    void rectangle_drawn(const QRectF& rectangle);
    void vertex_picked(const QPointF& position);

protected:
    void drawForeground(QPainter* painter, const QRectF& rect) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool viewportEvent(QEvent* event) override;

private:
    enum class Drag { None, Pan, Rectangle };

    [[nodiscard]] QRectF image_bounds() const;
    [[nodiscard]] QPointF rectangle_point(const QPoint& viewport_position) const;
    void clear_overlays();
    void update_pixel(const QPoint& viewport_position);
    void finish_gesture();
    void update_cursor();

    std::optional<core::ImageHandle> image_;
    QImage labels_;
    QGraphicsPixmapItem* image_item_;
    QGraphicsPixmapItem* mask_item_;
    QGraphicsPathItem* inclusion_item_;
    QGraphicsPathItem* exclusions_item_;
    QGraphicsPathItem* blob_bounds_item_;
    QGraphicsRectItem* drawing_item_;
    QPolygonF blob_centroids_;
    bool fit_on_resize_{true};
    double fitted_scale_{1};
    DrawingMode drawing_mode_{DrawingMode::Off};
    Drag drag_{Drag::None};
    Qt::MouseButton drag_button_{Qt::NoButton};
    QPoint pan_position_;
    QPointF rectangle_start_;
};

} // namespace visionpro::ui
