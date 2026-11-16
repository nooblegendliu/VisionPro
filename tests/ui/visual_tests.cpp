#include "control_scene.hpp"
#include "editor_window.hpp"
#include "image_panel.hpp"
#include "tool_catalog.hpp"
#include "visual_preview.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QLineEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUndoStack>

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace infrastructure;
using namespace workflow;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}

template<class T> T id(std::string_view suffix)
{ return take(IdFactory::parse<T>("5ca4f87a-ea1e-4d62-89b0-" + std::string(suffix))); }

template<class T> T key(std::string_view name) { return take(SchemaIdFactory::parse<T>(name)); }

const fs::path fixture{VISIONPRO_S4_DIRECTORY};
const auto workflow_id = id<WorkflowId>("000000000103");
const auto reader_id = id<NodeId>("000000001301");
const auto threshold_id = id<NodeId>("000000001303");
const auto blob_id = id<NodeId>("000000001304");
const auto frame_id = key<CoordinateSystemId>("s4.sample.rotated");
const auto roi_id = key<ParameterId>("roi");
const ResourceReference image_resource{id<ResourceId>("000000000202"), ResourceKind::Image, {}};

const RegionDefinition& region(const Solution& solution, const NodeId& node_id)
{
    const auto* node = find_node(*find_workflow(solution, workflow_id), node_id);
    const auto binding = std::ranges::find(node->bindings, BindingTarget{roi_id}, &Binding::target);
    const auto& value = std::get<ConfigValue>(binding->sources.front());
    const auto* effective = value.type().kind() == TypeKind::Optional ? value.optional_value() : &value;
    return *effective->as_region2d();
}

QList<QPoint> members(const RasterMask& mask)
{
    QList<QPoint> points;
    for (std::size_t y = 0; y < mask.pixels.height(); ++y) {
        const auto row = take(mask.pixels.row(y));
        for (std::size_t x = 0; x < row.size(); ++x) {
            if (row[x] != std::byte{0}) { points.push_back({static_cast<int>(x), static_cast<int>(y)}); }
        }
    }
    return points;
}

NodeResult run_result(const RunHandle& run, const RunProgress& progress, const NodeId& node)
{
    const auto call = std::ranges::find(progress.nodes, node, &RunNodeProgress::node);
    if (call == progress.nodes.end() || !call->invocation.has_value()) {
        throw std::runtime_error("The fixture node did not run.");
    }
    return take(run.lookup(*call->invocation)).value();
}

template<class Edit> bool apply_roi_edit(QPushButton& button, const Edit& edit)
{
    bool accepted = false;
    QTimer action;
    action.setSingleShot(true);
    QObject::connect(&action, &QTimer::timeout, &button, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { return; }
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (dialog->objectName() == QStringLiteral("roiEditor") && buttons && edit(*dialog)) {
            buttons->button(QDialogButtonBox::Ok)->click();
            accepted = dialog->result() == QDialog::Accepted;
        }
        if (!accepted) { dialog->reject(); }
    });
    action.start(0);
    button.click();
    return accepted;
}

class VisualTests final : public QObject {
    Q_OBJECT

private slots:
    void fixed_geometry_matches_s4_outputs();
    void mask_resources_and_boundary_policy();
    void window_previews_and_edits_symbolic_roi();
};

void VisualTests::fixed_geometry_matches_s4_outputs()
{
    const auto registry = take(app::make_tool_registry());
    const auto document = take(load_solution(fixture, registry));
    const auto& solution = document.solution();
    const auto image = take(app::read_preview_resource(solution, image_resource, frame_id, true, fixture));
    const auto rotated = take(app::preview_roi(image, solution, workflow_id, threshold_id, roi_id,
        region(solution, threshold_id), fixture));
    const auto inherited = take(app::preview_roi(image, solution, workflow_id, blob_id, roi_id,
        region(solution, blob_id), fixture));
    const QList<QPoint> rotated_points{{3, 3}, {4, 3}, {3, 4}, {4, 4}, {5, 4}, {4, 5}, {5, 5}};
    const QList<QPoint> inherited_points{{4, 3}, {5, 3}, {4, 4}, {6, 4}, {5, 5}, {6, 5}};
    QCOMPARE(rotated.member_count, 7u);
    QCOMPARE(members(rotated.mask), rotated_points);
    QCOMPARE(inherited.member_count, 6u);
    QCOMPARE(members(inherited.mask), inherited_points);
    QVERIFY(inherited.effective_region && !inherited.outline_issue);
    QCOMPARE(inherited.effective_region->exclusions.size(), 1u);

    const auto execution = take(open_solution_execution(document, registry));
    const auto request = take(RunRequest::create(execution.snapshot, WorkflowTarget{workflow_id}, {},
        {TriggerKind::Manual, key<SchemaId>("s54.visual_fixture")}));
    const auto submission = execution.engine->submit(request);
    const auto* run = std::get_if<RunHandle>(&submission);
    QVERIFY(run);
    const auto waited = take(run->wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 10s));
    QCOMPARE(waited.outcome, RunWaitOutcome::Satisfied);
    QCOMPARE(waited.progress.status, ExecutionStatus::Succeeded);
    const auto input = run_result(*run, waited.progress, reader_id);
    const auto roi = run_result(*run, waited.progress, threshold_id);
    const auto blob = run_result(*run, waited.progress, blob_id);
    const auto& mask_data = std::get<DataValue>(roi.output(key<OutputId>("roi_mask"))->content);
    const auto& components = std::get<DataValue>(blob.output(key<OutputId>("components"))->content);
    QCOMPARE(members(*mask_data.as_raster_mask()), members(rotated.mask));
    QCOMPARE(components.elements()->size(), 1u);
    const auto& component = components.elements()->front();
    QCOMPARE(*component.field(key<FieldId>("area_pixels"))->as_int64(), 6);
    const auto* centroid = component.field(key<FieldId>("centroid"))->as_point2d();
    QCOMPARE(QPointF(centroid->x, centroid->y), QPointF(5, 4));

    ImagePanel panel;
    take(panel.present_result(input, key<OutputId>("image"), &roi, &blob));
    QVERIFY(panel.frame() && panel.frame()->coordinate_frame() == frame_id);
    const auto items = panel.findChild<ImageView*>()->scene()->items();
    QVERIFY(std::ranges::any_of(items, [](const QGraphicsItem* item) {
        const auto* path = dynamic_cast<const QGraphicsPathItem*>(item);
        return path && path->path().boundingRect() == QRectF(3.5, 2.5, 3, 3);
    }));
    auto foreign_data = blob.data();
    foreign_data.identity.run.run = take(RuntimeIdFactory::next(foreign_data.identity.run.run));
    foreign_data.identity.invocation.workflow_run = foreign_data.identity.run.run;
    const auto foreign = take(NodeResult::create(foreign_data));
    QCOMPARE(std::get<Diagnostic>(panel.present_result(input, key<OutputId>("image"), &roi, &foreign)).code,
        ErrorCode::InvalidValue);
}

void VisualTests::mask_resources_and_boundary_policy()
{
    const auto registry = take(app::make_tool_registry());
    auto solution = take(load_solution(fixture, registry)).solution();
    const auto image = take(app::read_preview_resource(solution, image_resource, frame_id, true, fixture));
    auto roi = region(solution, threshold_id);
    roi.source = RegionPrimitive{frame_id, MeasurementUnit::Pixel, AxisAlignedRect{-1, 0, 3, 2}};
    const auto evaluate = [&] { return app::preview_roi(image, solution, workflow_id, threshold_id, roi_id, roi, fixture); };
    QCOMPARE(std::get<Diagnostic>(evaluate()).code, ErrorCode::ImageBounds);
    roi.boundary_policy = RegionBoundaryPolicy::Intersect;
    QCOMPARE(take(evaluate()).member_count, 4u);
    roi.source = RegionPrimitive{frame_id, MeasurementUnit::Pixel, AxisAlignedRect{10, 0, 2, 2}};
    QCOMPARE(std::get<Diagnostic>(evaluate()).code, ErrorCode::EmptyRegion);
    roi.empty_policy = EmptyRegionPolicy::ValidEmptyOutput;
    QCOMPARE(take(evaluate()).member_count, 0u);

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto path = temporary.filePath(QStringLiteral("members.png"));
    QImage mask_pixels(9, 9, QImage::Format_Grayscale8);
    mask_pixels.fill(0);
    mask_pixels.scanLine(2)[1] = 127;
    mask_pixels.scanLine(4)[3] = 255;
    QVERIFY(mask_pixels.save(path));
    const ResourceReference mask{id<ResourceId>("000000000204"), ResourceKind::Mask, {}};
    solution.resources.push_back({mask.id, "Preview mask", mask.kind, ExternalFileResource{
        {PathBase::Absolute, path.toStdString(), {}}, ResourceReadPolicy::PerInvocation, {}, {}}});
    MaskConfig geometry{mask, frame_id, 9, 9, {PixelDepth::UInt8, ChannelOrder::Mono},
        {frame_id, 9, 9, {}, MaskSampling::None}, {MaskValueMode::AtLeastThreshold, 128}};
    roi.boundary_policy = RegionBoundaryPolicy::Reject;
    roi.source = RegionPrimitive{frame_id, MeasurementUnit::Pixel, geometry};
    const QList<QPoint> expected{{3, 4}};
    QCOMPARE(members(take(evaluate()).mask), expected);
    geometry.mapping.target_width = 8;
    roi.source = RegionPrimitive{frame_id, MeasurementUnit::Pixel, geometry};
    QVERIFY(std::holds_alternative<Diagnostic>(evaluate()));

    EditorWindow window(solution, registry);
    window.show();
    take(window.findChild<ImagePanel*>()->set_frame(image));
    auto* scene = qobject_cast<ControlScene*>(qobject_cast<QGraphicsView*>(window.centralWidget())->scene());
    QVERIFY(scene && scene->select_workflow(workflow_id) && scene->reveal_node(threshold_id));
    QTRY_VERIFY(window.findChild<QPushButton*>(QStringLiteral("editRegion")));
    auto* edit = window.findChild<QPushButton*>(QStringLiteral("editRegion"));
    QVERIFY(apply_roi_edit(*edit, [](QDialog& dialog) {
        auto* shape = dialog.findChild<QComboBox*>(QStringLiteral("roiSource"));
        auto* resource = dialog.findChild<QComboBox*>(QStringLiteral("roiMaskResource"));
        if (!shape || !resource || resource->count() < 2) { return false; }
        shape->setCurrentText(QStringLiteral("Raster mask"));
        resource->setCurrentIndex(1);
        return true;
    }));
    const auto slot = [&] {
        const auto* node = find_node(*find_workflow(window.draft(), workflow_id), threshold_id);
        auto found = std::ranges::find(node->resources, key<SchemaId>("mask"), &NodeResourceBinding::slot);
        return found != node->resources.end() ? std::optional{found->resource} : std::nullopt;
    };
    const auto& configured = std::get<MaskConfig>(std::get<RegionPrimitive>(region(window.draft(), threshold_id).source).geometry);
    QVERIFY(configured.resource == mask);
    QVERIFY(slot() == mask);
    scene->undoStack().undo();
    QVERIFY(!slot() && scene->undoStack().isClean());
}

void VisualTests::window_previews_and_edits_symbolic_roi()
{
    const auto registry = take(app::make_tool_registry());
    const auto document = take(load_solution(fixture, registry));
    EditorWindow window(document.solution(), registry);
    window.show();
    window.open_document(fixture);
    QTRY_VERIFY(!window.file_busy());
    const auto resource = std::ranges::find(document.solution().resources, image_resource.id, &ResourceDefinition::id);
    QVERIFY(resource != document.solution().resources.end());
    const auto& image_path = std::get<ExternalFileResource>(resource->location).path.path;
    window.open_preview_image(fixture / fs::path{QString::fromStdString(image_path).toStdWString()}, frame_id, false);
    QTRY_VERIFY(!window.file_busy());
    const auto* panel = window.findChild<ImagePanel*>();
    QVERIFY(panel && panel->frame() && panel->frame()->format().order == ChannelOrder::BGR);
    window.open_preview_resource(image_resource, frame_id, true);
    QTRY_VERIFY(!window.file_busy());
    QVERIFY(panel->frame() && panel->frame()->format().order == ChannelOrder::Mono && panel->isVisible());
    auto* view = qobject_cast<QGraphicsView*>(window.centralWidget());
    QVERIFY(view);
    auto* scene = qobject_cast<ControlScene*>(view->scene());
    QVERIFY(scene && scene->select_workflow(workflow_id) && scene->reveal_node(blob_id));
    auto* preview = window.findChild<QAction*>(QStringLiteral("previewSelectedRoi"));
    QVERIFY(preview && preview->isEnabled());
    preview->trigger();
    QTRY_VERIFY(!window.file_busy());
    QVERIFY(window.statusBar()->currentMessage().startsWith(QStringLiteral("ROI contains 6 pixels")));
    const auto items = panel->findChild<ImageView*>()->scene()->items();
    QVERIFY(std::ranges::any_of(items, [](const QGraphicsItem* item) {
        const auto* bitmap = dynamic_cast<const QGraphicsPixmapItem*>(item);
        return bitmap && !bitmap->pixmap().isNull() && qAlpha(bitmap->pixmap().toImage().pixel(4, 3)) > 0
            && qAlpha(bitmap->pixmap().toImage().pixel(5, 4)) == 0;
    }));
    QVERIFY(scene->undoStack().isClean());

    const auto before = region(window.draft(), blob_id);
    QTRY_VERIFY(window.findChild<QPushButton*>(QStringLiteral("editRegion")));
    auto* edit = window.findChild<QPushButton*>(QStringLiteral("editRegion"));
    QVERIFY(apply_roi_edit(*edit, [](QDialog& dialog) {
        auto* correction = dialog.findChild<QLineEdit*>(QStringLiteral("roiCorrectionX"));
        if (!correction) { return false; }
        correction->setText(QStringLiteral("2"));
        return true;
    }));
    QVERIFY(scene->undoStack().canUndo());
    const auto& updated = region(window.draft(), blob_id);
    QVERIFY(updated.id == before.id && updated.exclusions == before.exclusions);
    const auto& inherited = std::get<InheritedRegion>(updated.source);
    QVERIFY(inherited.node == threshold_id && inherited.output == key<OutputId>("effective_roi"));
    QCOMPARE(inherited.transform->matrix[2], 2.0);
    scene->undoStack().undo();
    QVERIFY(region(window.draft(), blob_id) == before && scene->undoStack().isClean());
    scene->undoStack().redo();
    QCOMPARE(std::get<InheritedRegion>(region(window.draft(), blob_id).source).transform->matrix[2], 2.0);
}

} // namespace
} // namespace visionpro::ui

QTEST_MAIN(visionpro::ui::VisualTests)
#include "visual_tests.moc"
