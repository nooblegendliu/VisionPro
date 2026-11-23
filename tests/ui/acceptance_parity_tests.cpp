#include "control_scene.hpp"
#include "editor_window.hpp"
#include "run_controller.hpp"
#include "run_summary.hpp"
#include "tool_catalog.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/core/shared_data.hpp>
#include <visionpro/infrastructure/subgraph_package.hpp>
#include <visionpro/infrastructure/camera.hpp>
#include <visionpro/infrastructure/communication.hpp>

#include <QAction>
#include <QDockWidget>
#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QGraphicsView>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QProcess>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <ranges>
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
    auto* view = window.findChild<QGraphicsView*>(QStringLiteral("workflowCanvas"));
    return *qobject_cast<ControlScene*>(view->scene());
}

void wait_for_file(EditorWindow& window)
{
    require(QTest::qWaitFor([&] { return !window.file_busy(); }, 10000),
        "The editor file action did not finish.");
}

QPushButton* inspector_button(EditorWindow& window, const char* name, const QString& target)
{
    if (window.findChild<QDockWidget*>(QStringLiteral("inspectorDock"))->isHidden()) { action(window, "showProperties").trigger(); }
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

json run_cli(const fs::path& package, const WorkflowId& workflow, const QStringList& extra = {},
    const QString& command = QStringLiteral("run"))
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
    QStringList args{command, QStringLiteral("--solution"), path_text(package / solution_manifest_name),
        QStringLiteral("--workflow"), QString::fromStdString(workflow.to_string())};
    args.append(extra);
    cli.start(QString::fromUtf8(VISIONPRO_TEST_CLI), args);
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
    action(window, "showProperties").trigger();
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
    void compares_saved_blob_loop();
    void compares_saved_loop_in_group();
    void imports_and_runs_blob_groups();
    void reproduces_saved_calculations();
    void reproduces_saved_shared_workflows();
    void reproduces_interleaved_workpieces();
    void reproduces_saved_reentrant_workflows();
    void reproduces_saved_calibrated_measurement();
    void reproduces_saved_workpiece_inspection();
    void sends_saved_five_lane_inspection_over_tcp();
    void configures_and_acquires_saved_camera();
    void configures_and_arms_saved_camera();
    void reproduces_saved_communication_data();
    void reproduces_saved_communication();
};

namespace {
json complete_editor_run(EditorWindow& window, const char* command, std::optional<int> threshold = {}, bool accept_defaults = false)
{
    auto* controller = window.findChild<RunController*>();
    require(controller && QTest::qWaitFor([&] { return controller->can_start(); }, 5000), "Editor still busy.");
    const auto previous = controller->runs().size();
    bool accepted = false;
    QTimer fill_dialog;
    fill_dialog.setSingleShot(true);
    if (threshold || accept_defaults) {
        QObject::connect(&fill_dialog, &QTimer::timeout, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) { return; }
            if (!ok_button(*dialog)) { dialog->reject(); return; }
            if (threshold) {
                auto* mode = dialog->findChild<QComboBox*>(QStringLiteral("runInputMode.threshold"));
                auto* value = dialog->findChild<QLineEdit*>(QStringLiteral("runInputValue.threshold"));
                if (!mode || !value) { dialog->reject(); return; }
                mode->setCurrentIndex(1);
                value->setText(QString::number(*threshold));
            }
            ok_button(*dialog)->click();
            accepted = dialog->result() == QDialog::Accepted;
            if (!accepted) { dialog->reject(); }
        });
        fill_dialog.start(0);
    }
    action(window, command).trigger();
    require(!(threshold || accept_defaults) || accepted, "The editor did not accept the public inputs.");
    const bool finished = QTest::qWaitFor([&] { return controller->can_start() && controller->runs().size() == previous + 1; }, 8000);
    require(finished, "The editor did not finish a new run: " + window.statusBar()->currentMessage().toStdString());
    const auto run = controller->runs().back();
    return take(app::summarize_run(run, take(run.inspect()), {}));
}

void check_blob_group(const json& report, NodeId group, int calls, bool empty = false)
{
    require(report.at("status") == "Succeeded" && report.at("quiescent") == true,
        "Group run failed: " + report.at("diagnostics").dump());
    const auto& node = named(report.at("nodes"), "id", group.to_string());
    const auto& scope = named(report.at("groups"), "group_node", group.to_string());
    require(scope.at("group_invocation") == node.at("invocation") && scope.at("parent_scope") == node.at("scope")
        && node.at("selected_outputs") == json::array({"measured"}), "The Group lost its invocation or named outlet.");
    require(output(node, "calls") == std::to_string(calls) && output(node, "total_area") == (empty ? "0" : "15"),
        "Unexpected Group state or total area.");
    const auto& points = output(node, "points");
    require(points.size() == (empty ? 0u : 2u) && (empty ||
        (points.at(0).at("x") == 2 && points.at(0).at("y") == 1.5 &&
         points.at(1).at("x") == 8 && points.at(1).at("y") == 6)), "Unexpected point set.");
    for (auto port : {"points", "calls"}) {
        const auto& trace = named(scope.at("exports"), "output", port).at("producer");
        require(has_run_identity(trace, report) && trace.at("scope") == scope.at("scope"),
            "A Group export crossed an instance boundary.");
        const auto& producer = named(report.at("nodes"), "id", trace.at("node"));
        require(producer.at("invocation") == trace.at("invocation") && producer.at("attempt") == trace.at("attempt")
            && output(producer, trace.at("output").get<std::string>()) == output(node, port),
            "A Group export does not resolve to its internal call.");
    }
    const auto iterations = std::ranges::count_if(report.at("iterations"), [&](const json& item) {
        return item.at("parent_scope") == scope.at("scope");
    });
    require(iterations == (empty ? 0 : 2), "The nested loop belongs to the wrong Group instance.");
}
} // namespace

void AcceptanceParityTests::imports_and_runs_blob_groups()
{
    try {
        QTemporaryDir scratch;
        require(scratch.isValid(), "No temporary directory.");
        const fs::path root(scratch.path().toStdU16String());
        const auto source = root / "source", host = root / "host", package = root / "portable";
        fs::copy(fs::path(QString::fromUtf8(VISIONPRO_S62_DIRECTORY).toStdU16String()), source, fs::copy_options::recursive);
        const auto registry = take(app::make_tool_registry());
        const auto original = take(load_solution(source, registry));
        const auto& source_flow = original.solution().workflows.at(0);
        const auto original_group = source_flow.entries.at(0);
        check_blob_group(run_cli(source, source_flow.id), original_group, 1);

        const auto target_flow = take(IdFactory::generate<WorkflowId>());
        const auto schema = take(SchemaIdFactory::parse<SchemaId>("test.s62.host"));
        Workflow workflow{target_flow, "Imported instances", {schema, 1}};
        Solution target{take(IdFactory::generate<SolutionId>()), "Separate host", solution_format_version, 1, {workflow}};
        take(transfer_solution(host, target, SolutionTransfer::SaveAs));

        EditorWindow window(original.solution(), registry);
        window.show();
        window.open_document(source);
        wait_for_file(window);
        require(scene(window).select_workflow(source_flow.id), "Could not select the export source.");
        window.export_subgraph(package, original_group);
        wait_for_file(window);
        require(fs::is_regular_file(package / subgraph_manifest_name),
            "Group export failed: " + window.statusBar()->currentMessage().toStdString());
        window.open_document(host);
        wait_for_file(window);
        require(scene(window).select_workflow(target_flow), "Could not select the separate host workflow.");
        auto import_instance = [&] {
            const auto n = window.draft().workflows.at(0).nodes.size();
            window.import_subgraph(package);
            wait_for_file(window);
            require(window.draft().workflows.at(0).nodes.size() > n, window.statusBar()->currentMessage().toStdString());
            const auto selected = scene(window).selected_nodes();
            require(selected.size() == 1, "Import must select the new Group.");
            return selected.front();
        };
        const auto a = import_instance();
        const auto b = import_instance();
        require(a != b && a != original_group && b != original_group, "Import reused a Group identity.");
        const auto outlet = take(SchemaIdFactory::parse<ControlOutputId>("measured"));
        const auto edge = take(IdFactory::generate<ControlLinkId>());
        require(scene(window).edit_draft(QStringLiteral("Call both imported Groups"), [&](Solution& draft) -> Result<std::monostate> {
            auto& flow = draft.workflows.front();
            flow.entries = {a};
            flow.control_links.push_back({edge, a, outlet, b});
            return std::monostate{};
        }), "Could not connect both imported instances.");
        action(window, "saveSolution").trigger();
        wait_for_file(window);
        fs::rename(source, root / "source-moved");
        fs::rename(package, root / "package-moved");
        window.open_document(host);
        wait_for_file(window);
        require(scene(window).select_workflow(target_flow), "The saved host did not reload.");

        const auto desktop = complete_editor_run(window, "runWorkflow");
        const auto cli = run_cli(host, target_flow);
        require(desktop.at("revision") == cli.at("revision") && desktop.at("session") != cli.at("session")
            && desktop.at("call_order") == cli.at("call_order"), "The CLI did not reproduce the saved workflow.");
        for (const auto instance : {a, b}) {
            check_blob_group(desktop, instance, 1);
            check_blob_group(cli, instance, 1);
            require(named(desktop.at("nodes"), "id", instance.to_string()).at("outputs")
                == named(cli.at("nodes"), "id", instance.to_string()).at("outputs"), "Saved instance outputs differ.");
        }
        const auto debug = [&](NodeId id, const char* action_id, int cutoff) {
            require(scene(window).reveal_node(id), "Could not select the imported Group.");
            auto report = complete_editor_run(window, action_id, cutoff);
            const auto subtree = take(descendants(window.draft().workflows.front(), id));
            require(report.at("groups").size() == 1 && std::ranges::all_of(report.at("call_order"), [&](const json& n) {
                return n == id.to_string() || std::ranges::any_of(subtree, [&](const auto& child) { return n == child.to_string(); });
            }), "Independent execution ran an outer neighbor or another instance.");
            return report;
        };
        check_blob_group(debug(a, "debugSelectedNode", 255), a, 1, true);
        const auto isolated_cli = run_cli(host, target_flow, {QStringLiteral("--group"), QString::fromStdString(a.to_string()),
            QStringLiteral("--input"), QStringLiteral("threshold=255")});
        check_blob_group(isolated_cli, a, 1, true);
        require(isolated_cli.at("groups").size() == 1, "CLI debug entered another Group.");
        check_blob_group(debug(a, "runSelectedGroup", 128), a, 2);
        const auto subsequent = complete_editor_run(window, "runWorkflow");
        check_blob_group(subsequent, a, 3);
        check_blob_group(subsequent, b, 2);
    } catch (const std::exception& failure) { QFAIL(failure.what()); }
}

namespace {
// Channels generate fresh message IDs in each process. Translate using the
// corresponding producer so the comparison still checks end-to-end identity.
void match_message_ids(json& value, const std::map<std::string, std::string>& ids)
{
    if (auto* text = value.get_ptr<std::string*>()) {
        if (const auto found = ids.find(*text); found != ids.end()) { *text = found->second; }
    } else if (value.is_structured()) {
        for (auto& child : value) { match_message_ids(child, ids); }
    }
}

void compare_saved_workflows(const char* directory)
{
    QTemporaryDir scratch;
    require(scratch.isValid(), "Temporary directory unavailable.");
    const fs::path sample(QString::fromUtf8(directory).toStdU16String());
    const auto package = fs::path(scratch.path().toStdU16String()) / "saved workflows";
    const auto registry = take(app::make_tool_registry());
    const auto original = take(load_solution(sample, registry));
    EditorWindow window(original.solution(), registry);
    window.show();
    window.open_document(sample);
    wait_for_file(window);
    window.transfer_document(package, SolutionTransfer::SaveAs);
    wait_for_file(window);
    window.open_document(package);
    wait_for_file(window);
    for (const auto& flow : original.solution().workflows) {
        require(scene(window).select_workflow(flow.id), "Could not select the reloaded workflow.");
        require(find_workflow(window.draft(), flow.id)->admission == flow.admission, "Saved workflow admission settings changed.");
        const auto desktop = complete_editor_run(window, "runWorkflow", {}, !flow.interface.inputs.empty());
        auto cli = run_cli(package, flow.id);
        require(cli.at("status") == "Succeeded" && desktop.at("status") == cli.at("status")
            && desktop.at("revision") == cli.at("revision") && desktop.at("session") != cli.at("session"),
            "Both processes must complete the same saved workflow revision.");
        std::map<std::string, std::string> ids;
        for (const auto& call : cli.at("nodes")) {
            for (const auto& slot : call.at("outputs")) {
                if (slot.at("id") != "message_id" || slot.at("status") != "Produced") { continue; }
                const auto& producer = named(desktop.at("nodes"), "id", call.at("id").get<std::string>());
                const auto actual = output(producer, "message_id").get<std::string>();
                require(slot.at("value") != actual && ids.emplace(slot.at("value").get<std::string>(), actual).second,
                    "Each send must issue a new message ID in each process.");
            }
        }
        match_message_ids(cli, ids);
        const auto& calls = desktop.at("nodes");
        require(calls.size() == cli.at("nodes").size(), "Desktop and CLI produced different invocation counts.");
        for (std::size_t i = 0; i < calls.size(); ++i) {
            const auto& a = calls.at(i);
            const auto& b = cli.at("nodes").at(i);
            require(a.at("id") == b.at("id") && a.at("scope") == b.at("scope")
                && a.at("status") == b.at("status") && a.at("judgement") == b.at("judgement")
                && a.at("outputs") == b.at("outputs") && a.at("selected_outputs") == b.at("selected_outputs"),
                "Desktop and CLI disagree at node " + a.at("id").get<std::string>());
        }
    }
}
} // namespace

void AcceptanceParityTests::reproduces_saved_calculations()
{
    try { compare_saved_workflows(VISIONPRO_S63_DIRECTORY); }
    catch (const std::exception& failure) { QFAIL(failure.what()); }
}

void AcceptanceParityTests::reproduces_saved_shared_workflows()
{
    try { compare_saved_workflows(VISIONPRO_S64_DIRECTORY); }
    catch (const std::exception& failure) { QFAIL(failure.what()); }
}

namespace {
json replay_cli(const fs::path& package, const fs::path& sequence, const fs::path& scratch)
{
    QProcess cli;
    cli.setWorkingDirectory(path_text(scratch));
    cli.setStandardOutputFile(path_text(scratch / "replay.json"));
    cli.start(QString::fromUtf8(VISIONPRO_TEST_CLI), {QStringLiteral("replay"), QStringLiteral("--solution"),
        path_text(package / solution_manifest_name), QStringLiteral("--sequence"), path_text(sequence)});
    const bool exited = cli.waitForFinished(15000);
    require(exited && cli.exitStatus() == QProcess::NormalExit && cli.exitCode() == 0,
        "CLI replay failed: " + cli.readAllStandardError().toStdString());
    return json::parse(read_file(scratch / "replay.json").toStdString()).at("runs");
}

json replay_editor(EditorWindow& window, const json& steps)
{
    auto runs = json::array();
    for (const auto& step : steps) {
        require(scene(window).select_workflow(uuid<WorkflowId>(step.at("workflow"))), "Could not select the next station.");
        QTimer::singleShot(0, &window, [&step] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) { return; }
            auto* piece = dialog->findChild<QPlainTextEdit*>(QStringLiteral("runInputValue.piece"));
            auto* measure = dialog->findChild<QLineEdit*>(QStringLiteral("runInputValue.measure"));
            if (!piece || !measure || !ok_button(*dialog)) { dialog->reject(); return; }
            piece->setPlainText(QString::fromStdString(step.at("inputs").at("piece").get<std::string>()));
            measure->setText(QString::fromStdString(step.at("inputs").at("measure").get<std::string>()));
            ok_button(*dialog)->click();
            if (dialog->result() != QDialog::Accepted) { dialog->reject(); }
        });
        runs.push_back(complete_editor_run(window, "runWorkflow"));
    }
    return runs;
}

json checked_workpiece_runs(json runs, const Solution& saved, const json& steps, const json& expected, bool edited)
{
    require(runs.size() == steps.size(), "Replay stopped before the final duplicate.");
    const auto session = runs.front().at("session");
    const auto& deliveries = expected.at(edited ? "edited_deliveries" : "baseline_deliveries");
    std::set<WorkflowRunId> identities;
    std::map<std::string, std::string> messages;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        auto& run = runs.at(i);
        const auto& step = steps.at(i);
        require(run.at("session") == session && identities.insert(runtime_id<WorkflowRunId>(run.at("run"))).second
            && run.at("solution") == saved.id.to_string() && run.at("workflow") == step.at("workflow")
            && run.at("revision") == std::to_string(saved.revision) && run.at("quiescent") == true
            && run.at("status") == "Succeeded" && run.at("load_diagnostics").empty() && run.at("diagnostics").empty(),
            "Each step must run the requested saved workflow, in one session with a distinct run ID.");
        const auto& flow = *find_workflow(saved, uuid<WorkflowId>(run.at("workflow")));
        const auto& nodes = run.at("nodes");
        const auto& measure = named(nodes, "id", flow.entries.front().to_string());
        const auto& join = named(nodes, "id", flow.nodes.at(1).id.to_string());
        const auto& send = named(nodes, "id", flow.nodes.at(2).id.to_string());
        const auto& read = named(nodes, "id", flow.nodes.at(3).id.to_string());
        const auto binding_source = [](const json& node, std::string_view target) -> const json& {
            return named(node.at("bindings"), "target", std::string(target)).at("sources").at(0);
        };
        const auto& measured = binding_source(join, "value");
        require(binding_source(measure, "left").at("input") == "measure" && binding_source(join, "key").at("input") == "piece"
            && measured.at("node") == measure.at("id") && measured.at("output") == "value",
            "The join must use the supplied workpiece key and this station's corrected measurement.");
        const bool ready = expected.at("outlet").at(i) == "ready";
        require(run.at("call_order").size() == expected.at("calls").at(i).get<std::size_t>()
            && join.at("selected_outputs") == json::array({expected.at("outlet").at(i)})
            && output(join, "status").at("member") == expected.at("status").at(i)
            && named(run.at("control_links"), "id", flow.control_links.at(1).id.to_string()).at("status") == (ready ? "Arrived" : "Closed"),
            "Unexpected invocation count or join outlet at step " + std::to_string(i + 1));
        if (ready) {
            const auto position = messages.size();
            const auto& delivery = deliveries.at(position);
            const auto sequence = std::to_string(position + 1);
            const auto& record = output(join, "record");
            const auto& received = output(read, "messages");
            const auto message_id = output(send, "message_id").get<std::string>();
            (void)uuid<MessageId>(message_id);
            require(binding_source(send, "payload").at("node") == join.at("id") && binding_source(send, "payload").at("output") == "record"
                && binding_source(send, "key").at("input") == "piece"
                && record.at("width") == std::to_string(delivery.at("width").get<int>())
                && record.at("height") == std::to_string(delivery.at("height").get<int>())
                && received.size() == 1 && received.front().at("payload") == record
                && received.front().at("key") == delivery.at("piece") && received.front().at("domain") == "inspection.piece"
                && received.front().at("queue_id") == saved.queues.front().id.to_string()
                && received.front().at("message_id") == message_id && received.front().at("sequence") == sequence
                && output(send, "sequence") == sequence && messages.emplace(message_id, "delivery-" + sequence).second,
                "The FIFO delivery must carry the matched workpiece record and its issuing message ID exactly once.");
        } else {
            require(send.at("status") == "Skipped" && read.at("status") == "Skipped"
                && named(join.at("outputs"), "id", "record").at("status") == "NotProduced",
                "An incomplete or duplicate workpiece produced a delivery.");
        }
        for (auto& node : run.at("nodes")) {
            require(node.at("diagnostics").empty(), "Unexpected node diagnostic in workpiece replay.");
            for (auto& binding : node.at("bindings")) {
                for (auto& source : binding.at("sources")) {
                    if (source.at("kind") == "result") { (void)check_producer(source, run, node); }
                    if (source.at("kind") == "public_input") {
                        require(has_run_identity(source, run) && source.at("origin") == "Provided"
                            && step.at("inputs").contains(source.at("input").get<std::string>()),
                            "Binding did not refer to a public input supplied to this run.");
                        omit_run_ids(source);
                    }
                }
            }
        }
        run = normalize(std::move(run));
    }
    require(messages.size() == deliveries.size(), "Replay did not deliver both complete workpieces.");
    match_message_ids(runs, messages);
    return runs;
}
}

void AcceptanceParityTests::reproduces_interleaved_workpieces()
{
    try {
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const fs::path temp(scratch.path().toStdU16String());
        const fs::path sample(VISIONPRO_S612_CROSS_DIRECTORY);
        const auto saved = temp / "edited workstations";
        const auto registry = take(app::make_tool_registry());
        const auto original = take(load_solution(sample, registry));
        const auto steps = json::parse(read_file(sample / "runs.json").toStdString());
        const auto expected = json::parse(read_file(sample / "expected.json").toStdString());
        EditorWindow window(original.solution(), registry);
        window.show();
        const auto compare = [&](const fs::path& package, bool edited) {
            window.open_document(package);
            wait_for_file(window);
            const auto document = take(load_solution(package, registry));
            require(document.diagnostics().empty(), "The saved workpiece solution produced load diagnostics.");
            auto desktop = replay_editor(window, steps);
            auto cli = replay_cli(package, sample / "runs.json", temp);
            require(desktop.front().at("session") != cli.front().at("session"), "Independent hosts shared a session.");
            desktop = checked_workpiece_runs(std::move(desktop), document.solution(), steps, expected, edited);
            cli = checked_workpiece_runs(std::move(cli), document.solution(), steps, expected, edited);
            require(desktop == cli, "Cross-workflow replay mismatch: " + json::diff(desktop, cli).dump().substr(0, 1800));
            const auto shared = take(window.findChild<RunController*>()->shared_preview());
            require(shared.has_value(), "Could not inspect the delivery queue after replay.");
            const auto queue = take(shared->data->preview(document.solution().queues.front().id));
            require(queue.enqueued == 2 && queue.received == 2 && queue.messages.empty() && !queue.rejected && !queue.dropped_oldest,
                "Two physical enqueues and two FIFO reads are required with no drops or rejections.");
        };
        compare(sample, false);
        const auto& width = original.solution().workflows.front();
        QVERIFY(scene(window).select_workflow(width.id) && scene(window).reveal_node(width.entries.front()));
        drive_dialog(window, "editBinding", QStringLiteral("right"), {set_source, source_kind(QStringLiteral("Constant")),
            [&](QDialog& dialog) -> QPushButton* {
            auto* value = qobject_cast<QInputDialog*>(&dialog);
            if (!value) { return nullptr; }
            value->setTextValue(QString::number(expected.at("edited_width_offset").get<int>()));
            return ok_button(dialog);
        }, accept_binding});
        window.transfer_document(saved, SolutionTransfer::SaveAs);
        wait_for_file(window);
        compare(saved, true);
    } catch (const std::exception& failure) { QFAIL(failure.what()); }
}

void AcceptanceParityTests::reproduces_saved_reentrant_workflows()
{
    try { compare_saved_workflows(VISIONPRO_S65_DIRECTORY); }
    catch (const std::exception& failure) { QFAIL(failure.what()); }
}

namespace {
void check_camera_frame(const json& run, NodeId node_id, ResourceId source, std::int64_t frame)
{
    require(run.at("quiescent") == true && run.at("status") == "Succeeded", "Camera run failed: " + run.at("diagnostics").dump());
    const auto& node = named(run.at("nodes"), "id", node_id.to_string());
    const auto& image = output(node, "image");
    const int expected = frame % 2 == 1 ? 7 : 31;
    require(image.at("frame").at("source") == source.to_string() && image.at("frame").at("sequence") == std::to_string(frame)
        && image.at("width") == 2 && image.at("height") == 2 && image.at("current_frame") == "camera"
        && image.at("samples").at("minimum") == expected && image.at("samples").at("maximum") == expected
        && !image.at("captured_at_unix_ns").is_null() && output(node, "frame_number") == std::to_string(frame),
        "Frame identity, time or 2x2 pixel values did not match the simulated camera: " + image.dump());
}
}

void AcceptanceParityTests::configures_and_acquires_saved_camera()
{
    try {
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const auto sample = fs::path(VISIONPRO_S610_DIRECTORY);
        const auto saved = fs::path(scratch.path().toStdU16String()) / "camera acquisition";
        const auto registry = take(app::make_tool_registry());
        const auto document = take(load_solution(sample, registry));
        const auto& flow = document.solution().workflows.front();
        const auto node_id = flow.entries.front();
        EditorWindow window(document.solution(), registry);
        window.show();
        window.open_document(sample);
        wait_for_file(window);
        QVERIFY(scene(window).select_workflow(flow.id));
        QVERIFY(scene(window).reveal_node(node_id));
        drive_dialog(window, "newCameraDevice", {}, {[](QDialog& dialog) -> QPushButton* {
            auto* name = dialog.findChild<QLineEdit*>(QStringLiteral("cameraDeviceName"));
            auto* adapter = dialog.findChild<QComboBox*>(QStringLiteral("cameraAdapter"));
            auto* serial = dialog.findChild<QLineEdit*>(QStringLiteral("cameraSerial"));
            if (!name || !adapter || !serial || adapter->findData(QStringLiteral("visionpro.camera.mvs")) < 0) { return nullptr; }
            name->setText(QStringLiteral("桌面采集相机"));
            serial->setText(QStringLiteral("SIM-001"));
            return ok_button(dialog);
        }});
        const auto resource = find_node(*find_workflow(window.draft(), flow.id), node_id)->resources.front().resource.id;
        QVERIFY(resource != document.solution().resources.front().id);
        drive_dialog(window, "editBinding", QStringLiteral("timeout_ms"), {set_source, [](QDialog& dialog) -> QPushButton* {
            auto* value = qobject_cast<QInputDialog*>(&dialog);
            if (!value) { return nullptr; }
            value->setTextValue(QStringLiteral("1200"));
            return ok_button(dialog);
        }, accept_binding});
        drive_dialog(window, "editBinding", QStringLiteral("mode"), {set_source, [](QDialog& dialog) -> QPushButton* {
            auto* choice = dialog.findChild<QComboBox*>(QStringLiteral("choice"));
            if (!choice) { return nullptr; }
            const auto index = choice->findText(QStringLiteral("Software"), Qt::MatchContains);
            if (index < 0) { return nullptr; }
            choice->setCurrentIndex(index);
            return ok_button(dialog);
        }, accept_binding});
        window.transfer_document(saved, SolutionTransfer::SaveAs);
        wait_for_file(window);
        window.open_document(saved);
        wait_for_file(window);
        const auto& resources = window.draft().resources;
        const auto selected = std::ranges::find(resources, resource, &ResourceDefinition::id);
        QVERIFY(selected != resources.end());
        QCOMPARE(QString::fromStdString(selected->name), QStringLiteral("桌面采集相机"));
        QCOMPARE(take(camera_serial(*selected)), "SIM-001");
        const auto* saved_node = find_node(*find_workflow(window.draft(), flow.id), node_id);
        QCOMPARE(saved_node->resources.front().resource.id, resource);
        for (const auto& [parameter, expected] : {std::pair{"mode", "Software"}, {"timeout_ms", "1200"}}) {
            const auto target = BindingTarget{take(SchemaIdFactory::parse<ParameterId>(parameter))};
            const auto binding = std::ranges::find(saved_node->bindings, target, &Binding::target);
            QVERIFY(binding != saved_node->bindings.end());
            const auto& value = std::get<ConfigValue>(binding->sources.front());
            QCOMPARE(value.as_enum() ? std::string(value.as_enum()->text()) : std::to_string(*value.as_int64()), expected);
        }
        QVERIFY(scene(window).select_workflow(flow.id));
        const auto desktop = complete_editor_run(window, "runWorkflow");
        const auto cli = run_cli(saved, flow.id);
        for (const auto* report : {&desktop, &cli}) {
            check_camera_frame(*report, node_id, resource, 1);
            QVERIFY(output(named(report->at("nodes"), "id", node_id.to_string()), "serial") == "SIM-001");
        }
    } catch (const std::exception& failure) { QFAIL(failure.what()); }
}

void AcceptanceParityTests::configures_and_arms_saved_camera()
{
    try {
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const auto sample = fs::path(VISIONPRO_S610_DIRECTORY);
        const auto saved = fs::path(scratch.path().toStdU16String()) / "trigger camera";
        const auto registry = take(app::make_tool_registry());
        const auto document = take(load_solution(sample, registry));
        const auto& flow = document.solution().workflows.at(1);
        EditorWindow window(document.solution(), registry);
        window.show();
        window.open_document(sample);
        wait_for_file(window);
        QVERIFY(scene(window).select_workflow(flow.id));
        QTRY_VERIFY(inspector_button(window, "editWorkflowTrigger", {})
            && inspector_button(window, "editWorkflowTrigger", {})->text() == QString::fromUtf8(flow.trigger->source.text()));
        drive_dialog(window, "editWorkflowTrigger", {}, {[](QDialog& dialog) -> QPushButton* {
            auto* source = dialog.findChild<QLineEdit*>(QStringLiteral("triggerSource"));
            auto* input = dialog.findChild<QComboBox*>(QStringLiteral("triggerCameraImage"));
            auto* frame = dialog.findChild<QComboBox*>(QStringLiteral("triggerCameraCoordinate"));
            auto* mode = dialog.findChild<QComboBox*>(QStringLiteral("triggerCameraMode"));
            auto* timeout = dialog.findChild<QSpinBox*>(QStringLiteral("triggerCameraTimeout"));
            if (!source || !input || !frame || !mode || !timeout || frame->currentText() != QStringLiteral("camera")
                || mode->currentText() != QStringLiteral("Hardware") || !input->currentText().contains(QStringLiteral("image"))) { return nullptr; }
            source->setText(QStringLiteral("desktop.camera"));
            timeout->setValue(3000);
            return dialog.findChild<QPushButton*>(QStringLiteral("newTriggerCameraDevice"));
        }, [](QDialog& dialog) -> QPushButton* {
            auto* name = dialog.findChild<QLineEdit*>(QStringLiteral("cameraDeviceName"));
            if (!name) { return nullptr; }
            name->setText(QStringLiteral("桌面触发相机"));
            return ok_button(dialog);
        }, [](QDialog& dialog) -> QPushButton* {
            auto* camera = dialog.findChild<QComboBox*>(QStringLiteral("triggerCameraDevice"));
            if (!camera || !camera->currentText().contains(QStringLiteral("桌面触发相机"))) { return nullptr; }
            return ok_button(dialog);
        }});
        window.transfer_document(saved, SolutionTransfer::SaveAs);
        wait_for_file(window);
        window.open_document(saved);
        wait_for_file(window);
        QVERIFY(scene(window).select_workflow(flow.id));
        const auto* saved_flow = find_workflow(window.draft(), flow.id);
        QVERIFY(saved_flow && saved_flow->trigger && saved_flow->trigger->camera);
        const auto device = saved_flow->trigger->camera->device;
        QVERIFY(device != document.solution().resources.front().id);
        QCOMPARE(saved_flow->trigger->source.text(), "desktop.camera");
        QCOMPARE(saved_flow->trigger->camera->timeout_ms, 3000u);
        const auto camera = simulated_camera();
        camera->set_periodic_hardware_pulses(false);
        const auto restore = qScopeGuard([&] { camera->set_connected(true); camera->set_periodic_hardware_pulses(true); });
        auto& controller = *window.findChild<RunController*>();
        window.findChild<QDockWidget*>(QStringLiteral("runDock"))->show();
        auto* status = window.findChild<QLabel*>(QStringLiteral("cameraTriggerStatus"));
        QVERIFY(status);
        action(window, "armWorkflowTrigger").trigger();
        QTRY_VERIFY(status->isVisible() && status->text().startsWith(QStringLiteral("Camera armed")));
        QVERIFY(!window.findChild<QAction*>(QStringLiteral("sendExternalFrame"))->isEnabled());
        for (std::size_t i = 1; i <= 2; ++i) {
            QVERIFY(camera->wait_until_armed(std::chrono::steady_clock::now() + std::chrono::seconds(2)));
            camera->pulse();
            QTRY_VERIFY(controller.runs().size() == i && controller.can_start());
        }
        QTRY_VERIFY(status->text().contains(QStringLiteral("received 2 · accepted 2 · rejected 0 · missing 0 · duplicates 0")));
        camera->set_connected(false);
        QTRY_VERIFY(status->text().startsWith(QStringLiteral("Camera stopped · fault")));
        QCOMPARE(controller.runs().size(), 2u);
        camera->set_connected(true);
        action(window, "armWorkflowTrigger").trigger();
        QTRY_VERIFY(status->text().startsWith(QStringLiteral("Camera armed")));
        QVERIFY(camera->wait_until_armed(std::chrono::steady_clock::now() + std::chrono::seconds(2)));
        action(window, "cancelWorkflowTrigger").trigger();
        camera->pulse();
        QTRY_VERIFY(status->text().startsWith(QStringLiteral("Camera stopped · received 0 · accepted 0")));
        QCOMPARE(controller.runs().size(), 2u);
        const auto& first_input = controller.runs().front().request().inputs();
        const auto frame_input = std::ranges::find_if(first_input, [](const auto& value) { return value.id.text() == "frame"; });
        QVERIFY(frame_input != first_input.end() && frame_input->value.as_int64());
        const auto first = *frame_input->value.as_int64();
        // A suite may already have used SIM-001. Reproduce its physical sequence in the fresh CLI process.
        const auto cli = run_cli(saved, flow.id, {QStringLiteral("--camera-frames"), QString::number(first + 1)}, QStringLiteral("trigger"));
        QVERIFY(cli.at("camera").at("received") == first + 1 && cli.at("camera").at("accepted") == first + 1
            && cli.at("camera").at("rejected") == 0 && cli.at("camera").at("missing") == 0 && cli.at("camera").at("duplicates") == 0);
        for (std::size_t i = 0; i != 2; ++i) {
            const auto number = first + static_cast<std::int64_t>(i);
            const auto& run = controller.runs().at(i);
            QVERIFY(run.request().trigger().source.text() == "desktop.camera" && run.request().correlation()
                && run.request().correlation()->domain().text() == "desktop.camera"
                && run.request().correlation()->key().ends_with("/" + std::to_string(number)));
            const auto& event = cli.at("events").at(static_cast<std::size_t>(number - 1));
            QVERIFY(event.at("frame") == std::to_string(number) && event.at("source") == "desktop.camera");
            check_camera_frame(take(app::summarize_run(run, take(run.inspect()), {})), flow.entries.front(), device, number);
            check_camera_frame(event.at("result"), flow.entries.front(), device, number);
        }
    } catch (const std::exception& failure) { QFAIL(failure.what()); }
}

void AcceptanceParityTests::reproduces_saved_communication_data()
{
    QTest::addColumn<bool>("plc");
    QTest::newRow("tcp") << false;
    QTest::newRow("modbus") << true;
}

#ifdef VISIONPRO_TEST_PYTHON
namespace {
struct LoopbackPeer {
    QProcess process;
    int port;

    explicit LoopbackPeer(bool plc, bool record_tcp = false)
    {
        QStringList arguments{QStringLiteral("-u"), QStringLiteral("-B"), QStringLiteral(VISIONPRO_TEST_PEER),
            QStringLiteral("--protocol"), plc ? QStringLiteral("modbus") : QStringLiteral("tcp")};
        if (record_tcp) { arguments << QStringLiteral("--record-tcp"); }
        if (plc) {
            arguments << QStringLiteral("--fault") << QStringLiteral("unknown-exception")
                      << QStringLiteral("--fault") << QStringLiteral("lost-ack");
        }
        process.start(QStringLiteral(VISIONPRO_TEST_PYTHON), arguments);
        require(process.waitForStarted(5000), "Could not launch the loopback peer: " + process.errorString().toStdString());
        require(process.canReadLine() || process.waitForReadyRead(5000), "The loopback peer did not announce a port.");
        bool valid{};
        port = process.readLine().trimmed().toInt(&valid);
        require(valid && port > 0 && port <= 65535, "Invalid loopback peer port.");
    }
    ~LoopbackPeer() { process.kill(); process.waitForFinished(3000); }
};

void select_node_properties(EditorWindow& window, const Node& node)
{
    require(scene(window).reveal_node(node.id), "Could not select the communication node.");
    require(QTest::qWaitFor([&] {
        auto* name = inspector_button(window, "renameNode", {});
        return name && name->toolTip() == QString::fromStdString(node.name);
    }, 3000), "The selected node's inspector did not open.");
}

void select_result(EditorWindow& window, const Node& node)
{
    window.findChild<QDockWidget*>(QStringLiteral("runDock"))->show();
    auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("runNodes"));
    require(tree && QTest::qWaitFor([&] {
        for (int row = 0; row < tree->topLevelItemCount(); ++row) {
            auto* item = tree->topLevelItem(row);
            if (item->toolTip(0) == QString::fromStdString(node.id.to_string())) { tree->setCurrentItem(item); return true; }
        }
        return false;
    }, 3000), "Could not select the node in Run and Results.");
}
}
#endif

void AcceptanceParityTests::reproduces_saved_communication()
{
#ifndef VISIONPRO_TEST_PYTHON
    QSKIP("This loopback acceptance test needs Python 3.");
#else
    QFETCH(bool, plc);
    try {
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        LoopbackPeer peer(plc);
        const auto sample = fs::path(VISIONPRO_S611_DIRECTORY);
        const auto saved = fs::path(scratch.path().toStdU16String()) / "edited communication";
        const auto registry = take(app::make_tool_registry());
        const auto document = take(load_solution(sample, registry));
        const auto& flow = document.solution().workflows.at(plc ? 1 : 0);
        const auto& sender = flow.nodes.front();
        const auto title = plc ? QStringLiteral("桌面测试寄存器") : QStringLiteral("桌面二进制收发");
        EditorWindow window(document.solution(), registry);
        window.show();
        window.open_document(sample);
        wait_for_file(window);
        QVERIFY(scene(window).select_workflow(flow.id));
        select_node_properties(window, sender);
        // TCP creates a new resource and both receives select it. PLC edits its shared endpoint in place.
        drive_dialog(window, plc ? "editEndpoint" : "newEndpoint", {}, {[&](QDialog& dialog) -> QPushButton* {
            auto* name = dialog.findChild<QLineEdit*>(QStringLiteral("endpointName"));
            auto* host = dialog.findChild<QLineEdit*>(QStringLiteral("endpointHost"));
            auto* port = dialog.findChild<QSpinBox*>(QStringLiteral("endpointPort"));
            auto* unit = dialog.findChild<QSpinBox*>(QStringLiteral("endpointUnit"));
            if (!name || !host || !port || !unit || unit->isVisible() != plc) { return nullptr; }
            name->setText(title);
            host->setText(QStringLiteral("127.0.0.256"));
            port->setValue(peer.port);
            if (plc) { unit->setValue(7); }
            return ok_button(dialog);
        }, [](QDialog& dialog) -> QPushButton* {
            auto* warning = dialog.findChild<QLabel*>(QStringLiteral("endpointError"));
            if (!warning || !warning->isVisible()) { return nullptr; }
            dialog.findChild<QLineEdit*>(QStringLiteral("endpointHost"))->setText(QStringLiteral("127.0.0.1"));
            return ok_button(dialog);
        }});
        const auto bound = find_node(*find_workflow(window.draft(), flow.id), sender.id)->resources.front().resource.id;
        QVERIFY((bound == sender.resources.front().resource.id) == plc);
        if (!plc) {
            for (const auto& receiver : flow.nodes | std::views::drop(1)) {
                select_node_properties(window, receiver);
                drive_dialog(window, "selectNodeResource", {}, {[&](QDialog& dialog) -> QPushButton* {
                    auto* choices = dialog.findChild<QComboBox*>(QStringLiteral("choice"));
                    if (!choices || choices->findText(QStringLiteral("PLC"), Qt::MatchContains) >= 0) { return nullptr; }
                    const auto index = choices->findText(title, Qt::MatchContains);
                    if (index < 0) { return nullptr; }
                    choices->setCurrentIndex(index);
                    return ok_button(dialog);
                }});
            }
            // Exercise the public Binary run input independently of a persisted literal.
            QTimer::singleShot(0, [] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) { return; }
                auto* mode = dialog->findChild<QComboBox*>(QStringLiteral("runInputMode.packet"));
                auto* value = dialog->findChild<QPlainTextEdit*>(QStringLiteral("runInputValue.packet"));
                if (!mode || !value) { dialog->reject(); return; }
                mode->setCurrentIndex(1);
                value->setPlainText(QStringLiteral("43 00 ff"));
                ok_button(*dialog)->click();
            });
            const auto supplied = complete_editor_run(window, "runWorkflow");
            const auto& echoed = output(named(supplied.at("nodes"), "id", flow.nodes.at(1).id.to_string()), "payload");
            QVERIFY(echoed.at("hex") == "4300ff" && echoed.at("size") == 3);
            select_node_properties(window, sender);
            drive_dialog(window, "editBinding", QStringLiteral("payload"), {set_source, source_kind(QStringLiteral("Constant")),
                [](QDialog& dialog) -> QPushButton* {
                    auto* hex = dialog.findChild<QPlainTextEdit*>(QStringLiteral("binaryHexEditor"));
                    if (!hex) { return nullptr; }
                    hex->setPlainText(QStringLiteral("4g"));
                    return ok_button(dialog);
                }, [](QDialog& dialog) -> QPushButton* {
                    auto* error = dialog.findChild<QLabel*>(QStringLiteral("binaryHexError"));
                    if (!error || !error->isVisible()) { return nullptr; }
                    dialog.findChild<QPlainTextEdit*>(QStringLiteral("binaryHexEditor"))->setPlainText(QStringLiteral("41 00\n42"));
                    return ok_button(dialog);
                }, accept_binding});
        }
        window.transfer_document(saved, SolutionTransfer::SaveAs);
        wait_for_file(window);
        window.open_document(saved);
        wait_for_file(window);
        QVERIFY(scene(window).select_workflow(flow.id));
        const auto& catalog = window.draft().resources;
        const auto entry = std::ranges::find(catalog, bound, &ResourceDefinition::id);
        QVERIFY(entry != catalog.end());
        QCOMPARE(QString::fromStdString(entry->name), title);
        const auto config = take(communication_endpoint(*entry));
        QCOMPARE(config.host, "127.0.0.1");
        QCOMPARE(config.port, peer.port);
        if (plc) { QCOMPARE(config.unit, 7); }
        for (const auto& node : find_workflow(window.draft(), flow.id)->nodes) { QCOMPARE(node.resources.front().resource.id, bound); }
        auto* details = window.findChild<QPlainTextEdit*>(QStringLiteral("runDetails"));
        QVERIFY(details);
        if (plc) {
            const auto failed = complete_editor_run(window, "runWorkflow");
            QVERIFY(failed.at("status") == "Failed");
            select_result(window, sender);
            QTRY_VERIFY(details->toPlainText().contains(QStringLiteral("modbus.exception"))
                && details->toPlainText().contains(QStringLiteral("code 66 (unmapped)")));

            const auto lost = complete_editor_run(window, "runWorkflow");
            QVERIFY(lost.at("status") == "Failed" && lost.at("quiescent") == true);
            const auto& write = named(lost.at("nodes"), "id", sender.id.to_string());
            QVERIFY(write.at("attempts").size() == 1 && named(write.at("outputs"), "id", "confirmed").at("status") == "NotProduced");
            const auto& uncertain = named(write.at("diagnostics"), "code", "ExternalEffectUnknown");
            const auto& operation = uncertain.at("operation_key");
            QVERIFY(operation.at("session") == lost.at("session") && operation.at("run") == lost.at("run")
                && operation.at("node") == write.at("id") && operation.at("scope") == write.at("scope")
                && operation.at("invocation") == write.at("invocation"));
            QVERIFY(uncertain.at("external").at("provider_domain") == "winsock.status" && uncertain.at("external").at("raw_code") == "0");
            const auto part = [&](const char* key) { return QString::fromStdString(operation.at(key).get<std::string>()); };
            const auto displayed_key = QStringLiteral("operation · Session %1 · Run %2 · Node %3 · Scope %4 · Invocation %5")
                .arg(part("session"), part("run"), part("node"), part("scope"), part("invocation"));
            select_result(window, sender);
            QTRY_VERIFY(details->toPlainText().contains(QStringLiteral("ExternalEffectUnknown"))
                && details->toPlainText().contains(QStringLiteral("winsock.status · code 0 (mapped)"))
                && details->toPlainText().contains(QStringLiteral("confirmed = Not produced"))
                && details->toPlainText().contains(displayed_key));

            const auto verification = uuid<WorkflowId>("5ca4f87a-ea1e-4d62-89b0-000000061140");
            const auto* read_only = find_workflow(window.draft(), verification);
            QVERIFY(read_only && read_only->nodes.size() == 1 && scene(window).select_workflow(verification));
            const auto& reader = read_only->nodes.front();
            QVERIFY(std::get<ToolConfiguration>(reader.configuration).type.text() == "visionpro.plc.read");
            const auto readback = complete_editor_run(window, "runWorkflow");
            QVERIFY(readback.at("status") == "Succeeded"
                && output(named(readback.at("nodes"), "id", reader.id.to_string()), "value") == "4660");
            select_result(window, reader);
            QTRY_VERIFY(details->toPlainText().contains(QStringLiteral("value = 4660")));
            QVERIFY(scene(window).select_workflow(flow.id));
        }
        const auto desktop = complete_editor_run(window, "runWorkflow", {}, !plc);
        const auto cli = run_cli(saved, flow.id);
        for (const auto* report : {&desktop, &cli}) {
            QVERIFY(report->at("quiescent") == true && report->at("status") == "Succeeded");
            const auto& first = named(report->at("nodes"), "id", sender.id.to_string());
            const auto& second = named(report->at("nodes"), "id", flow.nodes.at(1).id.to_string());
            if (plc) {
                QVERIFY(output(first, "confirmed") == true && output(second, "value") == "4660");
            } else {
                const auto& last = named(report->at("nodes"), "id", flow.nodes.at(2).id.to_string());
                QVERIFY(output(first, "bytes_sent") == "3" && output(second, "payload") == json({{"hex", "410042"}, {"size", 3}})
                    && output(last, "payload") == json({{"hex", "4e455854"}, {"size", 4}}));
            }
            QVERIFY(!first.at("host_elapsed_ns").is_null() && first.at("adapter_duration") == json({{"unit", "unknown"}, {"value", nullptr}}));
        }
        select_result(window, flow.nodes.at(1));
        QTRY_VERIFY(details->toPlainText().contains(QStringLiteral("Adapter duration: unknown · unit unknown")));
        if (!plc) {
            QTRY_VERIFY(details->toPlainText().contains(QStringLiteral("41 00 42")));
            auto* bytes = window.findChild<QPushButton*>(QStringLiteral("showRunBytes"));
            QVERIFY(bytes && bytes->isVisible());
            QString displayed;
            QTimer::singleShot(0, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) { return; }
                if (auto* view = dialog->findChild<QPlainTextEdit*>(QStringLiteral("binaryHexOutput"))) { displayed = view->toPlainText(); }
                dialog->accept();
            });
            bytes->click();
            QCOMPARE(displayed, QStringLiteral("41 00 42"));
        }
    } catch (const std::exception& failure) { QFAIL(failure.what()); }
#endif
}

void AcceptanceParityTests::reproduces_saved_calibrated_measurement()
{
    try {
        const auto verify = [](const json& run, double ref_x, double ref_y, double distance) {
            require(run.at("status") == "Succeeded", "Measurement failed: " + run.dump());
            const auto& calls = run.at("nodes");
            const auto& threshold = named(calls, "id", "5ca4f87a-ea1e-4d62-89b0-000000006812");
            const auto& blob = named(calls, "id", "5ca4f87a-ea1e-4d62-89b0-000000006813");
            const auto& fit = named(calls, "id", "5ca4f87a-ea1e-4d62-89b0-000000006814");
            const auto& first = named(calls, "id", "5ca4f87a-ea1e-4d62-89b0-000000006815");
            const auto& second = named(calls, "id", "5ca4f87a-ea1e-4d62-89b0-000000006816");
            const auto& measure = named(calls, "id", "5ca4f87a-ea1e-4d62-89b0-000000006817");
            require(output(threshold, "binary").at("samples").at("nonzero_count") == 5
                && output(blob, "total_area_pixels") == "5", "The circle must select the five cross-shaped pixels.");
            const auto& source = output(fit, "source");
            const auto& acquired = named(fit.at("resources"), "resource", "5ca4f87a-ea1e-4d62-89b0-000000006821");
            require(source.at("resource") == acquired.at("resource") && source.at("revision") == "1"
                && acquired.at("revision") == "1" && source.at("kind") == "Calibration",
                "The fitted transform must come from the pinned calibration file.");
            const auto near = [](const json& actual, double expected) { return std::abs(actual.get<double>() - expected) <= 1e-6; };
            const auto& transform = output(fit, "transform");
            require(transform.at("model") == "Affine" && transform.at("from") == "camera" && transform.at("to") == "world"
                && near(output(fit, "rms_error_mm"), 0), "Unexpected affine fit or residual.");
            const auto& centroid = output(first, "point");
            const auto& reference = output(second, "point");
            require(centroid.at("coordinate_frame") == "world" && centroid.at("unit") == "Millimetre"
                && near(centroid.at("x"), 16) && near(centroid.at("y"), 8)
                && near(reference.at("x"), ref_x) && near(reference.at("y"), ref_y)
                && near(output(measure, "distance_mm"), distance), "Calibrated points or distance exceed the 1e-6 mm tolerance.");
        };
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const auto sample = fs::path(QString::fromUtf8(VISIONPRO_S68_DIRECTORY).toStdU16String());
        const auto saved = fs::path(scratch.path().toStdU16String()) / "measurement";
        const auto registry = take(app::make_tool_registry());
        const auto document = take(load_solution(sample, registry));
        const auto flow = document.solution().workflows.front().id;
        EditorWindow window(document.solution(), registry);
        window.show();
        window.open_document(sample);
        wait_for_file(window);
        QVERIFY(scene(window).select_workflow(flow));
        verify(complete_editor_run(window, "runWorkflow"), 20, 11, 5);
        const auto point_node = uuid<NodeId>("5ca4f87a-ea1e-4d62-89b0-000000006816");
        QVERIFY(scene(window).reveal_node(point_node));
        drive_dialog(window, "editBinding", QStringLiteral("point"), {set_source, source_kind(QStringLiteral("Constant")),
            [](QDialog& dialog) -> QPushButton* {
                auto* x = dialog.findChild<QLineEdit*>(QStringLiteral("pointX"));
                auto* y = dialog.findChild<QLineEdit*>(QStringLiteral("pointY"));
                if (!x || !y) { return nullptr; }
                x->setText(QStringLiteral("3"));
                y->setText(QStringLiteral("6"));
                return ok_button(dialog);
            }, accept_binding});
        window.transfer_document(saved, SolutionTransfer::SaveAs);
        wait_for_file(window);
        window.open_document(saved);
        wait_for_file(window);
        QVERIFY(scene(window).select_workflow(flow));
        const auto desktop = complete_editor_run(window, "runWorkflow");
        const auto cli = run_cli(saved, flow);
        verify(desktop, 16, 14, 6);
        verify(cli, 16, 14, 6);
        QVERIFY(desktop.at("revision") == cli.at("revision") && desktop.at("session") != cli.at("session"));
    } catch (const std::exception& failure) { QFAIL(failure.what()); }
}

namespace {
std::string inspection_id(int number)
{
    return "5ca4f87a-ea1e-4d62-89b0-00000000" + std::to_string(number);
}

void verify_inspection(const json& run, double last_blue, bool all_passed, const std::string& expected_message)
{
    require(run.at("status") == "Succeeded", "Inspection failed: " + run.at("diagnostics").dump());
    const auto call = [&](int n) -> const json& { return named(run.at("nodes"), "id", inspection_id(n)); };
    const auto& locate = call(6912);
    const auto& transform = output(locate, "transform");
    require(output(locate, "matched") == true && transform.at("from") == "workpiece" && transform.at("to") == "camera"
        && transform.at("matrix") == json::array({1,0,3,0,1,2,0,0,1})
        && named(locate.at("resources"), "slot", "template").at("revision") == "1", "Incorrect template placement or revision.");
    for (int lane = 0; lane < 5; ++lane) {
        const auto& mean = call(6920 + lane);
        const auto& trace = named(mean.at("bindings"), "target", "placement").at("sources").at(0);
        require(check_producer(trace, run, mean).at("id") == locate.at("id") && trace.at("output") == "transform",
            "All five ROIs must use the same localization invocation.");
        const auto blue = lane == 4 ? last_blue : 10.0 * (lane + 1);
        require(output(mean, "blue") == blue && output(mean, "green") == 73 && output(mean, "red") == 151
            && output(mean, "roi_mask").at("member_count") == 4, "Wrong translated ROI, channel order or precision.");
        const auto passed = lane < 4 || all_passed;
        require(output(call(6930 + lane), "passed") == passed && call(6930 + lane).at("judgement") == (passed ? "OK" : "NG"),
            "Incorrect closed-interval judgement.");
    }
    const int format = all_passed ? 6942 : 6944;
    const auto& send = call(format + 1);
    const auto& order = run.at("call_order");
    require(output(call(6940), "passed") == all_passed && call(6941).at("selected_outputs") == json::array({all_passed ? "true" : "false"})
        && std::ranges::count(order, json(inspection_id(6940))) == 1 && std::ranges::count(order, json(inspection_id(format + 1))) == 1
        && std::ranges::count(order, json(inspection_id(all_passed ? 6945 : 6943))) == 0, "Expected one join and one delivery.");
    const auto& payload = named(send.at("bindings"), "target", "payload").at("sources").at(0);
    require(check_producer(payload, run, send).at("id") == inspection_id(format) && output(call(format), "text") == expected_message
        && !output(send, "message_id").get<std::string>().empty(), "Queued message did not use the five actual measurements.");
}
} // namespace

void AcceptanceParityTests::reproduces_saved_workpiece_inspection()
{
    try {
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const auto sample = fs::path(QString::fromUtf8(VISIONPRO_S69_DIRECTORY).toStdU16String());
        const auto saved = fs::path(scratch.path().toStdU16String()) / "inspection";
        const auto registry = take(app::make_tool_registry());
        const auto document = take(load_solution(sample, registry));
        const auto inspection = uuid<WorkflowId>(inspection_id(6910));
        EditorWindow window(document.solution(), registry);
        window.show();
        window.open_document(sample);
        wait_for_file(window);
        window.transfer_document(saved, SolutionTransfer::SaveAs);
        wait_for_file(window);
        const auto compare = [&](double blue, bool passed, const std::string& message) {
            window.open_document(saved);
            wait_for_file(window);
            require(scene(window).select_workflow(inspection), "Missing saved inspection workflow.");
            const auto desktop = complete_editor_run(window, "runWorkflow");
            const auto cli = run_cli(saved, inspection);
            require(desktop.at("revision") == cli.at("revision") && desktop.at("session") != cli.at("session"),
                "Expected separate sessions running the same saved revision.");
            verify_inspection(desktop, blue, passed, message);
            verify_inspection(cli, blue, passed, message);
            const auto shared = take(window.findChild<RunController*>()->shared_preview());
            require(shared.has_value(), "No shared-data session for the inspection.");
            const auto queue = take(shared->data->preview(uuid<QueueId>(inspection_id(6904))));
            const auto text_id = take(SchemaIdFactory::parse<FieldId>("text"));
            require(queue.enqueued == 1 && queue.messages.size() == 1 && queue.messages.front().payload.field(text_id)
                && *queue.messages.front().payload.field(text_id)->as_string() == message
                && queue.messages.front().correlation.key() == "part-001", "Expected exactly one message for this workpiece.");
        };
        compare(50, false, "NG B=10,20,30,40,50");

        QVERIFY(scene(window).reveal_node(uuid<NodeId>(inspection_id(6924))));
        drive_dialog(window, "editRegion", QStringLiteral("roi"), {[](QDialog& dialog) -> QPushButton* {
            auto* y = dialog.findChild<QLineEdit*>(QStringLiteral("roiY"));
            if (!y || dialog.objectName() != QStringLiteral("roiEditor")) { return nullptr; }
            y->setText(QStringLiteral("10"));
            return ok_button(dialog);
        }});
        window.save_document();
        wait_for_file(window);
        compare(40.5, false, "NG B=10,20,30,40,40.5");

        QVERIFY(scene(window).reveal_node(uuid<NodeId>(inspection_id(6934))));
        drive_dialog(window, "editBinding", QStringLiteral("maximum"), {set_source,
            [](QDialog& dialog) -> QPushButton* {
                auto* constant = qobject_cast<QInputDialog*>(&dialog);
                if (!constant) { return nullptr; }
                constant->setTextValue(QStringLiteral("40.5"));
                return ok_button(dialog);
            }, accept_binding});
        window.save_document();
        wait_for_file(window);
        compare(40.5, true, "OK B=10,20,30,40,40.5");

        const auto classify = uuid<WorkflowId>(inspection_id(6950));
        QVERIFY(scene(window).select_workflow(classify));
        const auto desktop = complete_editor_run(window, "runWorkflow");
        const auto cli = run_cli(saved, classify);
        const auto& map = output(named(cli.at("nodes"), "id", inspection_id(6952)), "class_map");
        QVERIFY(map == output(named(desktop.at("nodes"), "id", inspection_id(6952)), "class_map"));
        QVERIFY(map.at("pixels").at("labels") == json::array({json::array({0,1}),json::array({2,1})}));
        const auto& classes = map.at("classes");
        QVERIFY(classes.size() == 3 && named(classes, "id", "0").at("count") == "1"
            && named(classes, "id", "1").at("count") == "2" && named(classes, "id", "2").at("count") == "1");
        for (const auto* run : {&cli, &desktop}) {
            QVERIFY(output(named(run->at("nodes"), "id", inspection_id(6953)), "passed") == true);
        }
        QVERIFY(scene(window).reveal_node(uuid<NodeId>(inspection_id(6952))));
        drive_dialog(window, "editClassColor", {}, {[](QDialog& dialog) -> QPushButton* {
            auto* id = dialog.findChild<QSpinBox*>(QStringLiteral("classColorId"));
            auto* rgb = dialog.findChild<QLineEdit*>(QStringLiteral("classColorRgb"));
            if (!id || !rgb) { return nullptr; }
            id->setValue(1);
            rgb->setText(QStringLiteral("#dc247a"));
            return ok_button(dialog);
        }});
        window.save_document();
        wait_for_file(window);
        window.open_document(saved);
        wait_for_file(window);
        QVERIFY(scene(window).select_workflow(classify));
        for (const auto& rerun : {complete_editor_run(window, "runWorkflow"), run_cli(saved, classify)}) {
            QVERIFY(map == output(named(rerun.at("nodes"), "id", inspection_id(6952)), "class_map"));
            QVERIFY(output(named(rerun.at("nodes"), "id", inspection_id(6953)), "passed") == true);
        }
    } catch (const std::exception& failure) { QFAIL(failure.what()); }
}

void AcceptanceParityTests::sends_saved_five_lane_inspection_over_tcp()
{
#if !defined(VISIONPRO_TEST_PYTHON) || !defined(VISIONPRO_S612_INSPECTION_FIXTURE)
    QSKIP("This inspection acceptance requires Python and OpenCV.");
#else
    try {
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        LoopbackPeer peer(false, true);
        const fs::path root(scratch.path().toStdU16String());
        const auto sample = root / "inspection";
        const auto saved = root / "edited inspection";
        QProcess prepare;
        prepare.start(QStringLiteral(VISIONPRO_TEST_PYTHON), {QStringLiteral("-B"), QStringLiteral(VISIONPRO_S612_INSPECTION_FIXTURE),
            path_text(sample), QStringLiteral("--port"), QString::number(peer.port)});
        const bool prepared = prepare.waitForFinished(10000) && prepare.exitStatus() == QProcess::NormalExit && prepare.exitCode() == 0;
        require(prepared, "Could not prepare inspection: " + prepare.readAllStandardError().toStdString());
        const auto registry = take(app::make_tool_registry());
        const auto document = take(load_solution(sample, registry));
        const auto flow = uuid<WorkflowId>(inspection_id(6910));

        for (const int part : {101, 102}) {
            const bool passed = part == 102;
            const std::string verdict = passed ? "OK" : "NG";
            const auto measurements = verdict + " B=10,20,30,40,50";
            const auto wire = std::string("工件=") + std::to_string(part) + " " + verdict;
            const auto hex = QByteArray::fromStdString(wire).toHex().toStdString();
            json desktop;
            {
                EditorWindow window(document.solution(), registry);
                window.show();
                window.open_document(passed ? saved : sample);
                wait_for_file(window);
                QVERIFY(scene(window).select_workflow(flow));
                if (passed) {
                    // Include lane 5's measured value in the closed acceptance interval.
                    const auto* judge = find_node(*find_workflow(window.draft(), flow), uuid<NodeId>(inspection_id(6934)));
                    QVERIFY(judge);
                    select_node_properties(window, *judge);
                    drive_dialog(window, "editBinding", QStringLiteral("maximum"), {set_source,
                        [](QDialog& dialog) -> QPushButton* {
                            auto* value = qobject_cast<QInputDialog*>(&dialog);
                            if (!value) { return nullptr; }
                            value->setTextValue(QStringLiteral("50"));
                            return ok_button(dialog);
                        }, accept_binding});
                    window.save_document();
                } else {
                    window.transfer_document(saved, SolutionTransfer::SaveAs);
                }
                wait_for_file(window);
                window.open_document(saved);
                wait_for_file(window);
                QVERIFY(scene(window).select_workflow(flow));
                QTimer input;
                connect(&input, &QTimer::timeout, [&] {
                    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    if (!dialog) { return; }
                    input.stop();
                    auto* field = dialog->findChild<QLineEdit*>(QStringLiteral("runInputValue.part"));
                    if (!field || !ok_button(*dialog)) { dialog->reject(); return; }
                    field->setText(QString::number(part));
                    ok_button(*dialog)->click();
                    if (dialog->result() != QDialog::Accepted) { dialog->reject(); }
                });
                input.start(10);
                desktop = complete_editor_run(window, "runWorkflow");
                const auto shared = take(window.findChild<RunController*>()->shared_preview());
                QVERIFY(shared.has_value());
                const auto queue = take(shared->data->preview(uuid<QueueId>(inspection_id(6904))));
                const auto text_id = take(SchemaIdFactory::parse<FieldId>("text"));
                QVERIFY(queue.enqueued == 1 && queue.messages.size() == 1);
                const auto& message = queue.messages.front();
                QCOMPARE(message.correlation.key(), "part-" + std::to_string(part));
                const auto& queued = named(desktop.at("nodes"), "id", inspection_id(passed ? 6943 : 6945));
                QCOMPARE(message.id.to_string(), output(queued, "message_id").get<std::string>());
                const auto* text = message.payload.field(text_id);
                QVERIFY(text && text->as_string() && *text->as_string() == measurements);
            }
            const auto check_peer = [&] {
                require(peer.process.canReadLine() || peer.process.waitForReadyRead(5000), "No complete TCP connection transcript.");
                require(json::parse(peer.process.readLine().toStdString()) == json::array({hex}),
                    "The TCP peer did not receive exactly the expected UTF-8 workpiece verdict.");
            };
            check_peer();
            auto cli = run_cli(saved, flow, {QStringLiteral("--input"), QStringLiteral("part=") + QString::number(part)});
            check_peer();
            QVERIFY(desktop.at("revision") == cli.at("revision") && desktop.at("session") != cli.at("session"));
            std::array<json, 2> reports{std::move(desktop), std::move(cli)};
            for (auto& run : reports) {
                verify_inspection(run, 50, passed, measurements);
                QVERIFY(run.at("quiescent") == true && run.at("diagnostics").empty() && run.at("load_diagnostics").empty());
                const auto& order = run.at("call_order");
                QVERIFY(order.size() == 20);
                for (const auto& id : order) {
                    const auto& node = named(run.at("nodes"), "id", id);
                    QVERIFY(std::ranges::count(order, id) == 1 && node.at("status") == "Succeeded"
                        && node.at("attempts").size() == 1 && node.at("diagnostics").empty());
                    for (const auto& binding : node.at("bindings")) {
                        for (const auto& trace : binding.at("sources")) {
                            if (trace.at("kind") == "result") { (void)check_producer(trace, run, node); }
                            else if (trace.at("kind") == "public_input") {
                                QVERIFY(has_run_identity(trace, run) && trace.at("input") == "part" && trace.at("origin") == "Provided");
                            }
                        }
                    }
                }
                const int label = passed ? 6971 : 6974;
                const auto call = [&](int id) -> const json& { return named(run.at("nodes"), "id", inspection_id(id)); };
                QVERIFY(output(call(6970), "text") == "part-" + std::to_string(part));
                QVERIFY(output(call(label), "text") == wire && output(call(label + 1), "payload").at("hex") == hex);
                QVERIFY(output(call(label + 2), "bytes_sent") == std::to_string(wire.size()));
                QVERIFY(std::ranges::find(order, json(inspection_id(passed ? 6976 : 6973))) == order.end());
                const auto id = output(call(passed ? 6943 : 6945), "message_id").get<std::string>();
                (void)uuid<MessageId>(id);
                match_message_ids(run, {{id, "delivery-for-part-" + std::to_string(part)}});
                for (auto& node : run.at("nodes")) {
                    if (node.contains("host_elapsed_ns")) {
                        QVERIFY(node.at("id") == inspection_id(label + 2)
                            && std::stoll(node.at("host_elapsed_ns").get<std::string>()) >= 0);
                        node.erase("host_elapsed_ns");
                    }
                    for (auto& binding : node.at("bindings")) {
                        for (auto& source : binding.at("sources")) {
                            if (source.at("kind") == "public_input") { omit_run_ids(source); }
                        }
                    }
                }
                run = normalize(std::move(run));
            }
            require(reports[0] == reports[1], "Five-lane GUI/CLI reports differ: " + json::diff(reports[0], reports[1]).dump().substr(0, 1800));
            qInfo().noquote() << QString::fromStdString(wire) << QString::fromStdString(measurements)
                << "20 calls per run, one frame per GUI/CLI connection";
        }
    } catch (const std::exception& failure) { QFAIL(failure.what()); }
#endif
}

void AcceptanceParityTests::compares_saved_blob_loop()
{
    try {
        QTemporaryDir scratch;
        require(scratch.isValid(), "Temporary directory unavailable.");
        const fs::path sample(QString::fromUtf8(VISIONPRO_S61_DIRECTORY).toStdU16String());
        const auto package = fs::path(scratch.path().toStdU16String()) / "loop copy";
        const auto registry = take(app::make_tool_registry());
        const auto original = take(load_solution(sample, registry));
        const auto& flow = original.solution().workflows.at(0);
        const auto group_node = std::ranges::find_if(flow.nodes, [](const Node& node) {
            const auto* group = std::get_if<GroupConfiguration>(&node.configuration);
            return group && group->loop.has_value();
        });
        require(group_node != flow.nodes.end(), "The sample has no loop.");
        const auto& group = std::get<GroupConfiguration>(group_node->configuration);
        const auto child = group.entries.at(0);
        EditorWindow window(original.solution(), registry);
        window.show();
        window.open_document(sample);
        wait_for_file(window);
        window.transfer_document(package, SolutionTransfer::SaveAs);
        wait_for_file(window);

        const auto compare = [&](int count) {
            window.open_document(package);
            wait_for_file(window);
            require(scene(window).select_workflow(flow.id), "Could not select the reloaded workflow.");
            auto* controller = window.findChild<RunController*>();
            require(controller && QTest::qWaitFor([&] { return controller->can_start(); }, 5000), "Editor still busy.");
            const auto previous = controller->runs().size();
            action(window, "runWorkflow").trigger();
            require(QTest::qWaitFor([&] { return controller->can_start() && controller->runs().size() == previous + 1; }, 8000),
                "Desktop could not complete the loop: " + window.statusBar()->currentMessage().toStdString());
            const auto& run = controller->runs().back();
            const auto gui = take(app::summarize_run(run, take(run.inspect()), {}));
            const auto cli = run_cli(package, flow.id);
            require(gui.at("revision") == cli.at("revision") && gui.at("session") != cli.at("session"),
                "The separate processes must execute the same saved revision.");
            for (const auto* report : {&gui, &cli}) {
                const auto& nodes = report->at("nodes");
                const auto& owner = named(nodes, "id", group_node->id.to_string());
                require(report->at("status") == "Succeeded" && report->at("quiescent") == true
                    && report->at("iterations").size() == static_cast<std::size_t>(count), report->dump());
                const auto& points = output(owner, "points");
                require(points.size() == static_cast<std::size_t>(count), "Wrong number of exported points.");
                std::set<json> scopes, calls;
                for (int i = 0; i < count; ++i) {
                    const auto& scope = report->at("iterations").at(i);
                    require(scope.at("index") == std::to_string(i) && scope.at("loop_invocation") == owner.at("invocation")
                        && scope.at("loop_node") == owner.at("id") && scope.at("parent_scope") == owner.at("scope")
                        && scopes.insert(scope.at("scope")).second, "Wrong iteration identity.");
                    const auto call = std::ranges::find_if(nodes, [&](const json& n) {
                        return n.at("scope") == scope.at("scope") && n.at("id") == child.to_string();
                    });
                    require(call != nodes.end() && calls.insert(call->at("invocation")).second
                        && output(*call, "item").at("centroid") == points.at(i), "Point came from the wrong call.");
                    const auto& trace = named(scope.at("exports"), "output", "points").at("producer");
                    require(has_run_identity(trace, *report) && trace.at("scope") == scope.at("scope")
                        && trace.at("invocation") == call->at("invocation") && trace.at("node") == call->at("id")
                        && trace.at("output") == "item" && trace.at("selector").at(0).at("field") == "centroid",
                        "The point export did not retain its exact source.");
                    require(points.at(i).at("x") == (i == 0 ? 2 : 8) && points.at(i).at("y") == (i == 0 ? 1.5 : 6.0)
                        && points.at(i).at("unit") == "Pixel", "Wrong centroid or unit.");
                }
                require(std::ranges::count(report->at("call_order"), json(child.to_string())) == count,
                    "Body dispatch count differs from the number of iterations.");
                require(output(owner, "areas") == (count ? json::array({"6","9"}) : json::array())
                    && output(owner, "first_area") == (count ? json("6") : json(nullptr))
                    && output(owner, "last_area") == (count ? json("9") : json(nullptr))
                    && output(owner, "total_area") == (count ? "15" : "0"), "Incorrect loop reductions.");
            }
            require(gui.at("call_order") == cli.at("call_order")
                && named(gui.at("nodes"), "id", group_node->id.to_string()).at("outputs")
                    == named(cli.at("nodes"), "id", group_node->id.to_string()).at("outputs"), "Desktop/CLI output mismatch.");
        };
        compare(2);
        // Observe a completed iteration before cancelling in the gap. An early cancel could pass without running the body.
        require(scene(window).edit_draft(QStringLiteral("Set loop interval"), [&](Solution& draft) -> Result<std::monostate> {
            auto& nodes = draft.workflows.front().nodes;
            auto& configured = *std::ranges::find(nodes, group_node->id, &Node::id);
            std::get<GroupConfiguration>(configured.configuration).loop->interval_ms = 30000;
            return std::monostate{};
        }), "Could not configure the cancellation interval.");
        auto* controller = window.findChild<RunController*>();
        const auto previous = controller->runs().size();
        action(window, "runWorkflow").trigger();
        require(QTest::qWaitFor([&] {
            if (controller->runs().size() != previous + 1) { return false; }
            const auto progress = take(controller->runs().back().inspect());
            return progress.iterations.size() == 1 && !progress.iterations.front().exports.empty();
        }, 5000), "The first iteration did not finish before the gap.");
        action(window, "cancelRun").trigger();
        require(QTest::qWaitFor([&] { return controller->can_start(); }, 3000), "Cancel waited for the 30s interval.");
        const auto& interrupted = controller->runs().back();
        const auto stopped = take(interrupted.inspect());
        const auto stopped_group = std::ranges::find(stopped.nodes, group_node->id, &RunNodeProgress::node);
        require(stopped.quiescent && stopped.status == ExecutionStatus::Cancelled && stopped.iterations.size() == 1
            && std::ranges::count(stopped.call_order, child) == 1 && stopped_group != stopped.nodes.end() && stopped_group->invocation,
            "Cancellation started another iteration or left the loop active.");
        const auto terminal = take(interrupted.lookup(*stopped_group->invocation));
        require(terminal && terminal->data().status == ExecutionStatus::Cancelled && terminal->data().outputs.empty(),
            "A cancelled loop published a partial point set.");
        action(window, "undoDraft").trigger();
        const auto threshold = std::ranges::find_if(flow.nodes, [](const Node& node) {
            const auto* tool = std::get_if<ToolConfiguration>(&node.configuration);
            return tool && tool->type.text() == "visionpro.image.threshold";
        });
        require(threshold != flow.nodes.end() && scene(window).reveal_node(threshold->id), "Threshold missing.");
        drive_dialog(window, "editBinding", QStringLiteral("threshold"), {set_source, source_kind(QStringLiteral("Constant")),
            [](QDialog& dialog) -> QPushButton* {
                auto* field = qobject_cast<QInputDialog*>(&dialog);
                if (!field) { return nullptr; }
                field->setTextValue(QStringLiteral("255"));
                return ok_button(dialog);
            }, accept_binding});
        action(window, "saveSolution").trigger();
        wait_for_file(window);
        compare(0);
    } catch (const std::exception& failure) { QFAIL(failure.what()); }
}

void AcceptanceParityTests::compares_saved_loop_in_group()
{
    try {
        QTemporaryDir scratch;
        require(scratch.isValid(), "Temporary directory unavailable.");
        const fs::path sample(QString::fromUtf8(VISIONPRO_S62_DIRECTORY).toStdU16String());
        const auto saved = fs::path(scratch.path().toStdU16String()) / "group-with-loop";
        const auto registry = take(app::make_tool_registry());
        const auto original = take(load_solution(sample, registry));
        const auto& flow = original.solution().workflows.at(0);
        const auto group = flow.entries.at(0);
        const auto threshold = uuid<NodeId>("5ca4f87a-ea1e-4d62-89b0-000000006202");
        EditorWindow window(original.solution(), registry);
        window.show();
        window.open_document(sample);
        wait_for_file(window);
        window.transfer_document(saved, SolutionTransfer::SaveAs);
        wait_for_file(window);

        const auto comparable = [](const json& run, bool empty) {
            require(run.at("call_order").size() == (empty ? 8u : 10u)
                && run.at("diagnostics").empty() && run.at("load_diagnostics").empty(),
                "Unexpected Group call count or diagnostics.");
            for (const auto& iteration : run.at("iterations")) {
                const auto& source = named(iteration.at("exports"), "output", "points").at("producer");
                const auto& body = named(run.at("nodes"), "invocation", source.at("invocation"));
                require(body.at("id") == source.at("node") && body.at("attempt") == source.at("attempt")
                    && body.at("scope") == iteration.at("scope") && source.at("scope") == body.at("scope")
                    && source.at("output") == "item" && source.at("selector").at(0).at("field") == "centroid",
                    "Collected point did not originate in this iteration.");
            }
            auto copy = run;
            // Relativize the three fresh run addresses, including those inside Group and loop exports.
            // Keep the node, scope, invocation and attempt of every source in the full comparison.
            const auto localize = [&](auto&& self, json& value) -> void {
                if (value.is_object() && value.contains("session")) {
                    for (const auto* key : {"session", "snapshot", "run"}) {
                        require(value.at(key) == run.at(key), "A source points outside this run.");
                        value.erase(key);
                    }
                }
                if (value.is_structured()) {
                    for (auto& child : value) { self(self, child); }
                }
            };
            localize(localize, copy);
            return copy;
        };
        const auto compare = [&](bool empty) {
            window.open_document(saved);
            wait_for_file(window);
            require(scene(window).select_workflow(flow.id), "Could not open the saved workflow.");
            const auto gui = complete_editor_run(window, "runWorkflow");
            const auto cli = run_cli(saved, flow.id);
            require(gui.at("session") != cli.at("session"), "Expected independent GUI and CLI sessions.");
            check_blob_group(gui, group, 1, empty);
            check_blob_group(cli, group, 1, empty);
            const auto desktop_report = comparable(gui, empty);
            const auto cli_report = comparable(cli, empty);
            require(desktop_report == cli_report,
                "Group/loop report differs: " + json::diff(desktop_report, cli_report).dump());
        };
        compare(false);
        require(scene(window).reveal_node(threshold), "Could not open the threshold inside the Group.");
        drive_dialog(window, "editBinding", QStringLiteral("threshold"), {set_source, source_kind(QStringLiteral("Constant")),
            [](QDialog& dialog) -> QPushButton* {
                auto* value = qobject_cast<QInputDialog*>(&dialog);
                if (!value) { return nullptr; }
                value->setTextValue(QStringLiteral("255"));
                return ok_button(dialog);
            }, accept_binding});
        action(window, "saveSolution").trigger();
        wait_for_file(window);
        compare(true);
    } catch (const std::exception& failure) { QFAIL(failure.what()); }
}

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
