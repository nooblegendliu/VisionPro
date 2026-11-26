#include "canvas_example.hpp"
#include "control_scene.hpp"
#include "editor_inspector.hpp"
#include "editor_window.hpp"
#include "image_archive_dialogs.hpp"
#include "image_source_fields.hpp"
#include "image_source_fields.hpp"
#include "run_controller.hpp"
#include "tool_catalog.hpp"

#include <simulation.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/camera.hpp>
#include <visionpro/infrastructure/camera_trigger.hpp>
#include <visionpro/infrastructure/image_archive.hpp>
#include <visionpro/workflow/validation.hpp>

#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGraphicsView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>

#include <algorithm>
#include <array>
#include <functional>
#include <initializer_list>
#include <limits>
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

template<class Widget> Widget* constant_editor(const QWidget& inspector, const QString& target)
{
    for (auto* field : inspector.findChildren<Widget*>(QStringLiteral("inlineConstant"))) {
        if (field->property("bindingTarget").toString() == target && field->isVisible()) { return field; }
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

QString exercise_dialog(QWidget& inspector, QPushButton& start, std::initializer_list<DialogStep> steps,
    Qt::Key final_key = Qt::Key_unknown)
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
            if (next == steps.end() && final_key != Qt::Key_unknown) {
                QTimer::singleShot(0, dialog, [dialog, final_key] {
                    QTest::keyClick(dialog->focusWidget() ? dialog->focusWidget() : dialog, final_key);
                });
            } else {
                QTimer::singleShot(0, button, [button] { QTest::mouseClick(button, Qt::LeftButton); });
            }
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
    void edits_threshold_inline_then_binds_output();
    void preserves_full_width_values_and_defaults();
    void locates_diagnostic_field_data();
    void locates_diagnostic_field();
    void locates_workflow_input_from_inside_group();
    void commits_binding_and_group_outlet_through_dialogs();
    void edits_loop_settings_and_collects_a_selected_field();
    void edits_and_reloads_dynamic_indices_and_format_options();
    void resets_only_the_selected_session_accumulator();
    void edits_shared_targets_and_previews_without_consuming();
    void edits_and_reloads_workflow_admission();
    void binds_camera_trigger_and_frame_as_one_edit();
    void edits_connects_and_reopens_error_policy();
    void configures_image_output_and_selects_upstream_marks();
    void browses_and_extracts_image_package();
};

ControlScene& scene(EditorWindow& window)
{
    return *qobject_cast<ControlScene*>(window.findChild<QGraphicsView*>(QStringLiteral("workflowCanvas"))->scene());
}

EditorInspector& properties(EditorWindow& window)
{
    window.findChild<QAction*>(QStringLiteral("showProperties"))->trigger();
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("propertiesDialog"));
    if (dialog && dialog->isVisible() && !dialog->isModal()) {
        for (auto* widget : dialog->findChildren<QWidget*>()) {
            if (auto* inspector = dynamic_cast<EditorInspector*>(widget)) { return *inspector; }
        }
    }
    throw std::runtime_error("The modeless properties dialog must contain the active inspector.");
}

NodeResult result_for(const RunHandle& run, const NodeId& node)
{
    const auto progress = take(run.inspect());
    const auto found = std::ranges::find(progress.nodes, node, &RunNodeProgress::node);
    if (found == progress.nodes.end() || !found->invocation) { throw std::runtime_error("Missing invocation"); }
    return take(run.lookup(*found->invocation)).value();
}

void InspectorTests::configures_image_output_and_selects_upstream_marks()
{
    const auto tools = take(app::make_tool_registry());
    const auto type = key<ToolTypeId>("visionpro.image.output");
    if (!tools.find_descriptor(type)) { QSKIP("The output image renderer requires OpenCV."); }
    auto draft = canvas_example();
    const auto integer_trigger = take(IdFactory::generate<VariableId>());
    const auto string_trigger = take(IdFactory::generate<VariableId>());
    draft.variables.push_back({integer_trigger,"计数判定",DataType::integer(),ConfigValue::integer(1)});
    draft.variables.push_back({string_trigger,"文字判定",DataType::string(),take(ConfigValue::string("NG"))});
    auto& flow = draft.workflows.front();
    auto target = std::ranges::find(flow.nodes,std::string("Merge"),&Node::name);
    const auto sink = target->id;
    target->configuration = ToolConfiguration{type,1};
    target->bindings.clear();
    const auto edge = std::ranges::find(flow.control_links,sink,&ControlLink::target);
    QVERIFY(edge != flow.control_links.end());
    const auto blob = std::ranges::find(flow.nodes,edge->source,&Node::id);
    blob->configuration = ToolConfiguration{key<ToolTypeId>("visionpro.image.blob"),1};
    ControlGraphModel model(draft,flow.id,tools);
    ControlScene canvas(model);
    EditorInspector inspector(model,canvas,tools);
    inspector.resize(470,600);
    inspector.show();
    inspector.pin(NodeTarget{flow.id,sink});
    auto* tabs = inspector.findChild<QTabBar*>("moduleConfigTabs");
    QVERIFY(tabs);
    QTRY_COMPARE(tabs->tabText(2),QStringLiteral("渲染设置"));
    QCOMPARE(tabs->tabText(1),QStringLiteral("渲染参数"));
    const auto control = [&](const char* id) -> QCheckBox* {
        for (auto* box : inspector.findChildren<QCheckBox*>(QLatin1String(id))) {
            if (box->isVisible()) { return box; }
        }
        return nullptr;
    };
    QTRY_VERIFY(control("output_save_enabled"));
    QVERIFY(!control("output_save_enabled")->isChecked());
    QVERIFY(!control("output_save_original"));
    control("output_save_enabled")->click();
    QTRY_VERIFY(control("output_save_original"));
    control("output_save_original")->click();
    QTRY_VERIFY(inspector.findChild<QPushButton*>("chooseImageOutputDirectory"));
    QTRY_VERIFY(control("output_ftp_enabled"));
    control("output_ftp_enabled")->click();
    const auto button = [&](const char* name) -> QPushButton* {
        for (auto* item : inspector.findChildren<QPushButton*>(QLatin1String(name))) {
            if (item->isVisible()) { return item; }
        }
        return nullptr;
    };
    QTRY_VERIFY(button("outputFtpCheck") && button("outputFtpPassword"));
    bool masked = false;
    QTimer::singleShot(0, [&] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            masked = dialog->textEchoMode() == QLineEdit::Password;
            dialog->reject();
        }
    });
    button("outputFtpPassword")->click();
    QVERIFY(masked);
    button("outputFtpCheck")->click();
    QTRY_VERIFY(button("outputFtpCheck")->isEnabled());
    auto* ftp_status = inspector.findChild<QLabel*>("outputFtpStatus");
    QVERIFY(ftp_status && ftp_status->text().contains(QStringLiteral("Invalid FTP")));
    QTRY_VERIFY(control("output_trigger_enabled"));
    control("output_trigger_enabled")->click();
    const auto target_node = [&] { return find_node(model.draft().workflows.front(),sink); };
    const auto trigger_type = [&] {
        for (auto* item : inspector.findChildren<QComboBox*>("output_trigger_source_type")) { if (item->isVisible()) { return item; } }
        return static_cast<QComboBox*>(nullptr);
    };
    QTRY_VERIFY(trigger_type());
    trigger_type()->setCurrentIndex(trigger_type()->findData("Integer"));
    QTRY_VERIFY(source_enum(*target_node(),"trigger_source_type","Judgement") == "Integer");
    QTRY_VERIFY(binding_button(inspector,"trigger") && binding_button(inspector,"trigger")->toolTip().contains("Int64"));
    QTRY_VERIFY(trigger_type() && trigger_type()->currentData() == "Integer");
    trigger_type()->setCurrentIndex(trigger_type()->findData("String"));
    QTRY_VERIFY(binding_button(inspector,"trigger") && binding_button(inspector,"trigger")->toolTip().contains("String"));
    const auto quick_source = [&] {
        for (auto* item : inspector.findChildren<QComboBox*>("outputTriggerSource")) { if (item->isVisible()) { return item; } }
        return static_cast<QComboBox*>(nullptr);
    };
    for (const auto& [name, variable] : {std::pair{QStringLiteral("计数判定"),integer_trigger},
        std::pair{QStringLiteral("文字判定"),string_trigger}}) {
        QTRY_VERIFY(quick_source() && quick_source()->findText(name,Qt::MatchContains)>0);
        auto* choices = quick_source();
        const int index = choices->findText(name,Qt::MatchContains);
        choices->setCurrentIndex(index);
        choices->activated(index);
        const auto* assignment = binding_for(model,sink,key<InputId>("trigger"));
        QVERIFY(assignment && std::get<GlobalReference>(assignment->sources.front()).variable == variable);
        QCOMPARE(source_enum(*target_node(),"trigger_source_type","Judgement"), variable == integer_trigger ? "Integer" : "String");
        QApplication::processEvents();
    }
    const auto has_ordinary_directory = [&] {
        for (auto* item : inspector.findChildren<QPushButton*>("chooseImageOutputDirectory")) {
            if (item->isVisible() && item->property("slot").toString() == "original_path") { return true; }
        }
        return false;
    };
    QTRY_VERIFY(has_ordinary_directory());
    tabs->setCurrentIndex(2);
    QTRY_VERIFY(button("outputMultilineText"));
    QTimer::singleShot(0,[] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            dialog->setTextValue(QStringLiteral("第一行\n第二行"));
            dialog->accept();
        }
    });
    button("outputMultilineText")->click();
    QVERIFY(source_value(*target_node(),"text") && *source_value(*target_node(),"text")->as_string() == "第一行\n第二行");
    const QString prefix = QStringLiteral("layer.") + QString::fromStdString(blob->id.to_string()) + ".components.";
    auto mark = [&] {
        for (auto* item : inspector.findChildren<QCheckBox*>(prefix + "centroids")) { if (item->isVisible()) { return item; } }
        return static_cast<QCheckBox*>(nullptr);
    };
    QTRY_VERIFY(mark());
    mark()->click();
    const auto* binding = binding_for(model,sink,key<InputId>("centroids"));
    QVERIFY(binding && binding->sources.size() == 1);
    QVERIFY(binding->cardinality == BindingCardinality::MultipleSources);
    QCOMPARE(std::get<OutputReference>(binding->sources.front()).node, blob->id);
    const auto style_button = [&] {
        for (auto* item : inspector.findChildren<QPushButton*>(prefix+"centroids.style")) { if (item->isVisible()) { return item; } }
        return static_cast<QPushButton*>(nullptr);
    };
    QTRY_VERIFY(style_button());
    QTimer::singleShot(0,[] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            dialog->findChild<QLineEdit*>("layerOkColor")->setText("#00ff00");
            dialog->findChild<QLineEdit*>("layerNgColor")->setText("#ff0000");
            dialog->findChild<QSpinBox*>("layerOpacity")->setValue(46);
            dialog->findChild<QSpinBox*>("layerThickness")->setValue(3);
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        }
    });
    style_button()->click();
    QCOMPARE(target_node()->display_styles.size(),1U);
    const auto style = target_node()->display_styles.front();
    QCOMPARE(style.source.node,blob->id);
    QVERIFY(style.style.ok_color == "#00ff00" && style.style.ng_color == "#ff0000" && style.style.opacity == 46 && style.style.thickness == 3);
    canvas.undoStack().undo();
    QVERIFY(target_node()->display_styles.empty());
    canvas.undoStack().undo();
    QVERIFY(!binding_for(model,sink,key<InputId>("centroids")));
}

void InspectorTests::browses_and_extracts_image_package()
{
    using namespace infrastructure;
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto native = std::filesystem::path(root.path().toStdU16String());
    ImageArchive archive(ArchivePackagePolicy{native,1});
    QByteArray encoded;
    QBuffer buffer(&encoded);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QImage source(9,5,QImage::Format_RGB32);
    source.fill(Qt::red);
    QVERIFY(source.save(&buffer,"PNG"));
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.constData());
    ToolInvocationIdentity call{take(IdFactory::generate<SolutionSessionId>()),take(RuntimeIdFactory::from_value<ConfigSnapshotId>(1)),
        {take(RuntimeIdFactory::from_value<WorkflowRunId>(1)),take(IdFactory::generate<NodeId>()),
            take(RuntimeIdFactory::from_value<ExecutionScopeId>(1)),take(RuntimeIdFactory::from_value<InvocationId>(1))},
        take(RuntimeIdFactory::from_value<AttemptId>(0))};
    auto receipt = archive.save(call,{},{{ArchiveKind::Rendered,native,"工件",ArchiveSuffix::Sequence,"png",
        std::vector<std::uint8_t>(bytes,bytes+encoded.size())}});
    QVERIFY2(!receipt.failure,receipt.failure ? receipt.failure->message.c_str() : "");
    take(archive.flush());
    QVERIFY(receipt.images.size() == 1 && receipt.images.front().package);
    const auto path = QString::fromStdU16String(receipt.images.front().package->u16string());
    std::unique_ptr<QDialog> dialog(open_image_packages(nullptr));
    auto* tree = dialog->findChild<QTreeWidget*>("packageEntries");
    auto* extract = dialog->findChild<QPushButton*>("extractImagePackages");
    QVERIFY(tree && extract);
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    QDragEnterEvent enter(QPoint(5,5),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(dialog.get(),&enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop(QPointF(5,5),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(dialog.get(),&drop);
    QTRY_COMPARE(tree->topLevelItemCount(),1);
    QCOMPARE(tree->topLevelItem(0)->childCount(),1);
    QTRY_VERIFY(std::ranges::any_of(dialog->findChildren<QLabel*>(),[](QLabel* label){ return !label->pixmap().isNull(); }));
    const auto saved = QDir(root.path()).filePath(QFileInfo(path).completeBaseName() + "/" + tree->topLevelItem(0)->child(0)->text(0));
    QTRY_VERIFY(QFileInfo::exists(saved));
    const QImage recovered(saved);
    QCOMPARE(recovered.pixelColor(0,0),QColor(Qt::red));
    call.invocation.invocation = take(RuntimeIdFactory::from_value<InvocationId>(2));
    receipt = archive.save(call,{},{{ArchiveKind::Original,native,"第二件",ArchiveSuffix::Sequence,"png",
        std::vector<std::uint8_t>(bytes,bytes+encoded.size())}});
    QVERIFY(!receipt.failure);
    take(archive.flush());
    mime.setUrls({QUrl::fromLocalFile(root.path())});
    QDragEnterEvent batch_enter(QPoint(5,5),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(dialog.get(),&batch_enter);
    QDropEvent batch_drop(QPointF(5,5),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(dialog.get(),&batch_drop);
    QTRY_COMPARE(tree->topLevelItemCount(),2);
    dialog->findChild<QCheckBox*>()->setChecked(false);
    extract->click();
    QTRY_COMPARE(QDir(root.path()).entryList({"*.png"}, QDir::Files).size(),2);
}

void InspectorTests::locates_diagnostic_field_data()
{
    QTest::addColumn<bool>("parameter");
    QTest::newRow("Enter on missing input") << false;
    QTest::newRow("double-click on invalid parameter") << true;
}

void InspectorTests::edits_threshold_inline_then_binds_output()
{
    auto tools = take(app::make_tool_registry());
    const auto type = key<ToolTypeId>("visionpro.image.threshold");
    if (!tools.find_descriptor(type)) { QSKIP("The threshold tool requires the OpenCV configuration."); }
    auto draft = canvas_example();
    auto& flow = draft.workflows.front();
    const auto producer = node_named(flow, "Reference").id;
    const auto merge = std::ranges::find(flow.nodes, std::string{"Merge"}, &Node::name);
    const auto target = key<ParameterId>("threshold");
    const auto consumer = merge->id;
    merge->configuration = ToolConfiguration{type, 1};
    merge->bindings = {{target, {ConfigValue::integer(128)}}};
    ControlGraphModel model(draft, flow.id, tools);
    ControlScene canvas(model);
    EditorInspector inspector(model, canvas, tools);
    inspector.resize(400, 400);
    inspector.show();
    QVERIFY(canvas.reveal_node(consumer));
    const auto spin = [&] { return constant_editor<QSpinBox>(inspector, QStringLiteral("threshold")); };
    const auto source = [&]() -> const BindingSource& {
        const auto* node = find_node(model.draft().workflows.front(), consumer);
        return std::ranges::find(node->bindings, BindingTarget{target}, &Binding::target)->sources.front();
    };
    QTRY_VERIFY(spin() && spin()->value() == 128);
    QCOMPARE(spin()->minimum(), 0);
    QCOMPARE(spin()->maximum(), 255);
    spin()->setFocus();
    spin()->selectAll();
    QTest::keyClicks(spin(), "150");
    QTest::keyClick(spin(), Qt::Key_Return);
    QCOMPARE(*std::get<ConfigValue>(source()).as_int64(), 150);
    QCOMPARE(canvas.undoStack().count(), 1);
    canvas.undoStack().undo();
    QCOMPARE(*std::get<ConfigValue>(source()).as_int64(), 128);
    QTRY_COMPARE(spin()->value(), 128);
    auto* edit = binding_button(inspector, QStringLiteral("threshold"));
    QVERIFY(edit && edit->text() == QStringLiteral("绑定来源…"));
    const auto changed = exercise_dialog(inspector, *edit, {
        [](QDialog& dialog) { return dialog.findChild<QPushButton*>(QStringLiteral("setBindingSource")); },
        [](QDialog& dialog) -> QPushButton* {
            auto* choices = dialog.findChild<QComboBox*>(QStringLiteral("choice"));
            return choices && select_choice(*choices, QStringLiteral("节点输出")) ? accept_button(dialog) : nullptr;
        },
        [](QDialog& dialog) -> QPushButton* {
            auto* choices = dialog.findChild<QComboBox*>(QStringLiteral("bindingSource"));
            return choices && select_choice(*choices, QStringLiteral("Reference ["), true) ? accept_button(dialog) : nullptr;
        },
        accept_button});
    QVERIFY2(changed.isEmpty(), qPrintable(changed));
    const auto* output = std::get_if<OutputReference>(&source());
    QVERIFY(output && output->node == producer && std::get<OutputId>(output->output) == key<OutputId>("value"));
    QTRY_VERIFY(!spin());
    auto* locate = inspector.findChild<QPushButton*>(QStringLiteral("locateBindingSource"));
    QVERIFY(locate);
    locate->click();
    QVERIFY(canvas.selected_nodes() == std::vector<NodeId>{producer});
}

void InspectorTests::preserves_full_width_values_and_defaults()
{
    auto entries = tools::simulation_registrations();
    auto& tool = std::ranges::find_if(entries, [](const auto& item) {
        return item.descriptor.id.text() == "visionpro.sim.add";
    })->descriptor;
    const auto fixed = [&](std::string_view name, const ConfigValue& initial) -> ParameterDescriptor& {
        ParameterDescriptor parameter{key<ParameterId>(name), std::string(name), initial.type(), false, initial};
        parameter.binding.time = BindingTime::SnapshotBuild;
        tool.parameters.push_back(std::move(parameter));
        return tool.parameters.back();
    };
    fixed("gain", take(ConfigValue::real(0.25))).constraints.real_range = RealRange{-1, 1};
    fixed("label", take(ConfigValue::string("sample"))).constraints.max_length = 12;
    fixed("enabled", ConfigValue::boolean(true));
    const auto mode = take(DataType::enumeration(key<SchemaId>("test.mode"), 1,
        {{key<EnumMemberId>("First"), "First option"}, {key<EnumMemberId>("Last"), "Last option"}}));
    fixed("mode", take(ConfigValue::enumeration(mode, key<EnumMemberId>("First"))));
    const auto tools = take(ToolRegistry::create(entries));
    const auto draft = canvas_example();
    const auto node_id = node_named(draft.workflows.front(), "Reference").id;
    ControlGraphModel model(draft, draft.workflows.front().id, tools);
    ControlScene canvas(model);
    EditorInspector inspector(model, canvas, tools);
    inspector.resize(440, 640);
    inspector.show();
    QVERIFY(canvas.reveal_node(node_id));
    const auto write = [&](const QString& name, const char* text) {
        QLineEdit* field = nullptr;
        QTRY_VERIFY((field = constant_editor<QLineEdit>(inspector, name)));
        field->setFocus();
        field->selectAll();
        QTest::keyClicks(field, text);
        QTest::keyClick(field, Qt::Key_Return);
        QCoreApplication::processEvents();
    };
    const auto field_value = [&](const BindingTarget& target) -> const ConfigValue* {
        const auto& bindings = find_node(model.draft().workflows.front(), node_id)->bindings;
        const auto found = std::ranges::find(bindings, target, &Binding::target);
        return found == bindings.end() ? nullptr : std::get_if<ConfigValue>(&found->sources.front());
    };
    const auto gain = key<ParameterId>("gain"), label = key<ParameterId>("label");
    write(QStringLiteral("gain"), "2");
    QVERIFY(!field_value(gain) && canvas.undoStack().isClean());
    auto* invalid = constant_editor<QLineEdit>(inspector, QStringLiteral("gain"));
    auto* feedback = invalid->parentWidget()->findChild<QLabel*>(QStringLiteral("constantError"));
    QVERIFY(feedback && feedback->isVisible() && !feedback->text().isEmpty());
    // Moving the node changes the undo stack without rebuilding its binding fields.
    QVERIFY(canvas.move_nodes(std::array{CanvasMove{model.canvas_id(node_id), QPointF(310, 310)}}));
    write(QStringLiteral("gain"), "0.12500000000000003");
    QVERIFY(field_value(gain) && *field_value(gain)->as_float64() == 0.12500000000000003);
    write(QStringLiteral("left"), "9223372036854775807");
    QVERIFY(*field_value(key<InputId>("left"))->as_int64() == std::numeric_limits<std::int64_t>::max());
    write(QStringLiteral("label"), "updated");
    QVERIFY(field_value(label) && *field_value(label)->as_string() == "updated");
    for (const auto& name : {QStringLiteral("enabled"), QStringLiteral("mode")}) {
        auto* combo = constant_editor<QComboBox>(inspector, name);
        QVERIFY(combo);
        combo->setFocus();
        QTest::keyClick(combo, name == QStringLiteral("mode") ? Qt::Key_End : Qt::Key_Home);
        QCoreApplication::processEvents();
    }
    QVERIFY(field_value(key<ParameterId>("enabled")) && !*field_value(key<ParameterId>("enabled"))->as_bool());
    QVERIFY(field_value(key<ParameterId>("mode")) && *field_value(key<ParameterId>("mode"))->as_enum() == key<EnumMemberId>("Last"));
    QCOMPARE(canvas.undoStack().count(), 6);
    // A stale editingFinished from the pre-undo form must not replace the redo entry.
    auto* pending = constant_editor<QLineEdit>(inspector, QStringLiteral("label"));
    QVERIFY(pending);
    pending->selectAll();
    QTest::keyClicks(pending, "discard");
    canvas.undoStack().undo();
    QTest::keyClick(pending, Qt::Key_Return);
    QVERIFY(canvas.undoStack().canRedo() && *field_value(label)->as_string() == "updated");
    while (canvas.undoStack().canUndo()) { canvas.undoStack().undo(); }
    QVERIFY(!field_value(gain) && !field_value(label) && !field_value(key<ParameterId>("enabled"))
        && !field_value(key<ParameterId>("mode")));
}

void InspectorTests::locates_diagnostic_field()
{
    QFETCH(bool, parameter);
    auto draft = canvas_example();
    auto& nodes = draft.workflows.front().nodes;
    auto child = std::ranges::find(nodes, std::string{"Prepare"}, &Node::name);
    QVERIFY(child != nodes.end());
    child->configuration = ToolConfiguration{key<ToolTypeId>("visionpro.calc.int64"), 1};
    const auto field = parameter ? QStringLiteral("operation") : QStringLiteral("left");
    if (parameter) {
        child->bindings.push_back({key<ParameterId>("operation"), {ConfigValue::integer(99)}});
    } else {
        std::erase_if(child->bindings, [](const Binding& item) { return item.target == BindingTarget{key<InputId>("left")}; });
    }
    const auto node = child->id;
    EditorWindow window(std::move(draft), take(app::make_tool_registry()));
    window.show();
    auto* list = window.findChild<QListWidget*>(QStringLiteral("editorDiagnostics"));
    auto* opener = window.findChild<QToolButton*>(QStringLiteral("openDiagnostics"));
    QVERIFY(list && opener);
    QTRY_COMPARE(list->count(), 1);
    QVERIFY2(list->item(0)->text().contains(field), qPrintable(list->item(0)->text()));
    opener->setFocus();
    QTest::keyClick(opener, Qt::Key_Space);
    QTRY_VERIFY(list->hasFocus());
    if (parameter) {
        const auto point = list->visualItemRect(list->currentItem()).center();
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseDClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    } else {
        QTest::keyClick(list, Qt::Key_Return);
    }
    QPointer<QDialog> properties = window.findChild<QDialog*>(QStringLiteral("propertiesDialog"));
    QTRY_VERIFY(properties && properties->isVisible() && !properties->isModal());
    QVERIFY(scene(window).selected_nodes() == std::vector<NodeId>{node});
    if (!parameter) {
        auto* input = constant_editor<QLineEdit>(*properties, field);
        QVERIFY(input);
        QTRY_VERIFY(input->hasFocus());
        auto* viewport = properties->findChild<QScrollArea*>()->viewport();
        QVERIFY(viewport->rect().contains(input->mapTo(viewport, input->rect().center())));
        QTest::keyClicks(input, "7");
        bool unexpected_modal = false;
        QTimer guard;
        guard.setSingleShot(true);
        connect(&guard, &QTimer::timeout, [&] {
            if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                unexpected_modal = true;
                modal->reject();
            }
        });
        guard.start(0);
        QTest::keyClick(input, Qt::Key_Return);
        guard.stop();
        QVERIFY(!unexpected_modal);
        QTRY_COMPARE(list->count(), 0);
        QVERIFY(properties && properties->isVisible());
        return;
    }
    auto* button = binding_button(*properties, field);
    QVERIFY(button);
    QTRY_VERIFY(button->hasFocus());
    auto* viewport = properties->findChild<QScrollArea*>()->viewport();
    QVERIFY(viewport->rect().contains(button->mapTo(viewport, button->rect().center())));
    bool opened = false;
    QTimer::singleShot(0, button, [&] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            opened = dialog->findChild<QPushButton*>(QStringLiteral("setBindingSource"));
            QTest::keyClick(dialog, Qt::Key_Escape);
        }
    });
    QTest::keyClick(button, Qt::Key_Space);
    QVERIFY(opened);
    QVERIFY(properties && properties->isVisible());
    QVERIFY(!QApplication::activeModalWidget());
}

void InspectorTests::locates_workflow_input_from_inside_group()
{
    auto draft = canvas_example();
    auto& flow = draft.workflows.front();
    const auto frame = key<InputId>("frame");
    flow.interface.inputs = {{frame, "Frame", DataType::integer()},
        {key<InputId>("batch"), "Batch", DataType::integer()}};
    flow.trigger = WorkflowTrigger{TriggerMode::External, key<SchemaId>("test.external"), frame};
    const auto child = node_named(flow, "Prepare").id;
    EditorWindow window(std::move(draft), registry());
    window.show();
    QVERIFY(scene(window).reveal_node(child));
    auto* list = window.findChild<QListWidget*>(QStringLiteral("editorDiagnostics"));
    auto* opener = window.findChild<QToolButton*>(QStringLiteral("openDiagnostics"));
    QVERIFY(list && opener);
    QTRY_COMPARE(list->count(), 1);
    QVERIFY2(list->item(0)->text().contains(QStringLiteral("batch")), qPrintable(list->item(0)->text()));
    QTest::keyClick(opener, Qt::Key_Space);
    QTRY_VERIFY(list->hasFocus());
    QTest::keyClick(list, Qt::Key_Return);
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("propertiesDialog"));
    QTRY_VERIFY(dialog && dialog->isVisible() && !dialog->isModal());
    auto* ports = dialog->findChild<QListWidget*>(QStringLiteral("publicPorts"));
    QVERIFY(ports);
    QTRY_VERIFY(ports->hasFocus());
    QCOMPARE(ports->currentItem()->data(Qt::UserRole).toString(), QStringLiteral("batch"));
    QVERIFY(scene(window).selected_nodes().empty());
}

void InspectorTests::binds_camera_trigger_and_frame_as_one_edit()
{
    const auto catalog = take(app::make_tool_registry());
    const auto saved = take(infrastructure::load_solution(VISIONPRO_CAMERA_DIRECTORY, catalog));
    const auto& original = saved.solution().workflows.front();
    const auto id = original.nodes.front().id;
    EditorWindow window(saved.solution(), catalog);
    window.show();
    auto& canvas = scene(window);
    QVERIFY(canvas.reveal_node(id));
    auto& inspector = properties(window);
    auto* tabs = inspector.findChild<QTabBar*>("moduleConfigTabs");
    QVERIFY(tabs && tabs->isVisible());
    QCOMPARE(tabs->tabText(1), QStringLiteral("相机接入"));
    auto* devices = inspector.findChild<QComboBox*>("selectNodeResource");
    auto* manager = inspector.findChild<QToolButton*>("openSourceCameraManager");
    QVERIFY(devices && devices->isVisible() && devices->currentIndex() > 0);
    QVERIFY(manager && manager->isVisible() && !manager->icon().isNull());
    QCOMPARE(devices->itemText(0), QStringLiteral("未关联"));
    auto* scroll = inspector.findChild<QScrollArea*>();
    QTRY_COMPARE(scroll->horizontalScrollBar()->maximum(), 0);
    QVERIFY(scroll->viewport()->rect().contains(manager->mapTo(scroll->viewport(), manager->rect().bottomRight())));
    QVERIFY(!inspector.findChild<QComboBox*>("source_camera_input")->isVisible());
    tabs->setCurrentIndex(1);
    QPushButton* entry{};
    QTRY_VERIFY((entry = inspector.findChild<QPushButton*>("configureSourceCameraTrigger")) && entry->isVisible());
    const auto baseline = canvas.undoStack().count();
    const auto clicked = exercise_dialog(inspector, *entry, {[](QDialog& dialog) -> QPushButton* {
        auto* camera = dialog.findChild<QComboBox*>("triggerCameraDevice");
        auto* frame = dialog.findChild<QComboBox*>("sourceTriggerFrameInput");
        auto* image = dialog.findChild<QComboBox*>("triggerCameraImage");
        if (!camera || !frame || !image || !camera->currentText().contains("SIM-001")
            || frame->currentData() != "frame" || image->currentIndex() < 0) { return nullptr; }
        return accept_button(dialog);
    }});
    QVERIFY2(clicked.isEmpty(), qPrintable(clicked));
    const auto& flow = window.draft().workflows.front();
    QVERIFY(flow.trigger && flow.trigger->camera && flow.trigger->camera->device == saved.solution().resources.front().id);
    QCOMPARE(flow.interface.inputs.size(), 2u);
    QCOMPARE(flow.interface.version, original.interface.version + 1);
    const auto input = key<InputId>("image");
    const auto* bound = binding_for(static_cast<const ControlGraphModel&>(canvas.graphModel()), id, input);
    QVERIFY(bound && bound->sources.size() == 1);
    const auto* scoped = std::get_if<ScopedInputReference>(&bound->sources.front());
    QVERIFY(scoped && scoped->workflow == flow.id && scoped->input == flow.trigger->camera->image_input);
    const auto& source = flow.nodes.front();
    const auto mode = std::ranges::find(source.bindings, BindingTarget{key<ParameterId>("camera_input")}, &Binding::target);
    QVERIFY(mode != source.bindings.end() && std::get<ConfigValue>(mode->sources.front()).as_enum()->text() == "Delivered");
    {
        std::shared_ptr<WorkflowSession> session = take(WorkflowSession::create(catalog));
        take(session->activate(window.draft()));
        auto control = take(TriggerControl::start(session, flow.id));
        auto camera = take(infrastructure::CameraTriggerStream::open(window.draft(), flow.id, control));
        const auto received = take(camera->next());
        const auto* run = std::get_if<RunHandle>(&received.submission);
        QVERIFY(run);
        const auto deadline = TriggerControl::Clock::now() + std::chrono::seconds(5);
        const auto complete = take(run->wait(RunWaitTarget::Quiescence, deadline));
        QCOMPARE(complete.progress.status, ExecutionStatus::Succeeded);
        const auto result = result_for(*run, id);
        const auto* pixels = result.output(key<OutputId>("image"));
        QVERIFY(pixels && std::holds_alternative<DataValue>(pixels->content));
        const auto* delivered = run->request().input(flow.trigger->camera->image_input);
        QVERIFY(delivered && std::get<DataValue>(pixels->content).as_image()->frame == delivered->value.as_image()->frame);
        QCOMPARE(camera->statistics().accepted, 1u);
        take(camera->close());
        take(control->stop(TriggerStopMode::Drain, deadline));
    }
    QCOMPARE(canvas.undoStack().count(), baseline + 1);
    canvas.undoStack().undo();
    QVERIFY(!window.draft().workflows.front().trigger && window.draft().workflows.front().interface.inputs.empty());
    canvas.undoStack().redo();
    QVERIFY(window.draft().workflows.front().trigger->camera);
    QTRY_VERIFY(inspector.findChild<QLabel*>("sourceCameraTriggerSummary"));

    // A flow may change its camera independently after the node starts using delivered frames.
    auto delivered = window.draft();
    auto& delivered_flow = delivered.workflows.front();
    auto replacement = delivered.resources.front();
    replacement.id = take(IdFactory::generate<ResourceId>());
    replacement.name = "流程触发相机 B";
    std::get<DeviceResource>(replacement.location).configuration = take(infrastructure::camera_device_config("SIM-002"));
    delivered.resources.push_back(replacement);
    delivered_flow.trigger->camera->device = replacement.id;
    EditorWindow linked_window(delivered, catalog);
    linked_window.show();
    QVERIFY(scene(linked_window).reveal_node(id));
    auto& linked_panel = properties(linked_window);
    auto* origin = linked_panel.findChild<QLabel*>("sourceDeliveredCamera");
    QVERIFY(origin && origin->text().contains(QStringLiteral("流程触发相机 B")));
    QVERIFY(!linked_panel.findChild<QComboBox*>("selectNodeResource"));
    linked_panel.findChild<QTabBar*>("moduleConfigTabs")->setCurrentIndex(1);
    auto* linked_edit = linked_panel.findChild<QPushButton*>("configureSourceCameraTrigger");
    QVERIFY(linked_edit && linked_edit->isEnabled());
    QVERIFY(exercise_dialog(linked_panel, *linked_edit, {[](QDialog& dialog) { return accept_button(dialog); }}).isEmpty());
    QVERIFY(linked_window.draft().workflows.front().nodes.front().resources.front().resource.id == saved.solution().resources.front().id);

    auto& public_inputs = delivered_flow.interface.inputs;
    const auto other_input = key<InputId>("other_image");
    public_inputs.push_back({other_input, "SDK 图像", DataType::image()});
    auto& bindings = delivered_flow.nodes.front().bindings;
    const auto image_binding = std::ranges::find(bindings, BindingTarget{input}, &Binding::target);
    image_binding->sources = {ScopedInputReference{delivered_flow.id, {}, other_input, {}}};
    EditorWindow input_window(std::move(delivered), catalog);
    input_window.show();
    QVERIFY(scene(input_window).reveal_node(id));
    auto& input_panel = properties(input_window);
    auto* generic = input_panel.findChild<QLabel*>("sourceDeliveredCamera");
    QVERIFY(generic && generic->text() == QStringLiteral("由本轮图像输入提供"));
    QVERIFY(!input_panel.findChild<QComboBox*>("selectNodeResource"));
    input_panel.findChild<QTabBar*>("moduleConfigTabs")->setCurrentIndex(1);
    auto* intake = input_panel.findChild<QComboBox*>("source_camera_input");
    const auto acquire = intake->findData("Acquire");
    QVERIFY(acquire >= 0);
    intake->setCurrentIndex(acquire);
    emit intake->activated(acquire);
    QTRY_VERIFY(input_panel.findChild<QComboBox*>("selectNodeResource"));
    QCOMPARE(input_panel.findChild<QComboBox*>("selectNodeResource")->currentData().toString(),
        QString::fromStdString(saved.solution().resources.front().id.to_string()));

    auto automatic = saved.solution();
    auto& other = automatic.workflows.front();
    const auto count = key<InputId>("tick");
    other.interface.inputs.push_back({count, "Clock", DataType::integer()});
    other.trigger = WorkflowTrigger{TriggerMode::Continuous, key<SchemaId>("other.clock"), count, 100};
    EditorWindow protected_window(std::move(automatic), catalog);
    protected_window.show();
    QVERIFY(scene(protected_window).reveal_node(id));
    auto& protected_panel = properties(protected_window);
    protected_panel.findChild<QTabBar*>("moduleConfigTabs")->setCurrentIndex(1);
    QTRY_VERIFY((entry = protected_panel.findChild<QPushButton*>("configureSourceCameraTrigger")) && !entry->isEnabled());
    QVERIFY(protected_window.draft().workflows.front().trigger->source == key<SchemaId>("other.clock"));
    QVERIFY(protected_panel.findChild<QPushButton*>("openSourceWorkflowTrigger")->isEnabled());
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

void InspectorTests::edits_connects_and_reopens_error_policy()
{
    const auto catalog = take(app::make_tool_registry());
    const auto source = take(IdFactory::generate<NodeId>());
    const auto handler = take(IdFactory::generate<NodeId>());
    const auto add = ToolConfiguration{key<ToolTypeId>("visionpro.sim.add"), 1};
    const auto left = key<InputId>("left"), right = key<InputId>("right");
    Workflow flow{take(IdFactory::generate<WorkflowId>()), "Recover", {key<SchemaId>("test.recovery"), 1},
        {{source, "Overflow", add, {{left, {ConfigValue::integer(std::numeric_limits<std::int64_t>::max())}},
            {right, {ConfigValue::integer(1)}}}},
         {handler, "Handle overflow", add, {{left, {ConfigValue::integer(40)}}, {right, {ConfigValue::integer(2)}}}}},
        {}, {}, {source}};
    const Solution draft{take(IdFactory::generate<SolutionId>()), "Recovery policy", 1, 1, {flow}};
    EditorWindow window(draft, catalog);
    window.show();
    auto& canvas = scene(window);
    QVERIFY(canvas.reveal_node(source));
    auto& inspector = properties(window);
    auto* tabs = inspector.findChild<QTabBar*>(QStringLiteral("moduleConfigTabs"));
    QVERIFY(tabs && tabs->isVisible());
    tabs->setCurrentIndex(1);
    QPushButton* edit{};
    QTRY_VERIFY((edit = inspector.findChild<QPushButton*>(QStringLiteral("editNodeExecutionPolicy"))) && edit->isVisible());
    const auto changed = exercise_dialog(inspector, *edit, {[](QDialog& dialog) -> QPushButton* {
        auto* timeout = dialog.findChild<QLineEdit*>(QStringLiteral("nodeTimeoutMs"));
        auto* retry = dialog.findChild<QLineEdit*>(QStringLiteral("nodeMaxRetries"));
        auto* outlet = dialog.findChild<QLineEdit*>(QStringLiteral("nodeErrorOutlet"));
        if (!timeout || !retry || !retry->isEnabled() || !outlet) { return nullptr; }
        timeout->setText(QStringLiteral("5000"));
        retry->setText(QStringLiteral("1"));
        outlet->setText(QStringLiteral("fault"));
        return accept_button(dialog);
    }});
    QVERIFY2(changed.isEmpty(), qPrintable(changed));
    const NodeExecutionPolicy expected{5000, 1, key<ControlOutputId>("fault")};
    QVERIFY(find_node(window.draft().workflows.front(), source)->execution_policy == expected);
    const auto& model = static_cast<const ControlGraphModel&>(canvas.graphModel());
    const auto from = model.canvas_id(source);
    QCOMPARE(model.portData(from, QtNodes::PortType::Out, 1, QtNodes::PortRole::Caption).toString(), QStringLiteral("Error (fault)"));
    QVERIFY(canvas.connect_control({from, 1, model.canvas_id(handler), 0}));
    QCOMPARE(window.draft().workflows.front().control_links.front().output, expected.error_output);
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const auto directory = std::filesystem::path(scratch.path().toStdWString()) / L"error policy";
    window.transfer_document(directory, infrastructure::SolutionTransfer::SaveAs);
    QTRY_VERIFY(!window.file_busy() && std::filesystem::exists(directory / "solution.json"));
    EditorWindow reopened(canvas_example(), catalog);
    reopened.show();
    reopened.open_document(directory);
    QTRY_VERIFY(!reopened.file_busy() && reopened.draft().id == draft.id);
    const auto& saved = reopened.draft().workflows.front();
    QVERIFY(find_node(saved, source)->execution_policy == expected);
    QCOMPARE(saved.control_links.front().output, expected.error_output);
    auto* controller = reopened.findChild<RunController*>();
    reopened.findChild<QAction*>(QStringLiteral("runWorkflow"))->trigger();
    QTRY_VERIFY(controller->can_start() && controller->runs().size() == 1);
    const auto& run = controller->runs().front();
    const auto progress = take(run.inspect());
    QCOMPARE(progress.status, ExecutionStatus::Succeeded);
    const auto failed = std::ranges::find(progress.nodes, source, &RunNodeProgress::node);
    QVERIFY(failed != progress.nodes.end() && failed->error_handled && failed->attempts.size() == 2);
    QCOMPARE(failed->status, ExecutionStatus::Failed);
    const auto recovered = result_for(run, handler);
    const auto value = *std::get<DataValue>(recovered.output(key<OutputId>("value"))->content).as_int64();
    QCOMPARE(value, 42);

    QProcess cli;
    cli.start(QStringLiteral(VISIONPRO_TEST_CLI), {QStringLiteral("run"), QStringLiteral("--solution"),
        QString::fromStdU16String((directory / "solution.json").u16string()), QStringLiteral("--workflow"),
        QString::fromStdString(flow.id.to_string())});
    QVERIFY(cli.waitForFinished(10000));
    const auto errors = cli.readAllStandardError();
    QVERIFY2(cli.exitStatus() == QProcess::NormalExit && cli.exitCode() == 0, errors.constData());
    const auto report = QJsonDocument::fromJson(cli.readAllStandardOutput()).object();
    QCOMPARE(report.value("status").toString(), QStringLiteral("Succeeded"));
    const auto nodes = report.value("nodes").toArray();
    QCOMPARE(nodes.size(), 2);
    const auto cli_node = [&](const NodeId& id) {
        for (const auto& node : nodes) {
            const auto object = node.toObject();
            if (object.value("id").toString() == QString::fromStdString(id.to_string())) { return object; }
        }
        return QJsonObject{};
    };
    const auto cli_failure = cli_node(source);
    QCOMPARE(cli_failure.value("status").toString(), QStringLiteral("Failed"));
    QCOMPARE(cli_failure.value("error_handled").toBool(), failed->error_handled);
    const auto attempts = cli_failure.value("attempts").toArray();
    QCOMPARE(attempts.size(), static_cast<qsizetype>(failed->attempts.size()));
    for (qsizetype i = 0; i < attempts.size(); ++i) {
        const auto& desktop = failed->attempts[static_cast<std::size_t>(i)];
        const auto attempt = attempts.at(i).toObject();
        QCOMPARE(attempt.value("attempt").toString(), QString::number(desktop.attempt.value()));
        QCOMPARE(desktop.status, ExecutionStatus::Failed);
        QCOMPARE(attempt.value("status").toString(), QStringLiteral("Failed"));
    }
    const auto outputs = cli_node(handler).value("outputs").toArray();
    QCOMPARE(outputs.size(), 1);
    QCOMPARE(outputs.at(0).toObject().value("id").toString(), QStringLiteral("value"));
    QCOMPARE(outputs.at(0).toObject().value("value").toString(), QString::number(value));
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
        inspector.refresh();
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
    properties(window);
    QTRY_VERIFY(binding_button(inspector, QStringLiteral("left")));
    const auto dynamic = exercise_dialog(inspector, *binding_button(inspector, QStringLiteral("left")), {
        set, [](QDialog& dialog) -> QPushButton* {
            auto* kind = dialog.findChild<QComboBox*>(QStringLiteral("choice"));
            return kind && select_choice(*kind, QStringLiteral("节点输出")) ? accept_button(dialog) : nullptr;
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
    reopened.findChild<QDockWidget*>(QStringLiteral("runDock"))->toggleViewAction()->trigger();
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
    }}, Qt::Key_Return);
    QVERIFY2(renamed.isEmpty(), qPrintable(renamed));
    QVERIFY(inspector.isVisible() && !QApplication::activeModalWidget());
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
            return choices && select_choice(*choices, QStringLiteral("节点输出")) ? accept_button(dialog) : nullptr;
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
