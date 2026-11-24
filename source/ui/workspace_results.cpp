#include "workspace_results.hpp"

#include "coordinate_text.hpp"
#include "data_type_text.hpp"
#include "image_panel.hpp"
#include "module_history_panel.hpp"
#include "run_image.hpp"
#include "run_panel.hpp"
#include "toolbox_catalog.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;

QLabel* text_page(QWidget* parent)
{
    auto* label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    label->setWordWrap(true);
    label->setMargin(8);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    return label;
}

QTabBar* tabs(QWidget* parent, const char* name, const QStringList& labels)
{
    auto* bar = new QTabBar(parent);
    bar->setObjectName(QString::fromLatin1(name));
    bar->setDocumentMode(true);
    bar->setExpanding(false);
    for (const auto& label : labels) { bar->addTab(label); }
    return bar;
}

std::span<const OutputDescriptor> declarations(const Node* node, const ToolDescriptor* tool)
{
    if (node) {
        if (const auto* group = std::get_if<GroupConfiguration>(&node->configuration)) { return group->interface.outputs; }
    }
    return tool ? std::span<const OutputDescriptor>(tool->outputs) : std::span<const OutputDescriptor>{};
}

bool image_type(const DataType& type)
{
    return type.kind() == TypeKind::Image || type.kind() == TypeKind::RasterMask || type == class_map_type();
}

QString absence(OutputAbsenceReason reason)
{
    switch (reason) {
    case OutputAbsenceReason::NotSelected: return QObject::tr("分支未选中");
    case OutputAbsenceReason::Disabled: return QObject::tr("模块已禁用");
    case OutputAbsenceReason::ToolDidNotProduce: return QObject::tr("工具未产出");
    case OutputAbsenceReason::ConditionNotMet: return QObject::tr("条件未满足");
    case OutputAbsenceReason::Failed: return QObject::tr("执行失败");
    case OutputAbsenceReason::Cancelled: return QObject::tr("执行已取消");
    case OutputAbsenceReason::TimedOut: return QObject::tr("执行超时");
    }
    return {};
}

const ImageHandle* image_pixels(const DataValue& value)
{
    if (const auto* image = value.as_image()) { return &image->image; }
    if (const auto* mask = value.as_raster_mask()) { return &mask->pixels; }
    if (value.type() == class_map_type()) {
        static const auto pixels = std::get<FieldId>(SchemaIdFactory::parse<FieldId>("pixels"));
        return &value.field(pixels)->as_image()->image;
    }
    return nullptr;
}

class ValueItem final : public QTreeWidgetItem {
public:
    ValueItem(QString label, DataValue v) : QTreeWidgetItem(), value(std::move(v))
    {
        setText(0, std::move(label));
        setText(1, type_text(value.type()));
        if (const auto* list = value.elements()) { count = list->size(); }
        else if (const auto* fields = value.fields()) { count = fields->size(); }
        else if (value.optional_value()) { count = 1; }
        setText(2, shared_value_text(value));
        if (const auto* image = image_pixels(value)) {
            const auto format = image->format();
            const char* orders[] = {"Mono", "RGB", "BGR", "RGBA", "BGRA"};
            const char* depths[] = {"UInt8", "UInt16", "Float32", "Float64"};
            setText(2, QStringLiteral("%1 × %2 · %3 %4").arg(image->width()).arg(image->height())
                .arg(QLatin1String(orders[static_cast<int>(format.order)]), QLatin1String(depths[static_cast<int>(format.depth)])));
        }
        if (count) { setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator); }
    }
    void fetch()
    {
        if (loaded && loaded < count) { delete takeChild(childCount() - 1); }
        const auto end = std::min(count, loaded + 64);
        for (; loaded < end; ++loaded) {
            if (auto* list = value.elements()) {
                addChild(new ValueItem(QStringLiteral("[%1]").arg(loaded), (*list)[loaded]));
            } else if (auto* fields = value.fields()) {
                const auto& field = (*fields)[loaded];
                const auto id = QString::fromUtf8(field.id.text());
                auto* child = new ValueItem(display_field(id), field.value);
                child->setData(0, Qt::UserRole, id);
                child->setToolTip(0, id);
                addChild(child);
            } else { addChild(new ValueItem(QObject::tr("值"), *value.optional_value())); }
        }
        if (loaded < count) {
            auto* next = new QTreeWidgetItem(this, {QObject::tr("显示更多（剩余 %1）").arg(count - loaded)});
            next->setData(0, Qt::UserRole + 1, true);
        }
    }
    DataValue value;
    std::size_t count{};
    std::size_t loaded{};
};

void configure_combo(QComboBox& combo, const char* name, const QString& prompt)
{
    combo.setObjectName(QString::fromLatin1(name));
    combo.setAccessibleName(prompt);
    combo.setPlaceholderText(prompt);
    combo.setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo.setMinimumContentsLength(8);
}
} // namespace

WorkspaceResults::WorkspaceResults(QDockWidget& upper, QDockWidget& lower, ImagePanel& image, ModuleHistoryPanel& history)
    : QObject(&upper), history_(history)
{
    image_tabs_ = tabs(&upper, "workspaceOutputTabs", {tr("模块图像"), tr("流程图像"), tr("模块输出数据")});
    result_tabs_ = tabs(&lower, "workspaceResultTabs", {tr("当前结果"), tr("历史结果"), tr("帮助")});
    auto* top = new QStackedWidget(&upper);
    top->addWidget(&image);
    auto* flow = new QWidget(top);
    auto* flow_layout = new QVBoxLayout(flow);
    flow_layout->setContentsMargins(0, 0, 0, 0);
    auto* selectors = new QHBoxLayout;
    source_ = new QComboBox(flow);
    configure_combo(*source_, "flowImageChannel", tr("选择流程图像来源"));
    call_ = new QComboBox(flow);
    configure_combo(*call_, "flowImageCall", tr("选择来源调用"));
    call_->hide();
    selectors->addWidget(source_, 1);
    selectors->addWidget(call_);
    flow_layout->addLayout(selectors);
    top->addWidget(flow);

    auto* output_page = new QWidget(top);
    auto* output_layout = new QVBoxLayout(output_page);
    output_layout->setContentsMargins(4, 2, 4, 2);
    output_origin_ = text_page(output_page);
    output_origin_->setMargin(1);
    output_origin_->setObjectName(QStringLiteral("moduleOutputOrigin"));
    output_layout->addWidget(output_origin_);
    outputs_ = new QTreeWidget(output_page);
    outputs_->setObjectName(QStringLiteral("moduleOutputTree"));
    outputs_->setAccessibleName(tr("模块输出数据"));
    outputs_->setHeaderLabels({tr("输出"), tr("类型"), tr("值 / 状态"), tr("单位")});
    outputs_->setUniformRowHeights(true);
    outputs_->header()->setStretchLastSection(false);
    outputs_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    outputs_->setContextMenuPolicy(Qt::ActionsContextMenu);
    auto* copy = outputs_->addAction(tr("复制字段标识"));
    copy->setShortcut(QKeySequence::Copy);
    copy->setShortcutContext(Qt::WidgetShortcut);
    connect(copy, &QAction::triggered, this, [this] {
        if (auto* item = outputs_->currentItem()) {
            const auto id = item->data(0, Qt::UserRole).toString();
            QApplication::clipboard()->setText(id.isEmpty() ? item->text(0) : id);
        }
    });
    connect(outputs_, &QTreeWidget::itemExpanded, this, [](QTreeWidgetItem* row) {
        if (auto* value = dynamic_cast<ValueItem*>(row); value && !value->loaded) { value->fetch(); }
    });
    connect(outputs_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* row) {
        if (row->data(0, Qt::UserRole + 1).toBool()) {
            static_cast<ValueItem*>(row->parent())->fetch();
            return;
        }
        const auto diagnostic = row->data(2, Qt::UserRole).toString();
        if (!diagnostic.isEmpty()) {
            if (output_detail_) { output_detail_->close(); }
            auto* details = new QMessageBox(QMessageBox::Information, tr("输出诊断"), diagnostic, QMessageBox::Close, outputs_);
            output_detail_ = details;
            details->setTextFormat(Qt::PlainText);
            details->setAttribute(Qt::WA_DeleteOnClose);
            details->show();
            return;
        }
        const auto* value = dynamic_cast<ValueItem*>(row);
        if (!value || row->parent() || !image_type(value->value.type()) || !output_run_ || !output_call_) { return; }
        const auto id = row->data(0, Qt::UserRole).toString();
        const auto record = output_run_->lookup(*output_call_);
        const auto* found = std::get_if<std::optional<NodeResult>>(&record);
        if (!found || !*found) { return; }
        const auto& ports = (*found)->data().outputs;
        const auto slot = std::ranges::find_if(ports, [&](const auto& entry) { return QString::fromUtf8(entry.id.text()) == id; });
        if (slot == ports.end()) { return; }
        if (output_detail_) { output_detail_->close(); }
        auto* dialog = new QDialog(outputs_);
        output_detail_ = dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(row->text(0));
        auto* panel = new ImagePanel(dialog);
        auto* layout = new QVBoxLayout(dialog);
        layout->addWidget(panel);
        const auto shown = show_run_image(*panel, *output_run_, *output_call_, slot->id, nullptr, true);
        panel->set_provenance(std::holds_alternative<Diagnostic>(shown)
            ? run_diagnostic_text(std::get<Diagnostic>(shown)) : output_origin_->text());
        dialog->resize(640, 420);
        dialog->show();
    });
    output_layout->addWidget(outputs_, 1);
    top->addWidget(output_page);
    upper.setWidget(top);
    connect(image_tabs_, &QTabBar::currentChanged, top, &QStackedWidget::setCurrentIndex);
    connect(image_tabs_, &QTabBar::currentChanged, this, [this, flow, flow_layout](int tab) {
        if (tab == 1 && !flow_image_) {
            flow_image_ = new ImagePanel(flow);
            flow_image_->setObjectName(QStringLiteral("flowImagePanel"));
            flow_layout->addWidget(flow_image_, 1);
        }
    });
    connect(image_tabs_, &QTabBar::currentChanged, this, &WorkspaceResults::view_changed);
    connect(source_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!workflow_ || index < 0) { return; }
        if (index == 0) { chosen_.erase(*workflow_); }
        else { chosen_.insert_or_assign(*workflow_, std::pair{ports_[index - 1], source_->itemText(index)}); }
        chosen_call_.reset();
        emit view_changed();
    });
    connect(call_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0 && static_cast<std::size_t>(index) < calls_.size()) {
            chosen_call_ = calls_[index];
            emit view_changed();
        }
    });
    auto* bottom = new QWidget(&lower);
    auto* bottom_layout = new QVBoxLayout(bottom);
    bottom_layout->setContentsMargins(2, 2, 2, 2);
    bottom_layout->setSpacing(1);
    context_ = new QLabel(bottom);
    context_->setTextFormat(Qt::PlainText);
    context_->setObjectName(QStringLiteral("resultContext"));
    context_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    bottom_layout->addWidget(context_);
    auto* pages = new QStackedWidget(bottom);
    current_ = text_page(pages);
    current_->setObjectName(QStringLiteral("currentDetectionResult"));
    pages->addWidget(current_);
    pages->addWidget(&history);
    help_ = text_page(pages);
    help_->setObjectName(QStringLiteral("moduleHelp"));
    pages->addWidget(help_);
    bottom_layout->addWidget(pages, 1);
    lower.setWidget(bottom);
    connect(result_tabs_, &QTabBar::currentChanged, pages, &QStackedWidget::setCurrentIndex);
    result_tabs_->setCurrentIndex(1);
    connect(result_tabs_, &QTabBar::currentChanged, this, &WorkspaceResults::view_changed);
}

void WorkspaceResults::reset_document()
{
    if (output_detail_) { output_detail_->close(); }
    chosen_.clear();
    workflow_.reset();
    source_generation_.reset();
    source_snapshot_.reset();
    calls_run_.reset();
    chosen_call_.reset();
    rendered_image_.reset();
    if (flow_image_) { flow_image_->clear_frame(); }
    output_run_.reset();
    output_call_.reset();
    rendered_outputs_.reset();
    outputs_->clear();
}

void WorkspaceResults::update(const Workflow* draft_flow, const Node* draft_node, const ToolRegistry& registry,
    std::uint64_t edit_generation, const QString& context)
{
    const auto run = history_.selected_run();
    context_->setText(context);
    context_->setToolTip(context);
    context_->setVisible(!context.isEmpty());
    if (result_tabs_->currentIndex() == 0) { current_->setText(history_.current_result()); }
    const auto* flow = run ? find_workflow(run->request().snapshot().solution(), run->identity().workflow) : draft_flow;
    const auto* node = draft_node && flow ? find_node(*flow, draft_node->id) : nullptr;
    const auto* tool_config = node ? std::get_if<ToolConfiguration>(&node->configuration) : nullptr;
    const auto* tool = tool_config ? (run ? run->request().snapshot().find_tool(tool_config->type)
                                         : registry.find_descriptor(tool_config->type)) : nullptr;
    if (result_tabs_->currentIndex() == 2) {
        help_->setText(tool ? tr("%1\n输入 %2 项　参数 %3 项　输出 %4 项\n双击模块可查看其输入绑定、参数和输出定义。")
            .arg(toolbox_entry(tool->id.text(), QString::fromStdString(tool->display_name)).name)
            .arg(tool->inputs.size()).arg(tool->parameters.size()).arg(tool->outputs.size())
            : tool_config ? tr("缺少该工具的描述。") : node ? tr("工具组的输入和输出由其公开接口定义。") : tr("选中模块后可查看工具说明。"));
    }
    if (image_tabs_->currentIndex() == 1) { update_flow_image(flow, registry, run, edit_generation); }
    else if (rendered_image_) { flow_image_->clear_frame(true); rendered_image_.reset(); }
    if (image_tabs_->currentIndex() == 2) { update_outputs(node, tool, run, history_.selection_message()); }
    else if (output_run_) { output_run_.reset(); rendered_outputs_.reset(); outputs_->clear(); }
}

void WorkspaceResults::update_outputs(const Node* node, const ToolDescriptor* tool,
    const std::optional<RunHandle>& run, const QString& message)
{
    output_run_ = run;
    output_call_ = history_.selected_invocation();
    std::optional<NodeResult> result;
    QString missing = message;
    if (run && output_call_) {
        auto query = run->lookup(*output_call_);
        if (auto* found = std::get_if<std::optional<NodeResult>>(&query)) { result = *found; }
        else {
            const auto& error = std::get<Diagnostic>(query);
            missing = error.code == ErrorCode::ResultRetired ? tr("结果已淘汰 · %1").arg(run_diagnostic_text(error)) : run_diagnostic_text(error);
        }
    }
    if (result && rendered_outputs_ == result->data().identity) { return; }
    outputs_->clear();
    rendered_outputs_.reset();
    QString title = node ? QString::fromStdString(node->name) : tr("请选择模块");
    if (result) {
        const auto& id = result->data().identity;
        rendered_outputs_ = id;
        title += tr(" · 第 %1 轮 · 作用域 %2 · 调用 %3 · 尝试 %4").arg(id.run.run.value())
            .arg(id.invocation.scope.value()).arg(id.invocation.invocation.value()).arg(id.attempt.value());
    } else if (!missing.isEmpty()) { title += QStringLiteral(" · ") + missing; }
    const auto ports = declarations(node, tool);
    if (node && ports.empty()) { title += tr(" · 没有声明数据输出"); }
    output_origin_->setText(title);
    for (const auto& port : ports) {
        const auto* slot = result ? result->output(port.id) : nullptr;
        const QString name = display_field(QString::fromStdString(port.display_name));
        auto* value = slot ? std::get_if<DataValue>(&slot->content) : nullptr;
        QTreeWidgetItem* row;
        if (value) {
            row = new ValueItem(name, *value);
            if (image_type(port.type)) { row->setToolTip(2, tr("双击或按 Enter 预览此输出")); }
        } else {
            QString reason = missing.isEmpty() ? tr("未产出") : missing;
            QString diagnostic;
            if (slot) {
                if (auto* invalid = std::get_if<InvalidOutput>(&slot->content)) {
                    diagnostic = run_diagnostic_text(invalid->diagnostic);
                    reason = tr("无效 · %1").arg(QString::fromStdString(invalid->diagnostic.message));
                } else { reason = absence(std::get<NotProduced>(slot->content).reason); }
            }
            if (diagnostic.isEmpty() && result && !result->data().diagnostics.empty()) {
                diagnostic = run_diagnostic_text(result->data().diagnostics.front());
            }
            row = new QTreeWidgetItem({name, type_text(port.type), reason});
            row->setData(2, Qt::UserRole, diagnostic);
            row->setToolTip(2, diagnostic.isEmpty() ? reason : tr("%1\n双击或按 Enter 查看诊断").arg(diagnostic));
        }
        const auto id = QString::fromUtf8(port.id.text());
        row->setData(0, Qt::UserRole, id);
        row->setToolTip(0, id);
        if (port.semantics.unit != MeasurementUnit::Unspecified) { row->setText(3, unit_symbol(port.semantics.unit)); }
        outputs_->addTopLevelItem(row);
    }
}

void WorkspaceResults::update_flow_image(const Workflow* flow, const ToolRegistry& registry,
    const std::optional<RunHandle>& run, std::uint64_t edit_generation)
{
    const auto flow_id = flow ? std::optional{flow->id} : std::nullopt;
    const auto snap = run ? std::optional{run->identity().snapshot} : std::nullopt;
    if (workflow_ != flow_id || source_snapshot_ != snap || (!run && source_generation_ != edit_generation)) {
        workflow_ = flow_id;
        source_snapshot_ = snap;
        source_generation_ = edit_generation;
        const QSignalBlocker guard(source_);
        ports_.clear();
        source_->clear();
        source_->addItem(tr("选择流程图像来源"));
        source_->setCurrentIndex(0);
        if (flow) {
            for (const auto& node : flow->nodes) {
                const auto* config = std::get_if<ToolConfiguration>(&node.configuration);
                const auto* tool = config ? (run ? run->request().snapshot().find_tool(config->type) : registry.find_descriptor(config->type)) : nullptr;
                for (const auto& port : declarations(&node, tool)) {
                    if (!image_type(port.type)) { continue; }
                    ports_.emplace_back(node.id, port.id);
                    source_->addItem(QStringLiteral("%1 / %2").arg(QString::fromStdString(node.name), display_field(QString::fromStdString(port.display_name))));
                }
            }
            const auto saved = chosen_.find(flow->id);
            if (saved != chosen_.end()) {
                auto found = std::ranges::find(ports_, saved->second.first);
                if (found == ports_.end()) {
                    ports_.push_back(saved->second.first);
                    source_->addItem(saved->second.second);
                    source_->setCurrentIndex(source_->count() - 1);
                } else { source_->setCurrentIndex(1 + static_cast<int>(found - ports_.begin())); }
            }
        }
    }
    const auto clear = [&](const QString& message) {
        if (flow_image_->frame()) { flow_image_->clear_frame(true); }
        rendered_image_.reset();
        flow_image_->set_provenance(message);
    };
    const int index = source_->currentIndex() - 1;
    if (!flow || index < 0 || !run) {
        call_->hide();
        clear(!flow ? tr("请选择流程") : index < 0 ? tr("请选择一个流程图像来源") : tr("%1 · 尚未运行").arg(source_->currentText()));
        return;
    }
    const auto& [node, port] = ports_[index];
    const auto origin = tr("第 %1 轮 · %2").arg(run->identity().run.value()).arg(source_->currentText());
    const auto state = run->inspect();
    if (const auto* failure = std::get_if<Diagnostic>(&state)) {
        call_->hide();
        clear(origin + QStringLiteral(" · ") + run_diagnostic_text(*failure));
        return;
    }
    const auto& progress = std::get<RunProgress>(state);
    const auto scope = history_.selected_scope();
    if (calls_run_ != run->identity() || calls_scope_ != scope) { chosen_call_.reset(); }
    calls_run_ = run->identity();
    calls_scope_ = scope;
    const auto parent = std::ranges::find(flow->containment, node, &Containment::child);
    const bool same_group = scope && parent != flow->containment.end()
        && (std::ranges::any_of(progress.iterations, [&](const auto& i) { return i.scope == scope && i.loop.node == parent->parent; })
            || std::ranges::any_of(progress.groups, [&](const auto& g) { return g.scope == scope && g.group.node == parent->parent; }));
    std::vector<InvocationKey> candidates;
    QString missing = tr("本轮没有此来源的调用");
    for (const auto& item : progress.nodes) {
        if (item.node != node || (same_group && item.scope != scope)) { continue; }
        if (item.invocation) { candidates.push_back(*item.invocation); }
        else { missing = tr("模块%1，尚无图像输出").arg(execution_status_text(item.status)); }
    }
    if (calls_ != candidates) {
        const QSignalBlocker guard(call_);
        calls_ = std::move(candidates);
        call_->clear();
        for (const auto& key : calls_) {
            call_->addItem(tr("作用域 %1 · 调用 %2").arg(key.scope.value()).arg(key.invocation.value()));
        }
        call_->setCurrentIndex(-1);
    }
    call_->setVisible(calls_.size() > 1);
    if (calls_.size() == 1) { chosen_call_ = calls_.front(); }
    const auto selected = chosen_call_ ? std::ranges::find(calls_, *chosen_call_) : calls_.end();
    {
        const QSignalBlocker guard(call_);
        call_->setCurrentIndex(selected == calls_.end() ? -1 : static_cast<int>(selected - calls_.begin()));
    }
    if (selected == calls_.end()) {
        clear(origin + QStringLiteral(" · ") + (calls_.empty() ? missing : tr("请选择调用")));
        return;
    }
    auto lookup = run->lookup(*selected);
    auto* published = std::get_if<std::optional<NodeResult>>(&lookup);
    if (!published || !*published) {
        clear(origin + QStringLiteral(" · ") + (published ? tr("尚未产出") : run_diagnostic_text(std::get<Diagnostic>(lookup))));
        return;
    }
    const auto& result = **published;
    const auto* slot = result.output(port);
    if (!slot) { clear(origin + tr(" · 本轮未产出该端口")); return; }
    if (const auto* omitted = std::get_if<NotProduced>(&slot->content)) {
        clear(origin + QStringLiteral(" · ") + absence(omitted->reason));
        return;
    }
    if (const auto* invalid = std::get_if<InvalidOutput>(&slot->content)) {
        clear(origin + QStringLiteral(" · ") + run_diagnostic_text(invalid->diagnostic));
        return;
    }
    if (rendered_image_ != result.data().identity || rendered_port_ != port) {
        const auto shown = show_run_image(*flow_image_, *run, *selected, port, nullptr, true);
        if (const auto* error = std::get_if<Diagnostic>(&shown)) { clear(origin + QStringLiteral(" · ") + run_diagnostic_text(*error)); return; }
        rendered_image_ = result.data().identity;
        rendered_port_ = port;
    }
    flow_image_->set_provenance(origin + tr(" · 作用域 %1 · 调用 %2").arg(selected->scope.value()).arg(selected->invocation.value()));
}

} // namespace visionpro::ui
