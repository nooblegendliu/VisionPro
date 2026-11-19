#include "editor_inspector.hpp"
#include "binary_editor.hpp"
#include "endpoint_editor.hpp"
#include "camera_editor.hpp"
#include "class_palette.hpp"
#include <visionpro/core/class_map.hpp>
#include "coordinate_text.hpp"
#include "control_scene.hpp"
#include "roi_editor.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/validation.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <charconv>
#include <functional>
#include <exception>
#include <limits>
#include <memory>

namespace visionpro::ui {
namespace {

using namespace core;
using namespace workflow;
using Check = Result<std::monostate>;
using Descriptor = std::variant<InputDescriptor, ParameterDescriptor>;
using PortId = std::variant<InputId, OutputId, ControlOutputId>;

QString qtext(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

template<SchemaIdentity Id>
QString id_text(const Id& id) { return qtext(id.text()); }

template<UuidIdentity Id>
QString short_id(const Id& id) { return qtext(id.to_string()).left(8); }

std::string utf8(const QString& text)
{
    const auto bytes = text.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

Diagnostic error(ErrorCode code, std::string message)
{
    return {code, Severity::Error, {}, std::move(message)};
}

void warn(QWidget* parent, const QString& message)
{
    QMessageBox::warning(parent, QStringLiteral("Edit properties"), message);
}

template<class Callback>
void guarded(QWidget* parent, Callback&& callback)
{
    try { std::forward<Callback>(callback)(); }
    catch (const std::exception& failure) { warn(parent, qtext(failure.what())); }
    catch (...) { warn(parent, QStringLiteral("The editor could not complete this operation.")); }
}

template<class Callback>
QPushButton* action(QWidget* owner, QString title, const char* name, Callback callback)
{
    auto* button = new QPushButton(std::move(title), owner);
    button->setObjectName(QString::fromLatin1(name));
    QObject::connect(button, &QPushButton::clicked, owner, [owner, callback = std::move(callback)] {
        guarded(owner, callback);
    });
    return button;
}

QGroupBox* section(QVBoxLayout& parent, const QString& title)
{
    auto* group = new QGroupBox(title);
    parent.addWidget(group);
    return group;
}

void close_buttons(QDialog& dialog, QVBoxLayout& layout)
{
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout.addWidget(buttons);
}

template<class Value>
using Choices = std::vector<std::pair<QString, Value>>;

template<class Value>
std::optional<Value> choose(QWidget* parent, const QString& title, const QString& field,
    const Choices<Value>& choices, std::size_t current = 0)
{
    if (choices.empty()) {
        warn(parent, QStringLiteral("No compatible choice is available in this scope."));
        return std::nullopt;
    }
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* picker = new QComboBox(&dialog);
    picker->setObjectName(QStringLiteral("choice"));
    for (const auto& [label, value] : choices) { picker->addItem(label); }
    picker->setCurrentIndex(static_cast<int>(current));
    form->addRow(field, picker);
    layout->addLayout(form);
    close_buttons(dialog, *layout);
    if (dialog.exec() != QDialog::Accepted || picker->currentIndex() < 0) { return std::nullopt; }
    return std::optional<Value>{std::in_place, choices[static_cast<std::size_t>(picker->currentIndex())].second};
}

Workflow* mutable_flow(Solution& solution, const WorkflowId& id)
{
    const auto found = std::ranges::find(solution.workflows, id, &Workflow::id);
    return found == solution.workflows.end() ? nullptr : &*found;
}

Node* mutable_node(Workflow& flow, const NodeId& id)
{
    const auto found = std::ranges::find(flow.nodes, id, &Node::id);
    return found == flow.nodes.end() ? nullptr : &*found;
}

const GroupConfiguration* group_at(const Workflow& flow, const std::optional<NodeId>& group)
{
    const auto* node = group ? find_node(flow, *group) : nullptr;
    return node ? std::get_if<GroupConfiguration>(&node->configuration) : nullptr;
}

std::optional<NodeId> parent_of(const Workflow& flow, const NodeId& node)
{
    const auto relation = std::ranges::find(flow.containment, node, &Containment::child);
    return relation == flow.containment.end() ? std::nullopt : std::optional<NodeId>{relation->parent};
}

struct Boundary {
    PublicInterface& interface;
    std::vector<OutputMapping>& outputs;
    std::vector<ControlOutputMapping>& controls;
    LoopConfiguration* loop{};
};

std::optional<Boundary> boundary_at(Workflow& flow, const std::optional<NodeId>& group)
{
    if (!group) { return Boundary{flow.interface, flow.outputs, flow.control_exports}; }
    auto* node = mutable_node(flow, *group);
    auto* value = node ? std::get_if<GroupConfiguration>(&node->configuration) : nullptr;
    if (!value) { return std::nullopt; }
    return Boundary{value->interface, value->outputs, value->control_exports, value->loop ? &*value->loop : nullptr};
}

std::vector<NodeId>* entries_at(Workflow& flow, const std::optional<NodeId>& group)
{
    if (!group) { return &flow.entries; }
    auto* node = mutable_node(flow, *group);
    auto* configuration = node ? std::get_if<GroupConfiguration>(&node->configuration) : nullptr;
    return configuration ? &configuration->entries : nullptr;
}

QString type_text(const DataType& type)
{
    switch (type.kind()) {
    case TypeKind::Bool: return QStringLiteral("Bool");
    case TypeKind::Int64: return QStringLiteral("Int64");
    case TypeKind::Float64: return QStringLiteral("Float64");
    case TypeKind::String: return QStringLiteral("String");
    case TypeKind::Binary: return QStringLiteral("Binary");
    case TypeKind::Enum:
    case TypeKind::Record: return type.schema_id() ? id_text(*type.schema_id()) : QStringLiteral("Schema");
    case TypeKind::List: return QStringLiteral("List<%1>").arg(type_text(*type.element_type()));
    case TypeKind::Optional: return QStringLiteral("Optional<%1>").arg(type_text(*type.element_type()));
    case TypeKind::Image: return QStringLiteral("Image");
    case TypeKind::Point2D: return QStringLiteral("Point2D");
    case TypeKind::Line2D: return QStringLiteral("Line2D");
    case TypeKind::Circle2D: return QStringLiteral("Circle2D");
    case TypeKind::Pose2D: return QStringLiteral("Pose2D");
    case TypeKind::Transform2D: return QStringLiteral("Transform2D");
    case TypeKind::Region2D: return QStringLiteral("Region2D");
    case TypeKind::RasterMask: return QStringLiteral("RasterMask");
    case TypeKind::ResourceReference: return QStringLiteral("Resource reference");
    }
    return {};
}

QString constant_text(const ConfigValue& value)
{
    if (const auto* boolean = value.as_bool()) { return *boolean ? QStringLiteral("true") : QStringLiteral("false"); }
    if (const auto* integer = value.as_int64()) { return QString::number(*integer); }
    if (const auto* real = value.as_float64()) { return QString::number(*real, 'g', 17); }
    if (const auto* string = value.as_string()) { return qtext(*string); }
    if (const auto* bytes = value.as_binary()) { return binary_summary(*bytes); }
    if (const auto* member = value.as_enum()) { return id_text(*member); }
    if (const auto* list = value.elements(); list && value.type().element_type()->kind() == TypeKind::String) {
        QJsonArray strings;
        for (const auto& item : *list) { strings.append(qtext(*item.as_string())); }
        return QString::fromUtf8(QJsonDocument(strings).toJson(QJsonDocument::Compact));
    }
    if (const auto* list = value.elements(); list && value.type().element_type()->kind() == TypeKind::Int64) {
        QStringList numbers;
        for (const auto& item : *list) { numbers.append(QString::number(*item.as_int64())); }
        return QStringLiteral("[") + numbers.join(QStringLiteral(", ")) + QStringLiteral("]");
    }
    if (const auto* point = value.as_point2d()) {
        return QStringLiteral("%1, %2 (%3)").arg(QString::number(point->x, 'g', 17),
            QString::number(point->y, 'g', 17), id_text(point->coordinate_frame));
    }
    if (value.type().kind() == TypeKind::Optional) {
        const auto* inner = value.optional_value();
        return inner ? constant_text(*inner) : QStringLiteral("None");
    }
    return type_text(value.type());
}

QString source_text(const Solution& solution, const BindingSource& source);

QString selector_text(const Selector& selector, const Solution* solution = nullptr, std::vector<IndexSource>* indices = nullptr)
{
    QStringList parts;
    for (const auto& step : selector) {
        if (const auto* field = std::get_if<FieldSelection>(&step)) { parts.append(id_text(field->field)); }
        else if (const auto* fixed = std::get_if<ElementSelection>(&step)) { parts.append(QString::number(fixed->index)); }
        else {
            const auto& index = std::get<DynamicElementSelection>(step).source;
            if (indices) {
                indices->push_back(index);
                parts.append(QStringLiteral("@%1").arg(indices->size()));
            } else if (solution) {
                parts.append(QStringLiteral("[%1]").arg(source_text(*solution, index_binding_source(index))));
            }
        }
    }
    return parts.join(u'/');
}

struct Selection {
    DataType type;
    Selector path;
};

Result<Selection> parse_selector(DataType type, const QString& path, std::span<const IndexSource> indices = {})
{
    Selector selected;
    if (path.isEmpty()) { return Selection{std::move(type), std::move(selected)}; }
    for (const auto& part : path.split(u'/')) {
        if (type.kind() == TypeKind::Record) {
            const auto field = std::ranges::find_if(type.fields(), [&](const FieldSchema& member) {
                return id_text(member.id) == part;
            });
            if (field == type.fields().end()) {
                return error(ErrorCode::UnknownField, "The record does not declare that field.");
            }
            selected.emplace_back(FieldSelection{field->id});
            type = DataType{field->type};
        } else if (type.kind() == TypeKind::List) {
            const bool dynamic = part.startsWith(u'@');
            const auto bytes = utf8(dynamic ? part.mid(1) : part);
            std::size_t index = 0;
            const auto [end, status] = std::from_chars(bytes.data(), bytes.data() + bytes.size(), index);
            if (status != std::errc{} || end != bytes.data() + bytes.size()) {
                return error(ErrorCode::InvalidSelector, "A List step requires a zero-based unsigned index.");
            }
            if (dynamic) {
                if (index == 0 || index > indices.size()) { return error(ErrorCode::InvalidSelector, "Choose a source for this index."); }
                selected.emplace_back(DynamicElementSelection{indices[index - 1]});
            } else { selected.emplace_back(ElementSelection{index}); }
            type = DataType{*type.element_type()};
        } else {
            return error(ErrorCode::InvalidSelector, "A selector can continue only through a Record or List.");
        }
    }
    return Selection{std::move(type), std::move(selected)};
}

QString system_text(SystemField field)
{
    switch (field) {
    case SystemField::ExecutionStatus: return QStringLiteral("Execution status");
    case SystemField::Judgement: return QStringLiteral("Judgement");
    case SystemField::Diagnostics: return QStringLiteral("Diagnostics");
    case SystemField::Elapsed: return QStringLiteral("Elapsed time");
    }
    return {};
}

QString output_text(const std::variant<OutputId, SystemField>& output)
{
    if (const auto* id = std::get_if<OutputId>(&output)) { return id_text(*id); }
    return system_text(std::get<SystemField>(output));
}

QString reference_text(const Solution& solution, const QString& prefix, const Selector& selector)
{
    return selector.empty() ? prefix : QStringLiteral("%1 / %2").arg(prefix, selector_text(selector, &solution));
}

QString source_text(const Solution& solution, const BindingSource& source)
{
    return std::visit([&](const auto& value) -> QString {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, ConfigValue>) {
            return constant_text(value);
        } else if constexpr (std::is_same_v<Value, OutputReference>) {
            const auto* flow = find_workflow(solution, value.workflow);
            const auto* producer = flow ? find_node(*flow, value.node) : nullptr;
            const auto name = producer ? qtext(producer->name) : QStringLiteral("Missing node");
            return reference_text(solution, QStringLiteral("%1 [%2] → %3").arg(name, short_id(value.node), output_text(value.output)),
                value.selector);
        } else if constexpr (std::is_same_v<Value, ScopedInputReference>) {
            return reference_text(solution, QStringLiteral("Public input %1").arg(id_text(value.input)), value.selector);
        } else if constexpr (std::is_same_v<Value, ResourceReference>) {
            const auto found = std::ranges::find(solution.resources, value.id, &ResourceDefinition::id);
            const auto name = found == solution.resources.end() ? QStringLiteral("Missing resource") : qtext(found->name);
            return QStringLiteral("%1 [%2]").arg(name, short_id(value.id));
        } else {
            const auto found = std::ranges::find(solution.variables, value.variable, &GlobalVariableDefinition::id);
            const auto name = found == solution.variables.end() ? QStringLiteral("Missing variable") : qtext(found->name);
            return reference_text(solution, QStringLiteral("%1 [%2]").arg(name, short_id(value.variable)), value.selector);
        }
    }, source);
}

const ToolDescriptor* metadata(const ToolRegistry& registry, const Node& node)
{
    const auto* configured = std::get_if<ToolConfiguration>(&node.configuration);
    const auto* tool = configured ? registry.find_descriptor(configured->type) : nullptr;
    return tool && tool->config_version == configured->config_version ? tool : nullptr;
}

struct TypedSource {
    BindingSource source;
    DataType type;
    ValueSemantics semantics;
};

Choices<TypedSource> outputs_in(const Workflow& flow, const ToolRegistry& registry,
    const std::optional<NodeId>& scope, const std::optional<NodeId>& exclude = std::nullopt)
{
    Choices<TypedSource> choices;
    for (const auto& node : flow.nodes) {
        if (node.id == exclude || parent_of(flow, node.id) != scope) { continue; }
        const auto prefix = QStringLiteral("%1 [%2]").arg(qtext(node.name), short_id(node.id));
        std::span<const OutputDescriptor> outputs;
        if (const auto* group = std::get_if<GroupConfiguration>(&node.configuration)) { outputs = group->interface.outputs; }
        else if (const auto* tool = metadata(registry, node)) { outputs = tool->outputs; }
        for (const auto& output : outputs) {
            choices.emplace_back(QStringLiteral("%1 → %2").arg(prefix, qtext(output.display_name)),
                TypedSource{OutputReference{flow.id, node.id, output.id, {}}, output.type, output.semantics});
        }
        for (const auto field : {SystemField::ExecutionStatus, SystemField::Judgement, SystemField::Diagnostics, SystemField::Elapsed}) {
            choices.emplace_back(QStringLiteral("%1 → %2").arg(prefix, system_text(field)),
                TypedSource{OutputReference{flow.id, node.id, field, {}}, std::get<DataType>(system_field_type(field)), {}});
        }
    }
    return choices;
}

BindingSource without_selector(BindingSource source)
{
    std::visit([](auto& value) {
        if constexpr (requires { value.selector; }) { value.selector.clear(); }
    }, source);
    return source;
}

Selector source_selector(const BindingSource& source)
{
    return std::visit([](const auto& value) -> Selector {
        if constexpr (requires { value.selector; }) { return value.selector; }
        return {};
    }, source);
}

template<class Match>
bool reachable_type(const DataType& type, const Match& match)
{
    if (match(type)) { return true; }
    if (type.kind() == TypeKind::List) { return reachable_type(*type.element_type(), match); }
    return type.kind() == TypeKind::Record && std::ranges::any_of(type.fields(), [&](const FieldSchema& field) {
        return reachable_type(field.type, match);
    });
}

using PickIndex = std::function<std::optional<IndexSource>(QWidget*, const IndexSource*)>;

template<class Validate>
std::optional<TypedSource> choose_reference(QWidget* parent, Choices<TypedSource> choices,
    const BindingSource* current, const Validate& validate, const PickIndex& pick_index = {}, const Solution* solution = nullptr)
{
    std::erase_if(choices, [&](const auto& choice) {
        return !reachable_type(choice.second.type, [&](const DataType& type) {
            return !std::holds_alternative<Diagnostic>(validate(source_kind(choice.second.source), type, choice.second.semantics));
        });
    });
    if (choices.empty()) {
        warn(parent, QStringLiteral("No compatible source is available in this scope."));
        return std::nullopt;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Choose source"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* source = new QComboBox(&dialog);
    source->setObjectName(QStringLiteral("bindingSource"));
    for (const auto& [label, typed] : choices) {
        source->addItem(QStringLiteral("%1 • %2").arg(label, type_text(typed.type)));
    }
    auto* selector = new QLineEdit(&dialog);
    selector->setObjectName(QStringLiteral("bindingSelector"));
    selector->setPlaceholderText(QStringLiteral("Example: 0/area_pixels"));
    selector->setToolTip(pick_index
        ? QStringLiteral("Separate field IDs and zero-based indices with /. Place the cursor on an index to choose its source.")
        : QStringLiteral("Separate exact Record field IDs and zero-based List indices with /."));
    auto* details = new QLabel(&dialog);
    details->setWordWrap(true);
    form->addRow(QStringLiteral("Source"), source);
    form->addRow(QStringLiteral("Selector"), selector);
    std::vector<IndexSource> indices;
    auto* index_labels = new QLabel(&dialog);
    index_labels->setWordWrap(true);
    index_labels->setVisible(false);
    const auto show_indices = [&] {
        QStringList labels;
        if (solution) {
            for (std::size_t i = 0; i < indices.size(); ++i) {
                labels.append(QStringLiteral("@%1 = %2").arg(i + 1).arg(source_text(*solution, index_binding_source(indices[i]))));
            }
        }
        index_labels->setText(labels.join(u'\n'));
        index_labels->setVisible(!labels.empty());
    };
    QPushButton* index_button = nullptr;
    if (pick_index) {
        index_button = action(&dialog, QStringLiteral("Choose index source…"), "chooseIndexSource", [&] {
            auto parts = selector->text().split(u'/');
            const auto position = selector->text().left(selector->cursorPosition()).count(u'/');
            auto& part = parts[position];
            bool ok = false;
            const auto prior = part.startsWith(u'@') ? part.mid(1).toULongLong(&ok) : 0;
            const auto* existing = ok && prior > 0 && prior <= indices.size() ? &indices[prior - 1] : nullptr;
            auto selected = pick_index(&dialog, existing);
            if (!selected) { return; }
            if (existing) {
                indices[prior - 1] = std::move(*selected);
                show_indices();
                return;
            }
            indices.push_back(std::move(*selected));
            const auto token = QStringLiteral("@%1").arg(indices.size());
            (void)part.toULongLong(&ok);
            if (part.isEmpty() || ok || part.startsWith(u'@')) { part = token; }
            else { parts.insert(position + 1, token); }
            selector->setText(parts.join(u'/'));
            show_indices();
        });
        form->addRow(index_button);
    }
    form->addRow(index_labels);
    form->addRow(details);
    layout->addLayout(form);

    const auto show_schema = [&] {
        const auto& type = choices[static_cast<std::size_t>(source->currentIndex())].second.type;
        const bool composite = type.kind() == TypeKind::List || type.kind() == TypeKind::Record;
        selector->setEnabled(composite);
        selector->clear();
        indices.clear();
        show_indices();
        if (index_button) {
            index_button->setEnabled(reachable_type(type, [](const DataType& value) { return value.kind() == TypeKind::List; }));
        }
        if (!composite) { details->clear(); return; }
        const auto& listed = type.kind() == TypeKind::List ? *type.element_type() : type;
        QStringList fields;
        if (listed.kind() == TypeKind::Record) {
            for (const auto& field : listed.fields()) { fields.append(id_text(field.id)); }
        }
        details->setText(fields.isEmpty() ? type_text(type)
            : QStringLiteral("%1\nFields: %2").arg(type_text(type), fields.join(QStringLiteral(", "))));
    };
    QObject::connect(source, &QComboBox::currentIndexChanged, &dialog, [&](int) { guarded(&dialog, show_schema); });
    if (current) {
        const auto base = without_selector(*current);
        const auto found = std::ranges::find_if(choices, [&](const auto& item) { return item.second.source == base; });
        if (found != choices.end()) {
            source->setCurrentIndex(static_cast<int>(found - choices.begin()));
            show_schema();
            selector->setText(selector_text(source_selector(*current), nullptr, &indices));
            show_indices();
        } else { show_schema(); }
    } else { show_schema(); }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    std::optional<TypedSource> result;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        guarded(&dialog, [&] {
            auto selected = choices[static_cast<std::size_t>(source->currentIndex())].second;
            auto parsed = parse_selector(selected.type, selector->text(), pick_index ? std::span<const IndexSource>(indices) : std::span<const IndexSource>{});
            if (const auto* failure = std::get_if<Diagnostic>(&parsed)) { warn(&dialog, qtext(failure->message)); return; }
            auto path = std::get<Selection>(std::move(parsed));
            if (const auto checked = validate(source_kind(selected.source), path.type, selected.semantics);
                const auto* failure = std::get_if<Diagnostic>(&checked)) { warn(&dialog, qtext(failure->message)); return; }
            std::visit([&](auto& value) {
                if constexpr (requires { value.selector; }) { value.selector = std::move(path.path); }
            }, selected.source);
            selected.type = std::move(path.type);
            result = std::move(selected);
            dialog.accept();
        });
    });
    layout->addWidget(buttons);
    dialog.exec();
    return result;
}

std::optional<ConfigValue> constant_result(QWidget* owner, Result<ConfigValue> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) {
        warn(owner, qtext(failure->message));
        return std::nullopt;
    }
    return std::get<ConfigValue>(std::move(result));
}

Choices<CoordinateFrameDefinition> frame_choices(const Solution& solution, const ValueSemantics& semantics)
{
    Choices<CoordinateFrameDefinition> choices;
    for (const auto& frame : solution.coordinates) {
        if (semantics.coordinate_system && semantics.coordinate_system != frame.id) { continue; }
        if (semantics.unit != MeasurementUnit::Unspecified && semantics.unit != frame.unit) { continue; }
        choices.emplace_back(id_text(frame.id), frame);
    }
    return choices;
}

std::optional<ConfigValue> point_constant(QWidget* owner, const Solution& solution,
    const ValueSemantics& semantics, const ConfigValue* prior)
{
    const auto frames = frame_choices(solution, semantics);
    if (frames.empty()) {
        warn(owner, QStringLiteral("The solution has no compatible coordinate frame."));
        return std::nullopt;
    }
    QDialog dialog(owner);
    dialog.setWindowTitle(QStringLiteral("Point constant"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* frame = new QComboBox(&dialog);
    auto* x = new QLineEdit(QStringLiteral("0"), &dialog);
    auto* y = new QLineEdit(QStringLiteral("0"), &dialog);
    frame->setObjectName(QStringLiteral("pointFrame"));
    x->setObjectName(QStringLiteral("pointX"));
    y->setObjectName(QStringLiteral("pointY"));
    const auto* point = prior ? prior->as_point2d() : nullptr;
    for (const auto& [name, value] : frames) {
        frame->addItem(QStringLiteral("%1 (%2)").arg(name, unit_symbol(value.unit)));
        if (point && point->coordinate_frame == value.id) { frame->setCurrentIndex(frame->count() - 1); }
    }
    if (point) {
        x->setText(QString::number(point->x, 'g', 17));
        y->setText(QString::number(point->y, 'g', 17));
    }
    form->addRow(QStringLiteral("Frame"), frame);
    form->addRow(QStringLiteral("X"), x);
    form->addRow(QStringLiteral("Y"), y);
    layout->addLayout(form);
    close_buttons(dialog, *layout);
    if (dialog.exec() != QDialog::Accepted) { return std::nullopt; }
    bool x_ok = false;
    bool y_ok = false;
    const auto x_value = x->text().toDouble(&x_ok);
    const auto y_value = y->text().toDouble(&y_ok);
    if (!x_ok || !y_ok) {
        warn(owner, QStringLiteral("Point coordinates require numeric values."));
        return std::nullopt;
    }
    const auto& selected = frames[static_cast<std::size_t>(frame->currentIndex())].second;
    return constant_result(owner, ConfigValue::point2d({selected.id, selected.unit, x_value, y_value}));
}

std::optional<ConfigValue> region_constant(QWidget* owner, const RoiEditContext& context, const ConfigValue* prior)
{
    auto region = edit_roi(owner, context, prior ? prior->as_region2d() : nullptr);
    return region ? constant_result(owner, ConfigValue::region2d(*region)) : std::nullopt;
}

std::optional<ConfigValue> edit_constant(QWidget* owner, const RoiEditContext& context, const DataType& type,
    const ConfigValue* prior, const QString& title = QStringLiteral("Constant"))
{
    if (prior && prior->type() != type) { prior = nullptr; }
    const auto initial = prior ? constant_text(*prior) : QString{};
    switch (type.kind()) {
    case TypeKind::List: {
        if (type.element_type()->kind() == TypeKind::String) {
            QInputDialog dialog(owner);
            dialog.setObjectName(QStringLiteral("stringListEditor"));
            dialog.setWindowTitle(QStringLiteral("String list"));
            dialog.setLabelText(QStringLiteral("Ordered JSON array of strings, for example [\"width\", \"height\"]"));
            dialog.setTextValue(prior ? constant_text(*prior) : QStringLiteral("[]"));
            while (dialog.exec() == QDialog::Accepted) {
                const auto document = QJsonDocument::fromJson(dialog.textValue().toUtf8());
                const auto array = document.array();
                if (!document.isArray() || !std::all_of(array.constBegin(), array.constEnd(), [](const QJsonValue& item) { return item.isString(); })) {
                    warn(&dialog, QStringLiteral("Enter a JSON array containing only strings."));
                    continue;
                }
                std::vector<ConfigValue> elements;
                for (const auto item : array) {
                    auto value = constant_result(owner, ConfigValue::string(utf8(item.toString())));
                    if (!value) { return {}; }
                    elements.push_back(std::move(*value));
                }
                return constant_result(owner, ConfigValue::list(DataType::string(), elements));
            }
            return {};
        }
        if (type.element_type()->kind() != TypeKind::Int64) {
            warn(owner, QStringLiteral("Choose a typed source for this list."));
            return std::nullopt;
        }
        QInputDialog dialog(owner);
        dialog.setObjectName(QStringLiteral("integerListEditor"));
        dialog.setWindowTitle(QStringLiteral("Integer list"));
        dialog.setLabelText(QStringLiteral("Comma-separated Int64 values in order (empty list: [])"));
        dialog.setTextValue(prior ? constant_text(*prior) : QStringLiteral("[]"));
        while (dialog.exec() == QDialog::Accepted) {
            auto text = dialog.textValue().trimmed();
            if (text.startsWith(u'[') && text.endsWith(u']')) { text = text.mid(1, text.size() - 2).trimmed(); }
            std::vector<ConfigValue> elements;
            bool valid = true;
            if (!text.isEmpty()) {
                for (const auto& token : text.split(u',')) {
                    const auto value = token.trimmed().toLongLong(&valid);
                    if (!valid) { break; }
                    elements.push_back(ConfigValue::integer(value));
                }
            }
            if (valid) { return constant_result(owner, ConfigValue::list(DataType::integer(), elements)); }
            warn(&dialog, QStringLiteral("Each comma-separated item must be a signed 64-bit integer."));
        }
        return std::nullopt;
    }
    case TypeKind::Binary: return edit_binary(owner, prior);
    case TypeKind::Bool: {
        const Choices<bool> values{{QStringLiteral("False"), false}, {QStringLiteral("True"), true}};
        const auto value = choose(owner, QStringLiteral("Constant"), QStringLiteral("Value"), values,
            prior && *prior->as_bool() ? 1 : 0);
        return value ? std::optional<ConfigValue>{ConfigValue::boolean(*value)} : std::nullopt;
    }
    case TypeKind::Enum: {
        Choices<EnumMemberId> values;
        std::size_t current = 0;
        for (const auto& member : type.enum_members()) {
            if (prior && *prior->as_enum() == member.id) { current = values.size(); }
            values.emplace_back(QStringLiteral("%1 [%2]").arg(qtext(member.display_name), id_text(member.id)), member.id);
        }
        auto selected = choose(owner, QStringLiteral("Constant"), QStringLiteral("Member"), values, current);
        return selected ? constant_result(owner, ConfigValue::enumeration(type, *selected)) : std::nullopt;
    }
    case TypeKind::Int64:
    case TypeKind::Float64:
    case TypeKind::String: {
        bool accepted = false;
        const auto value = QInputDialog::getText(owner, title, type_text(type),
            QLineEdit::Normal, initial, &accepted);
        if (!accepted) { return std::nullopt; }
        if (type.kind() == TypeKind::String) { return constant_result(owner, ConfigValue::string(utf8(value))); }
        bool valid = false;
        if (type.kind() == TypeKind::Int64) {
            const auto integer = value.toLongLong(&valid);
            if (valid) { return ConfigValue::integer(integer); }
        } else {
            const auto real = value.toDouble(&valid);
            if (valid) { return constant_result(owner, ConfigValue::real(real)); }
        }
        warn(owner, QStringLiteral("Enter a valid %1 constant.").arg(type_text(type)));
        return std::nullopt;
    }
    case TypeKind::Optional: {
        const Choices<bool> presence{{QStringLiteral("None"), false}, {QStringLiteral("Some"), true}};
        const auto* inner = prior ? prior->optional_value() : nullptr;
        const auto present = choose(owner, QStringLiteral("Optional constant"), QStringLiteral("Presence"), presence,
            inner ? 1 : 0);
        if (!present) { return std::nullopt; }
        if (!*present) { return ConfigValue::none(*type.element_type()); }
        auto value = edit_constant(owner, context, *type.element_type(), inner);
        return value ? std::optional<ConfigValue>{ConfigValue::some(*value)} : std::nullopt;
    }
    case TypeKind::Point2D: return point_constant(owner, context.solution, context.semantics, prior);
    case TypeKind::Region2D: return region_constant(owner, context, prior);
    default:
        warn(owner, QStringLiteral("This type has no constant editor. Choose a typed source or keep the existing value."));
        return std::nullopt;
    }
}

QString kind_text(BindingSourceKind kind)
{
    switch (kind) {
    case BindingSourceKind::Constant: return QStringLiteral("Constant");
    case BindingSourceKind::RunOutput: return QStringLiteral("Node output");
    case BindingSourceKind::PublicInput: return QStringLiteral("Public input");
    case BindingSourceKind::Resource: return QStringLiteral("Resource");
    case BindingSourceKind::GlobalRead: return QStringLiteral("Global variable");
    }
    return {};
}

QString conversion_text(ConversionKind kind)
{
    switch (kind) {
    case ConversionKind::Exact: return QStringLiteral("Exact");
    case ConversionKind::WrapOptional: return QStringLiteral("Wrap in Optional");
    case ConversionKind::Int64ToFloat64Explicit: return QStringLiteral("Int64 to Float64");
    }
    return {};
}

Check check_semantics(const ValueSemantics& source, const ValueSemantics& target)
{
    if ((source.unit != MeasurementUnit::Unspecified && target.unit != MeasurementUnit::Unspecified && source.unit != target.unit)
        || (source.coordinate_system && target.coordinate_system && source.coordinate_system != target.coordinate_system)) {
        return error(ErrorCode::CoordinateMismatch, "Source and target require compatible units and coordinate frames.");
    }
    return std::monostate{};
}

Choices<TypedSource> binding_choices(const Solution& solution, const Workflow& flow, const NodeId& node,
    const ToolRegistry& registry, BindingSourceKind kind, const BindingSource* current)
{
    const auto scope = parent_of(flow, node);
    if (kind == BindingSourceKind::RunOutput) { return outputs_in(flow, registry, scope, node); }
    Choices<TypedSource> choices;
    if (kind == BindingSourceKind::PublicInput) {
        const auto* group = group_at(flow, scope);
        if (scope && !group) { return choices; }
        const auto& boundary = group ? group->interface : flow.interface;
        for (const auto& input : boundary.inputs) {
            choices.emplace_back(QStringLiteral("%1 [%2]").arg(qtext(input.display_name), id_text(input.id)),
                TypedSource{ScopedInputReference{flow.id, scope, input.id, {}}, input.type, input.semantics});
        }
    } else if (kind == BindingSourceKind::Resource) {
        const auto* prior = current ? std::get_if<ResourceReference>(current) : nullptr;
        for (const auto& resource : solution.resources) {
            const auto pinned = prior && resource.id == prior->id ? prior->revision : std::nullopt;
            choices.emplace_back(QStringLiteral("%1 [%2]").arg(qtext(resource.name), short_id(resource.id)),
                TypedSource{ResourceReference{resource.id, resource.kind, pinned}, DataType::resource_reference(), {}});
        }
    } else if (kind == BindingSourceKind::GlobalRead) {
        const auto* prior = current ? std::get_if<GlobalReference>(current) : nullptr;
        for (const auto& variable : solution.variables) {
            const auto pinned = prior && variable.id == prior->variable ? prior->required_revision : std::nullopt;
            choices.emplace_back(QStringLiteral("%1 [%2]").arg(qtext(variable.name), short_id(variable.id)),
                TypedSource{GlobalReference{variable.id, pinned, {}}, variable.type, {}});
        }
    }
    return choices;
}

template<class Target>
std::optional<BindingSource> edit_source(QWidget* owner, const RoiEditContext& context, const Target& target,
    ConversionKind conversion, const BindingSource* current)
{
    Choices<BindingSourceKind> kinds;
    std::size_t previous_kind = 0;
    for (const auto kind : target.binding.allowed_sources) {
        if (target.binding.time == BindingTime::SnapshotBuild && kind != BindingSourceKind::Constant) { continue; }
        if (current && source_kind(*current) == kind) { previous_kind = kinds.size(); }
        kinds.emplace_back(kind_text(kind), kind);
    }
    const auto kind = kinds.size() == 1 ? std::optional{kinds.front().second}
        : choose(owner, QStringLiteral("Binding source"), QStringLiteral("Source kind"), kinds, previous_kind);
    if (!kind) { return std::nullopt; }
    if (*kind != BindingSourceKind::Constant) {
        PickIndex pick_index;
        if (target.binding.allow_dynamic_index && target.binding.time == BindingTime::BeforeInvocation) {
            pick_index = [&](QWidget* picker, const IndexSource* prior) -> std::optional<IndexSource> {
                const auto current_index = prior ? std::optional{index_binding_source(*prior)} : std::nullopt;
                Choices<TypedSource> choices;
                for (const auto allowed : target.binding.allowed_sources) {
                    if (allowed != BindingSourceKind::PublicInput && allowed != BindingSourceKind::RunOutput
                        && allowed != BindingSourceKind::GlobalRead) { continue; }
                    auto candidates = binding_choices(context.solution, *context.workflow, context.node->id,
                        context.registry, allowed, current_index ? &*current_index : nullptr);
                    for (auto& candidate : candidates) {
                        candidate.first = kind_text(allowed) + QStringLiteral(" • ") + candidate.first;
                        choices.push_back(std::move(candidate));
                    }
                }
                auto chosen = choose_reference(picker, std::move(choices), current_index ? &*current_index : nullptr,
                    [](BindingSourceKind, const DataType& type, const ValueSemantics&) -> Check {
                        if (type.kind() != TypeKind::Int64) { return error(ErrorCode::TypeMismatch, "An index source must select an Int64 value."); }
                        return std::monostate{};
                    });
                if (!chosen) { return std::nullopt; }
                auto result = make_index_source(chosen->source);
                if (const auto* issue = std::get_if<Diagnostic>(&result)) { warn(picker, qtext(issue->message)); return std::nullopt; }
                return std::get<IndexSource>(std::move(result));
            };
        }
        auto source = choose_reference(owner, binding_choices(context.solution, *context.workflow, context.node->id,
            context.registry, *kind, current), current,
            [&](BindingSourceKind selected, const DataType& type, const ValueSemantics& semantics) -> Check {
                const auto checked = validate_binding(target, selected, type, conversion);
                if (const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
                return check_semantics(semantics, target.semantics);
            }, pick_index, &context.solution);
        return source ? std::optional<BindingSource>{std::move(source->source)} : std::nullopt;
    }

    const auto& expected = target.binding.cardinality == BindingCardinality::MultipleSources
        ? *target.type.element_type() : target.type;
    auto type = expected;
    if (conversion == ConversionKind::WrapOptional && expected.kind() == TypeKind::Optional) { type = *expected.element_type(); }
    if (conversion == ConversionKind::Int64ToFloat64Explicit) { type = DataType::integer(); }
    if (const auto checked = validate_binding(target, *kind, type, conversion);
        const auto* failure = std::get_if<Diagnostic>(&checked)) {
        warn(owner, qtext(failure->message));
        return std::nullopt;
    }
    const auto* initial = current ? std::get_if<ConfigValue>(current) : nullptr;
    if (!initial && target.default_value) { initial = &*target.default_value; }
    if (!target.constraints.allowed_values.empty() && type == target.type) {
        Choices<ConfigValue> allowed;
        std::size_t index = 0;
        for (const auto& value : target.constraints.allowed_values) {
            if (initial && *initial == value) { index = allowed.size(); }
            allowed.emplace_back(constant_text(value), value);
        }
        auto value = choose(owner, QStringLiteral("Constant"), QStringLiteral("Value"), allowed, index);
        return value ? std::optional<BindingSource>{std::move(*value)} : std::nullopt;
    }
    auto value = edit_constant(owner, context, type, initial);
    return value ? std::optional<BindingSource>{std::move(*value)} : std::nullopt;
}

template<class Target>
Check check_constants(const Target& target, const Binding& binding)
{
    std::vector<ConfigValue> values;
    for (const auto& source : binding.sources) {
        const auto* value = std::get_if<ConfigValue>(&source);
        if (!value) { return std::monostate{}; }
        if (const auto checked = validate_binding(target, BindingSourceKind::Constant, value->type(), binding.conversion);
            const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
        switch (binding.conversion) {
        case ConversionKind::Exact: values.push_back(*value); break;
        case ConversionKind::WrapOptional: values.push_back(ConfigValue::some(*value)); break;
        case ConversionKind::Int64ToFloat64Explicit:
            values.push_back(std::get<ConfigValue>(ConfigValue::real(static_cast<double>(*value->as_int64()))));
            break;
        }
    }
    const auto effective = binding.cardinality == BindingCardinality::MultipleSources
        ? ConfigValue::list(*target.type.element_type(), values) : Result<ConfigValue>{values.front()};
    if (const auto* failure = std::get_if<Diagnostic>(&effective)) { return *failure; }
    const auto configured = std::get<ConfigValue>(effective);
    const auto checked = [&] {
        if constexpr (std::is_same_v<Target, InputDescriptor>) { return validate_input_value(target, configured); }
        else { return validate_parameter_value(target, configured); }
    }();
    if (const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
    return std::monostate{};
}

struct BindingEdit {
    std::optional<Binding> binding;
};

template<class Target>
std::optional<BindingEdit> edit_binding(QWidget* owner, const RoiEditContext& context, const Target& target,
    const Binding* existing)
{
    QDialog dialog(owner);
    dialog.setWindowTitle(QStringLiteral("Edit binding • %1").arg(qtext(target.display_name)));
    dialog.resize(500, 300);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("Target"), new QLabel(type_text(target.type), &dialog));
    auto* conversion = new QComboBox(&dialog);
    conversion->setObjectName(QStringLiteral("bindingConversion"));
    auto conversions = target.binding.allowed_conversions;
    if (existing && std::ranges::find(conversions, existing->conversion) == conversions.end()) {
        conversions.push_back(existing->conversion);
    }
    for (const auto kind : conversions) {
        conversion->addItem(conversion_text(kind));
        if (existing && kind == existing->conversion) { conversion->setCurrentIndex(conversion->count() - 1); }
    }
    form->addRow(QStringLiteral("Conversion"), conversion);
    layout->addLayout(form);
    auto* sources = new QListWidget(&dialog);
    sources->setObjectName(QStringLiteral("bindingSources"));
    layout->addWidget(sources);

    auto values = existing ? existing->sources : std::vector<BindingSource>{};
    const auto availability = existing ? existing->availability : AvailabilityPolicy{};
    const auto refresh = [&](int index) {
        sources->clear();
        for (const auto& source : values) {
            sources->addItem(QStringLiteral("%1 • %2").arg(kind_text(source_kind(source)), source_text(context.solution, source)));
        }
        if (!values.empty()) { sources->setCurrentRow(std::clamp(index, 0, sources->count() - 1)); }
    };
    refresh(0);
    const auto mode = [&] { return conversions[static_cast<std::size_t>(conversion->currentIndex())]; };
    auto* edits = new QHBoxLayout;
    const auto update_source = [&](int index) {
        const auto* original = index >= 0 ? &values[static_cast<std::size_t>(index)] : nullptr;
        auto edited = edit_source(&dialog, context, target, mode(), original);
        if (!edited) { return; }
        if (index < 0) {
            index = static_cast<int>(values.size());
            values.push_back(std::move(*edited));
        } else { values[static_cast<std::size_t>(index)] = std::move(*edited); }
        refresh(index);
    };
    const auto multiple = target.binding.cardinality == BindingCardinality::MultipleSources;
    edits->addWidget(action(&dialog, multiple ? QStringLiteral("Add Source") : QStringLiteral("Set Source"), "setBindingSource",
        [&] { update_source(multiple || values.empty() ? -1 : 0); }));
    if (multiple) {
        edits->addWidget(action(&dialog, QStringLiteral("Edit Source"), "editBindingSource",
            [&] { if (sources->currentRow() >= 0) { update_source(sources->currentRow()); } }));
    }
    edits->addWidget(action(&dialog, QStringLiteral("Remove Source"), "removeBindingSource", [&] {
        const int index = sources->currentRow();
        if (index < 0) { return; }
        values.erase(values.begin() + index);
        refresh(index);
    }));
    if (multiple) {
        const auto move = [&](int delta) {
            const auto index = sources->currentRow();
            const auto other = index + delta;
            if (index < 0 || other < 0 || other >= sources->count()) { return; }
            std::iter_swap(values.begin() + index, values.begin() + other);
            refresh(other);
        };
        edits->addWidget(action(&dialog, QStringLiteral("↑"), "earlierBindingSource", [move] { move(-1); }));
        edits->addWidget(action(&dialog, QStringLiteral("↓"), "laterBindingSource", [move] { move(1); }));
    }
    layout->addLayout(edits);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    std::optional<BindingEdit> result;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        guarded(&dialog, [&] {
            if (values.empty()) {
                result = BindingEdit{};
                dialog.accept();
                return;
            }
            Binding binding{target.id, values, target.binding.cardinality, mode(), availability};
            for (const auto checked : {validate_binding_shape(binding), check_constants(target, binding)}) {
                if (const auto* failure = std::get_if<Diagnostic>(&checked)) {
                    warn(&dialog, qtext(failure->message));
                    return;
                }
            }
            result = BindingEdit{std::move(binding)};
            dialog.accept();
        });
    });
    layout->addWidget(buttons);
    dialog.exec();
    return result;
}

template<class Target>
std::optional<BindingEdit> edit_region_binding(QWidget* owner, const RoiEditContext& context, const Target& target,
    const Binding* existing)
{
    const auto conversion = existing ? existing->conversion : ConversionKind::Exact;
    const auto* initial = existing ? std::get_if<ConfigValue>(&existing->sources.front())
        : (target.default_value ? &*target.default_value : nullptr);
    const bool optional = target.type.kind() == TypeKind::Optional && conversion == ConversionKind::Exact;
    auto value = region_constant(owner, context, initial && optional ? initial->optional_value() : initial);
    if (!value) { return std::nullopt; }
    if (optional) { value = ConfigValue::some(*value); }
    Binding binding{target.id, {std::move(*value)}, target.binding.cardinality, conversion,
        existing ? existing->availability : AvailabilityPolicy{}};
    for (const auto checked : {validate_binding_shape(binding), check_constants(target, binding)}) {
        if (const auto* failure = std::get_if<Diagnostic>(&checked)) {
            warn(owner, qtext(failure->message));
            return std::nullopt;
        }
    }
    return BindingEdit{std::move(binding)};
}

struct InspectorContext {
    ControlGraphModel& model;
    ControlScene& scene;
    const ToolRegistry& registry;
    QWidget* owner;
    const std::optional<ImageHandle>& preview_image;
    const EditorInspector::HashFile& hash_file;

    template<class Change>
    bool edit_flow(const WorkflowId& id, const QString& label, const Change& change) const
    {
        return scene.edit_draft(label, [&](Solution& solution) -> Check {
            auto* flow = mutable_flow(solution, id);
            return flow ? change(solution, *flow) : error(ErrorCode::UnknownTarget, "The workflow no longer exists.");
        });
    }

    template<class Change>
    bool edit_node(const WorkflowId& flow_id, const NodeId& node_id, const QString& label, const Change& change) const
    {
        return edit_flow(flow_id, label, [&](Solution& solution, Workflow& flow) -> Check {
            auto* node = mutable_node(flow, node_id);
            return node ? change(solution, flow, *node) : error(ErrorCode::UnknownTarget, "The node no longer exists.");
        });
    }
};

QString button_text(const QString& description)
{
    const auto line = description.simplified();
    return line.size() > 32 ? line.left(31) + QStringLiteral("…") : line;
}

template<class Toggle>
QCheckBox* checkbox(QWidget* owner, bool initial, const char* name, Toggle toggle)
{
    auto* field = new QCheckBox(owner);
    field->setObjectName(QString::fromLatin1(name));
    field->setChecked(initial);
    QObject::connect(field, &QCheckBox::toggled, owner, [owner, field, toggle = std::move(toggle)](bool value) {
        guarded(owner, [&] {
            if (!toggle(value)) {
                const QSignalBlocker blocked(field);
                field->setChecked(!value);
            }
        });
    });
    return field;
}

std::optional<std::string> rename(QWidget* owner, const QString& title, const QString& current)
{
    bool accepted = false;
    const auto value = QInputDialog::getText(owner, title, QStringLiteral("Display name"), QLineEdit::Normal, current, &accepted);
    if (!accepted || value == current) { return std::nullopt; }
    if (value.trimmed().isEmpty()) {
        warn(owner, QStringLiteral("Enter a display name."));
        return std::nullopt;
    }
    return utf8(value);
}

void edit_admission(const InspectorContext& context, WorkflowId id, WorkflowAdmission current)
{
    QDialog dialog(context.owner);
    dialog.setWindowTitle(QStringLiteral("Workflow admission"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* in_flight = new QLineEdit(QString::number(current.max_in_flight), &dialog);
    in_flight->setObjectName(QStringLiteral("maxInFlight"));
    in_flight->setToolTip(QStringLiteral("Started runs hold their slot until cleanup finishes."));
    auto* waiting = new QLineEdit(QString::number(current.queue_capacity), &dialog);
    waiting->setObjectName(QStringLiteral("queueCapacity"));
    waiting->setToolTip(QStringLiteral("Runs wait in submission order. Use 0 to disable waiting."));
    auto* overflow = new QComboBox(&dialog);
    overflow->setObjectName(QStringLiteral("admissionOverflow"));
    overflow->addItem(QStringLiteral("Return Busy"));
    overflow->addItem(QStringLiteral("Reject with diagnostic"));
    overflow->setCurrentIndex(current.overflow == AdmissionOverflow::Busy ? 0 : 1);
    form->addRow(QStringLiteral("Maximum in-flight runs"), in_flight);
    form->addRow(QStringLiteral("Waiting queue capacity"), waiting);
    form->addRow(QStringLiteral("When full"), overflow);
    layout->addLayout(form);
    close_buttons(dialog, *layout);
    while (dialog.exec() == QDialog::Accepted) {
        bool slots_ok{}, waiting_ok{};
        const auto max_in_flight = in_flight->text().toUInt(&slots_ok);
        const auto queue_capacity = waiting->text().toUInt(&waiting_ok);
        if (!slots_ok || !max_in_flight || !waiting_ok) {
            warn(&dialog, QStringLiteral("Enter a positive in-flight limit and a nonnegative queue capacity, each up to 4294967295."));
            continue;
        }
        const WorkflowAdmission policy{max_in_flight, queue_capacity,
            overflow->currentIndex() == 0 ? AdmissionOverflow::Busy : AdmissionOverflow::Reject};
        if (policy == current) { return; }
        context.edit_flow(id, QStringLiteral("Change workflow admission"), [&](Solution&, Workflow& draft) -> Check {
            draft.admission = policy;
            return std::monostate{};
        });
        return;
    }
}

void edit_trigger(const InspectorContext& context, WorkflowId id)
{
    const auto* flow = find_workflow(context.model.draft(), id);
    if (!flow) { return; }
    const auto current = flow->trigger;
    QDialog dialog(context.owner);
    dialog.setWindowTitle(QStringLiteral("Workflow trigger"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* mode = new QComboBox(&dialog);
    mode->setObjectName(QStringLiteral("triggerMode"));
    mode->addItems({QStringLiteral("Manual only"), QStringLiteral("External frames"), QStringLiteral("Continuous clock"),
        QStringLiteral("Camera frames")});
    mode->setCurrentIndex(current && current->camera ? 3 : current ? (current->mode == TriggerMode::External ? 1 : 2) : 0);
    auto* source = new QLineEdit(current ? id_text(current->source) : QString{}, &dialog);
    source->setObjectName(QStringLiteral("triggerSource"));
    auto* input = new QComboBox(&dialog);
    input->setObjectName(QStringLiteral("triggerFrameInput"));
    std::vector<InputId> frame_inputs;
    for (const auto& port : flow->interface.inputs) {
        if (port.type != DataType::integer()) { continue; }
        input->addItem(QStringLiteral("%1 (%2)").arg(qtext(port.display_name), id_text(port.id)));
        frame_inputs.push_back(port.id);
        if (current && current->frame_input == port.id) { input->setCurrentIndex(input->count() - 1); }
    }
    auto* interval = new QLineEdit(QString::number(current && current->interval_ms ? current->interval_ms : 100), &dialog);
    interval->setObjectName(QStringLiteral("triggerIntervalMs"));
    form->addRow(QStringLiteral("Mode"), mode);
    form->addRow(QStringLiteral("Source ID"), source);
    form->addRow(QStringLiteral("Frame number input"), input);
    form->addRow(QStringLiteral("Interval (ms)"), interval);
    layout->addLayout(form);
    auto* camera = new CameraTriggerEditor(&dialog, context.model.draft(), *flow);
    layout->addWidget(camera);
    const auto refresh = [=] {
        source->setEnabled(mode->currentIndex() != 0);
        input->setEnabled(mode->currentIndex() != 0);
        interval->setEnabled(mode->currentIndex() == 2);
        camera->setVisible(mode->currentIndex() == 3);
    };
    QObject::connect(mode, &QComboBox::currentIndexChanged, &dialog, refresh);
    refresh();
    close_buttons(dialog, *layout);
    while (dialog.exec() == QDialog::Accepted) {
        std::optional<WorkflowTrigger> trigger;
        if (mode->currentIndex()) {
            auto parsed = SchemaIdFactory::parse<SchemaId>(utf8(source->text()));
            if (const auto* failure = std::get_if<Diagnostic>(&parsed)) { warn(&dialog, qtext(failure->message)); continue; }
            if (input->currentIndex() < 0) { warn(&dialog, QStringLiteral("Add an Int64 public input for the frame number.")); continue; }
            const bool periodic = mode->currentIndex() == 2;
            bool valid{};
            const auto delay = interval->text().toUInt(&valid);
            if (periodic && (!valid || !delay)) { warn(&dialog, QStringLiteral("Enter a positive interval in milliseconds.")); continue; }
            trigger.emplace(WorkflowTrigger{periodic ? TriggerMode::Continuous : TriggerMode::External,
                std::get<SchemaId>(std::move(parsed)), frame_inputs[static_cast<std::size_t>(input->currentIndex())], periodic ? delay : 0});
            if (mode->currentIndex() == 3) {
                auto selected = camera->binding();
                if (const auto* failure = std::get_if<Diagnostic>(&selected)) { warn(&dialog, qtext(failure->message)); continue; }
                trigger->camera = std::get<CameraTriggerBinding>(std::move(selected));
            }
        }
        if (trigger != current) {
            context.edit_flow(id, QStringLiteral("Change workflow trigger"), [&](Solution& solution, Workflow& draft) -> Check {
                if (trigger && trigger->camera && camera->new_device()) { solution.resources.push_back(*camera->new_device()); }
                draft.trigger = trigger;
                return std::monostate{};
            });
        }
        return;
    }
}

void workflow_properties(QVBoxLayout& panel, const InspectorContext& context, const Workflow& flow)
{
    auto* group = section(panel, QStringLiteral("Workflow"));
    auto* form = new QFormLayout(group);
    const auto name = qtext(flow.name);
    const auto id = flow.id;
    auto* field = action(group, button_text(name), "renameWorkflow", [context, id, name] {
        const auto updated = rename(context.owner, QStringLiteral("Rename workflow"), name);
        if (!updated) { return; }
        context.edit_flow(id, QStringLiteral("Rename workflow"), [&](Solution&, Workflow& draft) -> Check {
            draft.name = *updated;
            return std::monostate{};
        });
    });
    field->setToolTip(name);
    form->addRow(QStringLiteral("Name"), field);
    form->addRow(QStringLiteral("Enabled"), checkbox(group, flow.enabled, "workflowEnabled", [context, id](bool enabled) {
        return context.edit_flow(id, QStringLiteral("Change workflow enablement"), [&](Solution&, Workflow& draft) -> Check {
            draft.enabled = enabled;
            return std::monostate{};
        });
    }));
    const auto policy = flow.admission;
    const auto summary = QStringLiteral("%1 running · %2 waiting · %3").arg(policy.max_in_flight).arg(policy.queue_capacity)
        .arg(policy.overflow == AdmissionOverflow::Busy ? QStringLiteral("Busy") : QStringLiteral("Reject"));
    form->addRow(QStringLiteral("Admission"), action(group, summary, "editWorkflowAdmission",
        [context, id, policy] { edit_admission(context, id, policy); }));
    const auto trigger = flow.trigger ? id_text(flow.trigger->source) : QStringLiteral("Manual only");
    form->addRow(QStringLiteral("Trigger source"), action(group, trigger, "editWorkflowTrigger",
        [context, id] { edit_trigger(context, id); }));
}

void move_to_group(const InspectorContext& context, const WorkflowId& id, const NodeId& selected)
{
    const auto* flow = find_workflow(context.model.draft(), id);
    const auto* node = flow ? find_node(*flow, selected) : nullptr;
    if (!node) { return; }
    const auto before = parent_of(*flow, selected);
    auto choices = Choices<std::optional<NodeId>>{{QStringLiteral("Workflow root"), std::nullopt}};
    std::vector<NodeId> excluded{selected};
    if (std::holds_alternative<GroupConfiguration>(node->configuration)) {
        const auto tree = descendants(*flow, selected);
        if (const auto* failure = std::get_if<Diagnostic>(&tree)) { warn(context.owner, qtext(failure->message)); return; }
        const auto& nested = std::get<std::vector<NodeId>>(tree);
        excluded.insert(excluded.end(), nested.begin(), nested.end());
    }
    std::size_t current = 0;
    for (const auto& candidate : flow->nodes) {
        if (!std::holds_alternative<GroupConfiguration>(candidate.configuration)
            || std::ranges::find(excluded, candidate.id) != excluded.end()) { continue; }
        if (before && *before == candidate.id) { current = choices.size(); }
        choices.emplace_back(QStringLiteral("%1 [%2]").arg(qtext(candidate.name), short_id(candidate.id)), candidate.id);
    }
    const auto target = choose(context.owner, QStringLiteral("Move to Group"), QStringLiteral("Parent"), choices, current);
    if (!target || *target == before) { return; }
    const bool moved = context.edit_node(id, selected, QStringLiteral("Change Group ownership"),
        [&](Solution&, Workflow& draft, Node& moving) -> Check {
            auto* from = entries_at(draft, parent_of(draft, moving.id));
            auto* to = entries_at(draft, *target);
            if (!from || !to) { return error(ErrorCode::InvalidContainment, "The Group scope does not exist."); }
            if (std::erase(*from, moving.id)) { to->push_back(moving.id); }
            std::erase_if(draft.containment, [&](const Containment& item) { return item.child == moving.id; });
            if (*target) { draft.containment.push_back({**target, moving.id}); }
            return std::monostate{};
        });
    if (moved) { context.scene.reveal_node(selected); }
}

void edit_execution_policy(const InspectorContext& context, WorkflowId flow, NodeId node,
    const ToolDescriptor& tool, const NodeExecutionPolicy& current)
{
    QDialog dialog(context.owner);
    dialog.setWindowTitle(QStringLiteral("Node execution policy"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* timeout = new QLineEdit(QString::number(current.timeout_ms), &dialog);
    timeout->setObjectName(QStringLiteral("nodeTimeoutMs"));
    auto* retries = new QLineEdit(QString::number(current.max_retries), &dialog);
    retries->setObjectName(QStringLiteral("nodeMaxRetries"));
    retries->setEnabled(tool.execution.retry != ToolRetry::Never || current.max_retries);
    const auto capability = tool.execution.retry == ToolRetry::ProtocolDeduplicated ? QStringLiteral("Protocol deduplication")
        : tool.execution.retry == ToolRetry::Idempotent ? QStringLiteral("Pure computation") : QStringLiteral("Retries disabled");
    auto* outlet = new QLineEdit(current.error_output ? id_text(*current.error_output) : QString{}, &dialog);
    outlet->setObjectName(QStringLiteral("nodeErrorOutlet"));
    form->addRow(QStringLiteral("Attempt timeout (ms, 0 disables)"), timeout);
    form->addRow(QStringLiteral("Retry capability"), new QLabel(capability, &dialog));
    form->addRow(QStringLiteral("Maximum retries"), retries);
    form->addRow(QStringLiteral("Error outlet ID (optional)"), outlet);
    layout->addLayout(form);
    close_buttons(dialog, *layout);
    while (dialog.exec() == QDialog::Accepted) {
        bool timeout_ok{}, retries_ok{};
        NodeExecutionPolicy next{timeout->text().toUInt(&timeout_ok), retries->text().toUInt(&retries_ok)};
        if (!timeout_ok || !retries_ok) {
            warn(&dialog, QStringLiteral("Timeout and retry limit must be unsigned 32-bit integers."));
            continue;
        }
        const auto name = outlet->text().trimmed();
        if (!name.isEmpty()) {
            auto parsed = SchemaIdFactory::parse<ControlOutputId>(utf8(name));
            if (const auto* bad = std::get_if<Diagnostic>(&parsed)) { warn(&dialog, qtext(bad->message)); continue; }
            next.error_output = std::get<ControlOutputId>(std::move(parsed));
        }
        auto checked = validate_node_execution_policy(next, tool);
        if (const auto* bad = std::get_if<Diagnostic>(&checked)) { warn(&dialog, qtext(bad->message)); continue; }
        if (current == next) { return; }
        context.edit_node(flow, node, QStringLiteral("Change node execution policy"), [&](Solution&, Workflow& scope, Node& draft) -> Check {
            if (current.error_output && current.error_output != next.error_output) {
                for (auto& link : scope.control_links) {
                    if (link.source != node || link.output != current.error_output) { continue; }
                    if (!next.error_output) { return error(ErrorCode::InvalidControlLink, "Disconnect the error outlet before removing it."); }
                    link.output = next.error_output;
                }
            }
            draft.execution_policy = next;
            return std::monostate{};
        });
        return;
    }
}

void node_properties(QVBoxLayout& panel, const InspectorContext& context, const Workflow& flow, const Node& node)
{
    auto* group = section(panel, QStringLiteral("Node"));
    auto* form = new QFormLayout(group);
    const auto id = flow.id;
    const auto selected = node.id;
    const auto name = qtext(node.name);
    const auto parent = parent_of(flow, selected);
    const auto* parent_node = parent ? find_node(flow, *parent) : nullptr;
    const auto* parent_group = group_at(flow, parent);
    const auto& entries = parent_group ? parent_group->entries : flow.entries;
    const auto parent_name = parent_node ? qtext(parent_node->name) : QStringLiteral("Workflow root");
    auto* field = action(group, button_text(name), "renameNode", [context, id, selected, name] {
        const auto updated = rename(context.owner, QStringLiteral("Rename node"), name);
        if (!updated) { return; }
        context.edit_node(id, selected, QStringLiteral("Rename node"), [&](Solution&, Workflow&, Node& draft) -> Check {
            draft.name = *updated;
            return std::monostate{};
        });
    });
    field->setToolTip(name);
    form->addRow(QStringLiteral("Name"), field);
    if (const auto* config = std::get_if<ToolConfiguration>(&node.configuration)) {
        const auto* tool = context.registry.find_descriptor(config->type);
        if (tool && tool->config_version == config->config_version) {
            const auto policy = node.execution_policy;
            form->addRow(QStringLiteral("Execution policy"),
                action(group, QStringLiteral("Timeout, retries and error outlet…"), "editNodeExecutionPolicy",
                    [context, id, selected, tool, policy] { edit_execution_policy(context, id, selected, *tool, policy); }));
        }
    }
    form->addRow(QStringLiteral("Enabled"), checkbox(group, node.enabled, "nodeEnabled", [context, id, selected](bool enabled) {
        return context.edit_node(id, selected, QStringLiteral("Change node enablement"), [&](Solution&, Workflow&, Node& draft) -> Check {
            draft.enabled = enabled;
            return std::monostate{};
        });
    }));
    const auto is_entry = std::ranges::find(entries, selected) != entries.end();
    form->addRow(QStringLiteral("Entry"), checkbox(group, is_entry, "nodeEntry", [context, id, selected](bool entry) {
        return context.edit_node(id, selected, QStringLiteral("Change scope entry"), [&](Solution&, Workflow& draft, Node& target) -> Check {
            auto* list = entries_at(draft, parent_of(draft, target.id));
            if (!list) { return error(ErrorCode::InvalidContainment, "The entry scope does not exist."); }
            std::erase(*list, selected);
            if (entry) { list->push_back(selected); }
            return std::monostate{};
        });
    }));
    auto* ownership = action(group, button_text(parent_name), "moveNodeToGroup", [context, id, selected] {
        move_to_group(context, id, selected);
    });
    ownership->setToolTip(QStringLiteral("Change the Group parent of this node."));
    form->addRow(QStringLiteral("Parent"), ownership);
}

void write_resource_binding(Node& node, const ResourceRequirement& slot, const std::optional<ResourceReference>& resource)
{
    const auto current = std::ranges::find(node.resources, slot.slot, &NodeResourceBinding::slot);
    if (current != node.resources.end()) {
        if (resource) { current->resource = *resource; }
        else { node.resources.erase(current); }
    } else if (resource) { node.resources.push_back({slot.slot, *resource}); }
}

Check bind_roi_mask(const Solution& solution, Node& node, const ResourceRequirement& slot, const ConfigValue& value)
{
    const auto* concrete = value.type().kind() == TypeKind::Optional ? value.optional_value() : &value;
    if (!concrete) {
        write_resource_binding(node, slot, {});
        return std::monostate{};
    }
    const auto* region = concrete->as_region2d();
    if (!region) { return std::monostate{}; }
    std::optional<ResourceReference> resource;
    bool inherited = false;
    const auto collect = [&](const RegionSource& source) -> Check {
        const auto* primitive = std::get_if<RegionPrimitive>(&source);
        if (!primitive) {
            inherited = true;
            return std::monostate{};
        }
        const auto* config = std::get_if<MaskConfig>(&primitive->geometry);
        if (!config) { return std::monostate{}; }
        const auto& reference = config->resource;
        if (resource && (resource->id != reference.id
                || (resource->revision && reference.revision && resource->revision != reference.revision))) {
            return error(ErrorCode::InvalidResource, "Use one mask resource and revision for the ROI and its exclusions.");
        }
        if (!resource || reference.revision) { resource = reference; }
        return std::monostate{};
    };
    if (auto checked = collect(region->source); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    for (const auto& exclusion : region->exclusions) {
        if (auto checked = collect(exclusion); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    }
    if (resource) {
        if (auto checked = validate_resource_reference(*resource, solution.resources); std::holds_alternative<Diagnostic>(checked)) {
            return checked;
        }
    }
    if (resource || !inherited) { write_resource_binding(node, slot, resource); }
    return std::monostate{};
}

template<class Target>
void open_binding(const InspectorContext& context, const WorkflowId& id, const NodeId& selected, const Target& target,
    bool region_only = false)
{
    const auto& solution = context.model.draft();
    const auto* flow = find_workflow(solution, id);
    const auto* node = flow ? find_node(*flow, selected) : nullptr;
    if (!node) { return; }
    const auto binding_id = BindingTarget{target.id};
    const auto found = std::ranges::find(node->bindings, binding_id, &Binding::target);
    const auto* existing = found == node->bindings.end() ? nullptr : &*found;
    const auto* tool = metadata(context.registry, *node);
    const auto preview_image = context.preview_image;
    const RoiEditContext roi{solution, context.registry, flow, node,
        tool && tool->region_capabilities ? &*tool->region_capabilities : nullptr, target.semantics,
        preview_image ? &*preview_image : nullptr};
    const auto edited = region_only ? edit_region_binding(context.owner, roi, target, existing)
        : edit_binding(context.owner, roi, target, existing);
    if (!edited || (!existing && !edited->binding)) { return; }
    if (existing && edited->binding) {
        const auto& next = *edited->binding;
        if (existing->sources == next.sources && existing->cardinality == next.cardinality
            && existing->conversion == next.conversion && existing->availability.accepted_absence == next.availability.accepted_absence) {
            return;
        }
    }
    context.edit_node(id, selected, QStringLiteral("Change binding"), [&]([[maybe_unused]] Solution& candidate, Workflow&, Node& draft) -> Check {
        if constexpr (std::is_same_v<Target, ParameterDescriptor>) {
            if (tool && tool->region_capabilities && target.id.text() == "roi") {
                const auto slot = std::ranges::find_if(tool->resources, [](const ResourceRequirement& requirement) {
                    return requirement.kind == ResourceKind::Mask && requirement.slot.text() == "mask";
                });
                const auto* value = target.default_value ? &*target.default_value : nullptr;
                if (edited->binding) {
                    const auto& sources = edited->binding->sources;
                    value = sources.size() == 1 ? std::get_if<ConfigValue>(&sources.front()) : nullptr;
                }
                if (value && slot != tool->resources.end()) {
                    if (auto checked = bind_roi_mask(candidate, draft, *slot, *value); std::holds_alternative<Diagnostic>(checked)) {
                        return checked;
                    }
                }
            }
        }
        const auto current = std::ranges::find(draft.bindings, binding_id, &Binding::target);
        if (current != draft.bindings.end()) {
            if (edited->binding) { *current = *edited->binding; }
            else { draft.bindings.erase(current); }
        } else if (edited->binding) { draft.bindings.push_back(*edited->binding); }
        return std::monostate{};
    });
}

template<class Target>
void binding_row(QFormLayout& form, QWidget* owner, const InspectorContext& context,
    const Workflow& flow, const Node& node, const Target& target)
{
    const auto binding = std::ranges::find(node.bindings, BindingTarget{target.id}, &Binding::target);
    QString summary;
    const OutputReference* output = nullptr;
    if (binding != node.bindings.end() && !binding->sources.empty()) {
        summary = source_text(context.model.draft(), binding->sources.front());
        if (binding->sources.size() > 1) { summary += QStringLiteral(" +%1").arg(binding->sources.size() - 1); }
        else { output = std::get_if<OutputReference>(&binding->sources.front()); }
    } else {
        summary = target.default_value ? QStringLiteral("Default: %1").arg(constant_text(*target.default_value))
            : QStringLiteral("Unbound");
    }
    const auto name = qtext(target.display_name);
    auto* edit = action(owner, button_text(summary), "editBinding", [context, id = flow.id, node_id = node.id, target] {
        open_binding(context, id, node_id, target);
    });
    edit->setProperty("bindingTarget", id_text(target.id));
    edit->setToolTip(QStringLiteral("%1 • %2\n%3").arg(name, type_text(target.type), summary));
    const bool constant_roi = target.binding.cardinality == BindingCardinality::SingleSource
        && (target.type.kind() == TypeKind::Region2D
            || (target.type.kind() == TypeKind::Optional && target.type.element_type()->kind() == TypeKind::Region2D))
        && std::ranges::find(target.binding.allowed_sources, BindingSourceKind::Constant) != target.binding.allowed_sources.end()
        && std::ranges::find(target.binding.allowed_conversions, ConversionKind::Exact) != target.binding.allowed_conversions.end()
        && (binding == node.bindings.end() || (binding->sources.size() == 1
            && std::holds_alternative<ConfigValue>(binding->sources.front())
            && (binding->conversion == ConversionKind::Exact || binding->conversion == ConversionKind::WrapOptional)));
    if (constant_roi) {
        auto* row = new QHBoxLayout;
        row->addWidget(edit, 1);
        auto* region = action(owner, QStringLiteral("Edit ROI…"), "editRegion", [context, id = flow.id, node_id = node.id, target] {
            open_binding(context, id, node_id, target, true);
        });
        region->setProperty("bindingTarget", id_text(target.id));
        row->addWidget(region);
        form.addRow(name, row);
    } else if (output && output->workflow == flow.id) {
        auto* row = new QHBoxLayout;
        row->addWidget(edit, 1);
        auto* locate = action(owner, QStringLiteral("↗"), "locateBindingSource", [context, source = output->node] {
            context.scene.reveal_node(source);
        });
        locate->setToolTip(QStringLiteral("Locate the producer node on the canvas."));
        row->addWidget(locate);
        form.addRow(name, row);
    } else { form.addRow(name, edit); }
}

bool file_resource_kind(ResourceKind kind)
{
    return kind == ResourceKind::Image || kind == ResourceKind::Mask || kind == ResourceKind::Template
        || kind == ResourceKind::Calibration || kind == ResourceKind::Binary;
}

void select_resource(const InspectorContext& context, const WorkflowId& id, const NodeId& selected,
    const ResourceRequirement& slot)
{
    const auto& solution = context.model.draft();
    const auto* flow = find_workflow(solution, id);
    const auto* node = flow ? find_node(*flow, selected) : nullptr;
    if (!node) { return; }
    const auto found = std::ranges::find(node->resources, slot.slot, &NodeResourceBinding::slot);
    const auto old = found == node->resources.end() ? std::nullopt : std::optional{found->resource};
    const auto* config = std::get_if<ToolConfiguration>(&node->configuration);
    const bool camera = config && config->type.text() == "visionpro.camera.acquire" && slot.kind == ResourceKind::Device;
    const auto endpoint = config && slot.kind == ResourceKind::Device ? endpoint_adapter(config->type) : std::nullopt;
    Choices<std::optional<ResourceReference>> choices{{QStringLiteral("Unbound"), std::nullopt}};
    std::size_t current = 0;
    for (const auto& resource : solution.resources) {
        if (resource.kind != slot.kind || (camera && !is_camera_device(resource)) || (endpoint && !matches_endpoint(resource, *endpoint))) { continue; }
        auto pin = old && old->id == resource.id ? old->revision : std::nullopt;
        if ((slot.kind == ResourceKind::Calibration || slot.kind == ResourceKind::Template) && !pin) {
            if (const auto* file = std::get_if<ManagedResource>(&resource.location)) { pin = file->revision; }
            if (const auto* file = std::get_if<ExternalFileResource>(&resource.location)) { pin = file->revision; }
        }
        ResourceReference ref{resource.id, resource.kind, pin};
        if (old && old->id == resource.id) { current = choices.size(); }
        auto label = QStringLiteral("%1 [%2]").arg(qtext(resource.name), short_id(resource.id));
        if (endpoint) { label += QStringLiteral(" · ") + endpoint_address(resource); }
        choices.emplace_back(std::move(label), ref);
    }
    auto choice = choose(context.owner, QStringLiteral("Choose resource"), QStringLiteral("Resource"), choices, current);
    if (!choice || *choice == old) { return; }
    context.edit_node(id, selected, QStringLiteral("Change resource binding"), [&](Solution&, Workflow&, Node& draft) -> Check {
        write_resource_binding(draft, slot, *choice);
        return std::monostate{};
    });
}

void select_external_file(const InspectorContext& context, const WorkflowId& id, const NodeId& selected,
    const ResourceRequirement& slot)
{
    const auto path = QFileDialog::getOpenFileName(context.owner, QStringLiteral("Choose external resource"));
    if (path.isEmpty()) { return; }
    const auto apply = [context, owner = QPointer(context.owner), id, selected, slot, file = QFileInfo(path)](std::optional<std::string> digest) {
        if (!owner) { return; }
        context.edit_node(id, selected, QStringLiteral("Choose external file"), [&](Solution& solution, Workflow&, Node& draft) -> Check {
            auto created = IdFactory::generate<ResourceId>();
            if (const auto* failure = std::get_if<Diagnostic>(&created)) { return *failure; }
            const auto resource = std::get<ResourceId>(created);
            const auto policy = digest ? ResourceReadPolicy::SnapshotLocked : ResourceReadPolicy::PerInvocation;
            const auto revision = digest ? std::optional<std::uint64_t>{1} : std::nullopt;
            solution.resources.push_back({resource, utf8(file.fileName()), slot.kind,
                ExternalFileResource{{PathBase::Absolute, utf8(file.absoluteFilePath()), {}}, policy, digest, revision}});
            write_resource_binding(draft, slot, ResourceReference{resource, slot.kind, revision});
            return std::monostate{};
        });
    };
    if (slot.kind == ResourceKind::Calibration || slot.kind == ResourceKind::Template) {
        if (context.hash_file) { context.hash_file(path, apply); }
    } else { apply(std::nullopt); }
}

void resource_row(QFormLayout& form, QWidget* owner, const InspectorContext& context, const Workflow& flow,
    const Node& node, const ResourceRequirement& slot)
{
    const auto bound = std::ranges::find(node.resources, slot.slot, &NodeResourceBinding::slot);
    const auto* configuration = std::get_if<ToolConfiguration>(&node.configuration);
    const auto endpoint = configuration && slot.kind == ResourceKind::Device ? endpoint_adapter(configuration->type) : std::nullopt;
    auto summary = bound == node.resources.end() ? QStringLiteral("Unbound")
        : source_text(context.model.draft(), bound->resource);
    bool compatible_endpoint = true;
    if (endpoint && bound != node.resources.end()) {
        const auto& resources = context.model.draft().resources;
        const auto item = std::ranges::find(resources, bound->resource.id, &ResourceDefinition::id);
        compatible_endpoint = item != resources.end() && matches_endpoint(*item, *endpoint);
        summary += compatible_endpoint ? QStringLiteral(" · ") + endpoint_address(*item)
                                       : QStringLiteral(" · Expected %1").arg(qtext(*endpoint));
    }
    auto* row = new QHBoxLayout;
    auto* edit = action(owner, button_text(summary), "selectNodeResource", [context, id = flow.id, selected = node.id, slot] {
        select_resource(context, id, selected, slot);
    });
    edit->setToolTip(summary);
    row->addWidget(edit, 1);
    const bool locked_file = slot.kind == ResourceKind::Calibration || slot.kind == ResourceKind::Template;
    if (file_resource_kind(slot.kind) && (!locked_file || context.hash_file)) {
        row->addWidget(action(owner, QStringLiteral("File…"), "chooseExternalResource",
            [context, id = flow.id, selected = node.id, slot] { select_external_file(context, id, selected, slot); }));
    }
    const bool camera = configuration && configuration->type.text() == "visionpro.camera.acquire" && slot.kind == ResourceKind::Device;
    if (camera || endpoint) {
        const auto save_device = [context, id = flow.id, selected = node.id, slot, endpoint](std::optional<ResourceId> original) {
            const auto& catalog = context.model.draft().resources;
            const auto found = original ? std::ranges::find(catalog, *original, &ResourceDefinition::id) : catalog.end();
            if (original && (found == catalog.end() || (endpoint ? !matches_endpoint(*found, *endpoint) : !is_camera_device(*found)))) { return; }
            const auto* previous = original ? &*found : nullptr;
            auto edited = endpoint ? edit_endpoint(context.owner, *endpoint, previous) : edit_camera_device(context.owner, previous);
            if (!edited) { return; }
            context.edit_node(id, selected, endpoint ? QStringLiteral("Configure endpoint") : QStringLiteral("Configure camera device"),
                [&](Solution& solution, Workflow&, Node& draft) -> Check {
                const auto entry = std::ranges::find(solution.resources, edited->id, &ResourceDefinition::id);
                if (entry == solution.resources.end()) { solution.resources.push_back(*edited); }
                else { *entry = *edited; }
                write_resource_binding(draft, slot, ResourceReference{edited->id, ResourceKind::Device, {}});
                return std::monostate{};
            });
        };
        row->addWidget(action(owner, QStringLiteral("New…"), endpoint ? "newEndpoint" : "newCameraDevice", [save_device] { save_device({}); }));
        if (bound != node.resources.end() && compatible_endpoint) {
            row->addWidget(action(owner, QStringLiteral("Edit device…"), endpoint ? "editEndpoint" : "editCameraDevice",
                [save_device, resource = bound->resource.id] { save_device(resource); }));
        }
    }
    form.addRow(id_text(slot.slot), row);
}

Choices<SharedTargetId> compatible_targets(const Solution& solution, const SharedTargetRequirement& requirement)
{
    Choices<SharedTargetId> choices;
    const auto add = [&](const auto& definition) {
        choices.emplace_back(QStringLiteral("%1 [%2]").arg(qtext(definition.name), qtext(definition.id.to_string())), definition.id);
    };
    if (requirement.kind == SharedTargetKind::Variable) {
        for (const auto& item : solution.variables) { if (item.type == requirement.type) { add(item); } }
    } else {
        for (const auto& item : solution.queues) { if (item.message_schema == requirement.type) { add(item); } }
    }
    return choices;
}

QString target_label(const Choices<SharedTargetId>& choices, const SharedTargetId& target)
{
    for (const auto& [label, id] : choices) { if (target == id) { return label; } }
    return QStringLiteral("Unavailable [%1]").arg(std::visit([](const auto& id) { return qtext(id.to_string()); }, target));
}

void shared_targets(QVBoxLayout& panel, const InspectorContext& context, const Workflow& flow, const Node& node,
    const SharedTargetRequirement& requirement)
{
    const auto choices = compatible_targets(context.model.draft(), requirement);
    const auto current = std::get<ToolConfiguration>(node.configuration).shared_targets;
    const auto title = requirement.kind == SharedTargetKind::Variable ? QStringLiteral("Global variables") : QStringLiteral("Queues");
    auto* group = section(panel, title);
    auto* form = new QFormLayout(group);
    QStringList labels;
    for (const auto& target : current) { labels.append(target_label(choices, target)); }
    const auto summary = labels.isEmpty() ? QStringLiteral("Select target…") : labels.join(QStringLiteral(" → "));
    auto* button = action(group, button_text(summary), "editSharedTargets", [=, id = flow.id, selected = node.id] {
        QDialog dialog(context.owner);
        dialog.setWindowTitle(title);
        auto* layout = new QVBoxLayout(&dialog);
        auto* picker = new QComboBox(&dialog);
        picker->setObjectName(QStringLiteral("compatibleSharedTargets"));
        for (const auto& [label, target] : choices) { picker->addItem(label); }
        if (!requirement.multiple && current.size() == 1) {
            picker->setCurrentIndex(picker->findText(target_label(choices, current.front())));
        }
        layout->addWidget(picker);
        auto* list = new QListWidget(&dialog);
        list->setObjectName(QStringLiteral("orderedSharedTargets"));
        layout->addWidget(list);
        auto targets = current;
        auto* edits = new QHBoxLayout;
        const auto render = [&](int row) {
            list->clear();
            for (const auto& target : targets) { list->addItem(target_label(choices, target)); }
            if (list->count()) { list->setCurrentRow(std::clamp(row, 0, list->count() - 1)); }
        };
        render(0);
        auto* set = action(&dialog, requirement.multiple ? QStringLiteral("Add") : QStringLiteral("Set"), "setSharedTarget", [&] {
            if (picker->currentIndex() < 0) { return; }
            const auto& target = choices[static_cast<std::size_t>(picker->currentIndex())].second;
            if (!requirement.multiple) { targets = {target}; }
            else if (std::ranges::find(targets, target) == targets.end()) { targets.push_back(target); }
            render(static_cast<int>(std::ranges::find(targets, target) - targets.begin()));
        });
        set->setEnabled(!choices.empty());
        edits->addWidget(set);
        edits->addWidget(action(&dialog, QStringLiteral("Remove"), "removeSharedTarget", [&] {
            const auto index = list->currentRow();
            if (index >= 0) { targets.erase(targets.begin() + index); render(index); }
        }));
        const auto move = [&](int delta) {
            const auto index = list->currentRow(), other = index + delta;
            if (index < 0 || other < 0 || other >= list->count()) { return; }
            std::iter_swap(targets.begin() + index, targets.begin() + other);
            render(other);
        };
        if (requirement.multiple) {
            edits->addWidget(action(&dialog, QStringLiteral("↑"), "earlierSharedTarget", [&] { move(-1); }));
            edits->addWidget(action(&dialog, QStringLiteral("↓"), "laterSharedTarget", [&] { move(1); }));
        }
        layout->addLayout(edits);
        close_buttons(dialog, *layout);
        if (dialog.exec() != QDialog::Accepted || targets == current) { return; }
        context.edit_node(id, selected, QStringLiteral("Change shared targets"), [&](Solution& solution, Workflow&, Node& editing) -> Check {
            if (!targets.empty()) {
                const auto* tool = metadata(context.registry, editing);
                if (!tool) { return error(ErrorCode::UnknownTool, "The selected tool metadata is unavailable."); }
                auto shape = validate_shared_targets(*tool, targets);
                if (const auto* failure = std::get_if<Diagnostic>(&shape)) { return *failure; }
            }
            const auto compatible = compatible_targets(solution, requirement);
            for (const auto& target : targets) {
                if (std::ranges::none_of(compatible, [&](const auto& item) { return item.second == target; })) {
                    return error(ErrorCode::TypeMismatch, "A selected shared target is missing or has an incompatible type.");
                }
            }
            std::get<ToolConfiguration>(editing.configuration).shared_targets = targets;
            return std::monostate{};
        });
    });
    button->setToolTip(summary);
    form->addRow(type_text(requirement.type), button);
}

void tool_settings(QVBoxLayout& panel, const InspectorContext& context, const Workflow& flow, const Node& node)
{
    const auto* tool = metadata(context.registry, node);
    if (!tool) {
        auto* status = new QLabel(QStringLiteral("Tool metadata is unavailable for this configuration version."));
        status->setWordWrap(true);
        panel.addWidget(status);
        return;
    }
    if (tool->shared_target) { shared_targets(panel, context, flow, node, *tool->shared_target); }
    if (std::ranges::any_of(tool->outputs, [](const auto& output) { return output.type == class_map_type(); })) {
        auto* group = section(panel, QStringLiteral("Display"));
        auto* layout = new QVBoxLayout(group);
        layout->addWidget(action(group, QStringLiteral("Edit class color…"), "editClassColor", [context, flow_id = flow.id, id = node.id] {
            const auto* placement = find_placement(context.model.draft(), flow_id, id);
            const auto palette = placement ? placement->class_palette : ClassPalette{};
            auto edited = edit_class_color(context.owner, palette);
            if (edited && *edited != palette) { context.scene.set_class_palette(id, std::move(*edited)); }
        }));
    }
    if (!tool->inputs.empty()) {
        auto* group = section(panel, QStringLiteral("Inputs"));
        auto* form = new QFormLayout(group);
        for (const auto& input : tool->inputs) { binding_row(*form, group, context, flow, node, input); }
    }
    if (!tool->parameters.empty()) {
        auto* group = section(panel, QStringLiteral("Parameters"));
        auto* form = new QFormLayout(group);
        for (const auto& parameter : tool->parameters) { binding_row(*form, group, context, flow, node, parameter); }
    }
    if (!tool->resources.empty()) {
        auto* group = section(panel, QStringLiteral("Resources"));
        auto* form = new QFormLayout(group);
        for (const auto& slot : tool->resources) { resource_row(*form, group, context, flow, node, slot); }
    }
}

struct PortName {
    std::string id;
    std::string display;
};

std::optional<PortName> port_name(QWidget* owner, QString title, std::optional<PortName> existing = std::nullopt)
{
    QDialog dialog(owner);
    dialog.setWindowTitle(std::move(title));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* key = new QLineEdit(&dialog);
    key->setObjectName(QStringLiteral("publicPortId"));
    key->setPlaceholderText(QStringLiteral("e.g. score"));
    auto* name = new QLineEdit(&dialog);
    name->setObjectName(QStringLiteral("publicPortName"));
    if (existing) {
        key->setText(qtext(existing->id));
        key->setReadOnly(true);
        name->setText(qtext(existing->display));
    }
    form->addRow(QStringLiteral("Port ID"), key);
    form->addRow(QStringLiteral("Display name"), name);
    layout->addLayout(form);
    close_buttons(dialog, *layout);
    if (dialog.exec() != QDialog::Accepted) { return std::nullopt; }
    auto id = utf8(key->text());
    if (const auto checked = SchemaIdFactory::parse<SchemaId>(id);
        const auto* failure = std::get_if<Diagnostic>(&checked)) { warn(owner, qtext(failure->message)); return std::nullopt; }
    return PortName{std::move(id), utf8(name->text().isEmpty() ? key->text() : name->text())};
}

struct PortType {
    DataType type;
    ValueSemantics semantics;
};

Choices<PortType> input_types(const ToolRegistry& registry)
{
    Choices<PortType> types;
    for (const auto type : {DataType::boolean(), DataType::integer(), DataType::real(), DataType::string(),
        DataType::binary(), DataType::image(), DataType::resource_reference()}) {
        types.emplace_back(type_text(type), PortType{type, {}});
    }
    const auto append = [&](const auto& item) {
        const bool known = std::ranges::any_of(types, [&](const auto& choice) {
            return choice.second.type == item.type && choice.second.semantics == item.semantics;
        });
        if (!known) {
            types.emplace_back(QStringLiteral("%1 • %2").arg(type_text(item.type), qtext(item.display_name)),
                PortType{item.type, item.semantics});
        }
    };
    for (const auto& tool : registry.descriptors()) {
        for (const auto& parameter : tool.parameters) { append(parameter); }
        for (const auto& input : tool.inputs) { append(input); }
        for (const auto& output : tool.outputs) { append(output); }
    }
    return types;
}

template<class Change>
bool edit_boundary(const InspectorContext& context, const WorkflowId& flow, const std::optional<NodeId>& scope,
    const QString& title, const Change& change)
{
    return context.edit_flow(flow, title, [&](Solution& solution, Workflow& workflow) -> Check {
        const auto boundary = boundary_at(workflow, scope);
        return boundary ? change(solution, workflow, *boundary)
            : error(ErrorCode::UnknownTarget, "The public interface no longer exists.");
    });
}

Check next_interface_version(const Solution& solution, const ToolRegistry& registry, PublicInterface& interface)
{
    // Copies may share a published schema. Give the changed shape its own version.
    auto latest = interface.version;
    const auto update = [&](const SchemaId& id, std::uint32_t version) {
        if (id == interface.id) { latest = std::max(latest, version); }
    };
    for (const auto& flow : solution.workflows) {
        update(flow.interface.id, flow.interface.version);
        for (const auto& node : flow.nodes) {
            if (const auto* group = std::get_if<GroupConfiguration>(&node.configuration)) {
                update(group->interface.id, group->interface.version);
            }
        }
    }
    for (const auto& tool : registry.descriptors()) { update(tool.schema.id, tool.schema.version); }
    if (latest == std::numeric_limits<std::uint32_t>::max()) {
        return error(ErrorCode::IdentityExhausted, "The public schema has no available next version.");
    }
    interface.version = latest + 1;
    return std::monostate{};
}

template<class Port>
std::optional<PortName> current_name(const Port* port)
{
    if (!port) { return std::nullopt; }
    return PortName{std::string(port->id.text()), port->display_name};
}

void public_input(const InspectorContext& context, const Workflow& flow, const std::optional<NodeId>& scope,
    const InputDescriptor* existing)
{
    const auto name = port_name(context.owner, QStringLiteral("Public input"), current_name(existing));
    if (!name) { return; }
    const auto id = std::get<InputId>(SchemaIdFactory::parse<InputId>(name->id));
    std::optional<PortType> type;
    if (!existing) {
        type = choose(context.owner, QStringLiteral("Public input"), QStringLiteral("Type"), input_types(context.registry));
        if (!type) { return; }
    }
    edit_boundary(context, flow.id, scope, QStringLiteral("Change public input"), [&](Solution& solution, Workflow&, Boundary boundary) -> Check {
        auto& inputs = boundary.interface.inputs;
        const auto current = std::ranges::find(inputs, id, &InputDescriptor::id);
        if (!type && current != inputs.end()) { current->display_name = name->display; return std::monostate{}; }
        if (current != inputs.end()) { return error(ErrorCode::DuplicateSchemaMember, "That public input ID already exists."); }
        if (!type) { return error(ErrorCode::UnknownTarget, "The public input no longer exists."); }
        if (const auto version = next_interface_version(solution, context.registry, boundary.interface);
            std::holds_alternative<Diagnostic>(version)) { return version; }
        BindingPolicy policy{{BindingSourceKind::RunOutput, BindingSourceKind::PublicInput, BindingSourceKind::GlobalRead}};
        if (type->type.kind() != TypeKind::Image && type->type.kind() != TypeKind::RasterMask) {
            policy.allowed_sources.push_back(BindingSourceKind::Constant);
        }
        if (type->type.kind() == TypeKind::ResourceReference) { policy.allowed_sources.push_back(BindingSourceKind::Resource); }
        inputs.push_back({id, name->display, type->type, true, {}, {}, std::move(policy), type->semantics});
        return std::monostate{};
    });
}

void public_output(const InspectorContext& context, const Workflow& flow, const std::optional<NodeId>& scope,
    const OutputDescriptor* existing)
{
    const auto name = port_name(context.owner, QStringLiteral("Public output"), current_name(existing));
    if (!name) { return; }
    const auto id = std::get<OutputId>(SchemaIdFactory::parse<OutputId>(name->id));
    const auto* group = group_at(flow, scope);
    const auto* loop = group && group->loop ? &*group->loop : nullptr;
    const auto& outputs = group ? group->outputs : flow.outputs;
    const auto mapped = std::ranges::find(outputs, id, &OutputMapping::output);
    const auto prior = mapped == outputs.end() ? std::nullopt : std::optional<BindingSource>{mapped->source};
    auto selected = choose_reference(context.owner, outputs_in(flow, context.registry, scope), prior ? &*prior : nullptr,
        [&](BindingSourceKind, const DataType& type, const ValueSemantics& semantics) -> Check {
            if (loop || !existing) { return std::monostate{}; }
            if (type != existing->type) { return error(ErrorCode::TypeMismatch, "The public output requires an exact type match."); }
            return check_semantics(semantics, existing->semantics);
        });
    if (!selected) { return; }
    auto output_type = selected->type;
    std::optional<LoopExport> reduction;
    if (loop) {
        const auto previous = std::ranges::find(loop->exports, id, &LoopExport::output);
        const bool numeric = output_type.kind() == TypeKind::Int64 || output_type.kind() == TypeKind::Float64;
        Choices<LoopExportMode> modes{{QStringLiteral("First"), LoopExportMode::First},
            {QStringLiteral("Last"), LoopExportMode::Last}, {QStringLiteral("Collect"), LoopExportMode::Collect}};
        if (numeric) { modes.emplace_back(QStringLiteral("Sum"), LoopExportMode::Sum); }
        const auto current = previous == loop->exports.end() ? modes.end()
            : std::ranges::find_if(modes, [&](const auto& item) { return item.second == previous->mode; });
        const auto mode = choose(context.owner, QStringLiteral("Loop output"), QStringLiteral("Export mode"),
            modes, current == modes.end() ? 0 : static_cast<std::size_t>(current - modes.begin()));
        if (!mode) { return; }
        reduction.emplace(LoopExport{id, *mode, {}});
        if (*mode == LoopExportMode::Collect) { output_type = DataType::list(output_type); }
        else if (*mode == LoopExportMode::Sum) {
            const auto zero = output_type.kind() == TypeKind::Int64 ? ConfigValue::integer(0)
                : std::get<ConfigValue>(ConfigValue::real(0.0));
            const auto* initial = previous != loop->exports.end() && previous->initial && previous->initial->type() == output_type
                ? &*previous->initial : &zero;
            reduction->initial = edit_constant(context.owner, {context.model.draft(), context.registry}, output_type,
                initial, QStringLiteral("Sum initial value"));
            if (!reduction->initial) { return; }
        } else {
            const Choices<bool> empty{{QStringLiteral("Require an item"), false}, {QStringLiteral("Return None"), true}};
            const auto allow = choose(context.owner, QStringLiteral("Loop output"), QStringLiteral("Empty loop"), empty,
                existing && existing->type == DataType::optional(output_type) ? 1 : 0);
            if (!allow) { return; }
            if (*allow) { output_type = DataType::optional(output_type); }
        }
    }
    auto source = std::get<OutputReference>(std::move(selected->source));
    edit_boundary(context, flow.id, scope, QStringLiteral("Change public output"), [&](Solution& solution, Workflow&, Boundary boundary) -> Check {
        auto& ports = boundary.interface.outputs;
        const auto declaration = std::ranges::find(ports, id, &OutputDescriptor::id);
        if (declaration == ports.end() || declaration->type != output_type
            || (loop && declaration->semantics != selected->semantics)) {
            if (const auto version = next_interface_version(solution, context.registry, boundary.interface);
                std::holds_alternative<Diagnostic>(version)) { return version; }
        }
        if (declaration != ports.end()) {
            if (!existing) { return error(ErrorCode::DuplicateSchemaMember, "That public output ID already exists."); }
            declaration->display_name = name->display;
            if (loop) {
                declaration->type = output_type;
                declaration->semantics = selected->semantics;
            }
        } else {
            ports.push_back({id, name->display, output_type, selected->semantics});
        }
        const auto export_at = std::ranges::find(boundary.outputs, id, &OutputMapping::output);
        if (export_at == boundary.outputs.end()) { boundary.outputs.push_back({id, std::move(source)}); }
        else { export_at->source = std::move(source); }
        if (boundary.loop && reduction) {
            auto& exports = boundary.loop->exports;
            const auto entry = std::ranges::find(exports, id, &LoopExport::output);
            if (entry == exports.end()) { exports.push_back(std::move(*reduction)); }
            else { *entry = std::move(*reduction); }
        }
        return std::monostate{};
    });
}

struct ControlSource {
    NodeId node;
    std::optional<ControlOutputId> outlet;
};

void public_control(const InspectorContext& context, const Workflow& flow, const std::optional<NodeId>& scope,
    const ControlOutputDescriptor* existing)
{
    const auto name = port_name(context.owner, QStringLiteral("Public control outlet"), current_name(existing));
    if (!name) { return; }
    const auto id = std::get<ControlOutputId>(SchemaIdFactory::parse<ControlOutputId>(name->id));
    const auto* group = group_at(flow, scope);
    const auto& exports = group ? group->control_exports : flow.control_exports;
    const auto prior = std::ranges::find(exports, id, &ControlOutputMapping::output);
    Choices<ControlSource> choices;
    std::size_t selected = 0;
    const auto add = [&](QString label, const NodeId& node, std::optional<ControlOutputId> outlet) {
        if (prior != exports.end() && prior->source == node && prior->source_output == outlet) { selected = choices.size(); }
        choices.emplace_back(std::move(label), ControlSource{node, std::move(outlet)});
    };
    for (const auto& node : flow.nodes) {
        if (parent_of(flow, node.id) != scope) { continue; }
        const auto title = QStringLiteral("%1 [%2]").arg(qtext(node.name), short_id(node.id));
        const auto* config = std::get_if<GroupConfiguration>(&node.configuration);
        const auto* tool = metadata(context.registry, node);
        if (!config && !tool) { continue; }
        add(QStringLiteral("%1 → Success").arg(title), node.id, std::nullopt);
        const auto& outlets = config ? config->interface.control_outputs : tool->control_outputs;
        for (const auto& outlet : outlets) {
            add(QStringLiteral("%1 → %2").arg(title, qtext(outlet.display_name)), node.id, outlet.id);
        }
    }
    const auto source = choose(context.owner, QStringLiteral("Public control outlet"), QStringLiteral("Source outlet"),
        choices, selected);
    if (!source) { return; }
    edit_boundary(context, flow.id, scope, QStringLiteral("Change public control outlet"),
        [&](Solution& solution, Workflow&, Boundary boundary) -> Check {
        auto& declarations = boundary.interface.control_outputs;
        const auto declaration = std::ranges::find(declarations, id, &ControlOutputDescriptor::id);
        if (declaration != declarations.end()) {
            if (!existing) { return error(ErrorCode::DuplicateSchemaMember, "That public control outlet ID already exists."); }
            declaration->display_name = name->display;
        } else {
            if (const auto version = next_interface_version(solution, context.registry, boundary.interface);
                std::holds_alternative<Diagnostic>(version)) { return version; }
            declarations.push_back({id, name->display});
        }
        const auto mapped = std::ranges::find(boundary.controls, id, &ControlOutputMapping::output);
        const ControlOutputMapping mapping{id, source->node, source->outlet};
        if (mapped == boundary.controls.end()) { boundary.controls.push_back(mapping); }
        else { *mapped = mapping; }
        return std::monostate{};
    });
}

enum class BoundaryKind { Input, Output, Control };

void edit_public_port(const InspectorContext& context, const WorkflowId& id, const std::optional<NodeId>& scope,
    BoundaryKind kind, const std::optional<PortId>& port)
{
    const auto* flow = find_workflow(context.model.draft(), id);
    if (!flow) { return; }
    const auto* group = group_at(*flow, scope);
    if (scope && !group) { return; }
    const auto& interface = group ? group->interface : flow->interface;
    switch (kind) {
    case BoundaryKind::Input: {
        const auto existing = port ? std::ranges::find(interface.inputs, std::get<InputId>(*port), &InputDescriptor::id)
            : interface.inputs.end();
        if (!port || existing != interface.inputs.end()) {
            public_input(context, *flow, scope, existing == interface.inputs.end() ? nullptr : &*existing);
        }
        break;
    }
    case BoundaryKind::Output: {
        const auto existing = port ? std::ranges::find(interface.outputs, std::get<OutputId>(*port), &OutputDescriptor::id)
            : interface.outputs.end();
        if (!port || existing != interface.outputs.end()) {
            public_output(context, *flow, scope, existing == interface.outputs.end() ? nullptr : &*existing);
        }
        break;
    }
    case BoundaryKind::Control: {
        const auto existing = port ? std::ranges::find(interface.control_outputs, std::get<ControlOutputId>(*port),
            &ControlOutputDescriptor::id) : interface.control_outputs.end();
        if (!port || existing != interface.control_outputs.end()) {
            public_control(context, *flow, scope, existing == interface.control_outputs.end() ? nullptr : &*existing);
        }
        break;
    }
    }
}

void remove_public_port(const InspectorContext& context, const WorkflowId& flow, const std::optional<NodeId>& scope,
    const PortId& port)
{
    edit_boundary(context, flow, scope, QStringLiteral("Remove public port"),
        [&](Solution& solution, Workflow& edited, Boundary boundary) -> Check {
        if (const auto version = next_interface_version(solution, context.registry, boundary.interface);
            std::holds_alternative<Diagnostic>(version)) { return version; }
        std::visit([&](const auto& id) {
            using Id = std::decay_t<decltype(id)>;
            if constexpr (std::is_same_v<Id, InputId>) {
                std::erase_if(boundary.interface.inputs, [&](const auto& input) { return input.id == id; });
                if (auto* owner = scope ? mutable_node(edited, *scope) : nullptr) {
                    std::erase_if(owner->bindings, [&](const Binding& binding) { return binding.target == BindingTarget{id}; });
                }
            } else if constexpr (std::is_same_v<Id, OutputId>) {
                std::erase_if(boundary.interface.outputs, [&](const auto& output) { return output.id == id; });
                std::erase_if(boundary.outputs, [&](const auto& entry) { return entry.output == id; });
                if (boundary.loop) { std::erase_if(boundary.loop->exports, [&](const LoopExport& item) { return item.output == id; }); }
            } else {
                std::erase_if(boundary.interface.control_outputs, [&](const auto& control) { return control.id == id; });
                std::erase_if(boundary.controls, [&](const auto& entry) { return entry.output == id; });
                if (scope) {
                    std::erase_if(edited.control_links, [&](const ControlLink& link) {
                        return link.source == *scope && link.output == id;
                    });
                }
            }
        }, port);
        return std::monostate{};
    });
}

void edit_loop(const InspectorContext& context, const WorkflowId& workflow, const NodeId& scope)
{
    const auto* flow = find_workflow(context.model.draft(), workflow);
    const auto* group = flow ? group_at(*flow, scope) : nullptr;
    if (!group || !group->loop) { return; }
    const auto& loop = *group->loop;
    QDialog dialog(context.owner);
    dialog.setWindowTitle(QStringLiteral("Loop settings"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* step = new QLineEdit(QString::number(loop.step), &dialog);
    step->setObjectName(QStringLiteral("loopStep"));
    auto* inclusive = new QCheckBox(QStringLiteral("Include end value"), &dialog);
    inclusive->setObjectName(QStringLiteral("loopInclusiveEnd"));
    inclusive->setChecked(loop.inclusive_end);
    auto* interval = new QLineEdit(QString::number(loop.interval_ms), &dialog);
    interval->setObjectName(QStringLiteral("loopInterval"));
    auto* stop = new QComboBox(&dialog);
    stop->setObjectName(QStringLiteral("loopBreakSource"));
    std::vector<std::optional<NodeId>> stops{std::nullopt};
    stop->addItem(QStringLiteral("None"));
    for (const auto& node : flow->nodes) {
        if (parent_of(*flow, node.id) != scope) { continue; }
        stops.push_back(node.id);
        stop->addItem(QStringLiteral("%1 [%2]").arg(qtext(node.name), short_id(node.id)));
        if (loop.break_source == node.id) { stop->setCurrentIndex(stop->count() - 1); }
    }
    form->addRow(QStringLiteral("Step"), step);
    form->addRow(inclusive);
    form->addRow(QStringLiteral("Interval (ms)"), interval);
    form->addRow(QStringLiteral("Exit after node succeeds"), stop);
    layout->addLayout(form);
    close_buttons(dialog, *layout);
    while (dialog.exec() == QDialog::Accepted) {
        bool step_ok{}, interval_ok{};
        const auto step_value = step->text().toLongLong(&step_ok);
        const auto interval_value = interval->text().toLongLong(&interval_ok);
        if (!step_ok || step_value == 0 || !interval_ok || interval_value < 0) {
            warn(&dialog, QStringLiteral("Enter a nonzero integer step and a nonnegative interval in milliseconds."));
            continue;
        }
        const auto break_source = stops.at(static_cast<std::size_t>(stop->currentIndex()));
        if (loop.step == step_value && loop.interval_ms == interval_value && loop.inclusive_end == inclusive->isChecked()
            && loop.break_source == break_source) { return; }
        context.edit_node(workflow, scope, QStringLiteral("Change loop settings"), [&](Solution&, Workflow&, Node& node) -> Check {
            auto& setting = *std::get<GroupConfiguration>(node.configuration).loop;
            setting.step = step_value;
            setting.interval_ms = interval_value;
            setting.inclusive_end = inclusive->isChecked();
            setting.break_source = break_source;
            return std::monostate{};
        });
        break;
    }
}

bool reserved_loop_input(const PortId& port)
{
    const auto* input = std::get_if<InputId>(&port);
    return input && (input->text() == "start" || input->text() == "end" || input->text() == "index");
}

void boundary_controls(QVBoxLayout& panel, const InspectorContext& context, const Workflow& flow,
    const std::optional<NodeId>& scope)
{
    const auto* config = group_at(flow, scope);
    const bool loop = config && config->loop;
    auto* group = section(panel, loop ? QStringLiteral("Loop interface")
        : scope ? QStringLiteral("Group public interface") : QStringLiteral("Workflow public interface"));
    auto* layout = new QVBoxLayout(group);
    if (loop) {
        auto* settings = action(group, QStringLiteral("Loop settings…"), "editLoopSettings",
            [context, id = flow.id, scope] { edit_loop(context, id, *scope); });
        layout->addWidget(settings);
    }
    auto* ports = new QListWidget(group);
    ports->setObjectName(QStringLiteral("publicPorts"));
    ports->setMaximumHeight(150);
    layout->addWidget(ports);
    const auto& interface = config ? config->interface : flow.interface;
    const auto& outputs = config ? config->outputs : flow.outputs;
    const auto& controls = config ? config->control_exports : flow.control_exports;
    std::vector<std::pair<BoundaryKind, PortId>> ids;
    for (const auto& input : interface.inputs) {
        ports->addItem(QStringLiteral("Input • %1 [%2]").arg(qtext(input.display_name), id_text(input.id)));
        ports->item(ports->count() - 1)->setToolTip(type_text(input.type));
        ids.emplace_back(BoundaryKind::Input, input.id);
    }
    for (const auto& output : interface.outputs) {
        auto label = QStringLiteral("Output • %1 [%2]").arg(qtext(output.display_name), id_text(output.id));
        if (loop) {
            const auto& exports = config->loop->exports;
            const auto item = std::ranges::find(exports, output.id, &LoopExport::output);
            if (item != exports.end()) {
                switch (item->mode) {
                case LoopExportMode::First: label += QStringLiteral(" · First"); break;
                case LoopExportMode::Last: label += QStringLiteral(" · Last"); break;
                case LoopExportMode::Collect: label += QStringLiteral(" · Collect"); break;
                case LoopExportMode::Sum: label += QStringLiteral(" · Sum"); break;
                }
            }
        }
        ports->addItem(label);
        const auto mapping = std::ranges::find(outputs, output.id, &OutputMapping::output);
        const auto target = mapping == outputs.end() ? QStringLiteral("Unmapped") : source_text(context.model.draft(), mapping->source);
        ports->item(ports->count() - 1)->setToolTip(QStringLiteral("%1\n%2").arg(type_text(output.type), target));
        ids.emplace_back(BoundaryKind::Output, output.id);
    }
    for (const auto& control : interface.control_outputs) {
        ports->addItem(QStringLiteral("Control • %1 [%2]").arg(qtext(control.display_name), id_text(control.id)));
        const auto mapping = std::ranges::find(controls, control.id, &ControlOutputMapping::output);
        const auto* source = mapping == controls.end() ? nullptr : find_node(flow, mapping->source);
        auto description = QStringLiteral("Unmapped");
        if (source) {
            const auto outlet = mapping->source_output ? id_text(*mapping->source_output) : QStringLiteral("Success");
            description = QStringLiteral("%1 → %2").arg(qtext(source->name), outlet);
        }
        ports->item(ports->count() - 1)->setToolTip(description);
        ids.emplace_back(BoundaryKind::Control, control.id);
    }
    if (!ids.empty()) { ports->setCurrentRow(0); }
    auto* add = new QHBoxLayout;
    auto* kind = new QComboBox(group);
    kind->setObjectName(QStringLiteral("publicPortKind"));
    kind->addItems({QStringLiteral("Input"), QStringLiteral("Output")});
    if (!loop) { kind->addItem(QStringLiteral("Control outlet")); }
    add->addWidget(kind, 1);
    add->addWidget(action(group, QStringLiteral("Add"), "addPublicPort", [context, flow_id = flow.id, scope, kind] {
        edit_public_port(context, flow_id, scope, static_cast<BoundaryKind>(kind->currentIndex()), std::nullopt);
    }));
    layout->addLayout(add);
    auto* edit = new QHBoxLayout;
    auto* change = action(group, QStringLiteral("Edit"), "editPublicPort", [context, flow_id = flow.id, scope, ports, ids] {
        if (ports->currentRow() < 0) { return; }
        const auto& [kind, port] = ids[static_cast<std::size_t>(ports->currentRow())];
        edit_public_port(context, flow_id, scope, kind, port);
    });
    auto* remove = action(group, QStringLiteral("Remove"), "removePublicPort", [context, flow_id = flow.id, scope, ports, ids] {
        if (ports->currentRow() < 0) { return; }
        remove_public_port(context, flow_id, scope, ids[static_cast<std::size_t>(ports->currentRow())].second);
    });
    const auto update_controls = [ids, loop, change, remove](int index) {
        const bool editable = index >= 0 && static_cast<std::size_t>(index) < ids.size()
            && !(loop && reserved_loop_input(ids[static_cast<std::size_t>(index)].second));
        change->setEnabled(editable);
        remove->setEnabled(editable);
    };
    QObject::connect(ports, &QListWidget::currentRowChanged, group, update_controls);
    update_controls(ports->currentRow());
    edit->addWidget(change);
    edit->addWidget(remove);
    layout->addLayout(edit);
}

} // namespace

EditorInspector::EditorInspector(ControlGraphModel& model, ControlScene& scene, const core::ToolRegistry& registry,
    QWidget* parent, HashFile hash_file)
    : QWidget(parent), model_(model), scene_(scene), registry_(registry), hash_file_(std::move(hash_file)), scroll_(new QScrollArea(this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    layout->addWidget(scroll_);
    connect(&scene_, &QGraphicsScene::selectionChanged, this, [this] { queue_refresh(); });
    connect(&model_, &QtNodes::AbstractGraphModel::modelReset, this, [this] { queue_refresh(); });
    refresh();
}

void EditorInspector::set_preview_image(std::optional<ImageHandle> image)
{
    preview_image_ = std::move(image);
}

void EditorInspector::queue_refresh()
{
    if (refresh_pending_) { return; }
    refresh_pending_ = true;
    QTimer::singleShot(0, this, [this] {
        refresh_pending_ = false;
        guarded(this, [this] { refresh(); });
    });
}

void EditorInspector::refresh()
{
    auto content = std::make_unique<QWidget>();
    auto* layout = new QVBoxLayout(content.get());
    const InspectorContext context{model_, scene_, registry_, this, preview_image_, hash_file_};
    const auto flow_id = model_.workflow_id();
    const auto* flow = flow_id ? find_workflow(model_.draft(), *flow_id) : nullptr;
    if (flow) {
        const auto selected = scene_.selected_nodes();
        const auto* node = selected.size() == 1 ? find_node(*flow, selected.front()) : nullptr;
        if (node) {
            node_properties(*layout, context, *flow, *node);
            if (const auto* configuration = std::get_if<GroupConfiguration>(&node->configuration)) {
                if (!configuration->interface.inputs.empty()) {
                    auto* group = section(*layout, QStringLiteral("Inputs"));
                    auto* form = new QFormLayout(group);
                    for (const auto& target : configuration->interface.inputs) {
                        if (configuration->loop && target.id.text() == "index") { continue; }
                        binding_row(*form, group, context, *flow, *node, target);
                    }
                }
                boundary_controls(*layout, context, *flow, node->id);
            } else { tool_settings(*layout, context, *flow, *node); }
        } else {
            workflow_properties(*layout, context, *flow);
            boundary_controls(*layout, context, *flow, model_.scope());
        }
    } else {
        layout->addWidget(new QLabel(QStringLiteral("Add or select a workflow."), content.get()));
    }
    layout->addStretch();
    if (auto* previous = scroll_->takeWidget()) {
        previous->hide();
        previous->deleteLater();
    }
    scroll_->setWidget(content.release());
}

} // namespace visionpro::ui
