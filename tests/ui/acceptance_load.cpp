#include "control_scene.hpp"
#include "editor_window.hpp"
#include "image_panel.hpp"
#include "run_controller.hpp"
#include "tool_catalog.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <QtNodes/GraphicsView>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QDockWidget>
#include <QEvent>
#include <QEventLoop>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QUndoStack>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::infrastructure;
using namespace visionpro::workflow;
using namespace visionpro::ui;
using Clock = std::chrono::steady_clock;

constexpr std::size_t node_count = 100;
constexpr std::size_t edge_count = 300;
constexpr int width = 1920;
constexpr int height = 1080;
constexpr int object_left = 320;
constexpr int object_top = 240;
constexpr int object_width = 640;
constexpr int object_height = 360;
constexpr int sample_rounds = 18;
constexpr int io_rounds = 5;
constexpr int drags_per_load = 4;
constexpr auto pixels = MeasurementUnit::Pixel;

void require(bool condition, const std::string& detail)
{
    if (!condition) { throw std::runtime_error(detail); }
}

template<class T> T take(Result<T> result)
{
    if (const auto* error = std::get_if<Diagnostic>(&result)) {
        throw std::runtime_error(std::string(error_code_name(error->code)) + ": " + error->message);
    }
    return std::get<T>(std::move(result));
}

template<UuidIdentity Id> Id id(std::size_t number)
{
    const auto suffix = std::to_string(number);
    return take(IdFactory::parse<Id>("91000000-0000-4000-8000-" + std::string(12 - suffix.size(), '0') + suffix));
}

template<SchemaIdentity Id> Id key(std::string_view text) { return take(SchemaIdFactory::parse<Id>(text)); }

NodeId node_id(std::size_t index) { return id<NodeId>(1000 + index); }

Binding setting(std::string_view parameter, ConfigValue value)
{ return {key<ParameterId>(parameter), {std::move(value)}}; }

Binding mode(const ToolRegistry& registry, const ToolTypeId& tool, std::string_view parameter, std::string_view option)
{
    const auto* descriptor = registry.find_descriptor(tool);
    require(descriptor, "The load fixture requires the registered vision tools.");
    const auto name = key<ParameterId>(parameter);
    const auto field = std::ranges::find(descriptor->parameters, name, &ParameterDescriptor::id);
    require(field != descriptor->parameters.end(), "The fixture tool parameter is missing.");
    return setting(parameter, take(ConfigValue::enumeration(field->type, key<EnumMemberId>(option))));
}

Binding input_from(std::string_view port, const WorkflowId& flow, std::size_t source, std::string_view output)
{ return {key<InputId>(port), {OutputReference{flow, node_id(source), key<OutputId>(output), {}}}}; }

Solution workload(const ToolRegistry& registry)
{
    const auto frame = key<CoordinateSystemId>("s56.load.camera");
    const auto flow_id = id<WorkflowId>(2);
    const auto resource = id<ResourceId>(3);
    const auto reader = key<ToolTypeId>("visionpro.image.read_file");
    const auto threshold = key<ToolTypeId>("visionpro.image.threshold");
    const auto blob = key<ToolTypeId>("visionpro.image.blob");
    const auto adder = key<ToolTypeId>("visionpro.sim.add");
    const RegionDefinition roi{id<RegionId>(4), frame, pixels, RegionPrimitive{frame, pixels, FullImageRegion{}}, {}};

    Workflow flow{flow_id, "S5.6 load", {key<SchemaId>("s56.load.flow"), 1}, {}, {}, {}, {node_id(0)}};
    flow.nodes.reserve(node_count);
    flow.nodes.push_back({node_id(0), "Read 1080p frame", ToolConfiguration{reader, 1},
        {mode(registry, reader, "decode_mode", "Gray8"),
            setting("frame_origin", take(ConfigValue::point2d({frame, pixels, 0, 0})))},
        {{key<SchemaId>("source"), {resource, ResourceKind::Image, {}}}}});
    flow.nodes.push_back({node_id(1), "Threshold 1080p frame", ToolConfiguration{threshold, 1},
        {input_from("image", flow_id, 0, "image"), setting("threshold", ConfigValue::integer(128)),
            setting("roi", take(ConfigValue::region2d(roi))), mode(registry, threshold, "mode", "Binary")}});
    flow.nodes.push_back({node_id(2), "Measure rectangle", ToolConfiguration{blob, 1},
        {input_from("binary", flow_id, 1, "binary"), setting("connectivity", ConfigValue::integer(8))}});
    for (std::size_t index = 3; index < node_count; ++index) {
        flow.nodes.push_back({node_id(index), "Add " + std::to_string(index + 1), ToolConfiguration{adder, 1},
            {{key<InputId>("left"), {ConfigValue::integer(static_cast<std::int64_t>(index))}},
                {key<InputId>("right"), {ConfigValue::integer(1)}}}});
    }

    flow.control_links.reserve(edge_count);
    const auto add_edge = [&flow](std::size_t source, std::size_t target) {
        flow.control_links.push_back({id<ControlLinkId>(2000 + flow.control_links.size()), node_id(source), {}, node_id(target)});
    };
    for (std::size_t source = 0; source < node_count; ++source) {
        for (std::size_t distance = 1; distance <= 3 && source + distance < node_count; ++distance) {
            add_edge(source, source + distance);
        }
    }
    for (std::size_t source = 0; source < 6; ++source) { add_edge(source, source + 4); }
    require(flow.nodes.size() == node_count && flow.control_links.size() == edge_count, "The fixture graph size changed.");

    WorkflowLayout layout{flow_id};
    for (std::size_t index = 0; index < node_count; ++index) {
        layout.nodes.push_back({node_id(index), static_cast<double>(index % 10) * 260,
            static_cast<double>(index / 10) * 125});
    }
    return {id<SolutionId>(1), "S5.6 100-node acceptance load", solution_format_version, 1, {std::move(flow)},
        {{resource, "Deterministic grayscale frame", ResourceKind::Image,
            ExternalFileResource{{PathBase::SolutionDirectory, "input.png", {}}, ResourceReadPolicy::PerInvocation, {}, {}}}},
        {}, {}, {{frame, CoordinateKind::Image, pixels, {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded}},
        {std::move(layout)}};
}

void write_frame(const QString& path)
{
    QImage image(width, height, QImage::Format_Grayscale8);
    require(!image.isNull(), "Unable to allocate the load fixture image.");
    image.fill(24);
    for (int row = object_top; row < object_top + object_height; ++row) {
        std::fill_n(image.scanLine(row) + object_left, object_width, static_cast<uchar>(220));
    }
    require(image.save(path, "PNG"), "Unable to save the load fixture image.");
}

template<class Control> Control& control(QWidget& owner, const char* object_name)
{
    auto* found = owner.findChild<Control*>(QString::fromLatin1(object_name));
    require(found, std::string("Missing desktop control ") + object_name);
    return *found;
}

template<class Ready> void wait_until(const Ready& ready, const char* operation, EditorWindow& window)
{
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (!ready()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (ready()) { return; }
        require(Clock::now() < deadline, std::string(operation) + " timed out. " + window.statusBar()->currentMessage().toStdString());
        QThread::msleep(1);
    }
}

double milliseconds(Clock::duration elapsed) { return std::chrono::duration<double, std::milli>(elapsed).count(); }

class PaintProbe final : public QObject {
public:
    PaintProbe(QWidget& viewport, std::function<bool()> ready) : ready_(std::move(ready))
    { viewport.installEventFilter(this); }

    std::optional<Clock::time_point> completed;

protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (!queued_ && event->type() == QEvent::Paint && ready_()) {
            queued_ = true;
            QTimer::singleShot(0, this, [this] { completed = Clock::now(); });
        }
        return false;
    }

private:
    std::function<bool()> ready_;
    bool queued_{false};
};

double drag_and_paint(EditorWindow& window, const QPoint& offset)
{
    auto& view = control<QtNodes::GraphicsView>(window, "workflowCanvas");
    auto* scene = qobject_cast<ControlScene*>(view.scene());
    require(scene, "The desktop canvas has no control scene.");
    auto& graph = static_cast<ControlGraphModel&>(scene->graphModel());
    auto* graphic = scene->nodeGraphicsObject(graph.canvas_id(node_id(55)));
    require(graphic, "The selected load node has no canvas representation.");
    view.setFocus(Qt::MouseFocusReason);
    // Opening a document queues a fit-to-view. Let it finish before choosing the drag scale.
    QCoreApplication::processEvents();
    view.resetTransform();
    view.centerOn(graphic);
    const auto from = view.mapFromScene(graphic->mapToScene(graphic->boundingRect().center()));
    const auto before = graphic->pos();
    const int expected_index = scene->undoStack().index() + 1;
    auto& viewport = *view.viewport();
    require(viewport.rect().contains(from) && viewport.rect().contains(from + offset), "The drag target is outside the canvas.");
    require(view.itemAt(from) == graphic, "The drag start does not hit the selected load node.");
    PaintProbe paint(viewport, [&] { return scene->undoStack().index() == expected_index; });

    // Deliver the full button state to the same viewport on every Qt platform.
    const auto send_mouse = [&](QEvent::Type type, QPoint point, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(point), QPointF(viewport.mapToGlobal(point)), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&viewport, &event);
    };
    const auto started = Clock::now();
    send_mouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    require(scene->mouseGrabberItem() == graphic, "The selected load node did not grab the mouse.");
    send_mouse(QEvent::MouseMove, from + offset / 2, Qt::NoButton, Qt::LeftButton);
    send_mouse(QEvent::MouseMove, from + offset, Qt::NoButton, Qt::LeftButton);
    const auto expected = before + QPointF(offset);
    require(graphic->pos() == expected, "Mouse move did not preview the expected node position.");
    send_mouse(QEvent::MouseButtonRelease, from + offset, Qt::LeftButton, Qt::NoButton);
    require(scene->undoStack().index() == expected_index && graphic->pos() == expected,
        "Mouse drag did not publish the expected layout edit.");
    require(graph.nodeData(graphic->nodeId(), QtNodes::NodeRole::Position).toPointF() == expected,
        "The document layout did not retain the new node position.");
    wait_until([&] { return paint.completed.has_value(); }, "Canvas paint", window);
    return milliseconds(*paint.completed - started);
}

const DataValue& value(const NodeResult& result, std::string_view output)
{
    const auto* found = result.output(key<OutputId>(output));
    require(found && std::holds_alternative<DataValue>(found->content), "The run did not publish the expected output.");
    return std::get<DataValue>(found->content);
}

NodeResult result_for(const RunHandle& run, const RunProgress& progress, const NodeId& node)
{
    const auto call = std::ranges::find(progress.nodes, node, &RunNodeProgress::node);
    require(call != progress.nodes.end() && call->invocation.has_value(), "The expected node was not invoked.");
    auto result = take(run.lookup(*call->invocation));
    require(result.has_value(), "The expected invocation has no result.");
    return *result;
}

double show_and_paint(EditorWindow& window, std::size_t node, const ImageHandle& expected)
{
    window.findChild<QDockWidget*>(QStringLiteral("runDock"))->show();
    auto& tree = control<QTreeWidget>(window, "runNodes");
    const auto identity = QString::fromStdString(node_id(node).to_string());
    QTreeWidgetItem* item = nullptr;
    for (int row = 0; row < tree.topLevelItemCount(); ++row) {
        if (tree.topLevelItem(row)->toolTip(0) == identity) { item = tree.topLevelItem(row); break; }
    }
    require(item, "The run panel did not expose the result for the expected node.");
    tree.setCurrentItem(item);
    auto& show = control<QPushButton>(window, "showRunImage");
    auto& panel = control<ImagePanel>(window, "imagePanel");
    auto& view = control<ImageView>(panel, "imageView");
    require(show.isEnabled(), "The result has no image presentation action.");
    QCoreApplication::processEvents();

    bool changed = false;
    PaintProbe paint(*view.viewport(), [&] { return changed; });
    QObject::connect(&panel, &ImagePanel::frame_changed, &paint, [&] { changed = true; });
    const auto started = Clock::now();
    show.click();
    require(changed && panel.frame() && *panel.frame() == expected, "The panel did not display the selected run's image.");
    wait_until([&] { return paint.completed.has_value(); }, "Run image paint", window);
    return milliseconds(*paint.completed - started);
}

std::size_t logical_bytes(const ImageHandle& image)
{ return take(required_image_bytes({image.width(), image.height(), image.row_stride(), image.format()})); }

QJsonObject memory_sample(const QString& stage, const RunController& controller, const ImagePanel& panel)
{
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb = sizeof(process);
    require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), process.cb),
        "Windows process memory counters are unavailable.");

    std::vector<ImageHandle> buffers;
    const auto record = [&buffers](const ImageHandle& image) {
        const auto same = [&image](const ImageHandle& other) { return image.shares_storage_with(other); };
        if (std::ranges::none_of(buffers, same)) { buffers.push_back(image); }
    };
    for (const auto& run : controller.runs()) {
        const auto progress = take(run.inspect());
        const auto reader = result_for(run, progress, node_id(0));
        const auto threshold = result_for(run, progress, node_id(1));
        const auto* original = value(reader, "image").as_image();
        const auto* binary = value(threshold, "binary").as_image();
        const auto* mask = value(threshold, "roi_mask").as_raster_mask();
        require(original && binary && mask, "A retained run is missing one of its three image payloads.");
        record(original->image);
        record(binary->image);
        record(mask->pixels);
    }
    const auto count = buffers.size();
    std::size_t retained_bytes = 0;
    for (const auto& image : buffers) { retained_bytes += logical_bytes(image); }
    std::size_t extra_display_bytes = 0;
    if (const auto& displayed = panel.frame(); displayed && std::ranges::none_of(buffers, [&](const ImageHandle& image) {
            return image.shares_storage_with(*displayed);
        })) {
        extra_display_bytes = logical_bytes(*displayed);
    }

    return {{"stage", stage}, {"private_bytes", static_cast<qint64>(process.PrivateUsage)},
        {"working_set_bytes", static_cast<qint64>(process.WorkingSetSize)},
        {"retained_runs", static_cast<qint64>(controller.runs().size())},
        {"retained_distinct_image_buffers", static_cast<qint64>(count)},
        {"retained_logical_image_bytes", static_cast<qint64>(retained_bytes)},
        {"display_logical_image_bytes_outside_history", static_cast<qint64>(extra_display_bytes)},
        {"dropped_notifications", static_cast<qint64>(controller.dropped())}};
}

QJsonObject distribution(std::vector<double> samples)
{
    QJsonArray raw;
    for (const auto sample : samples) { raw.append(sample); }
    std::ranges::sort(samples);
    const auto at = [&](double fraction) {
        const auto rank = static_cast<std::size_t>(std::ceil(static_cast<double>(samples.size()) * fraction));
        return samples[rank - 1];
    };
    return {{"sample_count", static_cast<qint64>(samples.size())}, {"p50", at(0.5)}, {"p95", at(0.95)},
        {"max", samples.back()}, {"samples", raw}};
}

QJsonObject measure()
{
    constexpr auto display_width = 1440;
    constexpr auto display_height = 900;
    QTemporaryDir package;
    require(package.isValid(), "Unable to create the temporary load package.");
    const auto directory = std::filesystem::path(package.path().toStdWString());
    const auto registry = take(visionpro::app::make_tool_registry());
    const auto source = workload(registry);
    write_frame(package.filePath(QStringLiteral("input.png")));
    take(write_new_solution(directory, source));
    const Solution initial{id<SolutionId>(9), "Load probe", solution_format_version, 1, {}};
    EditorWindow window(initial, registry);
    window.resize(display_width, display_height);
    window.show();
    require(QTest::qWaitForWindowExposed(&window), "Unable to expose the desktop probe window.");

    std::vector<double> loads, saves, drags, runs, threshold_refresh, blob_refresh;
    QJsonArray memory;
    auto revision = source.revision;
    for (int round = 0; round < io_rounds; ++round) {
        const QPointer<QWidget> previous(window.findChild<QGraphicsView*>(QStringLiteral("workflowCanvas")));
        const auto loading = Clock::now();
        window.open_document(directory);
        wait_until([&] { return !window.file_busy(); }, "Load package", window);
        loads.push_back(milliseconds(Clock::now() - loading));
        require(previous.isNull() && window.findChild<QGraphicsView*>(QStringLiteral("workflowCanvas")) && window.draft().id == source.id && window.draft().revision == revision,
            "The desktop did not load the expected package revision.");
        const auto& flow = window.draft().workflows.front();
        require(flow.nodes.size() == node_count && flow.control_links.size() == edge_count,
            "The loaded desktop graph lost nodes or control edges.");

        for (int sample = 0; sample < drags_per_load; ++sample) {
            const int direction = sample % 2 == 0 ? 1 : -1;
            drags.push_back(drag_and_paint(window, QPoint{direction * 32, direction * 24}));
        }
        auto& save = control<QAction>(window, "saveSolution");
        require(save.isEnabled(), "The layout edit did not make the solution saveable.");
        const auto saving = Clock::now();
        save.trigger();
        wait_until([&] { return !window.file_busy(); }, "Save package", window);
        saves.push_back(milliseconds(Clock::now() - saving));
        require(window.draft().revision == ++revision && !window.isWindowModified(), "The desktop save did not commit the layout revision.");
    }

    auto& controller = control<RunController>(window, "runController");
    auto& panel = control<ImagePanel>(window, "imagePanel");
    auto& run = control<QAction>(window, "runWorkflow");
    require(controller.history_limit() == 16 && controller.observation_capacity() == 32, "The default load retention policy changed.");
    QTest::qWait(100);
    memory.append(memory_sample(QStringLiteral("before_runs"), controller, panel));
    std::optional<RunIdentity> previous;
    for (int round = 1; round <= sample_rounds; ++round) {
        require(run.isEnabled(), "The load workflow is not runnable.");
        const auto starting = Clock::now();
        run.trigger();
        wait_until([&] {
            return controller.can_start() && !controller.runs().empty()
                && (!previous || controller.runs().back().identity() != *previous);
        }, "Run and release", window);
        runs.push_back(milliseconds(Clock::now() - starting));

        const auto& finished = controller.runs().back();
        const auto progress = take(finished.inspect());
        require(progress.quiescent && progress.status == ExecutionStatus::Succeeded && progress.call_order.size() == node_count
                && progress.control_links.size() == edge_count && std::ranges::all_of(progress.control_links,
                [](const RunControlLinkProgress& link) { return link.status == ControlLinkStatus::Arrived; }),
            "The load workflow did not successfully execute all nodes and control links.");
        previous = finished.identity();
        const auto threshold = result_for(finished, progress, node_id(1));
        const auto blob = result_for(finished, progress, node_id(2));
        const auto* binary = value(threshold, "binary").as_image();
        require(binary && binary->image.width() == width && binary->image.height() == height,
            "The workflow did not publish a 1920x1080 image.");
        const auto* area = value(blob, "total_area_pixels").as_int64();
        const auto* components = value(blob, "count").as_int64();
        require(area && *area == object_width * object_height && components && *components == 1,
            "The load fixture did not produce its expected rectangle.");
        threshold_refresh.push_back(show_and_paint(window, 1, binary->image));
        blob_refresh.push_back(show_and_paint(window, 2, binary->image));
        memory.append(memory_sample(QStringLiteral("run_%1").arg(round), controller, panel));
    }
    require(controller.runs().size() == controller.history_limit(), "The configured history limit did not evict completed runs.");
    const auto retained = static_cast<qint64>(controller.runs().size());
    const auto dropped = static_cast<qint64>(controller.dropped());
    const auto canvas_pixels = control<QtNodes::GraphicsView>(window, "workflowCanvas").viewport()->size();
    const auto image_pixels = control<ImageView>(panel, "imageView").viewport()->size();
    const auto size_json = [](QSize size) { return QJsonArray{size.width(), size.height()}; };
    controller.set_limits(0, controller.observation_capacity());
    require(controller.runs().empty() && panel.frame().has_value(), "Clearing history released the image display's own frame.");
    memory.append(memory_sample(QStringLiteral("history_cleared"), controller, panel));
    panel.clear_frame();
    memory.append(memory_sample(QStringLiteral("display_released"), controller, panel));
    window.close();
    wait_until([&] { return !controller.has_session_work() && !window.isVisible(); }, "Close load window", window);

    return {{"fixture", QJsonObject{{"nodes", static_cast<qint64>(node_count)}, {"control_edges", static_cast<qint64>(edge_count)},
            {"image_width", width}, {"image_height", height}, {"sample_format", "UInt8 Mono"}, {"image_file", "PNG"},
            {"background", 24}, {"foreground", 220}, {"threshold", 128}, {"rectangle_xywh",
                QJsonArray{object_left, object_top, object_width, object_height}}, {"blob_area_pixels", object_width * object_height},
            {"vision_nodes", 3}, {"simulation_nodes", static_cast<qint64>(node_count - 3)}}},
        {"environment", QJsonObject{{"qt_version", QString::fromLatin1(qVersion())}, {"platform", QGuiApplication::platformName()},
            {"device_pixel_ratio", window.devicePixelRatioF()}, {"window_size", size_json(window.size())},
            {"canvas_input", "Synthetic QMouseEvent sequence to Qt canvas viewport"}, {"canvas_zoom_percent", 100},
            {"canvas_viewport_size_at_run", size_json(canvas_pixels)}, {"image_viewport_size_at_run", size_json(image_pixels)}}},
        {"policy", QJsonObject{{"rounds", sample_rounds}, {"io_rounds", io_rounds}, {"drags_per_load", drags_per_load},
            {"history_limit", 16}, {"observation_capacity", 32}, {"retained_runs", retained}, {"dropped_notifications", dropped}}},
        {"timings_ms", QJsonObject{{"canvas_drag_to_paint", distribution(std::move(drags))},
            {"load_to_ready", distribution(std::move(loads))}, {"save_to_committed", distribution(std::move(saves))},
            {"run_to_quiescent_ui", distribution(std::move(runs))}, {"threshold_show_to_paint", distribution(std::move(threshold_refresh))},
            {"blob_show_to_paint", distribution(std::move(blob_refresh))}}},
        {"memory_samples", memory}};
}
} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Measure the S5.6 desktop acceptance load."));
    parser.addHelpOption();
    const QCommandLineOption output(QStringLiteral("output"), QStringLiteral("Write measurements to a JSON file."), QStringLiteral("path"));
    parser.addOption(output);
    parser.process(application);
    try {
        const auto json = QJsonDocument(measure()).toJson(QJsonDocument::Indented);
        if (parser.isSet(output)) {
            QSaveFile file(parser.value(output));
            require(file.open(QIODevice::WriteOnly) && file.write(json) == json.size() && file.commit(),
                "Unable to publish the JSON measurement report.");
        } else {
            std::fwrite(json.constData(), 1, static_cast<std::size_t>(json.size()), stdout);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "S5.6 load probe failed: %s\n", error.what());
        return 1;
    }
}
