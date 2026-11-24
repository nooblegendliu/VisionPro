#include "solution_run_actions.hpp"

#include "run_controller.hpp"
#include "run_panel.hpp"

#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMenu>
#include <QPlainTextEdit>
#include <QVBoxLayout>

#include <algorithm>

namespace visionpro::ui {
namespace {
using namespace workflow;

QString close_reason_label(ControlCloseReason reason)
{
    switch (reason) {
    case ControlCloseReason::NotSelected: return QObject::tr("未选中");
    case ControlCloseReason::Disabled: return QObject::tr("已禁用");
    case ControlCloseReason::Failed: return QObject::tr("上游失败");
    case ControlCloseReason::Cancelled: return QObject::tr("已取消");
    case ControlCloseReason::TimedOut: return QObject::tr("已超时");
    }
    return {};
}

QString public_value(const OutputSlot& port)
{
    if (const auto* value = std::get_if<core::DataValue>(&port.content)) { return shared_value_text(*value); }
    if (const auto* invalid = std::get_if<InvalidOutput>(&port.content)) { return run_diagnostic_text(invalid->diagnostic); }
    switch (std::get<NotProduced>(port.content).reason) {
    case OutputAbsenceReason::NotSelected: return QObject::tr("未选中");
    case OutputAbsenceReason::Disabled: return QObject::tr("已禁用");
    case OutputAbsenceReason::ToolDidNotProduce: return QObject::tr("工具未产出");
    case OutputAbsenceReason::ConditionNotMet: return QObject::tr("条件未满足");
    case OutputAbsenceReason::Failed: return QObject::tr("失败");
    case OutputAbsenceReason::Cancelled: return QObject::tr("已取消");
    case OutputAbsenceReason::TimedOut: return QObject::tr("超时");
    }
    return {};
}

void show_report(QWidget* owner, const SolutionRunReport& report)
{
    const auto name = [&](core::WorkflowId id) {
        if (!report.runs.empty()) {
            if (const auto* flow = find_workflow(report.runs.front().request().snapshot().solution(), id)) {
                return QString::fromStdString(flow->name);
            }
        }
        return QString::fromStdString(id.to_string());
    };
    QStringList lines{QObject::tr("第 %1 轮　%2").arg(report.sequence).arg(execution_status_text(report.status))};
    for (const auto& child : report.runs) {
        auto state = child.inspect();
        const auto label = std::holds_alternative<RunProgress>(state) ? execution_status_text(std::get<RunProgress>(state).status)
            : run_diagnostic_text(std::get<core::Diagnostic>(state));
        lines << QObject::tr("%1　第 %2 轮　%3").arg(name(child.identity().workflow)).arg(child.identity().run.value()).arg(label);
        const auto result = child.public_result();
        if (const auto* public_result = std::get_if<WorkflowPublicResult>(&result)) {
            for (const auto& port : public_result->outputs) {
                auto line = QStringLiteral("  %1 = %2").arg(QString::fromUtf8(port.id.text()), public_value(port));
                const auto origin = std::ranges::find(public_result->exports, port.id, &RunExportTrace::output);
                if (origin != public_result->exports.end()) {
                    const auto& call = origin->producer.invocation;
                    const auto* flow = find_workflow(child.request().snapshot().solution(), child.identity().workflow);
                    const auto* node = flow ? find_node(*flow, call.node) : nullptr;
                    line += QObject::tr("　%1 · 作用域 %2 · 调用 %3 · 尝试 %4")
                        .arg(QString::fromStdString(node ? node->name : call.node.to_string())).arg(call.scope.value())
                        .arg(call.invocation.value()).arg(origin->producer.attempt.value());
                }
                lines << line;
            }
        } else { lines << run_diagnostic_text(std::get<core::Diagnostic>(result)); }
    }
    for (auto id : report.skipped) { lines << QObject::tr("%1　跳过").arg(name(id)); }
    for (const auto& edge : report.links) {
        const auto outlet = edge.link.output ? QStringLiteral(" · ") + QString::fromUtf8(edge.link.output->text()) : QString{};
        auto state = edge.status == ControlLinkStatus::Arrived ? QObject::tr("已到达")
            : edge.status == ControlLinkStatus::Closed ? QObject::tr("已关闭") : QObject::tr("等待");
        if (edge.close_reason) { state += QStringLiteral(" · ") + close_reason_label(*edge.close_reason); }
        lines << QObject::tr("%1%2 → %3　%4").arg(name(edge.link.source), outlet, name(edge.link.target), state);
    }
    for (const auto& item : report.deliveries) {
        lines << QObject::tr("%1 第 %2 轮 · %3 → %4 第 %5 轮 · %6").arg(name(item.source.workflow)).arg(item.source.run.value())
            .arg(QString::fromUtf8(item.output.text())).arg(name(item.target.workflow)).arg(item.target.run.value())
            .arg(QString::fromUtf8(item.input.text()));
    }
    for (const auto& issue : report.diagnostics) { lines << run_diagnostic_text(issue); }
    auto* dialog = new QDialog(owner);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("solutionRunReportDialog"));
    dialog->setWindowTitle(QObject::tr("整方案运行结果"));
    auto* layout = new QVBoxLayout(dialog);
    auto* text = new QPlainTextEdit(lines.join(u'\n'), dialog);
    text->setReadOnly(true);
    layout->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->resize(640, 380);
    dialog->show();
}
} // namespace

SolutionRunActions add_solution_run_actions(QMenu& menu, RunController& controller,
    std::function<const Solution*()> current_draft, std::function<RunDocument()> current_document)
{
    auto* start = menu.addAction(QObject::tr("运行整方案…"));
    auto* stop = menu.addAction(QObject::tr("取消整方案运行"));
    auto* result = menu.addAction(QObject::tr("查看整方案运行结果…"));
    start->setObjectName(QStringLiteral("runSolution"));
    stop->setObjectName(QStringLiteral("cancelSolutionRun"));
    result->setObjectName(QStringLiteral("viewSolutionRunReport"));
    auto* owner = qobject_cast<QWidget*>(controller.parent());
    QObject::connect(stop, &QAction::triggered, &controller, &RunController::cancel_solution);
    QObject::connect(result, &QAction::triggered, &menu, [owner, &controller] {
        if (controller.solution_report()) { show_report(owner, *controller.solution_report()); }
    });
    QObject::connect(start, &QAction::triggered, &menu, [=, &controller] {
        const auto* active = current_draft();
        if (!active || !active->graph || !controller.can_start()) { return; }
        const auto draft = *active;
        const auto source = current_document();
        std::vector<SolutionEntryInputs> inputs;
        for (auto entry : draft.graph->entries) {
            const auto* flow = find_workflow(draft, entry);
            if (!flow || !flow->enabled) { continue; }
            auto ports = flow->interface.inputs;
            for (auto& input : ports) { input.display_name = flow->name + " / " + input.display_name; }
            auto entered = choose_run_inputs(owner, draft, ports, false);
            if (!entered) { return; }
            inputs.push_back({entry, std::move(*entered)});
        }
        controller.start_solution(draft, source, std::move(inputs));
    });
    return {start, stop, result};
}
} // namespace visionpro::ui
