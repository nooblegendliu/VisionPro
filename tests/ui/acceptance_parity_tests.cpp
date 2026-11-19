#include "control_scene.hpp"
#include "editor_window.hpp"
#include "run_controller.hpp"
#include "run_summary.hpp"
#include "tool_catalog.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/subgraph_package.hpp>

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

json run_cli(const fs::path& package, const WorkflowId& workflow, const QStringList& extra = {})
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
    QStringList args{QStringLiteral("run"), QStringLiteral("--solution"), path_text(package / solution_manifest_name),
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
    void imports_and_runs_blob_groups();
    void reproduces_saved_calculations();
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
    require(QTest::qWaitFor([&] { return controller->can_start() && controller->runs().size() == previous + 1; }, 8000),
        "The editor did not finish a new run: " + window.statusBar()->currentMessage().toStdString());
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

void AcceptanceParityTests::reproduces_saved_calculations()
{
    try {
        QTemporaryDir scratch;
        require(scratch.isValid(), "Temporary directory unavailable.");
        const fs::path sample(QString::fromUtf8(VISIONPRO_S63_DIRECTORY).toStdU16String());
        const auto package = fs::path(scratch.path().toStdU16String()) / "saved calculations";
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
            const auto desktop = complete_editor_run(window, "runWorkflow", {}, true);
            const auto cli = run_cli(package, flow.id);
            require(cli.at("status") == "Succeeded" && desktop.at("status") == cli.at("status")
                && desktop.at("revision") == cli.at("revision") && desktop.at("session") != cli.at("session"),
                "Both processes must complete the same saved workflow revision.");
            for (const auto& node : flow.nodes) {
                const auto& a = named(desktop.at("nodes"), "id", node.id.to_string());
                const auto& b = named(cli.at("nodes"), "id", node.id.to_string());
                require(a.at("status") == b.at("status") && a.at("judgement") == b.at("judgement")
                    && a.at("outputs") == b.at("outputs") && a.at("selected_outputs") == b.at("selected_outputs"),
                    "Desktop and CLI disagree at " + node.name);
            }
        }
    } catch (const std::exception& failure) { QFAIL(failure.what()); }
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
