#include "run_panel.hpp"

#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringList>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <map>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;

QString text(std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString id_text(const UuidIdentity auto& id) { return text(id.to_string()); }
QString id_text(const SchemaIdentity auto& id) { return text(id.text()); }
QString id_text(const RuntimeIdentity auto& id) { return QString::number(id.value()); }

template<class T>
QString id_text(const std::optional<T>& id) { return id ? id_text(*id) : QStringLiteral("—"); }

template<class Enum, std::size_t N>
QString enum_text(Enum value, const std::array<const char*, N>& names)
{
    const auto i = static_cast<std::size_t>(value);
    return i < names.size() ? QString::fromLatin1(names[i]) : QStringLiteral("Unknown");
}

QString status_text(ExecutionStatus status)
{
    return enum_text(status, std::array{"Pending", "Running", "Succeeded", "Failed", "Skipped", "Cancelled", "Timed out"});
}

QString input_origin_text(InputOrigin origin)
{
    return enum_text(origin, std::array{"Provided", "Default"});
}

QString run_identity_text(const RunIdentity& run)
{
    return QStringLiteral("Run %1 · Snapshot %2 · Session %3 · Workflow %4")
        .arg(id_text(run.run), id_text(run.snapshot.snapshot), id_text(run.snapshot.session), id_text(run.workflow));
}

QString invocation_text(const InvocationKey& call)
{
    return QStringLiteral("Node %1 · Scope %2 · Invocation %3")
        .arg(id_text(call.node), id_text(call.scope), id_text(call.invocation));
}

QString resource_text(const ResourceReference& resource)
{
    const auto kind = enum_text(resource.kind, std::array{
        "Image", "Mask", "Template", "Calibration", "Device", "Directory sequence", "Output path", "Binary"});
    return QStringLiteral("%1 %2 · Revision %3")
        .arg(kind, id_text(resource.id), resource.revision ? QString::number(*resource.revision) : QStringLiteral("—"));
}

QString selector_text(const Selector& selector)
{
    QString result;
    for (const auto& step : selector) {
        if (const auto* field = std::get_if<FieldSelection>(&step)) { result += QStringLiteral(".") + id_text(field->field); }
        else if (const auto* item = std::get_if<ElementSelection>(&step)) { result += QStringLiteral("[%1]").arg(item->index); }
        else { result += QStringLiteral("[index source]"); }
    }
    return result;
}

template<class... Visitors>
struct Overload : Visitors... { using Visitors::operator()...; };

QString source_text(const SourceTrace& source)
{
    const auto origin = std::visit(Overload{
        [](ConstantTrace value) { return value.defaulted ? QStringLiteral("Constant (Default)") : QStringLiteral("Constant"); },
        [](const ResultTrace& trace) {
            const auto& ref = trace.reference;
            const auto port = std::visit(Overload{
                [](const OutputId& output) { return id_text(output); },
                [](SystemField field) { return enum_text(field, std::array{"ExecutionStatus", "Judgement", "Diagnostics", "Elapsed"}); }
            }, ref.output);
            return QStringLiteral("Result %1%2 · Node %3 · Scope %4 · Invocation %5 · Attempt %6\n    %7")
                .arg(port, selector_text(ref.selector), id_text(ref.node), id_text(trace.scope),
                    id_text(trace.invocation), id_text(trace.attempt), run_identity_text(trace.run));
        },
        [](const PublicInputTrace& trace) {
            return QStringLiteral("Public input %1%2 (%3) · Scope %4 · Group %5\n    %6")
                .arg(id_text(trace.reference.input), selector_text(trace.reference.selector),
                    input_origin_text(trace.origin), id_text(trace.scope), id_text(trace.reference.group), run_identity_text(trace.run));
        },
        [](const IndependentInputTrace& trace) {
            return QStringLiteral("Independent input %1 (%2)\n    %3")
                .arg(id_text(trace.input), input_origin_text(trace.origin), run_identity_text(trace.run));
        },
        [](const GlobalReadTrace& trace) {
            const auto update = std::visit(Overload{
                [](const SessionInvocationKey& call) {
                    return QStringLiteral("Session %1 · Run %2 · %3")
                        .arg(id_text(call.session), id_text(call.key.workflow_run), invocation_text(call.key));
                },
                [](const ExternalUpdateOrigin& external) {
                    return QStringLiteral("Session %1 · External source %2").arg(id_text(external.session), id_text(external.source));
                }
            }, trace.update_origin);
            return QStringLiteral("Global %1%2 · Revision %3\n    %4")
                .arg(id_text(trace.variable), selector_text(trace.selector)).arg(trace.revision).arg(update);
        },
        [](const ResourceReference& ref) { return resource_text(ref); }
    }, source.origin);
    const auto accepted = source.accepted_absence
        ? QStringLiteral(" · Accepted absence %1").arg(enum_text(*source.accepted_absence,
            std::array{"Not selected", "Disabled", "Tool omitted output", "Condition unmet"})) : QString{};
    const auto role = source.selector_step ? QStringLiteral(" · Index at selector step %1 = %2").arg(*source.selector_step + 1)
        .arg(source.index_value ? QString::number(*source.index_value) : QStringLiteral("Unavailable")) : QString{};
    return QStringLiteral("  [%1]%2 %3%4").arg(source.source_index).arg(role, origin, accepted);
}

QString diagnostic_text(const Diagnostic& item)
{
    QStringList location;
    if (item.location.node) { location.append(QStringLiteral("node %1").arg(id_text(item.location.node))); }
    if (item.location.input) { location.append(QStringLiteral("input %1").arg(id_text(item.location.input))); }
    if (item.location.parameter) { location.append(QStringLiteral("parameter %1").arg(id_text(item.location.parameter))); }
    if (item.location.output) { location.append(QStringLiteral("output %1").arg(id_text(item.location.output))); }
    if (item.location.resource) { location.append(QStringLiteral("resource %1").arg(id_text(item.location.resource))); }
    const auto where = location.isEmpty() ? QString{} : QStringLiteral("\n  %1").arg(location.join(QStringLiteral(" · ")));
    return QStringLiteral("%1 [%2] %3%4")
        .arg(enum_text(item.severity, std::array{"Info", "Warning", "Error"}), text(error_code_name(item.code)), text(item.message), where);
}

template<ValueDomain Domain>
QString value_text(const BasicValue<Domain>& value, bool preview_list = true)
{
    if (const auto* content = value.as_bool()) { return *content ? QStringLiteral("true") : QStringLiteral("false"); }
    if (const auto* content = value.as_int64()) { return QString::number(*content); }
    if (const auto* content = value.as_float64()) { return QString::number(*content, 'g', 17); }
    if (const auto* content = value.as_string()) {
        constexpr std::size_t limit = 256;
        auto preview = text(std::string_view(*content).substr(0, limit));
        if (content->size() > limit) { preview += QStringLiteral("…"); }
        return preview;
    }
    if (const auto* content = value.as_enum()) { return id_text(*content); }
    if constexpr (Domain == ValueDomain::Data) {
        if (const auto* content = value.as_image()) {
            return QStringLiteral("Image %1 × %2 · %3").arg(content->image.width()).arg(content->image.height())
                .arg(id_text(content->image.coordinate_frame()));
        }
        if (const auto* content = value.as_raster_mask()) {
            return QStringLiteral("Raster mask %1 × %2 · %3").arg(content->pixels.width()).arg(content->pixels.height())
                .arg(id_text(content->pixels.coordinate_frame()));
        }
    }
    if (const auto* content = value.as_region2d()) { return QStringLiteral("ROI %1").arg(id_text(content->id)); }
    if (const auto* content = value.as_resource_reference()) { return resource_text(*content); }
    if (const auto* content = value.as_point2d()) {
        return QStringLiteral("Point (%1, %2) · %3")
            .arg(QString::number(content->x, 'g', 17), QString::number(content->y, 'g', 17), id_text(content->coordinate_frame));
    }
    if (const auto* list = value.elements()) {
        auto summary = QStringLiteral("List (%1 items)").arg(list->size());
        if (!preview_list || list->empty()) { return summary; }
        constexpr std::size_t limit = 8;
        QStringList items;
        for (std::size_t i = 0; i < std::min(list->size(), limit); ++i) { items.append(value_text((*list)[i], false)); }
        if (list->size() > limit) { items.append(QStringLiteral("…")); }
        return summary + QStringLiteral(" [") + items.join(QStringLiteral(", ")) + QStringLiteral("]");
    }
    if (const auto* bytes = value.as_binary()) { return QStringLiteral("Binary (%1 bytes)").arg(bytes->size()); }
    if (value.type().kind() == TypeKind::Optional) {
        const auto* item = value.optional_value();
        return item ? value_text(*item, preview_list) : QStringLiteral("None");
    }
    if (value.type().schema_id()) { return id_text(*value.type().schema_id()); }
    return enum_text(value.type().kind(), std::array{"Bool", "Int64", "Float64", "String", "Binary", "Enum",
        "Record", "List", "Optional", "Image", "Point2D", "Line2D", "Circle2D", "Pose2D", "Transform2D", "Region2D",
        "RasterMask", "ResourceReference"});
}

QString output_text(const OutputSlot& output)
{
    const auto value = std::visit(Overload{
        [](const DataValue& data) { return value_text(data); },
        [](NotProduced missing) {
            return QStringLiteral("Not produced (%1)").arg(enum_text(missing.reason,
                std::array{"Not selected", "Disabled", "Tool omitted output", "Failed", "Cancelled", "Timed out", "Condition unmet"}));
        },
        [](const InvalidOutput& invalid) { return QStringLiteral("Invalid · %1").arg(diagnostic_text(invalid.diagnostic)); }
    }, output.content);
    return QStringLiteral("%1 = %2").arg(id_text(output.id), value);
}

bool is_image(const OutputSlot& output)
{
    const auto* data = std::get_if<DataValue>(&output.content);
    return data && data->as_image();
}

QLabel* panel_label(QWidget* parent, const char* name)
{
    auto* label = new QLabel(parent);
    label->setObjectName(QString::fromLatin1(name));
    label->setTextFormat(Qt::PlainText);
    return label;
}

void set_text(QLabel& label, const QString& value)
{
    if (label.text() != value) { label.setText(value); }
}

template<class Value>
QString shared_summary(const Value& value)
{
    const auto* fields = value.fields();
    if (!fields) { return value_text(value); }
    QStringList parts;
    constexpr std::size_t limit = 8;
    for (std::size_t i = 0; i < std::min(limit, fields->size()); ++i) {
        const auto& field = (*fields)[i];
        parts.append(id_text(field.id) + QStringLiteral("=") + value_text(field.value));
    }
    if (fields->size() > limit) { parts.append(QStringLiteral("…")); }
    return value_text(value) + QStringLiteral(" {") + parts.join(QStringLiteral(", ")) + QStringLiteral("}");
}
} // namespace

QString shared_value_text(const core::DataValue& value) { return shared_summary(value); }
QString shared_value_text(const core::ConfigValue& value) { return shared_summary(value); }

RunPanel::RunPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("runPanel"));
    auto* layout = new QVBoxLayout(this);
    auto* history_row = new QHBoxLayout;
    layout->addLayout(history_row);
    history_ = new QComboBox(this);
    history_->setObjectName(QStringLiteral("runHistory"));
    history_->setMinimumContentsLength(16);
    history_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    auto* history_label = new QLabel(QStringLiteral("Run"), this);
    history_label->setBuddy(history_);
    history_row->addWidget(history_label);
    history_row->addWidget(history_, 1);
    notifications_ = panel_label(this, "runNotifications");
    history_row->addWidget(notifications_);

    identity_ = panel_label(this, "runIdentity");
    identity_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    identity_->setWordWrap(true);
    layout->addWidget(identity_);

    auto* status_row = new QHBoxLayout;
    layout->addLayout(status_row);
    status_ = panel_label(this, "runStatus");
    completion_ = panel_label(this, "runCompletion");
    cleanup_ = panel_label(this, "runCleanup");
    status_row->addWidget(status_);
    status_row->addWidget(completion_);
    status_row->addWidget(cleanup_, 1);

    auto* splitter = new QSplitter(this);
    layout->addWidget(splitter, 1);
    nodes_ = new QTreeWidget(splitter);
    nodes_->setObjectName(QStringLiteral("runNodes"));
    nodes_->setHeaderLabels({QStringLiteral("Node"), QStringLiteral("Status"), QStringLiteral("Iteration")});
    nodes_->setRootIsDecorated(false);
    nodes_->setUniformRowHeights(true);
    nodes_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    nodes_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    nodes_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    nodes_->setColumnHidden(2, true);

    auto* call = new QWidget(splitter);
    auto* call_layout = new QVBoxLayout(call);
    call_layout->setContentsMargins(0, 0, 0, 0);
    auto* result_row = new QHBoxLayout;
    call_layout->addLayout(result_row);
    judgement_ = panel_label(call, "runJudgement");
    result_row->addWidget(judgement_);
    outputs_ = new QComboBox(call);
    outputs_->setObjectName(QStringLiteral("runOutputs"));
    outputs_->setMinimumContentsLength(10);
    outputs_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    auto* output_label = new QLabel(QStringLiteral("Output"), call);
    output_label->setBuddy(outputs_);
    result_row->addWidget(output_label);
    result_row->addWidget(outputs_, 1);
    show_image_ = new QPushButton(QStringLiteral("Show Image"), call);
    show_image_->setObjectName(QStringLiteral("showRunImage"));
    result_row->addWidget(show_image_);
    details_ = new QPlainTextEdit(call);
    details_->setObjectName(QStringLiteral("runDetails"));
    details_->setReadOnly(true);
    details_->setMinimumHeight(90);
    call_layout->addWidget(details_, 1);
    splitter->setStretchFactor(1, 2);

    connect(history_, &QComboBox::currentIndexChanged, this, &RunPanel::select_history);
    connect(nodes_, &QTreeWidget::currentItemChanged, this, &RunPanel::refresh_call);
    connect(outputs_, &QComboBox::currentIndexChanged, this, &RunPanel::refresh_image_button);
    connect(show_image_, &QPushButton::clicked, this, &RunPanel::image_requested);
    clear();
}

void RunPanel::update_runs(std::span<const RunHandle> runs, std::uint64_t dropped)
{
    try {
        set_text(*notifications_, QStringLiteral("Dropped notifications: %1").arg(dropped));
        if (!std::ranges::equal(runs_, runs, {}, &RunHandle::identity, &RunHandle::identity)) {
            runs_.assign(runs.begin(), runs.end());
            rebuild_history();
        }
        refresh_run();
    } catch (...) { show_refresh_failure(); }
}

void RunPanel::clear()
{
    try {
        runs_.clear();
        pinned_run_.reset();
        rebuild_history();
        update_runs({}, 0);
    } catch (...) { show_refresh_failure(); }
}

std::optional<RunHandle> RunPanel::selected_run() const
{
    if (pinned_run_) { return pinned_run_; }
    return runs_.empty() ? std::nullopt : std::optional{runs_.back()};
}

const RunNodeProgress* RunPanel::selected_node() const
{
    auto* item = nodes_->currentItem();
    if (!progress_ || !item) { return nullptr; }
    const auto row = nodes_->indexOfTopLevelItem(item);
    if (row < 0 || static_cast<std::size_t>(row) >= progress_->nodes.size()) { return nullptr; }
    return &progress_->nodes[static_cast<std::size_t>(row)];
}

std::optional<InvocationKey> RunPanel::selected_invocation() const
{
    const auto* node = selected_node();
    return node ? node->invocation : std::nullopt;
}

std::optional<std::int64_t> RunPanel::selected_iteration_index() const
{
    const auto* node = selected_node();
    if (!node || !node->scope) { return std::nullopt; }
    const auto iteration = std::ranges::find(progress_->iterations, *node->scope, &RunLoopIteration::scope);
    return iteration == progress_->iterations.end() ? std::nullopt : std::optional{iteration->index};
}

QString RunPanel::selected_scope_path() const
{
    const auto* node = selected_node();
    return node && node->scope ? nodes_->currentItem()->text(2) : QString{};
}

std::optional<OutputId> RunPanel::selected_output() const
{
    const int row = outputs_->currentIndex();
    if (!displayed_result_ || row < 0 || static_cast<std::size_t>(row) >= displayed_result_->data().outputs.size()) {
        return std::nullopt;
    }
    return displayed_result_->data().outputs[static_cast<std::size_t>(row)].id;
}

void RunPanel::rebuild_history()
{
    const QSignalBlocker blocker(history_);
    history_->clear();
    history_->addItem(QStringLiteral("Latest run"));
    int selection = 0;
    const auto add_run = [&](const RunHandle& run, bool retired) {
        const auto* workflow = find_workflow(run.request().snapshot().solution(), run.identity().workflow);
        const auto name = workflow ? text(workflow->name) : id_text(run.identity().workflow);
        QString kind;
        if (const auto* target = std::get_if<NodeTarget>(&run.request().target())) {
            const auto* node = workflow ? find_node(*workflow, target->node) : nullptr;
            if (node && std::holds_alternative<GroupConfiguration>(node->configuration)) {
                kind = run.request().debug_state() == DebugStatePolicy::Active ? QStringLiteral(" · Group run") : QStringLiteral(" · Group debug");
            } else { kind = QStringLiteral(" · Node debug"); }
        }
        const auto suffix = retired ? QStringLiteral(" (outside history)") : QString{};
        const int row = history_->count();
        history_->addItem(QStringLiteral("Run %1 · %2%3%4").arg(id_text(run.identity().run), name, kind, suffix));
        history_->setItemData(row, run_identity_text(run.identity()), Qt::ToolTipRole);
        if (pinned_run_ && pinned_run_->identity() == run.identity()) { selection = row; }
    };
    for (auto it = runs_.rbegin(); it != runs_.rend(); ++it) { add_run(*it, false); }
    if (pinned_run_ && selection == 0) { add_run(*pinned_run_, true); }
    history_->setCurrentIndex(selection);
    history_->setEnabled(!runs_.empty() || pinned_run_.has_value());
}

void RunPanel::select_history(int row)
{
    try {
        const bool retired_row = static_cast<std::size_t>(history_->count()) > runs_.size() + 1;
        if (row <= 0) { pinned_run_.reset(); }
        else if (static_cast<std::size_t>(row) <= runs_.size()) {
            pinned_run_ = runs_[runs_.size() - static_cast<std::size_t>(row)];
        }
        if (retired_row && row < history_->count() - 1) { rebuild_history(); }
        refresh_run();
        emit selected_run_changed();
    } catch (...) { show_refresh_failure(); }
}

void RunPanel::refresh_run()
{
    const auto run = selected_run();
    if (!run) {
        reset_display();
        identity_->clear();
        status_->clear();
        completion_->clear();
        cleanup_->clear();
        show_details(QStringLiteral("No accepted run."));
        return;
    }

    const bool same_run = displayed_run_ == run->identity();
    if (same_run && progress_ && progress_->quiescent) { return; }
    const auto* old_node = same_run ? selected_node() : nullptr;
    const auto& solution = run->request().snapshot().solution();
    const auto* flow = find_workflow(solution, run->identity().workflow);
    set_text(*identity_, QStringLiteral("%1 · Revision %2").arg(run_identity_text(run->identity())).arg(solution.revision));
    if (!same_run) { reset_display(); }

    auto inspected = run->inspect();
    if (const auto* error = std::get_if<Diagnostic>(&inspected)) {
        reset_display();
        set_text(*status_, QStringLiteral("Status unavailable"));
        completion_->clear();
        cleanup_->clear();
        show_details(diagnostic_text(*error));
        return;
    }
    auto progress = std::get<RunProgress>(std::move(inspected));
    displayed_run_ = run->identity();
    const bool queued = run->admission() == RunAdmission::Queued && progress.status == ExecutionStatus::Pending;
    set_text(*status_, QStringLiteral("Status: %1").arg(queued ? QStringLiteral("Queued") : status_text(progress.status)));
    const bool pending = progress.status == ExecutionStatus::Pending || progress.status == ExecutionStatus::Running;
    set_text(*completion_, pending ? QStringLiteral("Logical completion: Pending") : QStringLiteral("Logical completion: Complete"));
    set_text(*cleanup_, progress.quiescent ? QStringLiteral("Cleanup: Quiescent") : QStringLiteral("Cleanup: Pending"));

    {
        const QSignalBlocker blocker(nodes_);
        nodes_->setColumnHidden(2, progress.iterations.empty() && progress.groups.empty());
        nodes_->headerItem()->setText(2, progress.groups.empty() ? QStringLiteral("Iteration") : QStringLiteral("Scope"));
        std::map<ExecutionScopeId, std::pair<ExecutionScopeId, QString>> scopes;
        for (const auto& iteration : progress.iterations) {
            scopes.emplace(iteration.scope, std::pair{iteration.loop.scope, QString::number(iteration.index)});
        }
        for (const auto& group : progress.groups) {
            const auto* owner = flow ? find_node(*flow, group.group.node) : nullptr;
            scopes.emplace(group.scope, std::pair{group.group.scope, owner ? text(owner->name) : id_text(group.group.node)});
        }
        std::map<ExecutionScopeId, QString> labels;
        for (const auto& [scope, owner] : scopes) {
            QStringList path{owner.second};
            auto ancestor = scopes.find(owner.first);
            while (ancestor != scopes.end() && path.size() <= static_cast<qsizetype>(scopes.size())) {
                path.prepend(ancestor->second.second);
                ancestor = scopes.find(ancestor->second.first);
            }
            labels.emplace(scope, QStringLiteral("%1 · Scope %2").arg(path.join(QStringLiteral(" / ")), id_text(scope)));
        }
        const auto count = static_cast<int>(progress.nodes.size());
        while (nodes_->topLevelItemCount() > count) { delete nodes_->takeTopLevelItem(nodes_->topLevelItemCount() - 1); }
        const int previous_count = nodes_->topLevelItemCount();
        int selected_row = 0;
        for (int row = 0; row < count; ++row) {
            const auto index = static_cast<std::size_t>(row);
            const auto& node = progress.nodes[index];
            auto* item = row < previous_count ? nodes_->topLevelItem(row) : new QTreeWidgetItem(nodes_);
            if (row >= previous_count || !progress_ || progress_->nodes[index].node != node.node) {
                const auto* definition = flow ? find_node(*flow, node.node) : nullptr;
                item->setText(0, definition ? text(definition->name) : id_text(node.node));
                item->setToolTip(0, id_text(node.node));
            }
            const auto state = status_text(node.status);
            if (state != item->text(1)) { item->setText(1, state); }
            const auto scope = node.scope ? labels.find(*node.scope) : labels.end();
            const auto label = scope == labels.end() ? QString{} : scope->second;
            if (label != item->text(2)) { item->setText(2, label); }
            if (old_node && old_node->node == node.node && old_node->scope == node.scope
                && (!old_node->invocation || old_node->invocation == node.invocation)) {
                selected_row = row;
            }
        }
        auto* selection = count ? nodes_->topLevelItem(selected_row) : nullptr;
        if (selection != nodes_->currentItem()) { nodes_->setCurrentItem(selection); }
    }
    progress_ = std::move(progress);
    refresh_call();
}

void RunPanel::refresh_call()
{
    try {
        const auto* node = selected_node();
        const auto run = selected_run();
        if (!progress_ || !run) { return; }

        QStringList lines;
        const auto& request = run->request();
        const auto kind = request.trigger().kind;
        if (kind == TriggerKind::External || kind == TriggerKind::Continuous) {
            lines.append(QStringLiteral("%1 trigger · %2 · %3").arg(kind == TriggerKind::External ? QStringLiteral("External")
                : QStringLiteral("Continuous"), id_text(request.trigger().source),
                run->admission() == RunAdmission::Queued ? QStringLiteral("Queued at admission") : QStringLiteral("Immediate admission")));
            if (const auto& id = request.correlation()) {
                lines.append(QStringLiteral("Correlation · %1 / %2").arg(id_text(id->domain()), text(id->key())));
            }
        }
        if (const auto* target = std::get_if<NodeTarget>(&request.target())) {
            const auto policy = request.debug_state() == DebugStatePolicy::Active ? QStringLiteral("Active") : QStringLiteral("Isolated");
            lines.append(QStringLiteral("%1 %2 · State policy %3").arg(request.debug_state() == DebugStatePolicy::Active
                ? QStringLiteral("Run target") : QStringLiteral("Debug target"), id_text(target->node), policy));
        }
        if (!request.inputs().empty()) {
            lines.append(QStringLiteral("Run inputs"));
            for (const auto& input : request.inputs()) {
                lines.append(QStringLiteral("%1 (%2) = %3").arg(id_text(input.id), input_origin_text(input.origin), value_text(input.value)));
            }
        }
        const bool ready = node && node->invocation && node->status != ExecutionStatus::Pending && node->status != ExecutionStatus::Running;
        if (!ready || !displayed_result_ || displayed_result_->data().identity.invocation != *node->invocation) {
            const QSignalBlocker blocker(outputs_);
            displayed_result_.reset();
            outputs_->clear();
            if (ready) {
                auto result = run->lookup(*node->invocation);
                if (const auto* error = std::get_if<Diagnostic>(&result)) { lines.append(diagnostic_text(*error)); }
                else { displayed_result_ = std::get<std::optional<NodeResult>>(std::move(result)); }
            }
            if (displayed_result_) {
                int image_row = -1;
                for (const auto& output : displayed_result_->data().outputs) {
                    if (image_row < 0 && is_image(output)) { image_row = outputs_->count(); }
                    outputs_->addItem(id_text(output.id));
                }
                if (image_row >= 0) { outputs_->setCurrentIndex(image_row); }
            }
        }
        outputs_->setEnabled(outputs_->count() > 0);
        if (node) {
            lines.append(node->invocation ? invocation_text(*node->invocation) : QStringLiteral("Node %1").arg(id_text(node->node)));
            if (node->scope) {
                const auto owner = std::ranges::find(progress_->groups, *node->scope, &RunGroupCall::scope);
                if (owner != progress_->groups.end()) {
                    lines.append(QStringLiteral("Group call · %1").arg(invocation_text(owner->group)));
                }
                const auto iteration = std::ranges::find(progress_->iterations, *node->scope, &RunLoopIteration::scope);
                if (iteration != progress_->iterations.end()) {
                    lines.append(QStringLiteral("Loop index %1 · %2").arg(iteration->index).arg(invocation_text(iteration->loop)));
                    for (const auto& exported : iteration->exports) {
                        if (exported.producer.invocation == node->invocation) {
                            lines.append(QStringLiteral("Loop output %1 · Attempt %2")
                                .arg(id_text(exported.output), id_text(exported.producer.attempt)));
                        }
                    }
                }
            }
            if (node->skip_reason) {
                lines.append(QStringLiteral("Skip reason: %1").arg(enum_text(*node->skip_reason,
                    std::array{"Disabled", "Upstream closed", "Fail fast", "Run cancelled", "Run timed out", "Not selected"})));
            }
            if (!node->bindings.empty()) { lines.append(QStringLiteral("Binding sources")); }
            for (const auto& binding : node->bindings) {
                lines.append(std::visit([](const auto& target) { return id_text(target); }, binding.target));
                for (const auto& source : binding.sources) { lines.append(source_text(source)); }
                for (const auto& region : binding.regions) {
                    const auto& identity = region.identity;
                    lines.append(QStringLiteral("  ROI %1 from %2 · %3 · Attempt %4\n    %5")
                        .arg(id_text(region.value.id), id_text(region.reference.output), invocation_text(identity.invocation),
                            id_text(identity.attempt), run_identity_text(identity.run)));
                }
            }
            if (!node->resources.empty()) { lines.append(QStringLiteral("Acquired resources")); }
            for (const auto& resource : node->resources) {
                lines.append(QStringLiteral("%1 · %2").arg(id_text(resource.slot), resource_text(resource.reference)));
            }
        }
        const auto reported = displayed_result_ ? enum_text(displayed_result_->data().judgement, std::array{"Not evaluated", "OK", "NG"})
            : QStringLiteral("—");
        set_text(*judgement_, QStringLiteral("Judgement: %1").arg(reported));
        if (displayed_result_) {
            const auto& result = displayed_result_->data();
            lines.append(QStringLiteral("Attempt %1").arg(id_text(result.identity.attempt)));
            if (result.elapsed) { lines.append(QStringLiteral("Elapsed: %1 ns").arg(result.elapsed->count())); }
            if (!result.outputs.empty()) { lines.append(QStringLiteral("Outputs")); }
            for (const auto& output : result.outputs) { lines.append(output_text(output)); }
            if (!result.selected_control_outputs.empty()) {
                QStringList outlets;
                for (const auto& port : result.selected_control_outputs) { outlets.append(id_text(port)); }
                lines.append(QStringLiteral("Selected control outlets: %1").arg(outlets.join(QStringLiteral(", "))));
            }
            const auto group = std::ranges::find(progress_->groups, result.identity.invocation, &RunGroupCall::group);
            if (group != progress_->groups.end()) {
                for (const auto& exported : group->exports) {
                    lines.append(QStringLiteral("Group output %1 · %2 · Attempt %3")
                        .arg(id_text(exported.output), invocation_text(exported.producer.invocation), id_text(exported.producer.attempt)));
                }
            }
            if (!result.diagnostics.empty()) { lines.append(QStringLiteral("Node diagnostics")); }
            for (const auto& item : result.diagnostics) { lines.append(diagnostic_text(item)); }
        }
        if (!progress_->diagnostics.empty()) { lines.append(QStringLiteral("Run diagnostics")); }
        for (const auto& item : progress_->diagnostics) { lines.append(diagnostic_text(item)); }
        show_details(lines.join(QLatin1Char('\n')));
        refresh_image_button();
    } catch (...) { show_refresh_failure(); }
}

void RunPanel::refresh_image_button()
{
    try {
        bool available = false;
        if (displayed_result_ && outputs_->currentIndex() >= 0) {
            const auto& result = displayed_result_->data();
            const auto index = static_cast<std::size_t>(outputs_->currentIndex());
            if (index < result.outputs.size()) { available = is_image(result.outputs[index]); }
            if (const auto run = selected_run(); !available && run && result.status == ExecutionStatus::Succeeded) {
                const auto* flow = find_workflow(run->request().snapshot().solution(), run->identity().workflow);
                const auto* node = flow ? find_node(*flow, result.identity.invocation.node) : nullptr;
                const auto* tool = node ? std::get_if<ToolConfiguration>(&node->configuration) : nullptr;
                available = tool && tool->type.text() == "visionpro.image.blob";
            }
        }
        show_image_->setEnabled(available);
    } catch (...) { show_refresh_failure(); }
}

void RunPanel::reset_display()
{
    const QSignalBlocker node_signals(nodes_);
    const QSignalBlocker output_signals(outputs_);
    displayed_run_.reset();
    progress_.reset();
    displayed_result_.reset();
    nodes_->clear();
    outputs_->clear();
    outputs_->setEnabled(false);
    judgement_->setText(QStringLiteral("Judgement: —"));
    show_image_->setEnabled(false);
}

void RunPanel::show_details(const QString& value)
{
    if (value == details_text_) { return; }
    const auto scroll = details_->verticalScrollBar()->value();
    details_->setPlainText(value);
    details_->verticalScrollBar()->setValue(scroll);
    details_text_ = value;
}

void RunPanel::show_refresh_failure()
{
    reset_display();
    status_->setText(QStringLiteral("Status unavailable"));
    completion_->clear();
    cleanup_->clear();
    show_details(QStringLiteral("Could not refresh the selected run."));
}

} // namespace visionpro::ui
