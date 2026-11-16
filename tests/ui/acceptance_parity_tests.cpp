#include "control_scene.hpp"
#include "editor_window.hpp"
#include "run_controller.hpp"
#include "run_summary.hpp"
#include "tool_catalog.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QGraphicsView>
#include <QInputDialog>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace infrastructure;
using namespace workflow;
using nlohmann::json;
namespace fs = std::filesystem;

void require(bool condition, std::string message)
{
    if (!condition) { throw std::runtime_error(std::move(message)); }
}

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}

template<UuidIdentity Id> Id uuid(const json& value)
{
    const auto& text = value.get_ref<const std::string&>();
    const auto id = take(IdFactory::parse<Id>(text));
    require(id.to_string() == text, "Expected a canonical UUID: " + text);
    return id;
}

template<RuntimeIdentity Id> Id runtime_id(const json& value)
{
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t number{};
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), number);
    require(converted.ec == std::errc{} && converted.ptr == text.data() + text.size()
        && text == std::to_string(number), "Expected a canonical runtime ID: " + text);
    return take(RuntimeIdFactory::from_value<Id>(number));
}

QString path_text(const fs::path& path) { return QString::fromStdU16String(path.u16string()); }

QByteArray read_file(const fs::path& path)
{
    QFile file(path_text(path));
    require(file.open(QIODevice::ReadOnly), "Could not read " + file.fileName().toStdString());
    return file.readAll();
}

QAction& action(EditorWindow& window, const char* name)
{
    auto* found = window.findChild<QAction*>(QString::fromLatin1(name));
    require(found && found->isEnabled(), std::string("Action unavailable: ") + name);
    return *found;
}

ControlScene& scene(EditorWindow& window)
{
    auto* view = qobject_cast<QGraphicsView*>(window.centralWidget());
    return *qobject_cast<ControlScene*>(view->scene());
}

void wait_for_file(EditorWindow& window)
{
    require(QTest::qWaitFor([&] { return !window.file_busy(); }, 10000),
        "The editor file action did not finish.");
}

QPushButton* inspector_button(EditorWindow& window, const char* name, const QString& target)
{
    for (auto* button : window.findChildren<QPushButton*>(QString::fromLatin1(name))) {
        if (button->isVisible() && button->property("bindingTarget").toString() == target) { return button; }
    }
    return nullptr;
}

QPushButton* ok_button(QDialog& dialog)
{
    const auto* buttons = dialog.findChild<QDialogButtonBox*>();
    return buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
}

using DialogStep = std::function<QPushButton*(QDialog&)>;

void drive_dialog(EditorWindow& window, const char* name, const QString& target,
    std::initializer_list<DialogStep> steps)
{
    QPushButton* start = nullptr;
    require(QTest::qWaitFor([&] {
        start = inspector_button(window, name, target);
        return start && start->isEnabled();
    }, 3000), "The inspector target did not become available: " + target.toStdString());
    auto next = steps.begin();
    QString stalled_at;
    QTimer poll;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, [&] {
        poll.stop();
        if (const auto* modal = QApplication::activeModalWidget()) { stalled_at = modal->windowTitle(); }
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QDialog*>(widget); dialog && dialog->isVisible() && dialog->isModal()) {
                dialog->reject();
            }
        }
    });
    QObject::connect(&poll, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || next == steps.end()) { return; }
        if (auto* button = (*next)(*dialog)) {
            ++next;
            QTimer::singleShot(0, button, [button] { button->click(); });
        }
    });
    poll.start(10);
    timeout.start(5000);
    start->click();
    require(next == steps.end() && timeout.isActive(),
        "Inspector edit stalled at step " + std::to_string(next - steps.begin() + 1) + ": " + stalled_at.toStdString());
}

DialogStep source_kind(QString text)
{
    return [text = std::move(text)](QDialog& dialog) -> QPushButton* {
        auto* types = dialog.findChild<QComboBox*>(QStringLiteral("choice"));
        if (!types || dialog.windowTitle() != QStringLiteral("Binding source")) { return nullptr; }
        const auto index = types->findText(text);
        if (index < 0) { return nullptr; }
        types->setCurrentIndex(index);
        return ok_button(dialog);
    };
}

QPushButton* set_source(QDialog& dialog) { return dialog.findChild<QPushButton*>(QStringLiteral("setBindingSource")); }

QPushButton* accept_binding(QDialog& dialog) { return set_source(dialog) ? ok_button(dialog) : nullptr; }

const json& named(const json& items, std::string_view key, const json& value)
{
    const auto item = std::ranges::find_if(items, [&](const json& object) { return object.at(key) == value; });
    require(item != items.end(), "Missing item " + std::string(key) + "=" + value.dump());
    return *item;
}

const json& output(const json& node, std::string_view id)
{
    const auto& slot = named(node.at("outputs"), "id", id);
    require(slot.at("status") == "Produced", "Output was not produced: " + std::string(id));
    return slot.at("value");
}

bool has_run_identity(const json& entry, const json& run)
{
    return entry.at("session") == run.at("session") && entry.at("snapshot") == run.at("snapshot")
        && entry.at("run") == run.at("run") && entry.at("workflow") == run.at("workflow");
}

const json& check_producer(const json& trace, const json& run, const json& consumer)
{
    require(has_run_identity(trace, run), "A trace referenced another run: " + trace.dump());
    const auto& producer = named(run.at("nodes"), "id", trace.at("node"));
    require(producer.at("status") == "Succeeded" && !producer.at("invocation").is_null()
        && trace.at("scope") == producer.at("scope") && trace.at("invocation") == producer.at("invocation")
        && trace.at("attempt") == producer.at("attempt"), "A trace referenced the wrong producer invocation: " + trace.dump());
    const auto& order = run.at("call_order");
    require(std::ranges::find(order, producer.at("id")) < std::ranges::find(order, consumer.at("id")),
        "A traced producer did not precede its consumer.");
    return producer;
}

void validate_report(const json& run, const json& fixture, const Solution& saved)
{
    require(uuid<SolutionId>(run.at("solution")) == saved.id && uuid<WorkflowId>(run.at("workflow")) == uuid<WorkflowId>(fixture.at("id"))
        && run.at("revision") == std::to_string(saved.revision), "Summary did not describe the saved solution and workflow.");
    (void)uuid<SolutionSessionId>(run.at("session"));
    (void)runtime_id<ConfigSnapshotId>(run.at("snapshot"));
    (void)runtime_id<WorkflowRunId>(run.at("run"));
    require(run.at("status") == "Succeeded" && run.at("quiescent") == true && run.at("diagnostics").empty()
        && run.at("load_diagnostics").empty(), "The run did not finish cleanly: " + run.at("diagnostics").dump());
    const auto& order = run.at("call_order");
    const auto& nodes = run.at("nodes");
    require(nodes.size() == fixture.at("expected").at("call_count").get<std::size_t>() && order.size() == nodes.size(),
        "Unexpected number of node calls.");
    std::set<InvocationId> invocations;
    for (const auto& id : order) {
        const auto& node = named(nodes, "id", id);
        require(invocations.insert(runtime_id<InvocationId>(node.at("invocation"))).second
            && runtime_id<ExecutionScopeId>(node.at("scope")).value() == 0
            && runtime_id<AttemptId>(node.at("attempt")).value() == 0, "Unexpected call identity in a root workflow.");
        require(node.at("status") == "Succeeded" && node.at("diagnostics").empty(), "A fixed fixture node did not succeed.");
        for (const auto& binding : node.at("bindings")) {
            for (const auto& trace : binding.at("sources")) {
                if (trace.at("kind") == "constant") { continue; }
                const auto& producer = check_producer(trace, run, node);
                require(trace.at("kind") == "result" && trace.at("output_kind") == "port", "Unexpected S4 binding source.");
                (void)output(producer, trace.at("output").get_ref<const std::string&>());
            }
            if (const auto regions = binding.find("regions"); regions != binding.end()) {
                for (const auto& region : *regions) {
                    const auto& trace = region.at("producer");
                    const auto& producer = check_producer(trace, run, node);
                    const auto& reference = region.at("reference");
                    require(reference.at("workflow") == run.at("workflow") && reference.at("node") == trace.at("node")
                        && reference.at("output") == trace.at("output")
                        && output(producer, trace.at("output").get_ref<const std::string&>()).at("id") == region.at("region"),
                        "An inherited ROI did not point to its producing port and region.");
                }
            }
        }
    }
    const auto& reader = named(nodes, "id", fixture.at("reader_node_id"));
    const auto& resource = named(reader.at("resources"), "resource", fixture.at("input_resource_id"));
    require(resource.at("kind") == "Image", "The reader did not acquire the expected image resource.");
    const auto& links = run.at("control_links");
    const auto* workflow = find_workflow(saved, uuid<WorkflowId>(fixture.at("id")));
    require(workflow && links.size() == workflow->control_links.size(), "The control links did not cover the saved workflow.");
    for (const auto& link : workflow->control_links) {
        const auto& edge = named(links, "id", link.id.to_string());
        require(edge.at("status") == "Arrived" && edge.at("close_reason").is_null(),
            "An unconditional fixture control link did not arrive.");
    }
}

// Each identity is checked within its run before these process-local addresses are removed.
void omit_run_ids(json& entry)
{
    entry.erase("session");
    entry.erase("snapshot");
    entry.erase("run");
}

json normalize(json report)
{
    omit_run_ids(report);
    for (auto& node : report.at("nodes")) {
        node.erase("invocation");
        for (auto& binding : node.at("bindings")) {
            for (auto& source : binding.at("sources")) {
                if (source.at("kind") != "result") { continue; }
                omit_run_ids(source);
                source.erase("invocation");
            }
            if (auto regions = binding.find("regions"); regions != binding.end()) {
                for (auto& region : *regions) {
                    auto& producer = region.at("producer");
                    omit_run_ids(producer);
                    producer.erase("invocation");
                }
            }
        }
    }
    return report;
}

json run_cli(const fs::path& package, const WorkflowId& workflow)
{
    QTemporaryDir io(path_text(package.parent_path() / "cli-XXXXXX"));
    require(io.isValid(), "Could not create a separate CLI working directory.");
    const auto result_path = io.filePath(QStringLiteral("run.json"));
    const auto diagnostic_path = io.filePath(QStringLiteral("run.err"));
    QProcess cli;
    cli.setWorkingDirectory(io.path());
    // Read the complete report after exit, including reports larger than a pipe buffer.
    cli.setStandardOutputFile(result_path);
    cli.setStandardErrorFile(diagnostic_path);
    cli.start(QString::fromUtf8(VISIONPRO_TEST_CLI), {QStringLiteral("run"), QStringLiteral("--solution"),
        path_text(package / solution_manifest_name), QStringLiteral("--workflow"), QString::fromStdString(workflow.to_string())});
    if (!cli.waitForFinished(10000) || cli.exitStatus() != QProcess::NormalExit || cli.exitCode() != 0) {
        QFile errors(diagnostic_path);
        const auto details = errors.open(QIODevice::ReadOnly) ? errors.readAll().toStdString() : std::string{};
        throw std::runtime_error("Independent CLI failed for " + workflow.to_string() + ": "
            + cli.errorString().toStdString() + " " + details);
    }
    const auto data = read_file(fs::path(result_path.toStdU16String()));
    try {
        return json::parse(data.toStdString());
    } catch (const json::parse_error& failure) {
        throw std::runtime_error("Invalid CLI report for " + workflow.to_string() + " (" + std::to_string(data.size())
            + " bytes): " + failure.what());
    }
}

void check_fixed_outputs(const json& run, const json& fixture, std::string_view phase)
{
    const bool edited = phase != "baseline";
    const auto node = [&](std::string_view field) -> const json& { return named(run.at("nodes"), "id", fixture.at(field)); };
    if (fixture.at("case") == "single") {
        const auto& blob = node("blob_node_id");
        const auto& judge = node("judge_node_id");
        require(output(blob, "count") == "2" && output(blob, "total_area_pixels") == "15"
            && output(node("threshold_node_id"), "binary").at("samples").at("nonzero_count") == 15,
            "Single-chain component/pixel counts changed unexpectedly.");
        const auto& components = output(blob, "components");
        require(components.at(0).at("area_pixels") == "6" && components.at(1).at("area_pixels") == "9"
            && components.at(0).at("centroid").at("x") == 2 && components.at(0).at("centroid").at("y") == 1.5,
            "Single-chain component geometry did not match the fixture.");
        const auto& source = named(judge.at("bindings"), "target", "value").at("sources").at(0);
        const auto selector = edited ? json::array({{{"index", 0}}, {{"field", "area_pixels"}}}) : json::array();
        require(source.at("output") == (edited ? "components" : "count") && source.at("selector") == selector
            && judge.at("judgement") == (edited ? "NG" : "OK") && output(judge, "passed") == !edited,
            "The single-chain judgement or its edited selector was lost.");
        qInfo().noquote() << QString::fromStdString(std::string(phase)) << "single: 4 calls, area 15, judge"
            << QString::fromStdString(judge.at("judgement").get<std::string>());
    } else if (fixture.at("case") == "five") {
        const auto values = edited ? json::array({"2", "2", "2", "2", "0"}) : json::array({"2", "2", "2", "0", "0"});
        require(output(node("collect_node_id"), "values") == values && output(node("deliver_node_id"), "values") == values,
            "Five-lane collect and delivery did not preserve input order.");
        const auto& lanes = fixture.at("lanes");
        for (std::size_t index = 0; index < lanes.size(); ++index) {
            const auto& lane = lanes.at(index);
            const auto& blob = named(run.at("nodes"), "id", lane.at("blob_node_id"));
            const auto& judge = named(run.at("nodes"), "id", lane.at("judge_node_id"));
            require(output(blob, "count") == values.at(index)
                && judge.at("judgement") == (values.at(index) == "2" ? "OK" : "NG"), "Incorrect five-lane count or judgement.");
        }
        qInfo().noquote() << QString::fromStdString(std::string(phase)) << "five: 18 calls, delivered"
            << QString::fromStdString(values.dump());
    } else {
        require(fixture.at("case") == "inherited_roi", "Unexpected fixed S4 workflow.");
        const auto& blob = node("blob_node_id");
        const auto& judge = node("judge_node_id");
        const auto& region = named(blob.at("bindings"), "target", "roi").at("regions").at(0);
        const auto& transform = region.at("reference").at("transform").at("matrix");
        const auto& area = output(blob, "total_area_pixels");
        const auto& centroid = output(blob, "components").at(0).at("centroid");
        require(transform.at(2) == (edited ? -1 : 1)
            && output(node("rotated_threshold_node_id"), "roi_mask").at("member_count") == 7
            && area == (edited ? "7" : "6")
            && centroid.at("x") == (edited ? 3 : 5) && centroid.at("y") == 4
            && judge.at("judgement") == (edited ? "NG" : "OK"),
            "Incorrect inherited ROI: transform " + transform.dump() + ", area " + area.dump() + ", centroid " + centroid.dump()
                + ", judgement " + judge.at("judgement").dump());
        qInfo().noquote() << QString::fromStdString(std::string(phase)) << "inherited_roi: 5 calls, mask 7, area"
            << (edited ? 7 : 6) << ", centroid x" << (edited ? 3 : 5);
    }
}

json compare_workflow(EditorWindow& window, const json& fixture, const fs::path& package,
    const Solution& saved, std::string_view phase)
{
    const auto workflow = uuid<WorkflowId>(fixture.at("id"));
    require(scene(window).select_workflow(workflow), "The editor could not select the S4 workflow.");
    auto* controller = window.findChild<RunController*>();
    require(controller && QTest::qWaitFor([&] { return controller->can_start(); }, 10000), "Previous execution did not quiesce.");
    const auto previous = controller->runs().size();
    action(window, "runWorkflow").trigger();
    const auto finished = QTest::qWaitFor([&] { return controller->can_start() && controller->runs().size() == previous + 1; }, 10000);
    require(finished, "Desktop execution did not quiesce: " + window.statusBar()->currentMessage().toStdString());
    const auto& handle = controller->runs().back();
    require(handle.identity().workflow == workflow && handle.request().inputs().empty(), "Unexpected desktop workflow or inputs.");
    auto gui = take(app::summarize_run(handle, take(handle.inspect()), {}));
    auto cli = run_cli(package, workflow);
    validate_report(gui, fixture, saved);
    validate_report(cli, fixture, saved);
    require(gui.at("session") != cli.at("session"), "The two entry points did not use independent sessions.");
    check_fixed_outputs(gui, fixture, phase);
    gui = normalize(std::move(gui));
    cli = normalize(std::move(cli));
    require(gui == cli, "GUI/CLI mismatch for " + fixture.at("case").get<std::string>() + ": "
        + json::diff(gui, cli).dump().substr(0, 1600));
    return gui;
}

void apply_semantic_edits(EditorWindow& window, const json& workflows)
{
    const auto& single = named(workflows, "case", "single");
    const auto& five = named(workflows, "case", "five");
    const auto& inherited = named(workflows, "case", "inherited_roi");
    const auto reveal = [&](const json& fixture, const json& id) {
        const auto workflow = uuid<WorkflowId>(fixture.at("id"));
        const auto node = uuid<NodeId>(id);
        const auto* flow = find_workflow(window.draft(), workflow);
        const auto* selected = flow ? find_node(*flow, node) : nullptr;
        require(selected && scene(window).select_workflow(workflow) && scene(window).reveal_node(node),
            "Could not reveal the editable fixture node " + node.to_string());
        const auto name = QString::fromStdString(selected->name);
        require(QTest::qWaitFor([&] {
            return std::ranges::any_of(window.findChildren<QPushButton*>(QStringLiteral("renameNode")),
                [&](const auto* button) { return button->isVisible() && button->toolTip() == name; });
        }, 3000), "Inspector did not refresh after selecting " + selected->name);
    };
    reveal(single, single.at("judge_node_id"));
    drive_dialog(window, "editBinding", QStringLiteral("value"), {set_source, source_kind(QStringLiteral("Node output")),
        [](QDialog& dialog) -> QPushButton* {
            auto* source = dialog.findChild<QComboBox*>(QStringLiteral("bindingSource"));
            auto* selector = dialog.findChild<QLineEdit*>(QStringLiteral("bindingSelector"));
            if (!source || !selector) { return nullptr; }
            for (int index = 0; index < source->count(); ++index) {
                if (!source->itemText(index).contains(QStringLiteral(" → components • "))) { continue; }
                source->setCurrentIndex(index);
                selector->setText(QStringLiteral("0/area_pixels"));
                return ok_button(dialog);
            }
            return nullptr;
        }, accept_binding});

    reveal(five, five.at("lanes").at(3).at("threshold_node_id"));
    drive_dialog(window, "editBinding", QStringLiteral("threshold"), {set_source, source_kind(QStringLiteral("Constant")),
        [](QDialog& dialog) -> QPushButton* {
            auto* constant = qobject_cast<QInputDialog*>(&dialog);
            if (!constant) { return nullptr; }
            constant->setTextValue(QStringLiteral("150"));
            return ok_button(dialog);
        }, accept_binding});

    reveal(inherited, inherited.at("blob_node_id"));
    drive_dialog(window, "editRegion", QStringLiteral("roi"), {[](QDialog& dialog) -> QPushButton* {
        auto* correction = dialog.findChild<QLineEdit*>(QStringLiteral("roiCorrectionX"));
        if (dialog.objectName() != QStringLiteral("roiEditor") || !correction) { return nullptr; }
        correction->setText(QStringLiteral("-1"));
        return ok_button(dialog);
    }});
}

class AcceptanceParityTests final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void compares_saved_s4_workflows();
};

void AcceptanceParityTests::compares_saved_s4_workflows()
{
    try {
        QTemporaryDir scratch;
        require(scratch.isValid(), "Could not create a temporary package.");
        const fs::path root(scratch.path().toStdU16String());
        const fs::path sample(QString::fromUtf8(VISIONPRO_S4_DIRECTORY).toStdU16String());
        const auto package = root / "editor package";
        const auto manifest = read_file(sample / solution_manifest_name);
        const auto catalog = json::parse(read_file(sample / "inputs.json").toStdString());
        const auto& workflows = catalog.at("workflows");
        require(workflows.size() == 3, "Expected the three S4 fixture workflows.");
        const auto registry = take(app::make_tool_registry());
        EditorWindow window(take(load_solution(sample, registry)).solution(), registry);
        window.show();
        window.open_document(sample);
        wait_for_file(window);
        window.transfer_document(package, SolutionTransfer::SaveAs);
        wait_for_file(window);
        const auto baseline = take(load_solution(package, registry));
        for (const auto& fixture : workflows) {
            (void)compare_workflow(window, fixture, package, baseline.solution(), "baseline");
        }

        apply_semantic_edits(window, workflows);
        require(window.isWindowModified(), "Semantic edits did not enter the document history.");
        action(window, "saveSolution").trigger();
        wait_for_file(window);
        const auto edited = take(load_solution(package, registry));
        require(edited.solution().revision == baseline.solution().revision + 1 && !window.isWindowModified(),
            "Semantic edits did not commit as a new revision.");
        std::vector<json> expected;
        for (const auto& fixture : workflows) {
            auto summary = compare_workflow(window, fixture, package, edited.solution(), "edited");
            summary.erase("revision");
            expected.push_back(std::move(summary));
        }

        const QPointF position(730.25, 385.5);
        const auto move = [&](const json& fixture) {
            const auto workflow = uuid<WorkflowId>(fixture.at("id"));
            require(scene(window).select_workflow(workflow), "Could not select the layout's workflow.");
            const auto reader = uuid<NodeId>(fixture.at("reader_node_id"));
            const auto& model = static_cast<const ControlGraphModel&>(scene(window).graphModel());
            const CanvasMove placement{model.canvas_id(reader), position};
            require(scene(window).move_nodes(std::span{&placement, 1}), "Could not move the reader on the canvas.");
        };
        for (const auto& fixture : workflows) { move(fixture); }
        action(window, "saveSolution").trigger();
        wait_for_file(window);
        window.open_document(package);
        wait_for_file(window);
        const auto reloaded = take(load_solution(package, registry));
        require(reloaded.solution().revision == edited.solution().revision + 1
            && window.draft().revision == reloaded.solution().revision, "Layout edit did not survive save and reload.");
        for (std::size_t index = 0; index < workflows.size(); ++index) {
            const auto& fixture = workflows.at(index);
            const auto layout = std::ranges::find(window.draft().layouts, uuid<WorkflowId>(fixture.at("id")), &WorkflowLayout::workflow);
            require(layout != window.draft().layouts.end(), "The saved workflow layout is absent.");
            const auto node = std::ranges::find(layout->nodes, uuid<NodeId>(fixture.at("reader_node_id")), &NodePlacement::node);
            require(node != layout->nodes.end() && QPointF(node->x, node->y) == position, "The saved node position changed.");
            auto actual = compare_workflow(window, fixture, package, reloaded.solution(), "layout");
            actual.erase("revision");
            require(actual == expected.at(index), "Saving the layout changed run semantics: "
                + json::diff(expected.at(index), actual).dump().substr(0, 1600));
        }
        const auto& resources = reloaded.solution().resources;
        for (const auto& image : catalog.at("resources")) {
            const auto id = uuid<ResourceId>(image.at("id"));
            const auto resource = std::ranges::find(resources, id, &ResourceDefinition::id);
            require(resource != resources.end(), "The package lost image resource " + id.to_string());
            const auto* file = std::get_if<ExternalFileResource>(&resource->location);
            require(file && file->path.base == PathBase::SolutionDirectory, "The copied image lost its package-relative location.");
            const auto original = sample / fs::path(QString::fromStdString(image.at("path").get<std::string>()).toStdU16String());
            const auto copied = reloaded.directory() / fs::path(QString::fromStdString(file->path.path).toStdU16String());
            const auto relative = fs::relative(copied, reloaded.directory());
            require(!relative.empty() && relative.is_relative() && *relative.begin() != ".." && !fs::equivalent(original, copied)
                && read_file(original) == read_file(copied), "The package did not preserve the fixed input as a separate file.");
        }
        require(read_file(sample / solution_manifest_name) == manifest, "The original sample manifest changed.");
        window.close();
        require(QTest::qWaitFor([&] { return !window.isVisible(); }, 10000), "The desktop session did not close.");
    } catch (const std::exception& failure) {
        QFAIL(failure.what());
    }
}
} // namespace
} // namespace visionpro::ui

QTEST_MAIN(visionpro::ui::AcceptanceParityTests)
#include "acceptance_parity_tests.moc"
