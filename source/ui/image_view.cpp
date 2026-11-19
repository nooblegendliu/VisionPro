#include "image_view.hpp"

#include <QEvent>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

namespace visionpro::ui {
namespace {

using namespace core;

constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};
constexpr PixelFormat bgr8{PixelDepth::UInt8, ChannelOrder::BGR};
constexpr double pixel_edge = -0.5;

Diagnostic error(ErrorCode code, std::string message)
{
    return {code, Severity::Error, {}, std::move(message)};
}

Result<QImage> copy_rows(const ImageHandle& image, QImage::Format format)
{
    if (image.width() > static_cast<std::size_t>((std::numeric_limits<int>::max)())
        || image.height() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return error(ErrorCode::SizeOverflow, "Image dimensions exceed the Qt display range.");
    }
    QImage copy(static_cast<int>(image.width()), static_cast<int>(image.height()), format);
    if (copy.isNull()) {
        return error(ErrorCode::InvalidImage, "Qt could not allocate the display image.");
    }
    const auto stride = static_cast<std::size_t>(copy.bytesPerLine());
    for (int y = 0; y < copy.height(); ++y) {
        const auto row = image.row(static_cast<std::size_t>(y));
        if (const auto* invalid = std::get_if<Diagnostic>(&row)) { return *invalid; }
        const auto pixels = std::get<std::span<const std::byte>>(row);
        auto* destination = copy.scanLine(y);
        std::memcpy(destination, pixels.data(), pixels.size_bytes());
        std::memset(destination + pixels.size_bytes(), 0, stride - pixels.size_bytes());
    }
    return copy;
}

Result<QPixmap> mask_pixmap(const RasterMask& mask, const ImageHandle& image)
{
    const auto checked = validate_raster_mask(mask, image);
    if (const auto* invalid = std::get_if<Diagnostic>(&checked)) { return *invalid; }
    if (mask.mapping.transform) {
        return error(ErrorCode::InvalidMask, "Display requires an evaluated member mask in the current image's local Pixel coordinates.");
    }
    if (mask.pixels.format() != mono8) {
        return error(ErrorCode::UnsupportedPixelFormat, "The display member mask must use UInt8 Mono pixels.");
    }
    auto copied = copy_rows(mask.pixels, QImage::Format_Indexed8);
    if (const auto* invalid = std::get_if<Diagnostic>(&copied)) { return *invalid; }
    auto& indexed = std::get<QImage>(copied);
    constexpr QRgb member_color = qRgba(16, 211, 166, 100);
    QList<QRgb> palette(256, qRgba(0, 0, 0, 0));
    for (int i = 0; i < palette.size(); ++i) {
        const bool member = mask.valid_pixels.mode == MaskValueMode::NonZero
            ? i != 0 : i >= mask.valid_pixels.threshold;
        if (member) { palette[i] = member_color; }
    }
    indexed.setColorTable(palette);
    auto pixmap = QPixmap::fromImage(std::move(indexed));
    if (pixmap.isNull()) {
        return error(ErrorCode::InvalidMask, "Qt could not create the display member mask.");
    }
    return pixmap;
}

QRectF circle_bounds(double x, double y, double radius)
{
    return {x - radius, y - radius, 2 * radius, 2 * radius};
}

void add_ring(QPainterPath& path, double x, double y, double outer, double inner)
{
    path.addEllipse(circle_bounds(x, y, outer));
    if (inner > 0) { path.addEllipse(circle_bounds(x, y, inner)); }
}

Result<QPainterPath> outline(const RegionSource& source, const ImageHandle& image)
{
    const auto* primitive = std::get_if<RegionPrimitive>(&source);
    if (!primitive) {
        return error(ErrorCode::MissingRequiredValue, "The inherited region must be resolved before it can be displayed.");
    }
    if (primitive->transform
        && ((primitive->transform->model != TransformModel::Similarity && primitive->transform->model != TransformModel::Affine)
            || primitive->transform->valid_domain || primitive->transform->calibration)) {
        return error(ErrorCode::UnsupportedConversion, "Display requires an unbounded Similarity or Affine region correction.");
    }
    const auto& geometry = primitive->geometry;
    QPainterPath path;
    if (std::holds_alternative<EmptyRegionShape>(geometry) || std::holds_alternative<MaskConfig>(geometry)) {
        // Raster masks have no independent vector outline. Their membership is drawn separately.
        return QPainterPath{};
    }
    if (std::holds_alternative<FullImageRegion>(geometry)) {
        path.addRect(0, 0, static_cast<double>(image.width()), static_cast<double>(image.height()));
    } else if (const auto* aligned = std::get_if<AxisAlignedRect>(&geometry)) {
        path.addRect(aligned->x, aligned->y, aligned->width, aligned->height);
    } else if (const auto* rotated = std::get_if<RotatedRect>(&geometry)) {
        path.addRect(-rotated->width / 2, -rotated->height / 2, rotated->width, rotated->height);
        QTransform pose;
        pose.translate(rotated->center_x, rotated->center_y);
        pose.rotateRadians(rotated->angle_radians);
        path = pose.map(path);
    } else if (const auto* circle = std::get_if<CircleRegion>(&geometry)) {
        path.addEllipse(circle_bounds(circle->center_x, circle->center_y, circle->radius));
    } else if (const auto* ring = std::get_if<RingRegion>(&geometry)) {
        add_ring(path, ring->center_x, ring->center_y, ring->outer_radius, ring->inner_radius);
    } else if (const auto* sector = std::get_if<SectorRingRegion>(&geometry)) {
        const auto x = sector->center_x, y = sector->center_y;
        if (std::abs(sector->sweep_radians) == 2 * std::numbers::pi) {
            add_ring(path, x, y, sector->outer_radius, sector->inner_radius);
        } else {
            // Qt arc angles run towards -y. ROI angles run towards +y.
            constexpr auto degrees = -180.0 / std::numbers::pi;
            const auto angle = sector->start_radians;
            const auto start = std::atan2(std::sin(angle), std::cos(angle)) * degrees;
            const auto sweep = sector->sweep_radians * degrees;
            const auto outer = circle_bounds(x, y, sector->outer_radius);
            path.arcMoveTo(outer, start);
            path.arcTo(outer, start, sweep);
            if (sector->inner_radius > 0) {
                path.arcTo(circle_bounds(x, y, sector->inner_radius), start + sweep, -sweep);
            } else { path.lineTo(x, y); }
            path.closeSubpath();
        }
    } else if (const auto* polygon = std::get_if<PolygonRegion>(&geometry)) {
        const auto& first = polygon->vertices.front();
        path.moveTo(first.x, first.y);
        for (const auto& vertex : std::span(polygon->vertices).subspan(1)) { path.lineTo(vertex.x, vertex.y); }
        path.closeSubpath();
    } else {
        return error(ErrorCode::UnsupportedRegion, "This region shape has no display adapter.");
    }
    if (primitive->transform) {
        const auto& m = primitive->transform->matrix;
        path = QTransform(m[0], m[3], m[1], m[4], m[2], m[5]).map(path);
    }
    return path;
}

QPen overlay_pen(const QColor& color, double width, Qt::PenStyle style = Qt::SolidLine)
{
    QPen pen(color, width, style);
    pen.setCosmetic(true);
    return pen;
}

Result<std::monostate> validate_blob(const ImageHandle& image, const BlobAnnotation& blob)
{
    const auto& center = blob.centroid;
    if (center.coordinate_frame != image.coordinate_frame() || center.unit != MeasurementUnit::Pixel) {
        return error(ErrorCode::CoordinateMismatch, "Blob geometry must use the current image's local Pixel coordinates.");
    }
    const auto checked = validate_point(center);
    if (const auto* invalid = std::get_if<Diagnostic>(&checked)) { return *invalid; }
    const auto& b = blob.bounds;
    if (!std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.width) || !std::isfinite(b.height)
        || b.x < 0 || b.y < 0 || b.width < 1 || b.height < 1
        || b.x + b.width > static_cast<double>(image.width()) || b.y + b.height > static_cast<double>(image.height())
        || center.x < b.x || center.y < b.y || center.x > b.x + b.width - 1 || center.y > b.y + b.height - 1
        || blob.area_pixels <= 0 || static_cast<double>(blob.area_pixels) > b.width * b.height) {
        return error(ErrorCode::InvalidGeometry, "Blob bounds, centroid, or area do not match the image pixel extent.");
    }
    return std::monostate{};
}

} // namespace

core::Result<QImage> to_display_image(const core::ImageHandle& image)
{
    if (image.format() == mono8) { return copy_rows(image, QImage::Format_Grayscale8); }
    if (image.format() == bgr8) { return copy_rows(image, QImage::Format_BGR888); }
    return error(ErrorCode::UnsupportedPixelFormat, "Display supports UInt8 Mono and UInt8 BGR images.");
}

ImageView::ImageView(QWidget* parent)
    : QGraphicsView(parent)
{
    auto* display_scene = new QGraphicsScene(this);
    setScene(display_scene);
    setBackgroundBrush(QColor(25, 29, 36));
    setRenderHint(QPainter::Antialiasing);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTransformationAnchor(QGraphicsView::NoAnchor);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    viewport()->setMouseTracking(true);

    image_item_ = display_scene->addPixmap({});
    mask_item_ = display_scene->addPixmap({});
    image_item_->setOffset(pixel_edge, pixel_edge);
    mask_item_->setOffset(pixel_edge, pixel_edge);
    mask_item_->setZValue(1);
    inclusion_item_ = display_scene->addPath({}, overlay_pen(QColor(255, 209, 85), 2));
    inclusion_item_->setZValue(2);
    exclusions_item_ = display_scene->addPath({}, overlay_pen(QColor(251, 124, 135), 2, Qt::DashLine));
    exclusions_item_->setZValue(3);
    blob_bounds_item_ = display_scene->addPath({}, overlay_pen(QColor(123, 194, 255), 1.5));
    blob_bounds_item_->setZValue(4);
    drawing_item_ = display_scene->addRect({}, overlay_pen(QColor(245, 247, 251), 1, Qt::DashLine));
    drawing_item_->setZValue(5);
    drawing_item_->hide();
}

core::Result<std::monostate> ImageView::set_image(const core::ImageHandle& image, const QList<QRgb>& palette)
{
    if (!palette.empty() && (image.format() != mono8 || palette.size() != 256)) {
        return error(ErrorCode::InvalidImage, "Class display requires Mono8 labels and a 256-color palette.");
    }
    auto displayed = palette.empty() ? to_display_image(image) : copy_rows(image, QImage::Format_Indexed8);
    if (const auto* invalid = std::get_if<Diagnostic>(&displayed)) { return *invalid; }
    auto pixels = std::get<QImage>(std::move(displayed));
    if (!palette.empty()) { pixels.setColorTable(palette); }
    auto pixmap = palette.empty() ? QPixmap::fromImage(std::move(pixels)) : QPixmap::fromImage(pixels);
    if (pixmap.isNull()) {
        return error(ErrorCode::InvalidImage, "Qt could not create the display pixmap.");
    }
    image_ = image;
    labels_ = palette.empty() ? QImage{} : std::move(pixels);
    image_item_->setPixmap(pixmap);
    clear_overlays();
    finish_gesture();
    setSceneRect(image_bounds());
    fit_image();
    emit pixel_changed({});
    return std::monostate{};
}

core::Result<std::monostate> ImageView::recolor_labels(const QList<QRgb>& palette)
{
    if (labels_.isNull() || palette.size() != 256) { return error(ErrorCode::InvalidImage, "No class raster is displayed."); }
    labels_.setColorTable(palette);
    const auto pixmap = QPixmap::fromImage(labels_);
    if (pixmap.isNull()) { return error(ErrorCode::InvalidImage, "Qt could not recolor the class raster."); }
    image_item_->setPixmap(pixmap);
    return std::monostate{};
}

core::Result<std::monostate> ImageView::set_overlays(
    const std::optional<core::RegionDefinition>& effective_region,
    const std::optional<core::RasterMask>& member_mask,
    std::span<const BlobAnnotation> blobs)
{
    if (!image_) {
        if (!effective_region && !member_mask && blobs.empty()) { return std::monostate{}; }
        return error(ErrorCode::MissingRequiredValue, "Display an image before setting overlays.");
    }

    QPainterPath inclusion;
    QPainterPath exclusions;
    if (effective_region) {
        const auto& region = *effective_region;
        if (region.coordinate_frame != image_->coordinate_frame() || region.unit != MeasurementUnit::Pixel) {
            auto invalid = error(ErrorCode::CoordinateMismatch, "The effective region must use the current image's local Pixel coordinates.");
            invalid.location.region = region.id;
            return invalid;
        }
        const auto checked = validate_region(region);
        if (const auto* invalid = std::get_if<Diagnostic>(&checked)) { return *invalid; }
        auto source_outline = outline(region.source, *image_);
        if (auto* invalid = std::get_if<Diagnostic>(&source_outline)) {
            invalid->location.region = region.id;
            return *invalid;
        }
        inclusion = std::get<QPainterPath>(std::move(source_outline));
        for (const auto& exclusion : region.exclusions) {
            auto path = outline(exclusion, *image_);
            if (auto* invalid = std::get_if<Diagnostic>(&path)) {
                invalid->location.region = region.id;
                return *invalid;
            }
            exclusions.addPath(std::get<QPainterPath>(path));
        }
    }

    QPainterPath blob_bounds;
    QPolygonF centroids;
    if (blobs.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return error(ErrorCode::SizeOverflow, "Blob count exceeds the Qt display range.");
    }
    centroids.reserve(static_cast<qsizetype>(blobs.size()));
    for (const auto& blob : blobs) {
        const auto checked = validate_blob(*image_, blob);
        if (const auto* invalid = std::get_if<Diagnostic>(&checked)) { return *invalid; }
        const auto& b = blob.bounds;
        // Blob origins are pixel indices. The outline encloses the entire pixel footprint.
        blob_bounds.addRect(b.x + pixel_edge, b.y + pixel_edge, b.width, b.height);
        centroids.append({blob.centroid.x, blob.centroid.y});
    }

    QPixmap mask;
    if (member_mask) {
        auto converted = mask_pixmap(*member_mask, *image_);
        if (const auto* invalid = std::get_if<Diagnostic>(&converted)) { return *invalid; }
        mask = std::get<QPixmap>(std::move(converted));
    }
    inclusion_item_->setPath(inclusion);
    exclusions_item_->setPath(exclusions);
    blob_bounds_item_->setPath(blob_bounds);
    blob_centroids_ = std::move(centroids);
    mask_item_->setPixmap(mask);
    viewport()->update();
    return std::monostate{};
}

void ImageView::clear_image()
{
    image_.reset();
    labels_ = {};
    image_item_->setPixmap({});
    clear_overlays();
    finish_gesture();
    setSceneRect({});
    resetTransform();
    emit pixel_changed({});
}

void ImageView::fit_image()
{
    if (!image_) { return; }
    resetTransform();
    fitInView(image_bounds(), Qt::KeepAspectRatio);
    fitted_scale_ = transform().m11();
    fit_on_resize_ = true;
}

void ImageView::set_drawing_mode(DrawingMode mode)
{
    if (mode == drawing_mode_) { return; }
    drawing_mode_ = mode;
    finish_gesture();
}

std::optional<QPointF> ImageView::image_coordinates(const QPoint& viewport_position) const
{
    if (!image_ || !viewport()->rect().contains(viewport_position)) { return std::nullopt; }
    const auto position = mapToScene(viewport_position);
    const auto bounds = image_bounds();
    if (position.x() < bounds.left() || position.y() < bounds.top()
        || position.x() >= bounds.right() || position.y() >= bounds.bottom()) {
        return std::nullopt;
    }
    return position;
}

std::optional<ImagePixel> ImageView::pixel_at(const QPoint& viewport_position) const
{
    const auto position = image_coordinates(viewport_position);
    if (!position) { return std::nullopt; }
    const auto x = static_cast<std::size_t>(std::floor(position->x() - pixel_edge));
    const auto y = static_cast<std::size_t>(std::floor(position->y() - pixel_edge));
    const auto order = image_->format().order;
    const auto channels = order == ChannelOrder::Mono ? 1u : 3u;
    const auto row = image_->row(y);
    const auto* pixels = std::get_if<std::span<const std::byte>>(&row);
    if (!pixels || x >= image_->width()) { return std::nullopt; }
    ImagePixel pixel{x, y, x + image_->origin_x(), y + image_->origin_y(), order, {}};
    for (std::size_t channel = 0; channel < channels; ++channel) {
        pixel.samples[channel] = std::to_integer<std::uint8_t>((*pixels)[x * channels + channel]);
    }
    return pixel;
}

void ImageView::drawForeground(QPainter* painter, const QRectF& rect)
{
    QGraphicsView::drawForeground(painter, rect);
    if (blob_centroids_.isEmpty()) { return; }
    painter->save();
    auto pen = overlay_pen(QColor(123, 194, 255), 5);
    pen.setCapStyle(Qt::RoundCap);
    painter->setPen(pen);
    painter->drawPoints(blob_centroids_);
    painter->restore();
}

void ImageView::mousePressEvent(QMouseEvent* event)
{
    const auto button = event->button();
    const auto position = event->position().toPoint();
    if (image_ && drag_ == Drag::None) {
        if (button == Qt::LeftButton && drawing_mode_ == DrawingMode::Vertex) {
            if (const auto point = image_coordinates(position)) { emit vertex_picked(*point); }
            event->accept();
            return;
        }
        if (button == Qt::MiddleButton || (button == Qt::LeftButton && drawing_mode_ == DrawingMode::Off)) {
            drag_ = Drag::Pan;
            pan_position_ = position;
        } else if (button == Qt::LeftButton && drawing_mode_ == DrawingMode::Rectangle && image_coordinates(position)) {
            drag_ = Drag::Rectangle;
            rectangle_start_ = rectangle_point(position);
            drawing_item_->setRect(QRectF(rectangle_start_, rectangle_start_));
            drawing_item_->show();
        }
        if (drag_ != Drag::None) {
            drag_button_ = button;
            update_cursor();
            event->accept();
            return;
        }
    }
    QGraphicsView::mousePressEvent(event);
}

void ImageView::mouseMoveEvent(QMouseEvent* event)
{
    const auto position = event->position().toPoint();
    if (drag_ == Drag::Pan) {
        const auto delta = position - pan_position_;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        pan_position_ = position;
        fit_on_resize_ = false;
    } else if (drag_ == Drag::Rectangle) {
        drawing_item_->setRect(QRectF(rectangle_start_, rectangle_point(position)).normalized());
    } else {
        QGraphicsView::mouseMoveEvent(event);
    }
    update_pixel(position);
}

void ImageView::mouseReleaseEvent(QMouseEvent* event)
{
    if (drag_ != Drag::None && event->button() == drag_button_) {
        const auto point = event->position().toPoint();
        const bool drew_rectangle = drag_ == Drag::Rectangle;
        const auto rectangle = drew_rectangle
            ? QRectF(rectangle_start_, rectangle_point(point)).normalized() : QRectF{};
        finish_gesture();
        event->accept();
        update_pixel(point);
        if (drew_rectangle && !rectangle.isEmpty()) { emit rectangle_drawn(rectangle); }
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void ImageView::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->pixelDelta().y();
    if (!image_ || delta == 0) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    if (drag_ == Drag::None) {
        const auto position = event->position().toPoint();
        const auto target = mapToScene(position);
        const auto scale_before = transform().m11();
        const auto scale_after = std::clamp(scale_before * std::pow(1.2, delta / 120.0),
            fitted_scale_ / 16, fitted_scale_ * 32);
        const auto factor = scale_after / scale_before;
        scale(factor, factor);
        centerOn(mapToScene(viewport()->rect().center()) + target - mapToScene(position));
        fit_on_resize_ = false;
        update_pixel(position);
    }
    event->accept();
}

void ImageView::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    if (image_ && fit_on_resize_) { fit_image(); }
}

bool ImageView::viewportEvent(QEvent* event)
{
    if (event->type() == QEvent::Leave) { emit pixel_changed({}); }
    if (event->type() == QEvent::UngrabMouse && drag_ != Drag::None) { finish_gesture(); }
    return QGraphicsView::viewportEvent(event);
}

QRectF ImageView::image_bounds() const
{
    return {pixel_edge, pixel_edge, static_cast<double>(image_->width()), static_cast<double>(image_->height())};
}

QPointF ImageView::rectangle_point(const QPoint& viewport_position) const
{
    const auto position = mapToScene(viewport_position);
    return {std::clamp(position.x(), 0.0, static_cast<double>(image_->width())),
        std::clamp(position.y(), 0.0, static_cast<double>(image_->height()))};
}

void ImageView::clear_overlays()
{
    mask_item_->setPixmap({});
    inclusion_item_->setPath({});
    exclusions_item_->setPath({});
    blob_bounds_item_->setPath({});
    blob_centroids_.clear();
    viewport()->update();
}

void ImageView::update_pixel(const QPoint& viewport_position)
{
    const auto pixel = pixel_at(viewport_position);
    if (!pixel) {
        emit pixel_changed({});
        return;
    }
    auto description = tr("Pixel (%1, %2)")
        .arg(static_cast<qulonglong>(pixel->x)).arg(static_cast<qulonglong>(pixel->y));
    if (image_->coordinate_frame() != image_->root_frame()) {
        description += tr(" · Root (%1, %2)")
            .arg(static_cast<qulonglong>(pixel->root_x)).arg(static_cast<qulonglong>(pixel->root_y));
    }
    const auto& value = pixel->samples;
    description += pixel->order == ChannelOrder::Mono
        ? (labels_.isNull() ? tr(" · Gray %1") : tr(" · Class ID %1")).arg(static_cast<int>(value[0]))
        : tr(" · B %1, G %2, R %3").arg(static_cast<int>(value[0])).arg(static_cast<int>(value[1])).arg(static_cast<int>(value[2]));
    emit pixel_changed(description);
}

void ImageView::finish_gesture()
{
    drag_ = Drag::None;
    drag_button_ = Qt::NoButton;
    drawing_item_->hide();
    update_cursor();
}

void ImageView::update_cursor()
{
    if (!image_) { viewport()->unsetCursor(); }
    else if (drag_ == Drag::Pan) { viewport()->setCursor(Qt::ClosedHandCursor); }
    else { viewport()->setCursor(drawing_mode_ == DrawingMode::Off ? Qt::OpenHandCursor : Qt::CrossCursor); }
}

} // namespace visionpro::ui
