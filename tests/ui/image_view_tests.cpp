#include "image_view.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace {

using namespace visionpro;

constexpr core::PixelFormat mono8{core::PixelDepth::UInt8, core::ChannelOrder::Mono};

template<class T>
T take(core::Result<T> result)
{
    if (const auto* diagnostic = std::get_if<core::Diagnostic>(&result)) {
        throw std::runtime_error(diagnostic->message);
    }
    return std::get<T>(std::move(result));
}

core::CoordinateSystemId frame(std::string_view name)
{
    return take(core::SchemaIdFactory::parse<core::CoordinateSystemId>(name));
}

core::ImageHandle cropped_bgr()
{
    std::array<std::byte, 48> source;
    source.fill(std::byte{238});
    for (std::size_t y = 0; y < 3; ++y) {
        for (std::size_t x = 0; x < 4; ++x) {
            const auto pixel = 16 * y + 3 * x;
            source[pixel] = static_cast<std::byte>(10 * x + y);
            source[pixel + 1] = static_cast<std::byte>(60 + 10 * x + y);
            source[pixel + 2] = static_cast<std::byte>(120 + 10 * x + y);
        }
    }
    const auto root = take(core::ImageHandle::copy({4, 3, 16, {core::PixelDepth::UInt8, core::ChannelOrder::BGR}},
        source, frame("root")));
    return take(root.crop({1, 1, 2, 2}, frame("cropped")));
}

core::ImageHandle cropped_mono(const core::PixelRect& rectangle)
{
    const std::vector<std::byte> pixels(96 * 72, std::byte{40});
    const auto root = take(core::ImageHandle::copy({96, 72, 96, mono8}, pixels, frame("root")));
    return take(root.crop(rectangle, frame("cropped")));
}

void move_pointer(QWidget* viewport, const QPoint& position, Qt::MouseButtons buttons = Qt::NoButton)
{
    QMouseEvent move(QEvent::MouseMove, position, viewport->mapToGlobal(position), Qt::NoButton, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(viewport, &move);
}

QImage render_viewport(ui::ImageView& view)
{
    QImage raster(view.viewport()->size(), QImage::Format_ARGB32);
    view.viewport()->render(&raster);
    return raster;
}

std::vector<QPainterPath> visible_paths(const ui::ImageView& view)
{
    std::vector<QPainterPath> paths;
    for (const auto* item : view.scene()->items(Qt::AscendingOrder)) {
        const auto* outline = qgraphicsitem_cast<const QGraphicsPathItem*>(item);
        if (outline && outline->isVisible() && !outline->path().isEmpty()) { paths.push_back(outline->path()); }
    }
    return paths;
}

class ImageViewTests final : public QObject {
    Q_OBJECT

private slots:
    void owns_and_displays_strided_cropped_bgr();
    void aligns_effective_mask_and_geometry_in_the_crop();
    void preserves_business_coordinates_through_zoom_pan_and_draw();
};

void ImageViewTests::owns_and_displays_strided_cropped_bgr()
{
    const auto converted = take(ui::to_display_image(cropped_bgr()));
    QCOMPARE(converted.size(), QSize(2, 2));
    QCOMPARE(converted.pixelColor(0, 0), QColor(131, 71, 11));
    QCOMPARE(converted.pixelColor(1, 0), QColor(141, 81, 21));
    QCOMPARE(converted.pixelColor(0, 1), QColor(132, 72, 12));
    QCOMPARE(converted.pixelColor(1, 1), QColor(142, 82, 22));

    ui::ImageView view;
    view.resize(360, 280);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    take(view.set_image(cropped_bgr()));
    const QPoint position = view.mapFromScene(QPointF(1, 1));
    const auto pixel = view.pixel_at(position);
    QVERIFY(pixel.has_value());
    QCOMPARE(pixel->x, std::size_t{1});
    QCOMPARE(pixel->y, std::size_t{1});
    QCOMPARE(pixel->root_x, std::size_t{2});
    QCOMPARE(pixel->root_y, std::size_t{2});
    QCOMPARE(pixel->order, core::ChannelOrder::BGR);
    QCOMPARE(pixel->samples[0], std::uint8_t{22});
    QCOMPARE(pixel->samples[1], std::uint8_t{82});
    QCOMPARE(pixel->samples[2], std::uint8_t{142});
    QCOMPARE(render_viewport(view).pixelColor(position), QColor(142, 82, 22));

    QSignalSpy pixel_changed(&view, &ui::ImageView::pixel_changed);
    move_pointer(view.viewport(), position);
    QCOMPARE(pixel_changed.size(), qsizetype{1});
    const auto description = pixel_changed.takeFirst().front().toString();
    QVERIFY(description.contains(QStringLiteral("Pixel (1, 1)")));
    QVERIFY(description.contains(QStringLiteral("Root (2, 2)")));
    QVERIFY(description.contains(QStringLiteral("B 22, G 82, R 142")));
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(view.viewport(), &leave);
    QCOMPARE(pixel_changed.takeFirst().front().toString(), QString{});

    const auto incompatible = take(core::ImageHandle::copy({1, 1, 2, {core::PixelDepth::UInt16, core::ChannelOrder::Mono}},
        std::array{std::byte{1}, std::byte{2}}, frame("other")));
    const auto failed = view.set_image(incompatible);
    QVERIFY(std::holds_alternative<core::Diagnostic>(failed));
    QCOMPARE(std::get<core::Diagnostic>(failed).code, core::ErrorCode::UnsupportedPixelFormat);
    QCOMPARE(render_viewport(view).pixelColor(position), QColor(142, 82, 22));
    view.clear_image();
    QVERIFY(!view.pixel_at(position));
}

void ImageViewTests::aligns_effective_mask_and_geometry_in_the_crop()
{
    const auto image = cropped_mono({2, 3, 9, 9});
    const auto placed = frame("placed");
    const auto& target = image.coordinate_frame();
    const auto px = core::MeasurementUnit::Pixel;
    const core::Transform2D correction{placed, target, px, px, core::TransformModel::Similarity,
        {1, 0, 1, 0, 1, 0, 0, 0, 1}};
    const core::RegionDefinition region{
        take(core::IdFactory::parse<core::RegionId>("00000000-0000-4000-8000-000000000001")), target, px,
        core::RegionPrimitive{placed, px, core::RotatedRect{4, 4, 4, 2, std::numbers::pi / 4}, 0, correction},
        {core::RegionPrimitive{target, px, core::AxisAlignedRect{5, 4, 1, 1}}}};
    // Fixed expected V08 members after shifting V07 and excluding (5,4).
    const std::array members{QPoint(4, 3), QPoint(5, 3), QPoint(4, 4), QPoint(6, 4), QPoint(5, 5), QPoint(6, 5)};
    std::array<std::byte, 9 * 11> pixels;
    pixels.fill(std::byte{1});
    for (const auto& member : members) {
        pixels[static_cast<std::size_t>(member.y() * 11 + member.x())] = std::byte{128};
    }
    const core::RasterMask mask{take(image.copy_with_pixels({9, 9, 11, mono8}, pixels)),
        {target, 9, 9}, {core::MaskValueMode::AtLeastThreshold, 128}};
    const std::array blobs{ui::BlobAnnotation{{4, 3, 3, 3}, {target, px, 5, 4}, 6}};

    ui::ImageView view;
    view.resize(460, 460);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    take(view.set_image(image));
    take(view.set_overlays(std::nullopt, mask));
    const auto member_layer = render_viewport(view);
    for (int y = 0; y < 9; ++y) {
        for (int x = 0; x < 9; ++x) {
            const auto color = member_layer.pixelColor(view.mapFromScene(QPointF(x, y)));
            const bool inside = std::ranges::find(members, QPoint(x, y)) != members.end();
            if (inside) { QVERIFY(color.green() > 80); }
            else { QCOMPARE(color, QColor(40, 40, 40)); }
        }
    }

    take(view.set_overlays(region, mask, blobs));
    const auto outlines = visible_paths(view);
    QCOMPARE(outlines.size(), std::size_t{3});
    const auto a = 1 / std::sqrt(2.0);
    const auto first = outlines[0].elementAt(0);
    const auto second = outlines[0].elementAt(1);
    QVERIFY(std::abs(first.x - (5 - a)) < 1e-9 && std::abs(first.y - (4 - 3 * a)) < 1e-9);
    QVERIFY(std::abs(second.x - (5 + 3 * a)) < 1e-9 && std::abs(second.y - (4 + a)) < 1e-9);
    QVERIFY(!outlines[0].contains(QPointF(3.1, 2.1)));
    QCOMPARE(outlines[1].boundingRect(), QRectF(5, 4, 1, 1));
    QCOMPARE(outlines[2].boundingRect(), QRectF(3.5, 2.5, 3, 3));
    const auto center_position = view.mapFromScene(QPointF(5, 4));
    const auto center_color = render_viewport(view).pixelColor(center_position);
    QVERIFY(center_color.blue() > 200);
    QCOMPARE(view.pixel_at(center_position)->samples[0], std::uint8_t{40});

    auto invalid_mask = mask;
    invalid_mask.mapping.target_frame = frame("other");
    QCOMPARE(std::get<core::Diagnostic>(view.set_overlays(std::nullopt, invalid_mask)).code,
        core::ErrorCode::CoordinateMismatch);
    auto deferred_mask = mask;
    deferred_mask.mapping.transform = core::Transform2D{target, target, px, px};
    deferred_mask.mapping.sampling = core::MaskSampling::Nearest;
    QCOMPARE(std::get<core::Diagnostic>(view.set_overlays(std::nullopt, deferred_mask)).code, core::ErrorCode::InvalidMask);

    auto unsupported = region;
    std::get<core::RegionPrimitive>(unsupported.source).transform->valid_domain = core::TransformDomain{0, 0, 8, 8};
    QCOMPARE(std::get<core::Diagnostic>(view.set_overlays(unsupported, mask)).code, core::ErrorCode::UnsupportedConversion);
    QCOMPARE(visible_paths(view), outlines);
    QCOMPARE(render_viewport(view).pixelColor(center_position), center_color);

    auto sector = region;
    auto& shape = std::get<core::RegionPrimitive>(sector.source);
    shape.geometry = core::SectorRingRegion{4, 4, 1, 3, -std::numbers::pi / 4, std::numbers::pi / 2};
    take(view.set_overlays(sector, {}));
    const auto sector_path = visible_paths(view).front();
    QVERIFY(sector_path.contains({7, 4}));
    QVERIFY(!sector_path.contains({5.5, 4})); // Inside the translated hole.
    QVERIFY(!sector_path.contains({4, 3}));   // Outside the positive sweep.
}

void ImageViewTests::preserves_business_coordinates_through_zoom_pan_and_draw()
{
    const auto image = cropped_mono({5, 7, 80, 60});
    ui::ImageView view;
    view.resize(480, 360);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    take(view.set_image(image));
    const auto fitted_scale = view.transform().m11();
    const auto cursor = view.mapFromScene(QPointF(33, 21));
    const auto fixed_coordinate = view.image_coordinates(cursor);
    QVERIFY(fixed_coordinate.has_value());

    auto* viewport = view.viewport();
    QWheelEvent wheel(cursor, viewport->mapToGlobal(cursor), {}, QPoint(0, 240), Qt::NoButton,
        Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(viewport, &wheel);
    QVERIFY(view.transform().m11() > fitted_scale);
    const auto after_zoom = view.image_coordinates(cursor);
    QVERIFY(after_zoom.has_value());
    QVERIFY(QLineF(*after_zoom, *fixed_coordinate).length() < 3 / view.transform().m11());

    view.set_rectangle_drawing(true);
    QSignalSpy rectangle_drawn(&view, &ui::ImageView::rectangle_drawn);
    const auto pan_end = cursor + QPoint(32, -20);
    QTest::mousePress(viewport, Qt::MiddleButton, Qt::NoModifier, cursor);
    move_pointer(viewport, pan_end, Qt::MiddleButton);
    QTest::mouseRelease(viewport, Qt::MiddleButton, Qt::NoModifier, pan_end);
    const auto after_pan = view.image_coordinates(cursor);
    QVERIFY(after_pan.has_value());
    QVERIFY(after_pan->x() < after_zoom->x() - 2);
    QVERIFY(after_pan->y() > after_zoom->y() + 1);
    QVERIFY(rectangle_drawn.isEmpty());

    const auto origin = view.mapFromScene(QPointF(24, 19));
    const auto end = view.mapFromScene(QPointF(40, 30));
    const auto crop_pixel = view.pixel_at(origin);
    QVERIFY(crop_pixel.has_value());
    QCOMPARE(crop_pixel->x, std::size_t{24});
    QCOMPARE(crop_pixel->y, std::size_t{19});
    QCOMPARE(crop_pixel->root_x, std::size_t{29});
    QCOMPARE(crop_pixel->root_y, std::size_t{26});
    QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, end);
    move_pointer(viewport, origin, Qt::LeftButton);
    QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, origin);
    QCOMPARE(rectangle_drawn.size(), qsizetype{1});
    const auto actual = rectangle_drawn.takeFirst().front().toRectF();
    const auto scene_rectangle = QRectF(view.mapToScene(origin), view.mapToScene(end)).normalized();
    QCOMPARE(actual, scene_rectangle);
    QVERIFY(QLineF(actual.topLeft(), QPointF(24, 19)).length() < 1 / view.transform().m11());
    QVERIFY(QLineF(actual.bottomRight(), QPointF(40, 30)).length() < 1 / view.transform().m11());
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier, origin);
    QVERIFY(rectangle_drawn.isEmpty());

    view.fit_image();
    QCOMPARE(view.transform().m11(), fitted_scale);
    QVERIFY(QLineF(*view.image_coordinates(cursor), *fixed_coordinate).length() < 1 / fitted_scale);
}

} // namespace

QTEST_MAIN(ImageViewTests)
#include "image_view_tests.moc"
