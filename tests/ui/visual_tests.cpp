#include "control_scene.hpp"
#include "editor_window.hpp"
#include "image_panel.hpp"
#include "run_image.hpp"
#include "run_panel.hpp"
#include "tool_catalog.hpp"
#include "visual_preview.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/workflow_session.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QUndoStack>

#include <algorithm>
#include <array>
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
const auto binary_source_id = id<NodeId>("000000001302");
const auto threshold_id = id<NodeId>("000000001303");
const auto blob_id = id<NodeId>("000000001304");
const auto judge_id = id<NodeId>("000000001305");
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

bool select_run_node(QTreeWidget& tree, const NodeId& node)
{
    const auto identity = QString::fromStdString(node.to_string());
    for (int row = 0; row < tree.topLevelItemCount(); ++row) {
        auto* item = tree.topLevelItem(row);
        if (item->toolTip(0) == identity) {
            tree.setCurrentItem(item);
            return true;
        }
    }
    return false;
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
    void edits_and_reloads_shape_members_data();
    void edits_and_reloads_shape_members();
    void fits_newly_bound_calibration_file();
    void window_previews_and_edits_symbolic_roi();
    void window_displays_images_from_the_selected_run();
    void displays_the_actual_dynamically_selected_group_image_data();
    void displays_the_actual_dynamically_selected_group_image();
    void window_displays_blob_from_one_time_debug_image_data();
    void window_displays_blob_from_one_time_debug_image();
};

void VisualTests::displays_the_actual_dynamically_selected_group_image_data()
{
    QTest::addColumn<bool>("global_frames");
    QTest::newRow("public frame list") << false;
    QTest::newRow("revision-pinned global frame list") << true;
}

void VisualTests::displays_the_actual_dynamically_selected_group_image()
{
    QFETCH(bool, global_frames);
    const auto group = id<NodeId>("000000001306");
    const auto global = id<VariableId>("000000001307");
    const auto frames = key<InputId>("frames");
    const auto choice = key<InputId>("choice");
    const auto binary = key<InputId>("binary");
    InputDescriptor capture{binary, "Selected image", DataType::image()};
    capture.binding.allowed_sources = {BindingSourceKind::PublicInput, BindingSourceKind::GlobalRead};
    capture.binding.allow_dynamic_index = true;
    const Selector path{DynamicElementSelection{IndexInputReference{workflow_id, {}, choice, {}}}};
    const BindingSource selected_frame = global_frames ? BindingSource{GlobalReference{global, 4, path}}
        : BindingSource{ScopedInputReference{workflow_id, {}, frames, path}};
    Workflow flow{workflow_id, "Selected frame", {key<SchemaId>("test.ui.dynamic_frame"), 1,
        {{frames, "Frames", DataType::list(DataType::image())}, {choice, "Choice", DataType::integer()}}},
        {{group, "Choose image", GroupConfiguration{{key<SchemaId>("test.ui.capture_frame"), 1, {capture}}, {blob_id}},
            {{binary, {selected_frame}}}},
         {blob_id, "Blob", ToolConfiguration{key<ToolTypeId>("visionpro.image.blob"), 1},
            {{binary, {ScopedInputReference{workflow_id, group, binary, {}}}},
             {key<ParameterId>("connectivity"), {ConfigValue::integer(4)}}}}},
        {}, {{group, blob_id}}, {group}};
    const auto registry = take(app::make_tool_registry());
    const auto doc = take(load_solution(fixture, registry));
    Solution solution{doc.solution().id, "Dynamic frame", solution_format_version, 1, {std::move(flow)},
        {}, {}, {}, doc.solution().coordinates};
    if (global_frames) {
        solution.variables.push_back({global, "Image history", DataType::list(DataType::image()), take(ConfigValue::list(DataType::image(), {}))});
    }
    const auto session = take(WorkflowSession::create(registry));
    const auto snapshot = take(session->activate(solution));
    const ImageLayout layout{2, 2, 2, {PixelDepth::UInt8, ChannelOrder::Mono}};
    const std::array first_pixels{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{0}};
    const std::array second_pixels{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255}};
    const std::array images{take(ImageHandle::copy(layout, first_pixels, frame_id)),
        take(ImageHandle::copy(layout, second_pixels, frame_id))};
    const auto values = take(DataValue::list(DataType::image(), {take(DataValue::image({images[0]})), take(DataValue::image({images[1]}))}));
    ImagePanel panel;
    for (const auto pick : {1, 0}) {
        const std::vector inputs{RunInput{frames, values}, RunInput{choice, DataValue::integer(pick)}};
        std::vector<GlobalVariableEntry> globals;
        if (global_frames) { globals.push_back({global, values, 4, ExternalUpdateOrigin{snapshot.identity().session, key<SchemaId>("test.capture")}}); }
        const auto request = take(RunRequest::create(snapshot, WorkflowTarget{workflow_id}, inputs,
            {TriggerKind::Manual, key<SchemaId>("test.ui.show_frame")}, {}, {}, globals));
        const auto submission = session->submit(request);
        const auto* run = std::get_if<RunHandle>(&submission);
        QVERIFY2(run, run ? "" : std::get<RunRejected>(submission).diagnostic.message.c_str());
        const auto waited = take(run->wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 10s));
        QVERIFY2(waited.progress.status == ExecutionStatus::Succeeded,
            waited.progress.diagnostics.empty() ? "" : waited.progress.diagnostics.front().message.c_str());
        const auto blob = run_result(*run, waited.progress, blob_id);
        take(show_run_image(panel, *run, blob.data().identity.invocation, key<OutputId>("components")));
        QVERIFY(panel.frame() && *panel.frame() == images[pick]);
    }
}

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

void VisualTests::edits_and_reloads_shape_members_data()
{
    QTest::addColumn<QString>("shape");
    QTest::addColumn<QList<QPoint>>("expected");
    QTest::newRow("circle") << QStringLiteral("Circle")
        << QList<QPoint>{{4,3},{3,4},{4,4},{5,4},{4,5}};
    QTest::newRow("ring") << QStringLiteral("Ring")
        << QList<QPoint>{{4,2},{3,3},{4,3},{5,3},{2,4},{3,4},{5,4},{6,4},{3,5},{4,5},{5,5},{4,6}};
    QTest::newRow("sector-across-zero") << QStringLiteral("Annular sector")
        << QList<QPoint>{{5,3},{5,4},{6,4},{5,5}};
    QTest::newRow("polygon") << QStringLiteral("Polygon")
        << QList<QPoint>{{3,3},{4,3},{5,3},{3,4},{4,4},{3,5}};
}

void VisualTests::edits_and_reloads_shape_members()
{
    QFETCH(QString, shape);
    QFETCH(QList<QPoint>, expected);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const fs::path package = fs::path(temporary.path().toStdWString()) / "shape";
    const auto registry = take(app::make_tool_registry());
    EditorWindow window(take(load_solution(fixture, registry)).solution(), registry);
    window.show();
    window.open_document(fixture);
    QTRY_VERIFY(!window.file_busy());
    window.transfer_document(package, SolutionTransfer::SaveAs);
    QTRY_VERIFY(!window.file_busy());
    const auto select_threshold = [&] {
        auto* canvas = qobject_cast<QGraphicsView*>(window.centralWidget());
        auto* scene = qobject_cast<ControlScene*>(canvas->scene());
        return scene && scene->select_workflow(workflow_id) && scene->reveal_node(threshold_id);
    };
    QVERIFY(select_threshold());
    QTRY_VERIFY(window.findChild<QPushButton*>(QStringLiteral("editRegion")));
    auto* edit = window.findChild<QPushButton*>(QStringLiteral("editRegion"));
    QVERIFY(apply_roi_edit(*edit, [&shape](QDialog& dialog) {
        auto* selector = dialog.findChild<QComboBox*>(QStringLiteral("roiSource"));
        if (selector->findText(shape) < 0) { return false; }
        selector->setCurrentText(shape);
        dialog.findChild<QLineEdit*>(QStringLiteral("roiX"))->setText(QStringLiteral("4"));
        dialog.findChild<QLineEdit*>(QStringLiteral("roiY"))->setText(QStringLiteral("4"));
        dialog.findChild<QLineEdit*>(QStringLiteral("roiInnerRadius"))->setText(QStringLiteral("1"));
        dialog.findChild<QLineEdit*>(QStringLiteral("roiRadius"))->setText(
            shape == QStringLiteral("Circle") ? QStringLiteral("1") : QStringLiteral("2"));
        dialog.findChild<QLineEdit*>(QStringLiteral("roiStart"))->setText(QStringLiteral("315"));
        dialog.findChild<QLineEdit*>(QStringLiteral("roiSweep"))->setText(QStringLiteral("90"));
        dialog.findChild<QPlainTextEdit*>(QStringLiteral("roiVertices"))->setPlainText(QStringLiteral("3,3\n5,3\n3,5"));
        return true;
    }));
    const auto edited = region(window.draft(), threshold_id);
    window.save_document();
    QTRY_VERIFY(!window.file_busy());
    window.open_document(package);
    QTRY_VERIFY(!window.file_busy());
    QVERIFY(region(window.draft(), threshold_id) == edited);
    window.open_preview_resource(image_resource, frame_id, true);
    QTRY_VERIFY(!window.file_busy());
    QVERIFY(select_threshold());
    window.preview_selected_roi();
    QTRY_VERIFY(!window.file_busy());
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("ROI contains %1 pixels.").arg(expected.size()));
    const auto* view = window.findChild<ImagePanel*>()->findChild<ImageView*>();
    QList<QPoint> actual;
    for (const auto* item : view->scene()->items()) {
        const auto* bitmap = dynamic_cast<const QGraphicsPixmapItem*>(item);
        if (!bitmap || !bitmap->pixmap().hasAlphaChannel()) { continue; }
        const auto layer = bitmap->pixmap().toImage();
        for (int y = 0; y < layer.height(); ++y) {
            for (int x = 0; x < layer.width(); ++x) {
                if (qAlpha(layer.pixel(x, y)) > 0) { actual.append({x, y}); }
            }
        }
    }
    QCOMPARE(actual, expected);
}

void VisualTests::fits_newly_bound_calibration_file()
{
    const auto registry = take(app::make_tool_registry());
    const auto flow = id<WorkflowId>("000000006810");
    Solution draft{id<SolutionId>("000000006800"), "Desktop calibration", solution_format_version, 1,
        {{flow, "Fit", {key<SchemaId>("test.fit"), 1}}}};
    draft.coordinates.push_back({key<CoordinateSystemId>("camera"), CoordinateKind::Image, MeasurementUnit::Pixel,
        {0,0}, {1,0}, {0,-1}, Handedness::LeftHanded});
    EditorWindow window(std::move(draft), registry);
    window.show();
    auto* frame_action = window.findChild<QAction*>(QStringLiteral("addCoordinateFrame"));
    QVERIFY(frame_action);
    QTimer::singleShot(0, &window, [] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { return; }
        if (auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("coordinateFrameId"))) {
            name->setText(QStringLiteral("world"));
            dialog->accept();
        } else { dialog->reject(); }
    });
    frame_action->trigger();
    QCOMPARE(window.draft().coordinates.size(), 2u);
    QCOMPARE(window.draft().coordinates.back().unit, MeasurementUnit::Millimetre);

    auto* tools = window.findChild<QListWidget*>(QStringLiteral("toolList"));
    QVERIFY(tools);
    for (int i = 0; i < tools->count(); ++i) {
        if (tools->item(i)->toolTip() == QStringLiteral("visionpro.calibration.fit")) { tools->setCurrentRow(i); break; }
    }
    QCOMPARE(tools->currentItem()->toolTip(), QStringLiteral("visionpro.calibration.fit"));
    window.findChild<QAction*>(QStringLiteral("addTool"))->trigger();
    QTRY_VERIFY(window.findChild<QPushButton*>(QStringLiteral("chooseExternalResource")));
    const auto directory = fixture.parent_path() / "s6-calibration";
    const auto path = directory / "resources" / "affine.json";
    const bool nonnative = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    const auto restore = qScopeGuard([nonnative] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nonnative); });
    QTimer::singleShot(0, &window, [path] {
        auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        if (!picker) { return; }
        picker->selectFile(QString::fromStdWString(path.wstring()));
        QMetaObject::invokeMethod(picker, "accept");
    });
    window.findChild<QPushButton*>(QStringLiteral("chooseExternalResource"))->click();
    QTRY_VERIFY(!window.file_busy());
    QCOMPARE(window.draft().resources.size(), 1u);
    const auto& resource = window.draft().resources.front();
    const auto& node = find_workflow(window.draft(), flow)->nodes.front();
    const auto& file = std::get<ExternalFileResource>(resource.location);
    QVERIFY(file.revision == 1 && file.read_policy == ResourceReadPolicy::SnapshotLocked && file.expected_sha256);
    const auto sample = take(load_solution(directory, registry));
    const auto known = std::ranges::find(sample.solution().resources, ResourceKind::Calibration, &ResourceDefinition::kind);
    QVERIFY(known != sample.solution().resources.end());
    QCOMPARE(*file.expected_sha256, std::get<ManagedResource>(known->location).sha256);
    QVERIFY(node.resources.size() == 1 && node.resources.front().resource.id == resource.id
        && node.resources.front().resource.revision == 1);

    window.findChild<QAction*>(QStringLiteral("runWorkflow"))->trigger();
    auto* panel = window.findChild<RunPanel*>();
    QTRY_COMPARE_WITH_TIMEOUT(panel->findChild<QLabel*>(QStringLiteral("runCleanup"))->text(),
        QStringLiteral("Cleanup: Quiescent"), 10000);
    const auto run = panel->selected_run();
    QVERIFY(run);
    const auto progress = take(run->inspect());
    QVERIFY2(progress.status == ExecutionStatus::Succeeded,
        progress.diagnostics.empty() ? "Calibration failed" : progress.diagnostics.front().message.c_str());
    const auto call = run_result(*run, progress, node.id);
    const auto& fitted = std::get<DataValue>(call.output(key<OutputId>("transform"))->content);
    const auto* transform = fitted.as_transform2d();
    QVERIFY(transform && transform->model == TransformModel::Affine && transform->from == key<CoordinateSystemId>("camera")
        && transform->to == key<CoordinateSystemId>("world") && transform->from_unit == MeasurementUnit::Pixel
        && transform->to_unit == MeasurementUnit::Millimetre);
    const auto* provenance = std::get<DataValue>(call.output(key<OutputId>("source"))->content).as_resource_reference();
    QVERIFY(provenance && *provenance == node.resources.front().resource);
    QVERIFY(select_run_node(*panel->findChild<QTreeWidget*>(QStringLiteral("runNodes")), node.id));
    const auto details = panel->findChild<QPlainTextEdit*>(QStringLiteral("runDetails"))->toPlainText();
    QVERIFY(details.contains(QStringLiteral("Affine · camera (px) → world (mm)")));
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

void VisualTests::window_displays_images_from_the_selected_run()
{
    const auto registry = take(app::make_tool_registry());
    const auto document = take(load_solution(fixture, registry));
    EditorWindow window(document.solution(), registry);
    window.show();
    window.open_document(fixture);
    QTRY_VERIFY(!window.file_busy());

    auto* canvas = qobject_cast<QGraphicsView*>(window.centralWidget());
    QVERIFY(canvas);
    auto* scene = qobject_cast<ControlScene*>(canvas->scene());
    QVERIFY(scene && scene->select_workflow(workflow_id));
    auto* start = window.findChild<QAction*>(QStringLiteral("runWorkflow"));
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("runDock"));
    auto* run_panel = window.findChild<RunPanel*>();
    auto* images = window.findChild<ImagePanel*>();
    QVERIFY(start && start->isEnabled() && dock && run_panel && images);
    auto* status = run_panel->findChild<QLabel*>(QStringLiteral("runStatus"));
    auto* completion = run_panel->findChild<QLabel*>(QStringLiteral("runCompletion"));
    auto* cleanup = run_panel->findChild<QLabel*>(QStringLiteral("runCleanup"));
    auto* judgement = run_panel->findChild<QLabel*>(QStringLiteral("runJudgement"));
    auto* nodes = run_panel->findChild<QTreeWidget*>(QStringLiteral("runNodes"));
    auto* outputs = run_panel->findChild<QComboBox*>(QStringLiteral("runOutputs"));
    auto* show_image = run_panel->findChild<QPushButton*>(QStringLiteral("showRunImage"));
    auto* provenance = images->findChild<QLabel*>(QStringLiteral("imageProvenance"));
    auto* view = images->findChild<ImageView*>();
    QVERIFY(status && completion && cleanup && judgement && nodes && outputs && show_image && provenance && view);

    start->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(cleanup->text(), QStringLiteral("Cleanup: Quiescent"), 10000);
    QCOMPARE(status->text(), QStringLiteral("Status: Succeeded"));
    QCOMPARE(completion->text(), QStringLiteral("Logical completion: Complete"));
    QVERIFY(dock->isVisible());
    const auto run = run_panel->selected_run();
    QVERIFY(run && run->identity().workflow == workflow_id);
    const auto progress = take(run->inspect());
    const auto binary = key<OutputId>("binary");
    const auto source = run_result(*run, progress, binary_source_id);
    const auto* expected_image = std::get<DataValue>(source.output(binary)->content).as_image();
    QVERIFY(expected_image);

    QVERIFY(select_run_node(*nodes, blob_id));
    const auto selected_blob = run_panel->selected_invocation();
    QVERIFY(selected_blob && selected_blob->node == blob_id);
    outputs->setCurrentIndex(outputs->findText(QStringLiteral("components")));
    QVERIFY(show_image->isEnabled());
    show_image->click();
    QVERIFY(images->frame() && *images->frame() == expected_image->image && images->isVisible());
    const auto* flow = find_workflow(run->request().snapshot().solution(), workflow_id);
    const auto blob_name = QString::fromStdString(find_node(*flow, blob_id)->name);
    QCOMPARE(provenance->text(), QStringLiteral("Run %1  ·  Snapshot %2  ·  %3")
        .arg(run->identity().run.value()).arg(run->identity().snapshot.snapshot.value()).arg(blob_name));
    const auto blob_items = view->scene()->items();
    QCOMPARE(std::ranges::count_if(blob_items, [](const QGraphicsItem* item) {
        const auto* path = dynamic_cast<const QGraphicsPathItem*>(item);
        return path && !path->path().isEmpty();
    }), 1);
    QVERIFY(std::ranges::any_of(blob_items, [](const QGraphicsItem* item) {
        const auto* path = dynamic_cast<const QGraphicsPathItem*>(item);
        return path && path->path().boundingRect() == QRectF(3.5, 2.5, 3, 3);
    }));
    QCOMPARE(judgement->text(), QStringLiteral("Judgement: Not evaluated"));

    QVERIFY(select_run_node(*nodes, threshold_id));
    outputs->setCurrentIndex(outputs->findText(QStringLiteral("binary")));
    QVERIFY(show_image->isEnabled());
    show_image->click();
    const auto threshold = run_result(*run, progress, threshold_id);
    const auto* threshold_image = std::get<DataValue>(threshold.output(binary)->content).as_image();
    QVERIFY(threshold_image && images->frame() && *images->frame() == threshold_image->image);
    const auto threshold_items = view->scene()->items();
    QVERIFY(std::ranges::any_of(threshold_items, [](const QGraphicsItem* item) {
        const auto* path = dynamic_cast<const QGraphicsPathItem*>(item);
        return path && !path->path().isEmpty();
    }));
    QVERIFY(std::ranges::any_of(threshold_items, [](const QGraphicsItem* item) {
        const auto* mask = dynamic_cast<const QGraphicsPixmapItem*>(item);
        if (!mask || !mask->pixmap().hasAlphaChannel()) { return false; }
        const auto pixels = mask->pixmap().toImage();
        return pixels.width() > 8 && pixels.height() > 8
            && qAlpha(pixels.pixel(4, 4)) > 0 && qAlpha(pixels.pixel(8, 8)) == 0;
    }));

    QVERIFY(select_run_node(*nodes, judge_id));
    QCOMPARE(status->text(), QStringLiteral("Status: Succeeded"));
    QCOMPARE(judgement->text(), QStringLiteral("Judgement: OK"));
    QVERIFY(!show_image->isEnabled());
    QVERIFY(images->frame() && *images->frame() == threshold_image->image);
    QVERIFY(scene->undoStack().isClean());
}

void VisualTests::window_displays_blob_from_one_time_debug_image_data()
{
    QTest::addColumn<bool>("in_group");
    QTest::newRow("standalone node") << false;
    QTest::newRow("Group public image") << true;
}

void VisualTests::window_displays_blob_from_one_time_debug_image()
{
    QFETCH(bool, in_group);
    const auto group_id = id<NodeId>("000000001306");
    const auto binary = key<InputId>("binary");
    const auto registry = take(app::make_tool_registry());
    const auto document = take(load_solution(fixture, registry));
    EditorWindow window(document.solution(), registry);
    window.show();
    window.open_document(fixture);
    QTRY_VERIFY(!window.file_busy());
    auto* canvas = qobject_cast<QGraphicsView*>(window.centralWidget());
    QVERIFY(canvas);
    auto* scene = qobject_cast<ControlScene*>(canvas->scene());
    QVERIFY(scene && scene->select_workflow(workflow_id));
    QVERIFY(scene->edit_draft(QStringLiteral("Prepare isolated Blob debug"), [&](Solution& draft) -> Result<std::monostate> {
        auto workflow = std::ranges::find(draft.workflows, workflow_id, &Workflow::id);
        auto blob = std::ranges::find(workflow->nodes, blob_id, &Node::id);
        std::erase_if(blob->bindings, [](const Binding& item) { return item.target == BindingTarget{roi_id}; });
        if (in_group) {
            auto child = *blob;
            auto input = std::ranges::find(child.bindings, BindingTarget{binary}, &Binding::target);
            input->sources = {ScopedInputReference{workflow_id, group_id, binary, {}}};
            const auto* tool = registry.find_descriptor(std::get<ToolConfiguration>(child.configuration).type);
            GroupConfiguration group{{key<SchemaId>("test.ui.blob_group"), 1, tool->inputs}, {child.id}};
            workflow->nodes = {{group_id, "Blob Group", std::move(group)}, std::move(child)};
            workflow->entries = {group_id};
            workflow->containment = {{group_id, blob_id}};
            workflow->control_links.clear();
            workflow->outputs.clear();
            workflow->interface.outputs.clear();
            std::erase_if(draft.layouts, [&](const auto& layout) { return layout.workflow == workflow_id; });
        }
        return std::monostate{};
    }));
    QVERIFY(scene->reveal_node(in_group ? group_id : blob_id));
    const int edit_index = scene->undoStack().index();

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto file = temporary.filePath(QStringLiteral("debug_binary.png"));
    QImage pixels(9, 9, QImage::Format_Grayscale8);
    pixels.fill(0);
    pixels.scanLine(1)[1] = 255;
    pixels.scanLine(1)[2] = 255;
    pixels.scanLine(2)[1] = 255;
    pixels.scanLine(2)[2] = 255;
    QVERIFY(pixels.save(file));

    auto* start = window.findChild<QAction*>(QStringLiteral("debugSelectedNode"));
    auto* run_panel = window.findChild<RunPanel*>();
    auto* images = window.findChild<ImagePanel*>();
    QVERIFY(start && start->isEnabled() && run_panel && images);
    auto* cleanup = run_panel->findChild<QLabel*>(QStringLiteral("runCleanup"));
    auto* show_image = run_panel->findChild<QPushButton*>(QStringLiteral("showRunImage"));
    QVERIFY(cleanup && show_image);
    bool accepted = false;
    QTimer dialog_action;
    dialog_action.setSingleShot(true);
    QObject::connect(&dialog_action, &QTimer::timeout, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { return; }
        if (dialog->objectName() == QStringLiteral("runInputsDialog")) {
            auto* path = dialog->findChild<QLineEdit*>(QStringLiteral("runInputFile.binary"));
            auto* frame = dialog->findChild<QComboBox*>(QStringLiteral("runInputFrame.binary"));
            auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("runInputButtons"));
            const int frame_index = frame ? frame->findText(QString::fromStdString(std::string(frame_id.text()))) : -1;
            if (path && buttons && frame_index >= 0) {
                path->setText(file);
                frame->setCurrentIndex(frame_index);
                buttons->button(QDialogButtonBox::Ok)->click();
                accepted = dialog->result() == QDialog::Accepted;
            }
        }
        if (!accepted) { dialog->reject(); }
    });
    dialog_action.start(0);
    start->trigger();
    QVERIFY(accepted);
    QTRY_COMPARE_WITH_TIMEOUT(cleanup->text(), QStringLiteral("Cleanup: Quiescent"), 10000);
    const auto debug = run_panel->selected_run();
    QVERIFY(debug && debug->request().debug_state() == DebugStatePolicy::Isolated);
    const auto progress = take(debug->inspect());
    QCOMPARE(progress.status, ExecutionStatus::Succeeded);
    if (in_group) {
        QVERIFY(progress.call_order == (std::vector<NodeId>{group_id, blob_id}));
        QVERIFY(select_run_node(*run_panel->findChild<QTreeWidget*>(QStringLiteral("runNodes")), blob_id));
    } else {
        QVERIFY(progress.call_order == std::vector<NodeId>{blob_id});
    }
    const auto* supplied = debug->request().input(binary);
    QVERIFY(supplied && supplied->origin == InputOrigin::Provided && supplied->value.as_image());
    const auto result = run_result(*debug, progress, blob_id);
    const auto& area = std::get<DataValue>(result.output(key<OutputId>("total_area_pixels"))->content);
    QCOMPARE(*area.as_int64(), 4);

    QVERIFY(show_image->isEnabled());
    show_image->click();
    QVERIFY(images->frame() && *images->frame() == supplied->value.as_image()->image);
    if (in_group) {
        const auto* label = images->findChild<QLabel*>(QStringLiteral("imageProvenance"));
        QVERIFY(label && label->text().contains(QStringLiteral("Blob Group · Scope"))
            && label->text().contains(QStringLiteral("Invocation")));
    }
    QCOMPARE(scene->undoStack().index(), edit_index);
    const auto* blob = find_node(*find_workflow(window.draft(), workflow_id), blob_id);
    const auto binding = std::ranges::find(blob->bindings, BindingTarget{binary}, &Binding::target);
    QVERIFY(binding != blob->bindings.end());
    if (in_group) {
        const auto* upstream = std::get_if<ScopedInputReference>(&binding->sources.front());
        QVERIFY(upstream && upstream->group == group_id && upstream->input == binary);
    } else {
        const auto* upstream = std::get_if<OutputReference>(&binding->sources.front());
        QVERIFY(upstream && upstream->node == binary_source_id);
    }
}

} // namespace
} // namespace visionpro::ui

QTEST_MAIN(visionpro::ui::VisualTests)
#include "visual_tests.moc"
