#include "background_result.hpp"
#include "camera_editor.hpp"
#include "control_scene.hpp"
#include "editor_inspector.hpp"
#include "editor_window.hpp"
#include "endpoint_editor.hpp"
#include "run_controller.hpp"
#include "run_panel.hpp"

#include <visionpro/core/shared_data.hpp>
#include <visionpro/infrastructure/camera.hpp>

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;
using ObjectId = std::variant<ResourceId, VariableId, QueueId, WorkflowId>;
using Values = std::map<ObjectId, QString>;

QString text(const std::string& value) { return QString::fromStdString(value); }

QString resource_definition(const ResourceDefinition& resource)
{
    if (is_camera_device(resource)) {
        const auto serial = infrastructure::camera_serial(resource);
        if (const auto* issue = std::get_if<Diagnostic>(&serial)) { return text(issue->message); }
        return QObject::tr("序列号 %1").arg(text(std::get<std::string>(serial)));
    }
    if (matches_endpoint(resource, "visionpro.tcp") || matches_endpoint(resource, "visionpro.modbus.tcp")) {
        return endpoint_address(resource);
    }
    if (const auto* file = std::get_if<ManagedResource>(&resource.location)) { return text(file->path.path); }
    if (const auto* file = std::get_if<ExternalFileResource>(&resource.location)) { return text(file->path.path); }
    if (const auto* out = std::get_if<OutputPathResource>(&resource.location)) { return text(out->directory.path); }
    if (const auto* device = std::get_if<DeviceResource>(&resource.location)) { return QString::fromUtf8(device->adapter.text()); }
    const auto& sequence = std::get<ImageSequenceResource>(resource.location);
    if (const auto* dir = std::get_if<DirectorySelection>(&sequence.selection)) { return text(dir->directory.path); }
    return QObject::tr("序列 · %1 个文件").arg(std::get<std::vector<ResourcePath>>(sequence.selection).size());
}

class ResourceRow final : public QTreeWidgetItem {
public:
    ResourceRow(QTreeWidgetItem* parent, ObjectId key, QStringList columns)
        : QTreeWidgetItem(parent, columns), id(std::move(key)) {}
    ObjectId id;
};

class ResourceDialog final : public QDialog {
public:
    ResourceDialog(QWidget* parent, ControlGraphModel& model, ControlScene& scene, EditorInspector& inspector,
        RunController& controller, bool editable)
        : QDialog(parent), model_(model), scene_(scene), inspector_(inspector), controller_(controller), editable_(editable)
    {
        setObjectName(QStringLiteral("solutionResourcesDialog"));
        setWindowTitle(tr("方案资源 · %1").arg(text(model_.draft().name)));
        setWindowModality(Qt::WindowModal);
        setAttribute(Qt::WA_DeleteOnClose);
        auto* layout = new QVBoxLayout(this);
        session_ = new QLabel(this);
        session_->setObjectName(QStringLiteral("resourceSession"));
        session_->setTextFormat(Qt::PlainText);
        session_->setWordWrap(true);
        layout->addWidget(session_);
        tree_ = new QTreeWidget(this);
        tree_->setObjectName(QStringLiteral("solutionResourceTree"));
        tree_->setHeaderLabels({tr("资源"), tr("草稿定义"), tr("会话值 / 状态"), tr("引用位置")});
        tree_->setAccessibleName(tr("方案资源定义、运行状态和引用"));
        tree_->setAlternatingRowColors(true);
        tree_->setColumnWidth(0, 175);
        tree_->setColumnWidth(1, 220);
        tree_->setColumnWidth(2, 175);
        layout->addWidget(tree_, 1);
        capability_ = new QLabel(this);
        capability_->setWordWrap(true);
        layout->addWidget(capability_);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        auto* refresh = buttons->addButton(tr("刷新"), QDialogButtonBox::ActionRole);
        refresh->setObjectName(QStringLiteral("refreshResources"));
        auto* create = buttons->addButton(tr("添加设备"), QDialogButtonBox::ActionRole);
        auto* menu = new QMenu(create);
        menu->addAction(tr("相机…"), this, [this] { save_device(edit_camera_device(this)); });
        menu->addAction(tr("TCP 连接…"), this, [this] { save_device(edit_endpoint(this, "visionpro.tcp", nullptr)); });
        menu->addAction(tr("Modbus TCP 连接…"), this, [this] { save_device(edit_endpoint(this, "visionpro.modbus.tcp", nullptr)); });
        create->setMenu(menu);
        create->setEnabled(editable_);
        edit_ = buttons->addButton(tr("编辑定义…"), QDialogButtonBox::ActionRole);
        edit_->setObjectName(QStringLiteral("editResourceDefinition"));
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
        connect(refresh, &QPushButton::clicked, this, [this] { rebuild(); });
        connect(edit_, &QPushButton::clicked, this, [this] { edit_current(); });
        connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { update_edit(); });
        resize(880, 490);
        rebuild();
    }
private:
    QTreeWidgetItem* group(const QString& title)
    {
        auto* item = new QTreeWidgetItem(tree_, {title});
        item->setExpanded(true);
        return item;
    }
    ResourceRow* append(QTreeWidgetItem* group, ObjectId id, const QString& name, const QString& definition)
    {
        auto* row = new ResourceRow(group, id, {name, definition, tr("—"), tr("—")});
        std::visit([row](auto key) { row->setToolTip(0, text(key.to_string())); }, id);
        row->setToolTip(1, definition);
        rows_.emplace(id, row);
        return row;
    }
    void show_references(const Solution& draft)
    {
        std::map<ObjectId, QStringList> references;
        for (const auto& flow : draft.workflows) {
            const auto flow_name = text(flow.name);
            if (flow.trigger && flow.trigger->camera) {
                references[flow.trigger->camera->device] << tr("%1 / 流程触发").arg(flow_name);
            }
            for (const auto& node : flow.nodes) {
                const auto path = flow_name + QStringLiteral(" / ") + text(node.name);
                for (const auto& slot : node.resources) { references[slot.resource.id] << path; }
                if (const auto* tool = std::get_if<ToolConfiguration>(&node.configuration)) {
                    for (const auto& id : tool->shared_targets) { std::visit([&](auto key) { references[key] << path; }, id); }
                }
                for (const auto& binding : node.bindings) {
                    for (const auto& source : binding.sources) {
                        if (const auto* ref = std::get_if<ResourceReference>(&source)) { references[ref->id] << path; }
                        if (const auto* ref = std::get_if<GlobalReference>(&source)) { references[ref->variable] << path; }
                        if (const auto* selector = binding_selector(source)) {
                            for (const auto& step : *selector) {
                                const auto* dynamic = std::get_if<DynamicElementSelection>(&step);
                                if (const auto* ref = dynamic ? std::get_if<IndexGlobalReference>(&dynamic->source) : nullptr) {
                                    references[ref->variable] << path;
                                }
                            }
                        }
                    }
                }
            }
        }
        for (auto& [id, uses] : references) {
            const auto row = rows_.find(id);
            if (row == rows_.end()) { continue; }
            uses.removeDuplicates();
            row->second->setText(3, uses.join(QStringLiteral("、")));
            row->second->setToolTip(3, uses.join(u'\n'));
        }
    }
    void rebuild()
    {
        const auto* selected = dynamic_cast<ResourceRow*>(tree_->currentItem());
        const auto old = selected ? std::optional{selected->id} : std::nullopt;
        ++refresh_;
        rows_.clear();
        tree_->clear();
        const auto& draft = model_.draft();
        auto* cameras = group(tr("相机"));
        auto* endpoints = group(tr("通信连接"));
        auto* variables = group(tr("全局变量"));
        auto* queues = group(tr("数据队列"));
        auto* triggers = group(tr("流程触发"));
        auto* files = group(tr("其他方案资源"));
        for (const auto& resource : draft.resources) {
            const bool endpoint = matches_endpoint(resource, "visionpro.tcp") || matches_endpoint(resource, "visionpro.modbus.tcp");
            auto* section = is_camera_device(resource) ? cameras : endpoint ? endpoints : files;
            auto* row = append(section, resource.id, text(resource.name), resource_definition(resource));
            if (section == cameras || section == endpoints) { row->setText(2, tr("未查询设备")); }
        }
        for (const auto& variable : draft.variables) {
            append(variables, variable.id, text(variable.name), tr("初值 %1").arg(shared_value_text(variable.initial_value)));
        }
        for (const auto& queue : draft.queues) {
            append(queues, queue.id, text(queue.name), tr("容量 %1").arg(queue.capacity));
        }
        for (const auto& flow : draft.workflows) {
            auto mode = tr("手动运行");
            if (flow.trigger) {
                mode = flow.trigger->camera ? tr("相机帧") : flow.trigger->mode == TriggerMode::External
                    ? tr("外部帧") : tr("时钟 · %1 ms").arg(flow.trigger->interval_ms);
                mode += QStringLiteral(" · ") + QString::fromUtf8(flow.trigger->source.text());
            }
            auto* row = append(triggers, flow.id, text(flow.name), mode);
            const auto camera = controller_.camera_status(flow.id);
            row->setText(2, camera.isEmpty() ? controller_.trigger_status(flow.id) : camera);
        }
        show_references(draft);
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
            auto* section = tree_->topLevelItem(i);
            if (!section->childCount()) { section->setText(1, tr("尚无定义")); }
        }
        if (old && rows_.contains(*old)) { tree_->setCurrentItem(rows_.at(*old)); }
        update_edit();
        auto preview = controller_.shared_preview();
        if (const auto* issue = std::get_if<Diagnostic>(&preview)) { session_->setText(run_diagnostic_text(*issue)); return; }
        const auto& live = std::get<std::optional<SessionSharedPreview>>(preview);
        if (!live) { session_->setText(tr("草稿定义 · 尚无活动运行快照")); return; }
        session_->setText(tr("草稿定义 · 会话快照 %1（刷新时）").arg(live->snapshot.identity().snapshot.value()));
        if (draft.variables.empty() && draft.queues.empty()) { return; }
        on_worker(this, [live = *live]() -> Result<Values> {
            Values values;
            auto globals = live.data->read_all();
            if (const auto* issue = std::get_if<Diagnostic>(&globals)) { return *issue; }
            for (const auto& entry : std::get<std::vector<GlobalVariableEntry>>(globals)) {
                values.emplace(entry.variable, shared_value_text(entry.current_value));
            }
            for (const auto& queue : live.snapshot.solution().queues) {
                auto contents = live.data->preview(queue.id);
                if (const auto* issue = std::get_if<Diagnostic>(&contents)) { values.emplace(queue.id, run_diagnostic_text(*issue)); }
                else { values.emplace(queue.id, tr("待取 %1 条").arg(std::get<QueuePreview>(contents).messages.size())); }
            }
            return values;
        }, [this, generation = refresh_](Result<Values> outcome) {
            if (generation != refresh_) { return; }
            if (const auto* issue = std::get_if<Diagnostic>(&outcome)) { session_->setText(run_diagnostic_text(*issue)); return; }
            const auto& values = std::get<Values>(outcome);
            for (const auto& [id, row] : rows_) {
                if (!std::holds_alternative<VariableId>(id) && !std::holds_alternative<QueueId>(id)) { continue; }
                const auto found = values.find(id);
                row->setText(2, found == values.end() ? tr("该快照无此定义") : found->second);
            }
        });
    }
    const ResourceDefinition* current_device() const
    {
        const auto* row = dynamic_cast<ResourceRow*>(tree_->currentItem());
        const auto* id = row ? std::get_if<ResourceId>(&row->id) : nullptr;
        if (!id) { return nullptr; }
        const auto& resources = model_.draft().resources;
        const auto pos = std::ranges::find(resources, *id, &ResourceDefinition::id);
        return pos != resources.end() ? &*pos : nullptr;
    }
    void update_edit()
    {
        const auto* row = dynamic_cast<ResourceRow*>(tree_->currentItem());
        const auto* device = current_device();
        const bool supported = (row && std::holds_alternative<WorkflowId>(row->id)) || (device &&
            (is_camera_device(*device) || matches_endpoint(*device, "visionpro.tcp") || matches_endpoint(*device, "visionpro.modbus.tcp")));
        edit_->setEnabled(editable_ && supported);
        edit_->setToolTip(!editable_ ? tr("方案为只读") : supported ? tr("编辑草稿定义") : tr("此处只读查看"));
        if (!row) { capability_->clear(); }
        else if (!editable_) { capability_->setText(tr("方案为只读。")); }
        else if (device && supported) { capability_->setText(tr("此处支持添加和编辑设备定义。")); }
        else if (supported) { capability_->setText(tr("可编辑此流程的触发设置。启动和停止位于该流程的工具栏。")); }
        else { capability_->setText(tr("只读查看定义、会话值和引用。")); }
    }
    void edit_current()
    {
        const auto* row = dynamic_cast<ResourceRow*>(tree_->currentItem());
        if (!row) { return; }
        if (const auto* flow = std::get_if<WorkflowId>(&row->id)) {
            inspector_.edit_workflow_trigger(this, *flow);
            rebuild();
        } else if (const auto* selected = current_device()) {
            const auto copy = *selected;
            save_device(is_camera_device(copy) ? edit_camera_device(this, &copy)
                : edit_endpoint(this, std::get<DeviceResource>(copy.location).adapter.text(), &copy));
        }
    }
    void save_device(std::optional<ResourceDefinition> device)
    {
        if (!device) { return; }
        const auto id = device->id;
        scene_.edit_draft(tr("编辑方案设备"), [&device](Solution& draft) -> Result<std::monostate> {
            auto it = std::ranges::find(draft.resources, device->id, &ResourceDefinition::id);
            if (it == draft.resources.end()) { draft.resources.push_back(*device); }
            else { *it = *device; }
            return std::monostate{};
        });
        rebuild();
        if (rows_.contains(id)) { tree_->setCurrentItem(rows_.at(id)); }
    }
    ControlGraphModel& model_;
    ControlScene& scene_;
    EditorInspector& inspector_;
    RunController& controller_;
    bool editable_;
    QLabel* session_{};
    QLabel* capability_{};
    QTreeWidget* tree_{};
    QPushButton* edit_{};
    std::size_t refresh_{};
    std::map<ObjectId, ResourceRow*> rows_;
};
} // namespace

void EditorWindow::show_solution_resources()
{
    if (auto* dialog = findChild<QDialog*>(QStringLiteral("solutionResourcesDialog")); dialog && dialog->isVisible()) {
        dialog->raise();
        dialog->activateWindow();
        return;
    }
    auto* dialog = new ResourceDialog(this, *model_, *scene_, *inspector_, *run_controller_, writable());
    dialog->show();
}
} // namespace visionpro::ui
