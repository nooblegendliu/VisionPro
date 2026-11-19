#include "canvas_example.hpp"
#include "control_scene.hpp"
#include "editor_inspector.hpp"
#include "editor_window.hpp"
#include "run_controller.hpp"
#include "tool_catalog.hpp"

#include <simulation.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/validation.hpp>

#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QGraphicsView>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QUndoStack>

#include <algorithm>
#include <functional>
#include <initializer_list>
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

template<SchemaIdentity Id> Id key(std::string_view text)
{ return take(SchemaIdFactory::parse<Id>(text)); }

ToolRegistry registry()
{
    auto registrations = tools::simulation_registrations();
    const auto add = key<ToolTypeId>("visionpro.sim.add");
    const auto registration = std::ranges::find_if(registrations,
        [&](const auto& item) { return item.descriptor.id == add; });
    if (registration == registrations.end()) { throw std::runtime_error("The add tool is missing."); }
    const auto object = take(DataType::record(key<SchemaId>("test.inspector.object"), 1,
        {{key<FieldId>("area_pixels"), DataType::integer()}}));
    registration->descriptor.outputs.push_back({key<OutputId>("objects"), "Objects", DataType::list(object),
        {}, false, {OutputAbsenceReason::ToolDidNotProduce}});
    return take(ToolRegistry::create(registrations));
}

const Node& node_named(const Workflow& flow, std::string_view name)
{
    const auto node = std::ranges::find(flow.nodes, name, &Node::name);
    if (node == flow.nodes.end()) { throw std::runtime_error("The example node is missing."); }
    return *node;
}

const Binding* binding_for(const ControlGraphModel& model, NodeId node, const InputId& target)
{
    const auto* selected = find_node(model.draft().workflows.front(), node);
    if (!selected) { return nullptr; }
    const auto binding = std::ranges::find(selected->bindings, BindingTarget{target}, &Binding::target);
    return binding == selected->bindings.end() ? nullptr : &*binding;
}

const GroupConfiguration& group_config(const ControlGraphModel& model, NodeId group)
{
    const auto* node = find_node(model.draft().workflows.front(), group);
    if (!node) { throw std::runtime_error("The example Group is missing."); }
    return std::get<GroupConfiguration>(node->configuration);
}

QPushButton* binding_button(const QWidget& inspector, const QString& target)
{
    for (auto* button : inspector.findChildren<QPushButton*>(QStringLiteral("editBinding"))) {
        if (button->property("bindingTarget").toString() == target) { return button; }
    }
    return nullptr;
}

QPushButton* accept_button(QDialog& dialog)
{
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    return buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
}

bool select_choice(QComboBox& combo, const QString& label, bool partial = false)
{
    for (int index = 0; index < combo.count(); ++index) {
        const auto item = combo.itemText(index);
        if (partial ? item.contains(label) : item == label) {
            combo.setCurrentIndex(index);
            return true;
        }
    }
    return false;
}

using DialogStep = std::function<QPushButton*(QDialog&)>;

QString exercise_dialog(QWidget& inspector, QPushButton& start, std::initializer_list<DialogStep> steps)
{
    if (auto* scroll = inspector.findChild<QScrollArea*>()) { scroll->ensureWidgetVisible(&start); }
    const auto* next = steps.begin();
    QString stalled_at;
    QTimer poll;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, [&] {
        poll.stop();
        if (const auto* modal = QApplication::activeModalWidget()) { stalled_at = modal->windowTitle(); }
        for (auto* window : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QDialog*>(window); dialog && dialog->isVisible() && dialog->isModal()) {
                dialog->reject();
            }
        }
    });
    QObject::connect(&poll, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || next == steps.end()) { return; }
        if (auto* button = (*next)(*dialog)) {
            ++next;
            QTimer::singleShot(0, button, [button] { QTest::mouseClick(button, Qt::LeftButton); });
        }
    });
    poll.start(10);
    timeout.start(5000);
    QTest::mouseClick(&start, Qt::LeftButton);
    if (next == steps.end() && timeout.isActive()) { return {}; }
    return QStringLiteral("Stopped at dialog step %1 of %2 (%3).")
        .arg(next - steps.begin() + 1).arg(steps.size()).arg(stalled_at);
}

class InspectorTests final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void commits_binding_and_group_outlet_through_dialogs();
    void edits_loop_settings_and_collects_a_selected_field();
    void edits_and_reloads_dynamic_indices_and_format_options();
    void resets_only_the_selected_session_accumulator();
    void edits_shared_targets_and_previews_without_consuming();
    void edits_and_reloads_workflow_admission();
};

ControlScene& scene(EditorWindow& window)
{
    return *qobject_cast<ControlScene*>(qobject_cast<QGraphicsView*>(window.centralWidget())->scene());
}

QWidget& properties(EditorWindow& window)
{
    return *window.findChild<QDockWidget*>(QStringLiteral("inspectorDock"))->widget();
}

NodeResult result_for(const RunHandle& run, const NodeId& node)
{
    const auto progress = take(run.inspect());
    const auto found = std::ranges::find(progress.nodes, node, &RunNodeProgress::node);
    if (found == progress.nodes.end() || !found->invocation) { throw std::runtime_error("Missing invocation"); }
    return take(run.lookup(*found->invocation)).value();
}

void InspectorTests::edits_and_reloads_workflow_admission()
{
    const auto catalog = take(app::make_tool_registry());
    EditorWindow window(canvas_example(), catalog);
    window.show();
    auto& inspector = properties(window);
    auto* edit = inspector.findChild<QPushButton*>(QStringLiteral("editWorkflowAdmission"));
    QVERIFY(edit && edit->text().contains(QStringLiteral("1 running · 0 waiting · Busy")));
    const auto changed = exercise_dialog(inspector, *edit, {[](QDialog& dialog) -> QPushButton* {
        auto* in_flight = dialog.findChild<QLineEdit*>(QStringLiteral("maxInFlight"));
        auto* capacity = dialog.findChild<QLineEdit*>(QStringLiteral("queueCapacity"));
        auto* overflow = dialog.findChild<QComboBox*>(QStringLiteral("admissionOverflow"));
        if (!in_flight || !capacity || !overflow) { return nullptr; }
        in_flight->setText(QStringLiteral("2"));
        capacity->setText(QStringLiteral("3"));
        overflow->setCurrentIndex(1);
        return accept_button(dialog);
    }});
    QVERIFY2(changed.isEmpty(), qPrintable(changed));
    const WorkflowAdmission configured{2, 3, AdmissionOverflow::Reject};
    QVERIFY(window.draft().workflows.front().admission == configured);
    auto& history = scene(window).undoStack();
    history.undo();
    QVERIFY(window.draft().workflows.front().admission == WorkflowAdmission{});
    history.redo();
    QVERIFY(window.draft().workflows.front().admission == configured);

    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const auto directory = std::filesystem::path(scratch.path().toStdWString()) / L"admission";
    window.transfer_document(directory, infrastructure::SolutionTransfer::SaveAs);
    QTRY_VERIFY(!window.file_busy() && std::filesystem::exists(directory / "solution.json"));
    EditorWindow reopened(canvas_example(), catalog);
    reopened.open_document(directory);
    QTRY_VERIFY(!reopened.file_busy() && reopened.draft().workflows.front().admission == configured);
    const auto* saved = properties(reopened).findChild<QPushButton*>(QStringLiteral("editWorkflowAdmission"));
    QVERIFY(saved && saved->text().contains(QStringLiteral("2 running · 3 waiting · Reject")));
}

void InspectorTests::edits_shared_targets_and_previews_without_consuming()
{
    const auto catalog = take(app::make_tool_registry());
    const auto queue1 = take(IdFactory::generate<QueueId>()), queue2 = take(IdFactory::generate<QueueId>());
    const auto first = take(IdFactory::generate<VariableId>()), second = take(IdFactory::generate<VariableId>());
    const auto flow = take(IdFactory::generate<WorkflowId>());
    const auto send = take(IdFactory::generate<NodeId>()), many = take(IdFactory::generate<NodeId>()), join = take(IdFactory::generate<NodeId>());
    const auto write = take(IdFactory::generate<NodeId>());
    const auto send_type = key<ToolTypeId>("visionpro.shared.queue_send_dimensions");
    const auto* sender = catalog.find_descriptor(send_type);
    QVERIFY(sender && sender->shared_target);
    const auto dimensions = sender->shared_target->type;
    const auto string = [](std::string_view value) { return take(ConfigValue::string(value)); };
    const auto payload = take(ConfigValue::record(dimensions, {{key<FieldId>("width"), ConfigValue::integer(10)},
        {key<FieldId>("height"), ConfigValue::integer(1)}}));
    const auto domain = key<InputId>("domain"), item = key<InputId>("key");
    Workflow workflow{flow, "Dimensions", {key<SchemaId>("test.shared_editor"), 1},
        {{send, "Send measurement", ToolConfiguration{send_type, 1, {queue1}},
            {{key<InputId>("payload"), {payload}}, {domain, {string("line.one")}}, {item, {string("A")}}}},
         {many, "Read values", ToolConfiguration{key<ToolTypeId>("visionpro.shared.global_read_many_int64"), 1, {first}}},
         {join, "Join parts", ToolConfiguration{key<ToolTypeId>("visionpro.shared.join_dimensions"), 1},
            {{key<ParameterId>("join"), {string("measure")}}, {key<InputId>("batch"), {ConfigValue::integer(1)}},
             {domain, {string("line.one")}}, {item, {string("A")}}, {key<InputId>("source"), {string("width")}},
             {key<InputId>("value"), {ConfigValue::integer(10)}}}},
         {write, "Write gain", ToolConfiguration{key<ToolTypeId>("visionpro.shared.global_write_int64"), 1, {first}},
            {{key<InputId>("value"), {ConfigValue::integer(9)}}}}},
        {{take(IdFactory::generate<ControlLinkId>()), send, {}, many},
         {take(IdFactory::generate<ControlLinkId>()), many, {}, join},
         {take(IdFactory::generate<ControlLinkId>()), join, key<ControlOutputId>("waiting"), write}}, {}, {send}};
    const QueueReceivePolicy policy{EmptyQueuePolicy::Fail, 1, QueueConsumerPolicy::SingleConsumer, QueueBatchPolicy::RequireExact};
    const auto other_schema = take(DataType::record(key<SchemaId>("test.other"), 1, dimensions.fields()));
    Solution draft{take(IdFactory::generate<SolutionId>()), "Shared preview", infrastructure::solution_format_version, 1, {workflow}, {},
        {{first, "Gain", DataType::integer(), ConfigValue::integer(2)}, {second, "Gain", DataType::integer(), ConfigValue::integer(3)},
         {take(IdFactory::generate<VariableId>()), "Label", DataType::string(), string("x")}},
        {{queue1, "Discard", dimensions, 2, QueueOverflowPolicy::Reject, policy}, {queue2, "Measurements", dimensions, 2, QueueOverflowPolicy::Reject, policy},
         {take(IdFactory::generate<QueueId>()), "Wrong schema", other_schema, 2, QueueOverflowPolicy::Reject, policy}}};
    EditorWindow window(std::move(draft), catalog);
    window.show();
    auto& inspector = properties(window);
    const auto reveal = [&](const NodeId& id) {
        const auto ok = scene(window).reveal_node(id);
        static_cast<EditorInspector&>(inspector).refresh();
        return ok;
    };
    const auto choose_target = [](const auto& id) -> DialogStep {
        return [label = QString::fromStdString(id.to_string())](QDialog& dialog) -> QPushButton* {
            auto* combo = dialog.findChild<QComboBox*>(QStringLiteral("compatibleSharedTargets"));
            if (!combo || combo->count() != 2 || !select_choice(*combo, label, true)) { return nullptr; }
            return dialog.findChild<QPushButton*>(QStringLiteral("setSharedTarget"));
        };
    };
    QVERIFY(reveal(send));
    auto* edit = inspector.findChild<QPushButton*>(QStringLiteral("editSharedTargets"));
    QVERIFY(edit);
    auto changed = exercise_dialog(inspector, *edit, {choose_target(queue2), accept_button});
    QVERIFY2(changed.isEmpty(), qPrintable(changed));
    QVERIFY(reveal(many));
    edit = inspector.findChild<QPushButton*>(QStringLiteral("editSharedTargets"));
    QVERIFY(edit);
    changed = exercise_dialog(inspector, *edit, {choose_target(second), [](QDialog& dialog) -> QPushButton* {
        auto* list = dialog.findChild<QListWidget*>(QStringLiteral("orderedSharedTargets"));
        if (!list || list->count() != 2 || list->currentRow() != 1) { return nullptr; }
        return dialog.findChild<QPushButton*>(QStringLiteral("earlierSharedTarget"));
    }, accept_button});
    QVERIFY2(changed.isEmpty(), qPrintable(changed));
    const auto& edited = window.draft().workflows.front();
    QVERIFY(std::get<ToolConfiguration>(find_node(edited, send)->configuration).shared_targets == std::vector<SharedTargetId>{queue2});
    const std::vector<SharedTargetId> ordered{second, first};
    QVERIFY(std::get<ToolConfiguration>(find_node(edited, many)->configuration).shared_targets == ordered);
    QVERIFY(reveal(join));
    auto* sources = binding_button(inspector, QStringLiteral("sources"));
    QVERIFY(sources);
    changed = exercise_dialog(inspector, *sources, {[](QDialog& dialog) {
        return dialog.findChild<QPushButton*>(QStringLiteral("setBindingSource"));
    }, [](QDialog& dialog) -> QPushButton* {
        auto* input = qobject_cast<QInputDialog*>(&dialog);
        if (!input || input->objectName() != QStringLiteral("stringListEditor")) { return nullptr; }
        input->setTextValue(QStringLiteral("[\"height\",\"width\"]"));
        return accept_button(dialog);
    }, accept_button});
    QVERIFY2(changed.isEmpty(), qPrintable(changed));
    auto* preview = window.findChild<QAction*>(QStringLiteral("previewSharedData"));
    QVERIFY(preview);
    preview->trigger();
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("sharedDataDialog"));
    auto* details = dialog->findChild<QPlainTextEdit*>(QStringLiteral("sharedDataPreview"));
    QVERIFY(details->isReadOnly());
    QTRY_VERIFY(details->toPlainText().contains(QStringLiteral("Draft definitions")) && details->toPlainText().contains(QStringLiteral("messages 0")));
    dialog->close();
    QTRY_VERIFY(!window.findChild<QDialog*>(QStringLiteral("sharedDataDialog")));

    auto* controller = window.findChild<RunController*>();
    window.findChild<QAction*>(QStringLiteral("runWorkflow"))->trigger();
    QTRY_VERIFY(controller->runs().size() == 1 && controller->can_start());
    const auto progress = take(controller->runs().front().inspect());
    QVERIFY2(progress.status == ExecutionStatus::Succeeded, progress.diagnostics.empty() ? "" : progress.diagnostics.front().message.c_str());
    const auto undo = scene(window).undoStack().index();
    preview->trigger();
    dialog = window.findChild<QDialog*>(QStringLiteral("sharedDataDialog"));
    details = dialog->findChild<QPlainTextEdit*>(QStringLiteral("sharedDataPreview"));
    auto* refresh = dialog->findChild<QPushButton*>(QStringLiteral("refreshSharedPreview"));
    QTRY_VERIFY(refresh->isEnabled() && details->toPlainText().contains(QStringLiteral("#1 · line.one / A")));
    const auto report = details->toPlainText();
    QVERIFY(report.contains(QStringLiteral("messages 1 · enqueued 1 · received 0")) && report.contains(QStringLiteral("width=10")));
    QVERIFY(report.contains(QStringLiteral("measure · batch 1 · pending 1 · completed 0 · expired 0 · duplicates 0 · late 0")));
    QTest::mouseClick(refresh, Qt::LeftButton);
    QTRY_VERIFY(refresh->isEnabled());
    QCOMPARE(details->toPlainText(), report);
    QCOMPARE(scene(window).undoStack().index(), undo);
    dialog->close();
    QVERIFY(report.contains(QStringLiteral("revision 1 · 9")));

    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const auto directory = std::filesystem::path(scratch.path().toStdWString()) / L"shared";
    window.transfer_document(directory, infrastructure::SolutionTransfer::SaveAs);
    QTRY_VERIFY(!window.file_busy() && std::filesystem::exists(directory / "solution.json"));
    window.open_document(directory);
    QTRY_VERIFY(!window.file_busy() && controller->can_start() && controller->runs().empty());
    window.findChild<QAction*>(QStringLiteral("runWorkflow"))->trigger();
    QTRY_VERIFY(controller->runs().size() == 1 && controller->can_start());
    const auto& run = controller->runs().front();
    QVERIFY(take(run.inspect()).status == ExecutionStatus::Succeeded);
    const auto output = [&](const NodeId& node, std::string_view port) {
        const auto result = result_for(run, node);
        return std::get<DataValue>(result.output(key<OutputId>(port))->content);
    };
    QVERIFY(*output(send, "sequence").as_string() == "1");
    const auto entries = output(many, "entries");
    const auto& gain = entries.elements()->at(1);
    QVERIFY(*gain.field(key<FieldId>("value"))->as_int64() == 2);
    QVERIFY(*gain.field(key<FieldId>("revision"))->as_string() == "0");
    QVERIFY(*output(join, "status").as_enum() == key<EnumMemberId>("Pending"));
}

void InspectorTests::edits_and_reloads_dynamic_indices_and_format_options()
{
    const auto flow_id = take(IdFactory::generate<WorkflowId>());
    const auto formatter = take(IdFactory::generate<NodeId>());
    const auto index = take(IdFactory::generate<NodeId>());
    const auto consumer = take(IdFactory::generate<NodeId>());
    const auto left = key<InputId>("left");
    const auto right = key<InputId>("right");
    const auto value = key<OutputId>("value");
    const auto output = key<OutputId>("used_values");
    const auto calc = ToolConfiguration{key<ToolTypeId>("visionpro.calc.int64"), 1};
    const std::vector list{ConfigValue::integer(6), ConfigValue::integer(9)};
    Workflow flow{flow_id, "Selector", {key<SchemaId>("test.selector"), 1},
        {{formatter, "Format", ToolConfiguration{key<ToolTypeId>("visionpro.format.int64_list"), 1},
            {{key<InputId>("list"), {take(ConfigValue::list(DataType::integer(), list))}},
             {key<InputId>("values"), {ConfigValue::integer(10), ConfigValue::integer(20)}, BindingCardinality::MultipleSources}}},
         {index, "Index", calc, {{left, {ConfigValue::integer(1)}}, {right, {ConfigValue::integer(0)}}}},
         {consumer, "Consumer", calc, {{left, {OutputReference{flow_id, formatter, output, {ElementSelection{0}}}}},
            {right, {ConfigValue::integer(0)}}}}},
        {{take(IdFactory::generate<ControlLinkId>()), formatter, {}, index},
         {take(IdFactory::generate<ControlLinkId>()), index, {}, consumer}}, {}, {formatter}};
    const Solution draft{take(IdFactory::generate<SolutionId>()), "Selector draft", infrastructure::solution_format_version, 1, {flow}};
    const auto catalog = take(app::make_tool_registry());
    EditorWindow window(draft, catalog);
    window.show();
    QVERIFY(scene(window).reveal_node(formatter));
    auto& inspector = properties(window);
    QTRY_VERIFY(binding_button(inspector, QStringLiteral("indices")));
    const auto set = [](QDialog& dialog) { return dialog.findChild<QPushButton*>(QStringLiteral("setBindingSource")); };
    const auto finish = [&](QDialog& dialog) { return set(dialog) ? accept_button(dialog) : nullptr; };
    const auto edit_literal = [&](const QString& target, const QString& literal, bool integer_list) {
        auto* entry = binding_button(inspector, target);
        if (!entry) { return QStringLiteral("Missing binding button: ") + target; }
        return exercise_dialog(inspector, *entry, {set, [&](QDialog& dialog) -> QPushButton* {
            auto* input = qobject_cast<QInputDialog*>(&dialog);
            if (!input || (integer_list && input->objectName() != QStringLiteral("integerListEditor"))) { return nullptr; }
            input->setTextValue(literal);
            return accept_button(dialog);
        }, finish});
    };
    for (const auto& [target, literal] : {std::pair{QStringLiteral("indices"), QStringLiteral("[0,99,1,2,3]")},
              std::pair{QStringLiteral("skip_out_of_range"), QStringLiteral("1")},
              std::pair{QStringLiteral("format"), QStringLiteral("<{value}>")}}) {
        const auto edited = edit_literal(target, literal, target != QStringLiteral("format"));
        QVERIFY2(edited.isEmpty(), qPrintable(edited));
        QCoreApplication::processEvents();
    }
    const auto reordered = exercise_dialog(inspector, *binding_button(inspector, QStringLiteral("values")), {
        [](QDialog& dialog) -> QPushButton* {
            auto* sources = dialog.findChild<QListWidget*>(QStringLiteral("bindingSources"));
            if (!sources || sources->count() != 2) { return nullptr; }
            sources->setCurrentRow(1);
            return dialog.findChild<QPushButton*>(QStringLiteral("earlierBindingSource"));
        }, finish});
    QVERIFY2(reordered.isEmpty(), qPrintable(reordered));
    QVERIFY(scene(window).reveal_node(consumer));
    QTRY_VERIFY(binding_button(inspector, QStringLiteral("left")));
    const auto dynamic = exercise_dialog(inspector, *binding_button(inspector, QStringLiteral("left")), {
        set, [](QDialog& dialog) -> QPushButton* {
            auto* kind = dialog.findChild<QComboBox*>(QStringLiteral("choice"));
            return kind && select_choice(*kind, QStringLiteral("Node output")) ? accept_button(dialog) : nullptr;
        }, [](QDialog& dialog) -> QPushButton* {
            auto* path = dialog.findChild<QLineEdit*>(QStringLiteral("bindingSelector"));
            auto* source = dialog.findChild<QComboBox*>(QStringLiteral("bindingSource"));
            if (!path || !source || path->text() != QStringLiteral("0") || !source->currentText().contains(QStringLiteral("used_values"))) { return nullptr; }
            return dialog.findChild<QPushButton*>(QStringLiteral("chooseIndexSource"));
        }, [](QDialog& dialog) -> QPushButton* {
            auto* source = dialog.findChild<QComboBox*>(QStringLiteral("bindingSource"));
            return source && select_choice(*source, QStringLiteral("Index ["), true) ? accept_button(dialog) : nullptr;
        }, [](QDialog& dialog) -> QPushButton* {
            auto* path = dialog.findChild<QLineEdit*>(QStringLiteral("bindingSelector"));
            return path && path->text() == QStringLiteral("@1") ? accept_button(dialog) : nullptr;
        }, finish});
    QVERIFY2(dynamic.isEmpty(), qPrintable(dynamic));

    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const auto directory = std::filesystem::path(scratch.path().toStdWString()) / L"edited";
    window.transfer_document(directory, infrastructure::SolutionTransfer::SaveAs);
    QTRY_VERIFY(!window.file_busy());
    EditorWindow reopened(canvas_example(), catalog);
    reopened.show();
    reopened.open_document(directory);
    QTRY_VERIFY(!reopened.file_busy() && reopened.draft().id == draft.id);
    const auto* stored = find_node(reopened.draft().workflows.front(), consumer);
    QVERIFY(stored);
    const OutputReference expected{flow_id, formatter, output, {DynamicElementSelection{IndexOutputReference{flow_id, index, value, {}}}}};
    QVERIFY(stored->bindings.front().sources == std::vector<BindingSource>{expected});
    QVERIFY(scene(reopened).reveal_node(consumer));
    QTRY_VERIFY(binding_button(properties(reopened), QStringLiteral("left")) &&
        binding_button(properties(reopened), QStringLiteral("left"))->toolTip().contains(QStringLiteral("Index [")));

    auto* controller = reopened.findChild<RunController*>();
    reopened.findChild<QAction*>(QStringLiteral("runWorkflow"))->trigger();
    QTRY_VERIFY(controller->runs().size() == 1 && controller->can_start());
    const auto& handle = controller->runs().back();
    const auto progress = take(handle.inspect());
    QVERIFY2(progress.status == ExecutionStatus::Succeeded, progress.diagnostics.empty() ? "" : progress.diagnostics.front().message.c_str());
    const auto formatted = result_for(handle, formatter);
    QCOMPARE(*std::get<DataValue>(formatted.output(key<OutputId>("text"))->content).as_string(), std::string("<6>,<9>,<20>,<10>"));
    const auto result = result_for(handle, consumer);
    QCOMPARE(*std::get<DataValue>(result.output(value)->content).as_int64(), 9);
    auto* tree = reopened.findChild<QTreeWidget*>(QStringLiteral("runNodes"));
    QTRY_COMPARE(tree->topLevelItemCount(), 3);
    for (int row = 0; row < tree->topLevelItemCount(); ++row) {
        auto* item = tree->topLevelItem(row);
        if (item->toolTip(0) == QString::fromStdString(consumer.to_string())) { tree->setCurrentItem(item); }
    }
    auto* details = reopened.findChild<QPlainTextEdit*>(QStringLiteral("runDetails"));
    QVERIFY(details->toPlainText().contains(QStringLiteral("Index at selector step 1 = 1")));
}

void InspectorTests::resets_only_the_selected_session_accumulator()
{
    const auto catalog = take(app::make_tool_registry());
    const ToolConfiguration accumulator{key<ToolTypeId>("visionpro.calc.int64_accumulate"), 1};
    const auto flow_id = take(IdFactory::generate<WorkflowId>());
    const auto local = take(IdFactory::generate<NodeId>());
    const auto selected = take(IdFactory::generate<NodeId>());
    const auto other = take(IdFactory::generate<NodeId>());
    const auto value = key<InputId>("value");
    const auto output = key<OutputId>("value");
    const auto mode = key<ParameterId>("mode");
    const auto initial = key<ParameterId>("initial");
    const auto* descriptor = catalog.find_descriptor(accumulator.type);
    QVERIFY(descriptor);
    const auto parameter = std::ranges::find(descriptor->parameters, mode, &ParameterDescriptor::id);
    QVERIFY(parameter != descriptor->parameters.end());
    const auto session = take(ConfigValue::enumeration(parameter->type, key<EnumMemberId>("Session")));
    Workflow flow{flow_id, "Counters", {key<SchemaId>("test.accumulators"), 1},
        {{local, "Per run", accumulator, {{value, {ConfigValue::integer(1)}}, {initial, {ConfigValue::integer(10)}}}},
         {selected, "First session", accumulator,
            {{value, {ConfigValue::integer(1)}}, {initial, {ConfigValue::integer(20)}}, {mode, {session}}}},
         {other, "Second session", accumulator,
            {{value, {ConfigValue::integer(1)}}, {initial, {ConfigValue::integer(30)}}, {mode, {session}}}}},
        {{take(IdFactory::generate<ControlLinkId>()), local, {}, selected},
         {take(IdFactory::generate<ControlLinkId>()), selected, {}, other}}, {}, {local}};
    Solution draft{take(IdFactory::generate<SolutionId>()), "Accumulators", infrastructure::solution_format_version, 1, {flow}};
    EditorWindow window(std::move(draft), catalog);
    window.show();
    auto* run = window.findChild<QAction*>(QStringLiteral("runWorkflow"));
    auto* reset = window.findChild<QAction*>(QStringLiteral("resetSelectedState"));
    auto* controller = window.findChild<RunController*>();
    const auto verify_run = [&](std::size_t index, int first_expected, int other_expected) {
        run->trigger();
        QTRY_VERIFY(controller->runs().size() == index + 1 && controller->can_start());
        const auto& handle = controller->runs().at(index);
        const auto progress = take(handle.inspect());
        QVERIFY2(progress.status == ExecutionStatus::Succeeded, progress.diagnostics.empty() ? "" : progress.diagnostics.front().message.c_str());
        const auto total = [&](const NodeId& node) {
            const auto result = result_for(handle, node);
            return *std::get<DataValue>(result.output(output)->content).as_int64();
        };
        QCOMPARE(total(local), 11);
        QCOMPARE(total(selected), first_expected);
        QCOMPARE(total(other), other_expected);
    };
    QVERIFY(scene(window).reveal_node(selected));
    QVERIFY(!reset->isEnabled());
    verify_run(0, 21, 31);
    verify_run(1, 22, 32);
    QVERIFY(scene(window).reveal_node(local));
    QVERIFY(!reset->isEnabled());
    QVERIFY(scene(window).reveal_node(selected));
    QTRY_VERIFY(reset->isEnabled());
    auto& inspector = properties(window);
    auto rename_selected = [&]() -> QPushButton* {
        auto* rename = inspector.findChild<QPushButton*>(QStringLiteral("renameNode"));
        return rename && rename->text() == QStringLiteral("First session") ? rename : nullptr;
    };
    QTRY_VERIFY(rename_selected());
    const auto renamed = exercise_dialog(inspector, *rename_selected(), {[](QDialog& dialog) -> QPushButton* {
        auto* input = qobject_cast<QInputDialog*>(&dialog);
        if (!input) { return nullptr; }
        input->setTextValue(QStringLiteral("Renamed accumulator"));
        return accept_button(dialog);
    }});
    QVERIFY2(renamed.isEmpty(), qPrintable(renamed));
    verify_run(2, 23, 33);
    QVERIFY(scene(window).reveal_node(selected));
    QTRY_VERIFY(reset->isEnabled());
    const auto undo_index = scene(window).undoStack().index();
    reset->trigger();
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Reset session state for Renamed accumulator")));
    QCOMPARE(scene(window).undoStack().index(), undo_index);
    verify_run(3, 21, 34);
    const auto prior = result_for(controller->runs().at(2), selected);
    QCOMPARE(*std::get<DataValue>(prior.output(output)->content).as_int64(), 23);
}

void InspectorTests::edits_loop_settings_and_collects_a_selected_field()
{
    const auto draft = canvas_example();
    const auto tools = registry();
    ControlGraphModel model(draft, draft.workflows.front().id, tools);
    ControlScene scene(model);
    EditorInspector inspector(model, scene, tools);
    inspector.show();
    QVERIFY(scene.add_group("Count objects", {}, ControlScene::GroupKind::Loop));
    const auto loop_id = scene.selected_nodes().at(0);
    QVERIFY(scene.open_group(model.canvas_id(loop_id)));
    QVERIFY(scene.add_tool(key<ToolTypeId>("visionpro.sim.add"), {}));
    const auto child = scene.selected_nodes().at(0);
    QVERIFY(group_config(model, loop_id).entries == std::vector{child});
    QVERIFY(scene.parent_scope());
    QVERIFY(scene.reveal_node(loop_id));
    QTRY_VERIFY(inspector.findChild<QPushButton*>(QStringLiteral("editLoopSettings")));
    QVERIFY(binding_button(inspector, QStringLiteral("start")) && binding_button(inspector, QStringLiteral("end")));
    QVERIFY(!binding_button(inspector, QStringLiteral("index")));
    const auto settings = exercise_dialog(inspector, *inspector.findChild<QPushButton*>(QStringLiteral("editLoopSettings")), {
        [](QDialog& dialog) -> QPushButton* {
            auto* step = dialog.findChild<QLineEdit*>(QStringLiteral("loopStep"));
            auto* end = dialog.findChild<QCheckBox*>(QStringLiteral("loopInclusiveEnd"));
            auto* interval = dialog.findChild<QLineEdit*>(QStringLiteral("loopInterval"));
            auto* stop = dialog.findChild<QComboBox*>(QStringLiteral("loopBreakSource"));
            if (!step || !end || !interval || !stop || stop->count() != 2) { return nullptr; }
            step->setText(QStringLiteral("-2"));
            end->setChecked(true);
            interval->setText(QStringLiteral("17"));
            stop->setCurrentIndex(1);
            return accept_button(dialog);
        }});
    QVERIFY2(settings.isEmpty(), qPrintable(settings));
    const auto edited = *group_config(model, loop_id).loop;
    QVERIFY(edited.step == -2 && edited.inclusive_end && edited.interval_ms == 17 && edited.break_source == child);
    QCoreApplication::processEvents();
    auto* kind = inspector.findChild<QComboBox*>(QStringLiteral("publicPortKind"));
    QVERIFY(kind && select_choice(*kind, QStringLiteral("Output")));
    const auto add = exercise_dialog(inspector, *inspector.findChild<QPushButton*>(QStringLiteral("addPublicPort")), {
        [](QDialog& dialog) -> QPushButton* {
            auto* id = dialog.findChild<QLineEdit*>(QStringLiteral("publicPortId"));
            if (!id) { return nullptr; }
            id->setText(QStringLiteral("areas"));
            return accept_button(dialog);
        },
        [](QDialog& dialog) -> QPushButton* {
            auto* source = dialog.findChild<QComboBox*>(QStringLiteral("bindingSource"));
            auto* selector = dialog.findChild<QLineEdit*>(QStringLiteral("bindingSelector"));
            if (!source || !selector || !select_choice(*source, QStringLiteral("Objects"), true)) { return nullptr; }
            selector->setText(QStringLiteral("0/area_pixels"));
            return accept_button(dialog);
        },
        [](QDialog& dialog) -> QPushButton* {
            auto* mode = dialog.findChild<QComboBox*>(QStringLiteral("choice"));
            return mode && select_choice(*mode, QStringLiteral("Collect")) ? accept_button(dialog) : nullptr;
        }});
    QVERIFY2(add.isEmpty(), qPrintable(add));
    const auto& result = group_config(model, loop_id);
    QCOMPARE(result.interface.outputs.size(), 1u);
    QVERIFY(result.interface.outputs.front().type == DataType::list(DataType::integer()));
    QVERIFY(result.loop->exports.front().mode == LoopExportMode::Collect);
    const OutputReference expected{draft.workflows.front().id, child, key<OutputId>("objects"),
        {ElementSelection{0}, FieldSelection{key<FieldId>("area_pixels")}}};
    QVERIFY(result.outputs.front().source == expected);
    scene.undoStack().undo();
    QVERIFY(group_config(model, loop_id).loop->exports.empty() && group_config(model, loop_id).interface.outputs.empty());
}

void InspectorTests::commits_binding_and_group_outlet_through_dialogs()
{
    const auto draft = canvas_example();
    const auto& flow = draft.workflows.front();
    const auto producer = node_named(flow, "Reference").id;
    const auto consumer = node_named(flow, "Finish").id;
    const auto group = node_named(flow, "Review").id;
    const auto internal = node_named(flow, "Review check").id;
    QVERIFY(std::ranges::none_of(flow.control_links, [&](const auto& link) {
        return link.source == producer && link.target == consumer;
    }));
    const auto tools = registry();
    ControlGraphModel model(draft, flow.id, tools);
    ControlScene scene(model);
    EditorInspector inspector(model, scene, tools);
    inspector.resize(660, 760);
    inspector.show();
    QVERIFY(QTest::qWaitForWindowExposed(&inspector));
    const QSignalSpy failures(&scene, &ControlScene::edit_failed);
    const auto target = key<InputId>("left");
    const auto output = key<OutputId>("objects");
    const auto selected_area = key<FieldId>("area_pixels");
    const auto approved = key<ControlOutputId>("approved");
    auto& history = scene.undoStack();

    QVERIFY(scene.reveal_node(consumer));
    QTRY_VERIFY(binding_button(inspector, QStringLiteral("left")));
    const auto choose_objects = [](QDialog& dialog, const QString& selector) -> QPushButton* {
        auto* source = dialog.findChild<QComboBox*>(QStringLiteral("bindingSource"));
        auto* path = dialog.findChild<QLineEdit*>(QStringLiteral("bindingSelector"));
        if (!source || !path) { return nullptr; }
        for (int index = 0; index < source->count(); ++index) {
            const auto item = source->itemText(index);
            if (!item.startsWith(QStringLiteral("Reference [")) || !item.contains(QStringLiteral("Objects"))) { continue; }
            source->setCurrentIndex(index);
            if (!path->isEnabled()) { return nullptr; }
            path->setText(selector);
            return accept_button(dialog);
        }
        return nullptr;
    };
    bool reported_bad_field = false;
    const auto binding_result = exercise_dialog(inspector, *binding_button(inspector, QStringLiteral("left")), {
        [](QDialog& dialog) { return dialog.findChild<QPushButton*>(QStringLiteral("setBindingSource")); },
        [](QDialog& dialog) -> QPushButton* {
            auto* choices = dialog.findChild<QComboBox*>(QStringLiteral("choice"));
            return choices && select_choice(*choices, QStringLiteral("Node output")) ? accept_button(dialog) : nullptr;
        },
        [&](QDialog& dialog) { return choose_objects(dialog, QStringLiteral("0/unknown_field")); },
        [&](QDialog& dialog) -> QPushButton* {
            auto* warning = qobject_cast<QMessageBox*>(&dialog);
            if (!warning) { return nullptr; }
            reported_bad_field = warning->text().contains(QStringLiteral("field"), Qt::CaseInsensitive);
            return qobject_cast<QPushButton*>(warning->button(QMessageBox::Ok));
        },
        [&](QDialog& dialog) { return choose_objects(dialog, QStringLiteral("0/area_pixels")); },
        [](QDialog& dialog) -> QPushButton* {
            return dialog.findChild<QPushButton*>(QStringLiteral("setBindingSource")) ? accept_button(dialog) : nullptr;
        }});
    QVERIFY2(binding_result.isEmpty(), qPrintable(binding_result));
    QVERIFY(reported_bad_field);
    const OutputReference expected{flow.id, producer, output, {ElementSelection{0}, FieldSelection{selected_area}}};
    const auto* binding = binding_for(model, consumer, target);
    QVERIFY(binding && binding->sources.size() == 1);
    const auto* reference = std::get_if<OutputReference>(&binding->sources.front());
    QVERIFY(reference && *reference == expected);
    QCOMPARE(history.count(), 1);

    QVERIFY(scene.reveal_node(group));
    QTRY_VERIFY(inspector.findChild<QPushButton*>(QStringLiteral("addPublicPort")));
    auto* kinds = inspector.findChild<QComboBox*>(QStringLiteral("publicPortKind"));
    QVERIFY(kinds && select_choice(*kinds, QStringLiteral("Control outlet")));
    const auto port_result = exercise_dialog(inspector, *inspector.findChild<QPushButton*>(QStringLiteral("addPublicPort")), {
        [](QDialog& dialog) -> QPushButton* {
            auto* id = dialog.findChild<QLineEdit*>(QStringLiteral("publicPortId"));
            auto* name = dialog.findChild<QLineEdit*>(QStringLiteral("publicPortName"));
            if (!id || !name) { return nullptr; }
            id->setText(QStringLiteral("approved"));
            name->setText(QStringLiteral("Approved"));
            return accept_button(dialog);
        },
        [](QDialog& dialog) -> QPushButton* {
            auto* choices = dialog.findChild<QComboBox*>(QStringLiteral("choice"));
            return choices && select_choice(*choices, QStringLiteral("Review check ["), true)
                ? accept_button(dialog) : nullptr;
        }});
    QVERIFY2(port_result.isEmpty(), qPrintable(port_result));
    const auto& edited_group = group_config(model, group);
    const auto mapping = std::ranges::find(edited_group.control_exports, approved, &ControlOutputMapping::output);
    QVERIFY(mapping != edited_group.control_exports.end() && mapping->source == internal && !mapping->source_output);
    QCOMPARE(edited_group.interface.control_outputs.size(), std::size_t{2});
    QCOMPARE(model.nodeData(model.canvas_id(group), QtNodes::NodeRole::OutPortCount).toUInt(), 3u);
    const auto shows_approved = [&] {
        const auto* ports = inspector.findChild<QListWidget*>(QStringLiteral("publicPorts"));
        return ports && !ports->findItems(QStringLiteral("Approved [approved]"), Qt::MatchContains).empty();
    };
    QTRY_VERIFY(shows_approved());
    QCOMPARE(history.count(), 2);
    QCOMPARE(failures.count(), 0);
    QCOMPARE(model.draft().workflows.front().control_links.size(), flow.control_links.size());
    const auto diagnostics = take(diagnose_solution(model.draft(), tools.descriptors()));
    QVERIFY2(diagnostics.empty(), diagnostics.empty() ? "" : diagnostics.front().message.c_str());

    history.undo();
    QCOMPARE(group_config(model, group).control_exports.size(), std::size_t{1});
    binding = binding_for(model, consumer, target);
    QVERIFY(binding && binding->sources == std::vector<BindingSource>{expected});
    history.undo();
    binding = binding_for(model, consumer, target);
    QVERIFY(binding && binding->sources == std::vector<BindingSource>{ConfigValue::integer(1)});
    QVERIFY(history.isClean());
    history.redo();
    history.redo();
    QCOMPARE(group_config(model, group).control_exports.size(), std::size_t{2});
    binding = binding_for(model, consumer, target);
    QVERIFY(binding && binding->sources == std::vector<BindingSource>{expected});
}

} // namespace
} // namespace visionpro::ui

QTEST_MAIN(visionpro::ui::InspectorTests)
#include "inspector_tests.moc"
