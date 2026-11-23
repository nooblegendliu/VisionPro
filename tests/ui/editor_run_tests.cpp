#include "control_scene.hpp"
#include "editor_window.hpp"
#include "run_controller.hpp"
#include "run_panel.hpp"
#include "tool_catalog.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <QAction>
#include <QDockWidget>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QGraphicsView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProcess>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QUndoStack>

#include <algorithm>
#include <stdexcept>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}

template<SchemaIdentity Id> Id key(std::string_view name) { return take(SchemaIdFactory::parse<Id>(name)); }

template<UuidIdentity Id> Id id(char suffix = '1')
{
    std::string text = "5fba0000-0000-4000-8000-000000000001";
    text.back() = suffix;
    return take(IdFactory::parse<Id>(text));
}

ControlScene& scene(EditorWindow& window)
{
    return *qobject_cast<ControlScene*>(window.findChild<QGraphicsView*>(QStringLiteral("workflowCanvas"))->scene());
}

QAction& action(EditorWindow& window, const char* name) { return *window.findChild<QAction*>(QString::fromLatin1(name)); }

template<class Fill> bool submit_dialog(QAction& entry, const Fill& fill)
{
    bool accepted = false;
    QTimer interaction;
    interaction.setSingleShot(true);
    QObject::connect(&interaction, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { return; }
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (buttons && fill(*dialog, *buttons->button(QDialogButtonBox::Ok))) {
            buttons->button(QDialogButtonBox::Ok)->click();
            accepted = dialog->result() == QDialog::Accepted;
        }
        if (!accepted) { dialog->reject(); }
    });
    interaction.start(0);
    entry.trigger();
    return accepted;
}

std::int64_t integer_output(const RunHandle& run, const NodeId& node, const OutputId& port)
{
    const auto progress = take(run.inspect());
    const auto invocation = std::ranges::find(progress.nodes, node, &RunNodeProgress::node);
    const auto result = take(run.lookup(invocation->invocation.value())).value();
    return *std::get<DataValue>(result.output(port)->content).as_int64();
}

class Counter final : public ToolInstance {
public:
    explicit Counter(const ToolExecutionConfiguration& configuration) : ToolInstance(configuration) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        const auto& invocation = context->data();
        const auto state = take(invocation.state->read());
        auto total = DataValue::integer(*state.as_int64() + *invocation.inputs.front().value.as_int64());
        auto written = invocation.state->write(total);
        if (auto* error = std::get_if<Diagnostic>(&written)) { return std::move(*error); }
        return InvocationStart{ToolCompletion{invocation.identity, ToolCompletionStatus::Succeeded, ToolJudgement::OK,
            {{key<OutputId>("total"), std::move(total)}}, {}}};
    }
};

class CounterFactory final : public ToolInstanceFactory {
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<Counter>(configuration)}; }
};

ToolRegistry counter_registry()
{
    const auto integer = DataType::integer();
    InputDescriptor delta{key<InputId>("delta"), "Delta", integer};
    delta.constraints.integer_range = IntegerRange{0, 100};
    delta.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::PublicInput};
    ToolDescriptor descriptor{key<ToolTypeId>("test.editor.counter"), "Counter", 1,
        {key<SchemaId>("test.editor.counter.schema"), 1}, {}, {delta},
        {{key<OutputId>("total"), "Total", integer}}};
    descriptor.execution.effects = ToolEffects::State;
    descriptor.state = NodeStateDefinition{integer, ConfigValue::integer(0), StateScope::SolutionSession,
        StateResetPolicy::DomainStart, StateCompatibilityPolicy::PreserveCompatible, StateMutationTiming::OnResultAccepted,
        StateConcurrencyPolicy::Serialized};
    const ToolRegistration registration{descriptor, {descriptor.schema.id, 1, 1, {}}, std::make_shared<CounterFactory>()};
    return take(ToolRegistry::create(std::span{&registration, 1}));
}

Solution counter_fixture()
{
    const auto delta = key<InputId>("delta");
    const auto flow_id = id<WorkflowId>();
    const auto first = id<NodeId>('1');
    const auto second = id<NodeId>('2');
    const ToolConfiguration counter{key<ToolTypeId>("test.editor.counter"), 1};
    const InputDescriptor public_delta{delta, "Delta", DataType::integer(), false, ConfigValue::integer(2)};
    Workflow flow{flow_id, "Counters", {key<SchemaId>("test.editor.flow"), 1, {public_delta}},
        {{first, "First counter", counter, {{delta, {ScopedInputReference{flow_id, {}, delta, {}}}}}},
            {second, "Second counter", counter, {{delta, {ConfigValue::integer(1)}}}}},
        {{id<ControlLinkId>(), first, {}, second}}, {}, {first}};
    return {id<SolutionId>(), "Counter draft", infrastructure::solution_format_version, 1, {std::move(flow)}};
}

class EditorRunTests final : public QObject {
    Q_OBJECT

private slots:
    void publishes_draft_and_isolates_debug();
    void runs_unsaved_absolute_resources();
    void selects_each_iteration_of_the_same_node();
    void runs_and_debugs_selected_group_with_public_inputs();
    void replays_saved_trigger_in_editor_and_cli();
    void arming_preserves_session_state();
};

void EditorRunTests::arming_preserves_session_state()
{
    auto draft = counter_fixture();
    auto& flow = draft.workflows.front();
    flow.trigger = WorkflowTrigger{TriggerMode::External, key<SchemaId>("fixture.frames"), key<InputId>("delta")};
    EditorWindow window(draft, counter_registry());
    window.show();
    auto& controller = *window.findChild<RunController*>();
    QVERIFY(submit_dialog(action(window, "runWorkflow"), [](QDialog&, QPushButton&) { return true; }));
    QTRY_VERIFY(controller.runs().size() == 1 && controller.can_start());
    const auto previous = controller.runs().front();
    action(window, "armWorkflowTrigger").trigger();
    QTRY_VERIFY(controller.can_send_frame(flow.id));
    QCOMPARE(controller.runs().size(), 1u);
    controller.send_frame(flow.id, 5);
    QTRY_VERIFY(controller.runs().size() == 2 && controller.can_start());
    const auto triggered = controller.runs().back();
    QCOMPARE(previous.identity().snapshot.session, triggered.identity().snapshot.session);
    QCOMPARE(integer_output(triggered, flow.nodes.front().id, key<OutputId>("total")), 7);
}

void EditorRunTests::replays_saved_trigger_in_editor_and_cli()
{
    const auto registry = take(app::make_tool_registry());
    const std::filesystem::path sample{VISIONPRO_S66_DIRECTORY};
    const auto loaded = take(infrastructure::load_solution(sample, registry));
    const auto& external = loaded.solution().workflows.at(0);
    const auto& periodic = loaded.solution().workflows.at(1);
    EditorWindow window(loaded.solution(), registry);
    window.show();
    window.open_document(sample);
    QTRY_VERIFY(!window.file_busy());
    QVERIFY(scene(window).select_workflow(external.id));
    auto& controller = *window.findChild<RunController*>();
    action(window, "workflowProperties").trigger();
    auto* configure = window.findChild<QPushButton*>(QStringLiteral("editWorkflowTrigger"));
    QTRY_VERIFY((configure = window.findChild<QPushButton*>(QStringLiteral("editWorkflowTrigger"))) && configure->isVisible());
    const auto source = QStringLiteral("s66.desktop.frames");
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { return; }
        if (auto* input = dialog->findChild<QLineEdit*>(QStringLiteral("triggerSource"))) { input->setText(source); }
        dialog->accept();
    });
    configure->click();
    QCOMPARE(QString::fromUtf8(window.draft().workflows.front().trigger->source.text()), source);
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const auto saved = std::filesystem::path(scratch.path().toStdU16String()) / "saved trigger";
    window.transfer_document(saved, infrastructure::SolutionTransfer::SaveAs);
    QTRY_VERIFY(!window.file_busy());
    window.open_document(saved);
    QTRY_VERIFY(!window.file_busy());
    QCOMPARE(QString::fromUtf8(window.draft().workflows.front().trigger->source.text()), source);
    QVERIFY(scene(window).select_workflow(external.id));
    auto& arm = action(window, "armWorkflowTrigger");
    auto& send = action(window, "sendExternalFrame");
    arm.trigger();
    const auto output = key<OutputId>("value");
    QStringList cli_args{QStringLiteral("trigger"), QStringLiteral("--solution"), QString::fromStdU16String((saved / "solution.json").u16string()),
        QStringLiteral("--workflow"), QString::fromStdString(external.id.to_string())};
    std::vector<std::int64_t> results;
    for (const auto frame : {11LL, 22LL, 4000000000LL}) {
        QTRY_VERIFY(send.isEnabled());
        QTimer::singleShot(0, [frame] {
            if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                dialog->setTextValue(QString::number(frame));
                dialog->accept();
            }
        });
        send.trigger();
        QTRY_VERIFY(controller.can_start() && controller.runs().size() == results.size() + 1);
        const auto run = controller.runs().back();
        QCOMPARE(run.request().trigger().kind, TriggerKind::External);
        QCOMPARE(QString::fromUtf8(run.request().trigger().source.text()), source);
        QCOMPARE(run.request().correlation()->key(), std::to_string(frame));
        const auto value = integer_output(run, external.entries.front(), output);
        QCOMPARE(value, frame);
        results.push_back(value);
        cli_args << QStringLiteral("--frame") << QString::number(frame);
    }
    action(window, "drainWorkflowTrigger").trigger();
    QTRY_VERIFY(arm.isEnabled() && !send.isEnabled());
    QProcess cli;
    cli.start(QStringLiteral(VISIONPRO_TEST_CLI), cli_args);
    QVERIFY(cli.waitForFinished(10000));
    const auto errors = cli.readAllStandardError();
    QVERIFY2(cli.exitStatus() == QProcess::NormalExit && !cli.exitCode(), errors.constData());
    const auto events = QJsonDocument::fromJson(cli.readAllStandardOutput()).object().value("events").toArray();
    QCOMPARE(events.size(), static_cast<qsizetype>(results.size()));
    for (qsizetype i = 0; i < events.size(); ++i) {
        const auto event = events[i].toObject();
        QCOMPARE(event.value("source").toString(), source);
        const auto result = event.value("result").toObject();
        QCOMPARE(result.value("quiescent").toBool(), true);
        QCOMPARE(result.value("status").toString(), QStringLiteral("Succeeded"));
        const auto outputs = result.value("nodes").toArray().at(0).toObject().value("outputs").toArray();
        QCOMPARE(outputs.at(0).toObject().value("value").toString(), QString::number(results[static_cast<std::size_t>(i)]));
    }
    QVERIFY(scene(window).select_workflow(periodic.id));
    arm.trigger();
    QTRY_VERIFY(controller.runs().size() >= 5);
    QCOMPARE(controller.runs().at(3).request().trigger().kind, TriggerKind::Continuous);
    QCOMPARE(*controller.runs().at(3).request().inputs()[0].value.as_int64(), 1);
    QCOMPARE(*controller.runs().at(4).request().inputs()[0].value.as_int64(), 2);
    action(window, "cancelWorkflowTrigger").trigger();
    QTRY_VERIFY(arm.isEnabled());
    arm.trigger();
    QTRY_VERIFY(action(window, "drainWorkflowTrigger").isEnabled());
    window.open_document(saved);
    QTRY_VERIFY(!window.file_busy() && !controller.has_session_work());
    QTest::qWait(250); // More than two periods of the retired subscription.
    QVERIFY(controller.runs().empty());
}

void EditorRunTests::runs_and_debugs_selected_group_with_public_inputs()
{
    const auto group_id = id<NodeId>('3');
    const auto child = id<NodeId>('1');
    const auto outside = id<NodeId>('2');
    const auto input = key<InputId>("delta");
    const auto output = key<OutputId>("total");
    const auto done = key<ControlOutputId>("done");
    auto draft = counter_fixture();
    auto& flow = draft.workflows.front();
    flow.nodes.front().bindings = {{input, {ScopedInputReference{flow.id, group_id, input, {}}}}};
    auto public_input = flow.interface.inputs.front();
    GroupConfiguration group{{key<SchemaId>("test.editor.group"), 1, {public_input},
        {{output, "Total", DataType::integer()}}, {{done, "Done"}}}, {child}, {{output, {flow.id, child, output, {}}}},
        {{done, child, {}}}};
    flow.nodes.push_back({group_id, "Measurement", std::move(group)});
    flow.containment = {{group_id, child}};
    flow.entries = {group_id};
    flow.control_links = {{id<ControlLinkId>(), group_id, done, outside}};
    EditorWindow window(std::move(draft), counter_registry());
    window.show();
    QVERIFY(scene(window).reveal_node(group_id));
    auto& run = action(window, "runSelectedGroup");
    auto& debug = action(window, "debugSelectedNode");
    QVERIFY(run.isEnabled() && debug.isEnabled());
    auto& controller = *window.findChild<RunController*>();
    window.findChild<QDockWidget*>(QStringLiteral("runDock"))->show();
    auto& panel = *window.findChild<RunPanel*>();
    auto& tree = *panel.findChild<QTreeWidget*>(QStringLiteral("runNodes"));
    auto& details = *panel.findChild<QPlainTextEdit*>(QStringLiteral("runDetails"));
    int calls = 0;
    for (auto* action : {&run, &debug, &run}) {
        QVERIFY(submit_dialog(*action, [](QDialog& dialog, QPushButton&) {
            auto* mode = dialog.findChild<QComboBox*>(QStringLiteral("runInputMode.delta"));
            auto* value = dialog.findChild<QLineEdit*>(QStringLiteral("runInputValue.delta"));
            if (!mode || !value) { return false; }
            mode->setCurrentIndex(1);
            value->setText(QStringLiteral("3"));
            return true;
        }));
        ++calls;
        QTRY_VERIFY(controller.runs().size() == static_cast<std::size_t>(calls) && controller.can_start());
        const auto& handle = controller.runs().back();
        const auto progress = take(handle.inspect());
        QVERIFY2(progress.status == ExecutionStatus::Succeeded, progress.diagnostics.empty() ? "" : progress.diagnostics.front().message.c_str());
        QCOMPARE(progress.groups.size(), 1u);
        QCOMPARE(progress.nodes.size(), 2u);
        const auto record = take(handle.lookup(progress.groups.front().group)).value();
        QVERIFY(record.data().selected_control_outputs == std::vector{done});
        QCOMPARE(*std::get<DataValue>(record.output(output)->content).as_int64(), calls == 3 ? 6 : 3);
        QVERIFY(handle.request().debug_state() == (action == &run ? DebugStatePolicy::Active : DebugStatePolicy::Isolated));
        QTRY_COMPARE(tree.topLevelItemCount(), 2);
        tree.setCurrentItem(tree.topLevelItem(1));
        QVERIFY(tree.currentItem()->text(2).contains(QStringLiteral("Measurement")));
        QVERIFY(panel.selected_invocation()->node == child && details.toPlainText().contains(QStringLiteral("Group call")));
        tree.setCurrentItem(tree.topLevelItem(0));
        QVERIFY(details.toPlainText().contains(QStringLiteral("Selected control outlets: done")));
    }
}

void EditorRunTests::selects_each_iteration_of_the_same_node()
{
    const auto flow_id = id<WorkflowId>();
    Workflow flow{flow_id, "Loop workflow", {key<SchemaId>("test.editor.loop"), 1}};
    Solution draft{id<SolutionId>(), "Loop draft", infrastructure::solution_format_version, 1, {std::move(flow)}};
    EditorWindow window(std::move(draft), take(app::make_tool_registry()));
    window.show();
    QVERIFY(submit_dialog(action(window, "addLoop"), [](QDialog&, QPushButton&) { return true; }));
    auto& canvas = scene(window);
    const auto loop = canvas.selected_nodes().at(0);
    const auto& model = static_cast<const ControlGraphModel&>(canvas.graphModel());
    QVERIFY(canvas.open_group(model.canvas_id(loop)));
    QVERIFY(canvas.add_tool(key<ToolTypeId>("visionpro.sim.add"), {}));
    const auto child = canvas.selected_nodes().at(0);
    QVERIFY(canvas.edit_draft(QStringLiteral("Set loop bounds and inputs"), [&](Solution& solution) -> Result<std::monostate> {
        auto& nodes = solution.workflows.front().nodes;
        auto& bindings = nodes.front().bindings;
        auto end = std::ranges::find(bindings, BindingTarget{key<InputId>("end")}, &Binding::target);
        end->sources = {ConfigValue::integer(2)};
        nodes.back().bindings = {{key<InputId>("left"), {ScopedInputReference{flow_id, loop, key<InputId>("index"), {}}}},
            {key<InputId>("right"), {ConfigValue::integer(10)}}};
        return std::monostate{};
    }));
    action(window, "runWorkflow").trigger();
    auto& controller = *window.findChild<RunController*>();
    QTRY_VERIFY(controller.runs().size() == 1 && controller.can_start());
    const auto progress = take(controller.runs().front().inspect());
    QVERIFY2(progress.status == ExecutionStatus::Succeeded, progress.diagnostics.empty() ? "" : progress.diagnostics.front().message.c_str());
    QCOMPARE(progress.iterations.size(), 2u);
    window.findChild<QDockWidget*>(QStringLiteral("runDock"))->show();
    auto& panel = *window.findChild<RunPanel*>();
    auto& tree = *panel.findChild<QTreeWidget*>(QStringLiteral("runNodes"));
    auto& details = *panel.findChild<QPlainTextEdit*>(QStringLiteral("runDetails"));
    QTRY_COMPARE(tree.topLevelItemCount(), 3);
    tree.setCurrentItem(tree.topLevelItem(0));
    QVERIFY(!panel.selected_iteration_index());
    for (int i = 0; i < 2; ++i) {
        auto* row = tree.topLevelItem(i + 1);
        QVERIFY(row->text(2).startsWith(QString::number(i) + QStringLiteral(" · Scope ")));
        tree.setCurrentItem(row);
        const auto selected = panel.selected_invocation();
        QVERIFY(selected && selected->node == child && selected->scope == progress.iterations[i].scope);
        QVERIFY(panel.selected_iteration_index() == i);
        QVERIFY(details.toPlainText().contains(QStringLiteral("value = %1").arg(10 + i)));
    }
}

void EditorRunTests::publishes_draft_and_isolates_debug()
{
    EditorWindow window(counter_fixture(), counter_registry());
    window.show();
    auto& control = *window.findChild<RunController*>();
    window.findChild<QDockWidget*>(QStringLiteral("runDock"))->show();
    auto& panel = *window.findChild<RunPanel*>();
    auto& workflow_action = action(window, "runWorkflow");
    auto& debug_action = action(window, "debugSelectedNode");
    auto* history = window.findChild<QComboBox*>(QStringLiteral("runHistory"));
    const auto with_default = [](QDialog& dialog, QPushButton&) {
        return dialog.objectName() == QStringLiteral("runInputsDialog");
    };
    const auto delta = key<InputId>("delta");
    const auto total = key<OutputId>("total");
    const auto first = id<NodeId>('1');
    const auto second = id<NodeId>('2');

    QVERIFY(workflow_action.isEnabled());
    QVERIFY(submit_dialog(workflow_action, with_default));
    QTRY_COMPARE(control.runs().size(), 1u);
    QTRY_VERIFY(control.can_start());
    const auto initial = control.runs().front();
    QCOMPARE(initial.request().input(delta)->origin, InputOrigin::Default);
    QCOMPARE(integer_output(initial, first, total), 2);
    QCOMPARE(integer_output(initial, second, total), 1);

    QVERIFY(scene(window).reveal_node(first) && debug_action.isEnabled());
    bool missing_rejected = false;
    QVERIFY(submit_dialog(debug_action, [&](QDialog& dialog, QPushButton& ok) {
        auto* field = dialog.findChild<QLineEdit*>(QStringLiteral("runInputValue.delta"));
        auto* error = dialog.findChild<QLabel*>(QStringLiteral("runInputError"));
        if (!field || !error) { return false; }
        ok.click();
        missing_rejected = dialog.isVisible() && !error->text().isEmpty() && control.runs().size() == 1;
        field->setText(QStringLiteral("9"));
        return true;
    }));
    QVERIFY(missing_rejected);
    QTRY_COMPARE(control.runs().size(), 2u);
    QTRY_VERIFY(control.can_start());
    const auto debug = control.runs().back();
    QVERIFY(debug.request().debug_state() == DebugStatePolicy::Isolated);
    QCOMPARE(debug.request().input(delta)->origin, InputOrigin::Provided);
    QCOMPARE(*debug.request().input(delta)->value.as_int64(), 9);
    const auto debug_progress = take(debug.inspect());
    QVERIFY(debug_progress.call_order == std::vector{first});
    QCOMPARE(integer_output(debug, first, total), 9);
    QCOMPARE(initial.identity().snapshot.session, debug.identity().snapshot.session);
    QVERIFY(scene(window).undoStack().isClean());
    const auto& persisted = window.draft().workflows.front();
    QCOMPARE(*persisted.interface.inputs.front().default_value->as_int64(), 2);
    QVERIFY(std::holds_alternative<ScopedInputReference>(persisted.nodes.front().bindings.front().sources.front()));

    history->setCurrentIndex(2);
    QVERIFY(panel.selected_run()->identity() == initial.identity());
    QVERIFY(submit_dialog(action(window, "runDisplayLimits"), [](QDialog& dialog, QPushButton&) {
        auto* size = dialog.findChild<QSpinBox*>(QStringLiteral("runHistoryLimit"));
        if (!size) { return false; }
        size->setValue(1);
        return true;
    }));
    QCOMPARE(control.runs().size(), 1u);
    QVERIFY(panel.selected_run()->identity() == initial.identity());

    QVERIFY(scene(window).edit_draft(QStringLiteral("Change workflow default"), [](Solution& draft) -> Result<std::monostate> {
        draft.workflows.front().interface.inputs.front().default_value = ConfigValue::integer(3);
        return std::monostate{};
    }));
    QVERIFY(submit_dialog(workflow_action, with_default));
    QTRY_VERIFY(control.can_start() && control.runs().front().identity() != debug.identity());
    const auto latest = control.runs().front();
    QCOMPARE(latest.identity().snapshot.session, initial.identity().snapshot.session);
    QVERIFY(latest.identity().snapshot.snapshot != initial.identity().snapshot.snapshot);
    QCOMPARE(integer_output(latest, first, total), 5);
    QCOMPARE(integer_output(latest, second, total), 2);
    QCOMPARE(*initial.request().input(delta)->value.as_int64(), 2);
    QVERIFY(panel.selected_run()->identity() == initial.identity());
    history->setCurrentIndex(0);
    QVERIFY(panel.selected_run()->identity() == latest.identity());

    QVERIFY(scene(window).edit_draft(QStringLiteral("Remove required binding"), [](Solution& draft) -> Result<std::monostate> {
        draft.workflows.front().nodes.front().bindings.clear();
        return std::monostate{};
    }));
    QVERIFY(submit_dialog(workflow_action, with_default));
    QTRY_VERIFY(control.can_start() && !window.statusBar()->currentMessage().startsWith(QStringLiteral("Preparing")));
    QCOMPARE(control.runs().size(), 1u);
    QVERIFY(control.runs().front().identity() == latest.identity());
}

void EditorRunTests::runs_unsaved_absolute_resources()
{
    const QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QFile payload(temporary.filePath(QStringLiteral("input.bin")));
    QVERIFY(payload.open(QIODevice::WriteOnly));
    QCOMPARE(payload.write(QByteArray(1, char{42})), 1);
    payload.close();
    const auto resource = id<ResourceId>();
    const auto node = id<NodeId>();
    const Node reader{node, "Read byte", ToolConfiguration{key<ToolTypeId>("visionpro.sim.file_byte"), 1}, {},
        {{key<SchemaId>("payload"), {resource, ResourceKind::Binary, {}}}}};
    Workflow workflow{id<WorkflowId>(), "Resource workflow", {key<SchemaId>("test.editor.read"), 1}, {reader}, {}, {}, {node}};
    Solution solution{id<SolutionId>(), "Unsaved resources", infrastructure::solution_format_version, 1, {std::move(workflow)},
        {{resource, "Input bytes", ResourceKind::Binary,
            ExternalFileResource{{PathBase::Absolute, payload.fileName().toStdString(), {}}, ResourceReadPolicy::PerInvocation, {}, {}}}}};
    EditorWindow window(std::move(solution), take(app::make_tool_registry()));
    window.show();
    auto& control = *window.findChild<RunController*>();
    auto& run = action(window, "runWorkflow");
    run.trigger();
    QTRY_COMPARE(control.runs().size(), 1u);
    QTRY_VERIFY(control.can_start());
    const auto first = control.runs().front();
    QCOMPARE(integer_output(first, node, key<OutputId>("value")), 42);

    QVERIFY(scene(window).edit_draft(QStringLiteral("Use solution-relative bytes"), [](Solution& draft) -> Result<std::monostate> {
        auto& file = std::get<ExternalFileResource>(draft.resources.front().location);
        file.path = {PathBase::SolutionDirectory, "input.bin", {}};
        return std::monostate{};
    }));
    run.trigger();
    QTRY_VERIFY(control.can_start());
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("explicit solution directory")));
    QCOMPARE(control.runs().size(), 1u);
    QVERIFY(control.runs().front().identity() == first.identity());
}

} // namespace
} // namespace visionpro::ui

QTEST_MAIN(visionpro::ui::EditorRunTests)
#include "editor_run_tests.moc"
