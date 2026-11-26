#include "workspace_results.hpp"
#include "image_source_fields.hpp"

#include "coordinate_text.hpp"
#include "data_type_text.hpp"
#include "image_panel.hpp"
#include "image_comparison.hpp"
#include "module_history_panel.hpp"
#include "run_image.hpp"
#include "run_panel.hpp"
#include "toolbox_catalog.hpp"
#include "workspace_icons.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
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

struct SourceResultField {
    std::string_view id;
    const char* name;
    std::string_view source{};
};

// The result tree follows the handbook. Port IDs are shared with saved bindings and the run API.
constexpr SourceResultField source_result_fields[] = {
    {"elapsed_ms", "耗时（ms）"}, {"image", "图像"}, {"image_1", "图像 1", "Camera"},
    {"image_2", "图像 2", "Camera"}, {"image_3", "图像 3", "Camera"}, {"gray_image", "灰度图像"},
    {"file_path", "当前图像路径", "Local"}, {"file_name", "当前图像名称", "Local"},
    {"camera_frame_number", "帧号", "Camera"}, {"lost_frames", "丢帧数", "Camera"},
    {"lost_packets", "丢包数", "Camera"}, {"acquire_failures", "相机获取失败次数", "Camera"}, {"sn", "SN 码"}
};
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
    compare_mode_ = new QCheckBox(tr("双画面对照（1×2）"),flow);
    compare_mode_->setObjectName("flowImageComparison");
    selectors->addWidget(compare_mode_);
    flow_layout->addLayout(selectors);
    connect(compare_mode_,&QCheckBox::toggled,this,[this,flow_layout,flow](bool on) {
        if (on && !comparison_) {
            comparison_ = new ImageComparison(flow);
            flow_layout->addWidget(comparison_,1);
        }
        if (comparison_) {
            comparison_->setVisible(on);
            if (!on) { comparison_->suspend(); }
        }
        source_->setVisible(!on);
        if (on) { call_->hide(); }
        if (flow_image_) { flow_image_->setVisible(!on); }
        emit view_changed();
    });
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
    camera_variable_menu_ = new QMenu(tr("设备诊断变量"), outputs_);
    outputs_->addAction(camera_variable_menu_->menuAction());
    camera_variable_menu_->menuAction()->setVisible(false);
    connect(outputs_, &QTreeWidget::itemExpanded, this, [](QTreeWidgetItem* row) {
        if (auto* value = dynamic_cast<ValueItem*>(row); value && !value->loaded) { value->fetch(); }
    });
    connect(outputs_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* row) {
        if (source_page_ && row->data(0, Qt::UserRole + 2).toBool()) { row = row->parent(); }
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
        if (!value || (!source_page_ && row->parent()) || !image_type(value->value.type()) || !output_run_ || !output_call_) { return; }
        const auto id = row->data(0, Qt::UserRole).toString();
        const auto record = output_run_->lookup(*output_call_);
        const auto* found = std::get_if<std::optional<NodeResult>>(&record);
        if (!found || !*found) { return; }
        const auto& ports = (*found)->data().outputs;
        const auto slot = std::ranges::find_if(ports, [&](const auto& entry) { return QString::fromUtf8(entry.id.text()) == id; });
        if (slot == ports.end()) { return; }
        if (source_page_) {
            image_tabs_->setCurrentIndex(0);
            emit source_preview_requested(slot->id);
            return;
        }
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
    auto* ref = new QHBoxLayout;
    reference_ = text_page(output_page);
    reference_->setText(tr("选择输出查看 C++ 取值表达式"));
    reference_->setToolTip(tr("run 是所选轮次的 RunHandle，call 是该轮对应节点和作用域的 InvocationKey。"));
    reference_->setMargin(1);
    reference_->setObjectName(QStringLiteral("sourceOutputReference"));
    auto* copy_ref = new QPushButton(tr("复制表达式"), output_page);
    copy_ref->setObjectName(QStringLiteral("copySourceReference"));
    copy_ref->setEnabled(false);
    ref->addWidget(reference_, 1); ref->addWidget(copy_ref);
    output_layout->addLayout(ref);
    connect(copy_ref, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(reference_->text()); });
    connect(outputs_, &QTreeWidget::currentItemChanged, this, [this, copy_ref](QTreeWidgetItem* row) {
        while (row && row->parent() && row->data(0,Qt::UserRole).toString().isEmpty()) { row = row->parent(); }
        const auto id = row ? row->data(0,Qt::UserRole).toString() : QString{};
        const bool has_output = source_node_ && !id.isEmpty();
        reference_->setText(has_output
            ? QStringLiteral("run.lookup_output(call, \"%1\", \"%2\")").arg(QString::fromStdString(source_node_->node.to_string()), id)
            : tr("选择输出查看 C++ 取值表达式"));
        copy_ref->setEnabled(has_output);
    });
    top->addWidget(output_page);
    upper.setWidget(top);
    connect(image_tabs_, &QTabBar::currentChanged, top, &QStackedWidget::setCurrentIndex);
    connect(image_tabs_, &QTabBar::currentChanged, this, [this, flow, flow_layout](int tab) {
        if (tab == 1 && !flow_image_) {
            flow_image_ = new ImagePanel(flow);
            flow_image_->setObjectName(QStringLiteral("flowImagePanel"));
            flow_layout->addWidget(flow_image_, 1);
            flow_image_->setVisible(!compare_mode_->isChecked());
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
    if (comparison_) { comparison_->reset_document(); }
    const QSignalBlocker block(compare_mode_);
    compare_mode_->setChecked(false);
    if (comparison_) { comparison_->hide(); }
    if (flow_image_) { flow_image_->show(); }
    source_->show();
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
    rendered_source_.reset();
    outputs_->clear();
}

void WorkspaceResults::update(const Solution& draft, const Workflow* draft_flow, const Node* draft_node,
    const ToolRegistry& registry, std::uint64_t edit_generation, const QString& context, bool editable)
{
    const auto run = history_.selected_run();
    context_->setText(context);
    context_->setToolTip(context);
    context_->setVisible(!context.isEmpty());
    if (result_tabs_->currentIndex() == 0) { current_->setText(history_.current_result()); }
    const auto* flow = run ? find_workflow(run->request().snapshot().solution(), run->identity().workflow) : draft_flow;
    const auto* node = draft_node && flow ? find_node(*flow, draft_node->id) : nullptr;
    const auto* tool_config = node ? std::get_if<ToolConfiguration>(&node->configuration) : nullptr;
    const auto* tool = tool_config ? (run ? run->request().snapshot().find_tool(*node)
                                         : registry.find_descriptor(tool_config->type)) : nullptr;
    const bool image_source = is_image_source(node);
    const bool image_output = is_image_output(node);
    const bool image_results = image_source || image_output;
    std::optional<ToolDescriptor> projected;
    if (!run && image_source && tool) { projected = specialize_tool(*tool, *node); tool = &*projected; }
    source_node_ = image_results ? std::optional{NodeTarget{flow->id,node->id}} : std::nullopt;
    if (source_page_ != image_results || reference_->property("imageSource").toBool() != image_source
        || !reference_->property("initialized").toBool()) {
        source_page_ = image_results;
        reference_->setProperty("imageSource", image_source);
        reference_->setProperty("initialized", true);
        image_tabs_->setTabText(0, image_source ? tr("图像") : tr("模块图像"));
        image_tabs_->setTabText(2, image_results ? tr("模块结果") : tr("模块输出数据"));
        image_tabs_->setTabVisible(1, !image_source);
        result_tabs_->setTabVisible(0, !image_source);
        if (image_source && result_tabs_->currentIndex() == 0) { result_tabs_->setCurrentIndex(1); }
        reference_->setVisible(image_results);
        reference_->parentWidget()->findChild<QPushButton*>("copySourceReference")->setVisible(image_results);
        outputs_->setHeaderLabels({image_results ? tr("结果名称") : tr("输出"), tr("类型"),
            image_results ? tr("当前值") : tr("值 / 状态"), tr("单位"), tr("变量管理")});
        outputs_->setColumnHidden(1,image_results); outputs_->setColumnHidden(3,image_results); outputs_->setColumnHidden(4,!image_results);
        output_origin_->setVisible(!image_source);
        outputs_->header()->resizeSection(0,image_results ? 185 : 100);
        if (image_results) { outputs_->header()->resizeSection(4,120); }
    }
    if (result_tabs_->currentIndex() == 2) {
        help_->setText(image_source ? tr("图像源为流程提供本轮图像。双击节点编辑来源和触发设置，在图像页导入本地图片。模块结果列出当前调用的输出，选择历史记录可回看该轮。")
            : image_output ? tr("输出图像订阅底图和前序检测结果，生成本次调用的结果图。存图按配置独立执行，历史结果可查看当时的图像及归档路径。")
            : tool ? tr("%1\n输入 %2 项　参数 %3 项　输出 %4 项\n双击模块可查看其输入绑定、参数和输出定义。")
            .arg(toolbox_entry(tool->id.text(), QString::fromStdString(tool->display_name)).name)
            .arg(tool->inputs.size()).arg(tool->parameters.size()).arg(tool->outputs.size())
            : tool_config ? tr("缺少该工具的描述。") : node ? tr("工具组的输入和输出由其公开接口定义。") : tr("选中模块后可查看工具说明。"));
    }
    const bool comparing = image_tabs_->currentIndex() == 1 && compare_mode_->isChecked() && comparison_;
    if (comparing) { comparison_->update_run(flow,registry,run,history_.selected_scope()); }
    else if (comparison_) { comparison_->suspend(); }
    if (image_tabs_->currentIndex() == 1 && !comparing) { update_flow_image(flow, registry, run, edit_generation); }
    else if (rendered_image_) { flow_image_->clear_frame(true); rendered_image_.reset(); }
    if (image_tabs_->currentIndex() == 2) {
        update_outputs(node, tool, run, history_.selection_message(), draft, edit_generation, editable);
    }
    else if (output_run_) { output_run_.reset(); rendered_outputs_.reset(); outputs_->clear(); }
}

void WorkspaceResults::update_outputs(const Node* node, const ToolDescriptor* tool,
    const std::optional<RunHandle>& run, const QString& message,
    const Solution& draft, std::uint64_t edit_generation, bool editable)
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
    const bool same_source = source_node_ && rendered_source_ == source_node_;
    const bool same_values = result ? rendered_outputs_ == result->data().identity
        : same_source && !rendered_outputs_ && output_message_ == missing && outputs_->topLevelItemCount() > 0;
    if (same_values && output_generation_ == edit_generation && mappings_editable_ == editable) { return; }
    QString selected;
    QStringList expanded;
    bool root_expanded = true;
    bool selected_image_data = false;
    if (same_source) {
        if (auto* row = outputs_->currentItem()) {
            selected = row->data(0, Qt::UserRole).toString();
            selected_image_data = row->data(0, Qt::UserRole + 2).toBool();
        }
        if (const auto* root = outputs_->topLevelItem(0)) {
            root_expanded = root->isExpanded();
            for (int i = 0; i < root->childCount(); ++i) {
                const auto* row = root->child(i);
                if (row->isExpanded()) { expanded << row->data(0, Qt::UserRole).toString(); }
            }
        }
    }
    rendered_source_ = source_node_;
    output_message_ = missing;
    output_generation_ = edit_generation;
    mappings_editable_ = editable;
    outputs_->clear();
    rendered_outputs_.reset();
    QString title = node ? QString::fromStdString(node->name) : tr("请选择模块");
    if (result) {
        const auto& id = result->data().identity;
        rendered_outputs_ = id;
        title += tr(" · 第 %1 轮").arg(id.run.run.value());
        if (!source_page_ || is_image_output(node)) {
            title += tr(" · 作用域 %1 · 调用 %2 · 尝试 %3").arg(id.invocation.scope.value())
                .arg(id.invocation.invocation.value()).arg(id.attempt.value());
        }
    } else if (!missing.isEmpty()) { title += QStringLiteral(" · ") + missing; }
    const auto ports = declarations(node, tool);
    if (node && ports.empty()) { title += tr(" · 没有声明数据输出"); }
    output_origin_->setText(title);
    QTreeWidgetItem* root = nullptr;
    if (source_page_) {
        const auto status = result ? tr("第 %1 轮").arg(result->data().identity.run.run.value()) : missing;
        root = new QTreeWidgetItem(outputs_, {QString::fromStdString(node->name), {}, status});
    }
    const auto* editable_flow = source_node_ ? find_workflow(draft, source_node_->workflow) : nullptr;
    const auto* editable_node = editable_flow ? find_node(*editable_flow, source_node_->node) : nullptr;
    const auto variable_caption = [&](const OutputId& port) -> QString {
        if (editable_node) {
            const auto binding = std::ranges::find(editable_node->result_variables, port, &ResultVariableBinding::output);
            if (binding != editable_node->result_variables.end()) {
                const bool global = binding->scope == VariableScope::Global;
                const auto& variables = global ? draft.variables : local_variable_definitions(*editable_flow, editable_node->id);
                const auto variable = std::ranges::find(variables, binding->variable, &GlobalVariableDefinition::id);
                const auto variable_name = variable == variables.end() ? tr("变量缺失") : QString::fromStdString(variable->name);
                return (global ? tr("全局 · %1") : tr("局部 · %1")).arg(variable_name);
            }
        }
        return {};
    };
    const auto map_variable = [&](QTreeWidgetItem* row, const OutputId& port, const QString& name) {
        const QString caption = variable_caption(port);
        auto* map = new QPushButton(workspace_icon(WorkspaceIcon::Link), caption, outputs_);
        map->setFlat(true);
        map->setObjectName(QStringLiteral("mapResultVariable.") + QString::fromUtf8(port.text()));
        map->setEnabled(editable && editable_node);
        const auto field_name = row->parent() != root ? tr("%1 · %2").arg(row->parent()->text(0), name) : name;
        const auto label = caption.isEmpty() ? tr("绑定全局或局部变量") : caption;
        map->setAccessibleName(tr("%1 · %2").arg(field_name, label));
        map->setToolTip(map->accessibleName());
        connect(map, &QPushButton::clicked, this, [this,target=*source_node_,port,field_name] {
            emit source_variable_requested(target.workflow,target.node,port,field_name);
        });
        outputs_->setItemWidget(row, 4, map);
    };
    camera_variable_menu_->clear();
    const auto source = source_page_ ? source_enum(*node, "source_type", "Local") : std::string_view{};
    camera_variable_menu_->menuAction()->setVisible(source == "Camera");
    camera_variable_menu_->setEnabled(editable && editable_node);
    if (source == "Camera") {
        for (const auto key : camera_diagnostic_outputs) {
            const auto port = source_key<OutputId>(key);
            const auto name = camera_diagnostic_name(key);
            const auto caption = variable_caption(port);
            auto* action = camera_variable_menu_->addAction(caption.isEmpty() ? name : tr("%1 · %2").arg(name, caption));
            connect(action, &QAction::triggered, this, [this, target=*source_node_,port,name] {
                emit source_variable_requested(target.workflow, target.node, port, name);
            });
        }
    }
    const auto add_port = [&](const OutputDescriptor& port, const QString& name) {
        const auto key = port.id.text();
        const auto* slot = result ? result->output(port.id) : nullptr;
        auto* value = slot ? std::get_if<DataValue>(&slot->content) : nullptr;
        QTreeWidgetItem* row;
        if (value) {
            row = new ValueItem(name, *value);
            if (source_page_ && key == "elapsed_ms" && value->as_float64()) {
                row->setText(2, tr("%1 ms").arg(*value->as_float64(), 0, 'f', 3));
                row->setToolTip(2, tr("%1 ms").arg(*value->as_float64(), 0, 'g', 16));
            }
            if (is_image_output(node) && key == "module_status" && result) {
                const auto state = result->data().status;
                if (const auto code = module_completion_code(state)) {
                    row->setText(2, tr("%1（%2）").arg(*code).arg(execution_status_text(state)));
                }
            }
            if (image_type(port.type)) { row->setToolTip(2, tr("双击或按 Enter 预览此输出")); }
        } else {
            QString reason = missing.isEmpty() ? tr("未产出") : missing;
            QString diagnostic;
            if (slot) {
                if (auto* invalid = std::get_if<InvalidOutput>(&slot->content)) {
                    diagnostic = run_diagnostic_text(invalid->diagnostic);
                    reason = tr("无效 · %1").arg(QString::fromStdString(invalid->diagnostic.message));
                } else {
                    const auto absent = std::get<NotProduced>(slot->content).reason;
                    const bool device_metric = key == "lost_frames" || key == "lost_packets"
                        || key == "acquire_failures" || key == "camera_frame_number";
                    reason = source_page_ && device_metric && absent == OutputAbsenceReason::ToolDidNotProduce
                        ? tr("设备未提供") : absence(absent);
                }
            }
            if (result && is_image_output(node)) {
                const auto& record = result->data();
                if (key == "module_status") {
                    if (const auto code = module_completion_code(record.status)) {
                        reason = tr("%1（%2）").arg(*code).arg(execution_status_text(record.status));
                    } else { reason = execution_status_text(record.status); }
                } else if (key == "elapsed_ms" && record.elapsed) {
                    reason = tr("%1 ms").arg(std::chrono::duration<double,std::milli>(*record.elapsed).count(), 0, 'f', 3);
                }
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
        if (root) {
            root->addChild(row);
            map_variable(row, port.id, name);
            if (port.type == DataType::image()) {
                const auto* img = value && value->as_image() ? &value->as_image()->image : nullptr;
                auto* entry = new QTreeWidgetItem(row, {tr("图像数据"), {}, img ? tr("双击预览") : row->text(2)});
                entry->setData(0, Qt::UserRole, id);
                entry->setData(0, Qt::UserRole + 2, true);
                const auto child = [&](const QString& name, const QString& output, const QString& format = {}) {
                    const auto key = source_key<OutputId>(output.toStdString());
                    if (std::ranges::find(ports, key, &OutputDescriptor::id) == ports.end()) { return; }
                    auto* item = new QTreeWidgetItem(row, {name,{},row->text(2)});
                    item->setData(0,Qt::UserRole,output);
                    if (const auto* slot = result ? result->output(key) : nullptr) {
                        if (const auto* datum = std::get_if<DataValue>(&slot->content)) {
                            const auto current = shared_value_text(*datum);
                            item->setText(2, format.isEmpty() ? current : tr("%1（%2）").arg(current, format));
                        }
                    }
                    map_variable(item, key, name);
                };
                child(tr("宽度"), id + "_width");
                child(tr("高度"), id + "_height");
                const auto format = !img ? QString{} : img->format().order == ChannelOrder::Mono ? tr("Mono8")
                    : img->format().order == ChannelOrder::RGB ? tr("RGB24") : tr("BGR8");
                child(tr("像素格式"), id == "image" ? QStringLiteral("pixel_format") : id + "_pixel_format", format);
            }
            if (expanded.contains(id)) { row->setExpanded(true); }
        } else { outputs_->addTopLevelItem(row); }
    };
    if (root && is_image_output(node)) {
        for (const auto& port : ports) {
            const auto key = port.id.text();
            if (key == "image_width" || key == "image_height" || key == "pixel_format") { continue; }
            add_port(port, key == "image" ? tr("输出图像") : display_field(QString::fromUtf8(port.id.text())));
        }
        root->setExpanded(root_expanded);
    } else if (root) {
        for (const auto& field : source_result_fields) {
            if (!field.source.empty() && field.source != source) { continue; }
            const auto port = std::ranges::find(ports, field.id, [](const auto& p) { return p.id.text(); });
            if (port != ports.end()) { add_port(*port, tr(field.name)); }
        }
        root->setExpanded(root_expanded);
    } else {
        for (const auto& port : ports) { add_port(port, display_field(QString::fromStdString(port.display_name))); }
    }
    if (!selected.isEmpty()) {
        for (QTreeWidgetItemIterator it(outputs_); *it; ++it) {
            if ((*it)->data(0, Qt::UserRole).toString() == selected && (*it)->data(0, Qt::UserRole + 2).toBool() == selected_image_data) {
                outputs_->setCurrentItem(*it);
                break;
            }
        }
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
