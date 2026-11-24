#include "editor_window.hpp"

#include "control_graph_model.hpp"
#include "run_controller.hpp"
#include "run_panel.hpp"
#include "workspace_toolbox.hpp"

#include <QAction>
#include <QHeaderView>
#include <QStackedWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <algorithm>

namespace visionpro::ui {
using namespace workflow;

void EditorWindow::set_overview_visible(bool visible)
{
    if (!view_) { return; }
    overview_action_->setChecked(visible);
    work_pages_->setCurrentWidget(visible ? static_cast<QWidget*>(overview_) : static_cast<QWidget*>(view_));
    if (auto* bar = canvas_host_->findChild<QToolBar*>(QStringLiteral("workflowRunBar"))) { bar->setVisible(!visible); }
    if (visible) { refresh_overview(); overview_->setFocus(); }
    refresh_actions();
}

void EditorWindow::refresh_overview()
{
    overview_->clear();
    const auto& solution = draft();
    const auto flow_name = [&](core::WorkflowId id) {
        const auto* flow = find_workflow(solution, id);
        return QString::fromStdString(flow ? flow->name : id.to_string());
    };
    auto* flows = new QTreeWidgetItem(overview_, {tr("流程")});
    for (const auto& flow : solution.workflows) {
        const bool entry = solution.graph && std::ranges::find(solution.graph->entries, flow.id) != solution.graph->entries.end();
        auto state = flow.enabled ? tr("已启用") : tr("已禁用");
        if (entry) { state += tr(" · 方案入口"); }
        auto* row = new QTreeWidgetItem(flows, {QString::fromStdString(flow.name), state});
        row->setData(0, Qt::UserRole, QString::fromStdString(flow.id.to_string()));
        row->setToolTip(0, tr("双击或按 Enter 打开流程"));
    }
    if (solution.workflows.empty()) { flows->setText(1, tr("可从流程栏的“+”添加流程")); }
    auto* control = new QTreeWidgetItem(overview_, {tr("执行连接")});
    auto* bindings = new QTreeWidgetItem(overview_, {tr("公开端口映射")});
    if (solution.graph) {
        for (const auto& edge : solution.graph->links) {
            new QTreeWidgetItem(control, {flow_name(edge.source) + QStringLiteral(" → ") + flow_name(edge.target),
                edge.output ? QString::fromUtf8(edge.output->text()) : tr("成功后执行")});
        }
        for (const auto& binding : solution.graph->bindings) {
            new QTreeWidgetItem(bindings, {
                flow_name(binding.source) + QStringLiteral(" / ") + QString::fromUtf8(binding.output.text()),
                flow_name(binding.target) + QStringLiteral(" / ") + QString::fromUtf8(binding.input.text())});
        }
    }
    if (control->childCount() == 0) { control->setText(1, tr("未配置")); }
    if (bindings->childCount() == 0) { bindings->setText(1, tr("未配置")); }
    const auto& report = run_controller_->solution_report();
    auto* round = new QTreeWidgetItem(overview_, {tr("方案运行报告")});
    if (report) {
        round->setText(1, tr("方案第 %1 轮 · %2 · 快照 %3").arg(report->sequence)
            .arg(execution_status_text(report->status)).arg(report->snapshot.snapshot.value()));
        for (std::size_t i = 0; i < report->runs.size(); ++i) {
            const auto& child = report->runs[i];
            const auto& id = child.identity();
            const auto* flow = find_workflow(child.request().snapshot().solution(), id.workflow);
            const auto progress = child.inspect();
            const auto* state = std::get_if<RunProgress>(&progress);
            auto* row = new QTreeWidgetItem(round, {QString::fromStdString(flow ? flow->name : id.workflow.to_string()),
                tr("流程第 %1 轮 · %2").arg(id.run.value()).arg(state ? execution_status_text(state->status) : tr("状态不可用"))});
            row->setData(0, Qt::UserRole, QString::fromStdString(id.workflow.to_string()));
            row->setData(1, Qt::UserRole, static_cast<qulonglong>(i));
        }
        for (auto id : report->skipped) {
            const auto* flow = report->runs.empty() ? nullptr
                : find_workflow(report->runs.front().request().snapshot().solution(), id);
            new QTreeWidgetItem(round, {QString::fromStdString(flow ? flow->name : id.to_string()), tr("跳过")});
        }
    } else {
        round->setText(1, run_controller_->solution_activity() == SolutionActivity::Idle ? tr("暂无方案报告") : tr("等待本次报告"));
    }
    overview_->expandAll();
    overview_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
}
} // namespace visionpro::ui
