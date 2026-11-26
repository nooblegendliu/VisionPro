#include "module_history_panel.hpp"
#include "image_source_fields.hpp"
#include <QDateTime>

#include "run_panel.hpp"
#include "workspace_appearance.hpp"
#include "workspace_style.hpp"

#include <QCheckBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;

enum HistoryColumn { Round, Execution, Verdict, Summary, Measured, Range, Elapsed };

void set_verdict_ink(QTreeWidgetItem& item, const QString& verdict)
{
    if (verdict == QStringLiteral("OK")) { item.setForeground(Verdict, workspace_colors().positive); }
    else if (verdict == QStringLiteral("NG")) { item.setForeground(Verdict, workspace_colors().negative); }
    else { item.setData(Verdict, Qt::ForegroundRole, QVariant{}); }
}

class HistoryRowDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(std::max({size.height(), workspace_size::row_height, option.fontMetrics.height() + workspace_size::gap}));
        return size;
    }

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        if (index.column() == Verdict && option->state.testFlag(QStyle::State_Selected)) {
            const auto ink = index.data(Qt::ForegroundRole);
            if (ink.isValid()) { option->palette.setBrush(QPalette::HighlightedText, ink.value<QBrush>()); }
        }
    }
};

QString dash() { return QStringLiteral("—"); }

QString elapsed_text(const std::optional<std::chrono::nanoseconds>& elapsed)
{
    if (!elapsed) { return dash(); }
    const auto ms = std::chrono::duration<double, std::milli>(*elapsed).count();
    return QStringLiteral("%1 ms").arg(ms, 0, 'g', 5);
}

QString algorithm_time(const NodeResultData& result)
{
    QStringList times;
    for (const auto& measurement : result.timings) {
        if (measurement.scope != TimingScope::ToolAlgorithm || !measurement.value) { continue; }
        double ms = *measurement.value;
        switch (measurement.unit) {
        case TimingUnit::Unknown: continue;
        case TimingUnit::Nanoseconds: ms /= 1'000'000.; break;
        case TimingUnit::Microseconds: ms /= 1'000.; break;
        case TimingUnit::Milliseconds: break;
        case TimingUnit::Seconds: ms *= 1'000.; break;
        }
        times.append(QStringLiteral("%1 ms").arg(ms, 0, 'g', 5));
    }
    return times.isEmpty() ? dash() : times.join(QStringLiteral(" / "));
}

QString traced_number(const RunNodeProgress& call, std::string_view name, bool parameter)
{
    for (const auto& binding : call.bindings) {
        if (std::holds_alternative<ParameterId>(binding.target) != parameter) { continue; }
        if (!std::visit([name](const auto& id) { return id.text() == name; }, binding.target)) { continue; }
        if (const auto* integer = std::get_if<std::int64_t>(&binding.numeric_value)) { return QString::number(*integer); }
        if (const auto* real = std::get_if<double>(&binding.numeric_value)) { return QString::number(*real, 'g', 17); }
    }
    return dash();
}

bool is_range_judge(const Node& definition)
{
    const auto* tool = std::get_if<ToolConfiguration>(&definition.configuration);
    return tool && tool->config_version == 1 && (tool->type.text() == "visionpro.judge.float_range"
        || tool->type.text() == "visionpro.judge.integer_range");
}

QString result_summary(const NodeResultData& result)
{
    QStringList fields;
    for (const auto& output : result.outputs) {
        if (const auto* value = std::get_if<DataValue>(&output.content)) {
            fields.append(QStringLiteral("%1 = %2").arg(QString::fromUtf8(output.id.text()), shared_value_text(*value)));
        } else if (const auto* invalid = std::get_if<InvalidOutput>(&output.content)) {
            fields.append(QStringLiteral("%1 · %2").arg(QString::fromUtf8(output.id.text()), run_diagnostic_text(invalid->diagnostic)));
        }
        if (fields.size() == 3) { break; }
    }
    return fields.isEmpty() ? dash() : fields.join(QStringLiteral(" · "));
}

std::optional<RunIdentity> identity(const std::optional<RunHandle>& run)
{
    return run ? std::optional{run->identity()} : std::nullopt;
}
} // namespace

ModuleHistoryPanel::ModuleHistoryPanel(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    auto* heading = new QHBoxLayout;
    module_ = new QLabel(this);
    module_->setTextFormat(Qt::PlainText);
    module_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    round_ = new QLabel(this);
    round_->setObjectName(QStringLiteral("historyRound"));
    round_->setFont(workspace_font(WorkspaceText::Secondary));
    follow_ = new QCheckBox(QStringLiteral("跟随最新"), this);
    follow_->setObjectName(QStringLiteral("followLatest"));
    follow_->setChecked(true);
    heading->addWidget(module_, 1);
    heading->addWidget(round_);
    heading->addWidget(follow_);
    layout->addLayout(heading);

    table_ = new QTreeWidget(this);
    table_->setObjectName(QStringLiteral("moduleHistoryTable"));
    table_->setRootIsDecorated(false);
    table_->setItemDelegate(new HistoryRowDelegate(table_));
    table_->setUniformRowHeights(true);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setHeaderLabels({QStringLiteral("轮次"), QStringLiteral("执行"), QStringLiteral("判定"),
        QStringLiteral("结果摘要"), QStringLiteral("实测"), QStringLiteral("允许范围"), QStringLiteral("模块耗时")});
    table_->header()->setFont(workspace_font(WorkspaceText::Secondary));
    for (const auto column : {Measured, Range, Elapsed}) {
        table_->headerItem()->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
    }
    table_->header()->setStretchLastSection(false);
    table_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->header()->setSectionResizeMode(Summary, QHeaderView::Stretch);
    layout->addWidget(table_, 1);

    connect(&WorkspaceAppearance::instance(), &WorkspaceAppearance::colorsChanged, this, [this] {
        for (int row = 0; row < table_->topLevelItemCount(); ++row) {
            auto* item = table_->topLevelItem(row);
            set_verdict_ink(*item, item->text(Verdict));
        }
    });
    connect(table_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) { pin_item(item); });
    connect(table_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        pin_item(item);
        emit browse_requested();
    });
    connect(follow_, &QCheckBox::toggled, this, [this](bool follow) {
        if (follow) {
            pinned_run_.reset();
            pinned_row_.reset();
        } else {
            pinned_run_ = selected_run_;
            const auto row = table_->indexOfTopLevelItem(table_->currentItem());
            if (row >= 0) { pinned_row_ = addresses_.at(static_cast<std::size_t>(row)); }
        }
        refresh();
        emit browse_requested();
    });
    refresh();
}

void ModuleHistoryPanel::clear()
{
    retained_.clear();
    pinned_run_.reset();
    pinned_row_.reset();
    workflow_.reset();
    node_.reset();
    node_name_.clear();
    range_judge_ = false;
    image_source_ = false;
    summaries_.clear();
    refresh();
}

void ModuleHistoryPanel::set_selection(const Workflow* workflow, const Node* node)
{
    const auto flow_id = workflow ? std::optional{workflow->id} : std::nullopt;
    const auto node_id = node ? std::optional{node->id} : std::nullopt;
    const auto node_name = node ? QString::fromStdString(node->name) : QString{};
    const bool range_judge = node && is_range_judge(*node);
    const bool source = is_image_source(node);
    if (workflow_ == flow_id && node_ == node_id && node_name_ == node_name
        && range_judge_ == range_judge && image_source_ == source) { return; }
    if (workflow_ != flow_id) {
        pinned_run_.reset();
        pinned_row_.reset();
    } else if (node_ != node_id && pinned_row_) {
        pinned_row_->invocation.reset(); // Keep the iteration while resolving the new module's call.
    }
    if (workflow_ != flow_id || node_ != node_id) {
        summaries_.clear();
    }
    workflow_ = flow_id;
    node_ = node_id;
    node_name_ = node_name;
    range_judge_ = range_judge;
    image_source_ = source;
    refresh();
}

void ModuleHistoryPanel::update_runs(std::span<const RunHandle> runs)
{
    retained_.assign(runs.begin(), runs.end());
    refresh();
}

void ModuleHistoryPanel::browse_run(const RunHandle& run)
{
    if (workflow_ != run.identity().workflow) { return; }
    pinned_run_ = run;
    pinned_row_.reset();
    refresh();
    emit browse_requested();
}

ModuleHistoryPanel::RunSummary ModuleHistoryPanel::summarize(const RunHandle& run) const
{
    RunSummary summary;
    const auto& solution = run.request().snapshot().solution();
    const auto* flow = find_workflow(solution, run.identity().workflow);
    const auto* definition = flow && node_ ? find_node(*flow, *node_) : nullptr;
    if (flow) { summary.flow_name = QString::fromStdString(flow->name); }
    if (definition) {
        summary.node_name = QString::fromStdString(definition->name);
        summary.range_judge = is_range_judge(*definition);
    }
    auto inspected = run.inspect();
    if (const auto* failure = std::get_if<Diagnostic>(&inspected)) {
        summary.diagnostic = run_diagnostic_text(*failure);
        return summary;
    }
    const auto& progress = std::get<RunProgress>(inspected);
    summary.complete = progress.quiescent;
    auto state = execution_status_text(progress.status);
    if (!progress.quiescent && progress.status != ExecutionStatus::Pending && progress.status != ExecutionStatus::Running) {
        state += QStringLiteral("（清理中）");
    }
    summary.run_statistics = QStringLiteral("执行%1").arg(state);
    summary.dispatch_count = QStringLiteral("已派发 %1 次").arg(progress.call_order.size());
    // Count only explicit range decisions; feature-location OK is a different judgement.
    std::set<NodeId> judges;
    if (flow) {
        for (const auto& node : flow->nodes) {
            if (is_range_judge(node)) { judges.insert(node.id); }
        }
    }
    if (!judges.empty()) {
        int ok = 0, ng = 0;
        for (const auto& call : progress.nodes) {
            if (!call.invocation || call.status != ExecutionStatus::Succeeded || !judges.contains(call.node)) { continue; }
            auto result = run.lookup(*call.invocation);
            if (const auto* record = std::get_if<std::optional<NodeResult>>(&result); record && *record) {
                if ((*record)->data().judgement == Judgement::OK) { ++ok; }
                else if ((*record)->data().judgement == Judgement::NG) { ++ng; }
            }
        }
        summary.run_statistics += QStringLiteral(" · %1 OK / %2 NG").arg(ok).arg(ng);
    }
    if (progress.elapsed) { summary.run_statistics += QStringLiteral(" · 流程 %1").arg(elapsed_text(progress.elapsed)); }
    if (!definition) { return summary; }
    // Body calls are appended as their scopes are created. Present the last iteration first.
    for (auto it = progress.nodes.rbegin(); it != progress.nodes.rend(); ++it) {
        const auto& call = *it;
        if (call.node != definition->id) { continue; }
        auto label = QStringLiteral("#%1").arg(run.identity().run.value());
        if (is_image_source(definition) && std::holds_alternative<NodeTarget>(run.request().target())) { label += tr(" · 隔离调试"); }
        auto tooltip = QStringLiteral("快照 %1").arg(run.identity().snapshot.snapshot.value());
        if (call.scope) {
            tooltip += QStringLiteral(" · 作用域 %1").arg(call.scope->value());
            const auto iteration = std::ranges::find(progress.iterations, *call.scope, &RunLoopIteration::scope);
            if (iteration != progress.iterations.end()) { label += QStringLiteral(" · 迭代 %1").arg(iteration->index); }
            else if (call.scope->value() != 0) { label += QStringLiteral(" · 域 %1").arg(call.scope->value()); }
        }
        Row row{call.scope, call.invocation, false,
            {label, execution_status_text(call.status), dash(), dash(), dash(), dash(), dash()}, tooltip, dash()};
        if (call.invocation) {
            row.tooltip += QStringLiteral(" · 调用 %1").arg(call.invocation->invocation.value());
            if (call.status != ExecutionStatus::Pending && call.status != ExecutionStatus::Running) {
                const auto lookup = run.lookup(*call.invocation);
                if (const auto* record = std::get_if<std::optional<NodeResult>>(&lookup); record && *record) {
                    const auto& published = (*record)->data();
                    row.published = true;
                    if (published.judgement == Judgement::OK) { row.cells[Verdict] = QStringLiteral("OK"); }
                    else if (published.judgement == Judgement::NG) { row.cells[Verdict] = QStringLiteral("NG"); }
                    if (summary.range_judge) {
                        row.cells[Measured] = traced_number(call, "value", false);
                        const auto minimum = traced_number(call, "minimum", true);
                        const auto maximum = traced_number(call, "maximum", true);
                        if (minimum != dash() || maximum != dash()) {
                            row.cells[Range] = QStringLiteral("[%1, %2]").arg(minimum, maximum);
                        }
                    } else if (is_image_source(definition)) {
                        const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(published.recorded_at.time_since_epoch()).count();
                        row.cells[Execution] = QDateTime::fromMSecsSinceEpoch(millis).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
                        QStringList data;
                        for (const auto& out : published.outputs) {
                            const auto* v = std::get_if<DataValue>(&out.content);
                            if (!v) { continue; }
                            if (out.id.text() == "image" && v->as_image()) {
                                const auto& img = v->as_image()->image;
                                data << tr("%1 × %2 px").arg(img.width()).arg(img.height());
                            } else if (out.id.text() == "sn" && v->as_int64()) { data << tr("SN %1").arg(*v->as_int64()); }
                            else if (out.id.text() == "file_name" && v->as_string()) { data << QString::fromUtf8(*v->as_string()); }
                        }
                        row.cells[Summary] = data.isEmpty() ? execution_status_text(published.status) : data.join(QStringLiteral(" · "));
                    } else if (is_image_output(definition)) {
                        QStringList files;
                        for (const auto& out : published.outputs) {
                            const auto* value = std::get_if<DataValue>(&out.content);
                            if (!value) { continue; }
                            if (out.id.text() == "image" && value->as_image()) {
                                const auto& image = value->as_image()->image;
                                files << tr("输出图像 %1 × %2 px").arg(image.width()).arg(image.height());
                            } else if ((out.id.text() == "original_name" || out.id.text() == "rendered_name" || out.id.text() == "big_file_path")
                                       && value->as_string()) {
                                files << QString::fromUtf8(*value->as_string());
                            }
                        }
                        row.cells[Summary] = files.isEmpty() ? tr("本次未生成图像或文件") : files.join(QStringLiteral(" · "));
                    } else { row.cells[Summary] = result_summary(published); }
                    if (published.status == ExecutionStatus::Failed && !published.diagnostics.empty()) {
                        row.cells[Summary] = run_diagnostic_text(published.diagnostics.front());
                    }
                    row.cells[Elapsed] = elapsed_text(published.elapsed);
                    row.algorithm_time = algorithm_time(published);
                } else if (const auto* diagnostic = std::get_if<Diagnostic>(&lookup)) {
                    row.cells[Summary] = diagnostic->code == ErrorCode::ResultRetired
                        ? tr("结果已淘汰 · %1").arg(run_diagnostic_text(*diagnostic)) : run_diagnostic_text(*diagnostic);
                }
            }
        }
        summary.rows.push_back(std::move(row));
    }
    return summary;
}

void ModuleHistoryPanel::pin_item(QTreeWidgetItem* item)
{
    const auto index = table_->indexOfTopLevelItem(item);
    if (index < 0 || static_cast<std::size_t>(index) >= addresses_.size()) { return; }
    const auto address = addresses_[static_cast<std::size_t>(index)];
    if (pinned_row_ == address) { return; }
    if (!pinned_run_ || pinned_run_->identity() != address.run) {
        const auto run = std::ranges::find(retained_, address.run, &RunHandle::identity);
        if (run == retained_.end()) { return; }
        pinned_run_ = *run;
    }
    pinned_row_ = address;
    refresh();
}

void ModuleHistoryPanel::refresh()
{
    const auto previous_run = identity(selected_run_);
    const auto previous_call = selected_call_;
    const auto previous_stats = statistics_;
    const auto previous_details = details_;
    const auto previous_message = message_;
    std::vector<RunHandle> visible;
    for (auto it = retained_.rbegin(); it != retained_.rend(); ++it) {
        if (it->identity().workflow == workflow_ && std::holds_alternative<WorkflowTarget>(it->request().target())) {
            visible.push_back(*it);
        }
    }
    if (pinned_run_ && std::ranges::find(visible, pinned_run_->identity(), &RunHandle::identity) == visible.end()) {
        visible.push_back(*pinned_run_);
    }
    std::erase_if(summaries_, [&](const auto& entry) {
        return std::ranges::find(visible, entry.first, &RunHandle::identity) == visible.end();
    });
    selected_run_ = pinned_run_;
    if (!selected_run_ && !visible.empty()) { selected_run_ = visible.front(); }
    selected_call_.reset();
    statistics_ = QStringLiteral("等待运行");
    details_.clear();
    message_ = node_ ? QStringLiteral("此流程尚无运行记录") : QStringLiteral("请选择一个模块查看输出");
    auto title = node_name_;
    QString round_caption;
    const QSignalBlocker table_blocker(table_);
    const QSignalBlocker follow_blocker(follow_);
    follow_->setEnabled(selected_run_.has_value());
    follow_->setChecked(!pinned_run_);
    addresses_.clear();
    int selected_index = -1;
    bool show_range = range_judge_;
    bool show_summary = !range_judge_;
    bool show_elapsed = false;
    for (const auto& run : visible) {
        const auto& id = run.identity();
        auto& summary = summaries_[id];
        if (!summary.complete) { summary = summarize(run); }
        if (!summary.rows.empty()) {
            show_range |= summary.range_judge;
            show_summary |= !summary.range_judge;
        }
        const bool selected = selected_run_->identity() == id;
        if (selected) {
            if (!summary.node_name.isEmpty()) { title = summary.node_name; }
            const auto round = QStringLiteral("第 %1 轮").arg(id.run.value());
            round_caption = (pinned_run_ ? QStringLiteral("历史 · ") : QStringLiteral("最新 · ")) + round;
            statistics_ = pinned_run_ ? round_caption : round;
            if (!summary.run_statistics.isEmpty()) { statistics_ += QStringLiteral(" · ") + summary.run_statistics; }
            details_ = QStringLiteral("%1 · %2 · 快照 %3").arg(summary.flow_name, round_caption)
                .arg(id.snapshot.snapshot.value());
            if (!summary.dispatch_count.isEmpty()) { details_ += u'\n' + summary.dispatch_count; }
            if (node_) { message_ = QStringLiteral("该轮没有此模块的调用"); }
            if (!summary.diagnostic.isEmpty()) {
                message_ = summary.diagnostic;
                details_ += u'\n' + summary.diagnostic;
            }
        }
        const bool ambiguous = selected && pinned_row_ && !pinned_row_->invocation
            && std::ranges::count(summary.rows, pinned_row_->scope, &Row::scope) > 1;
        if (selected && node_ && pinned_row_ && summary.diagnostic.isEmpty()) {
            message_ = ambiguous ? tr("此作用域有多次调用，请在历史结果中选择")
                                 : tr("此模块在锁定的作用域内没有调用");
        }
        for (const auto& row : summary.rows) {
            const int index = static_cast<int>(addresses_.size());
            auto* item = index < table_->topLevelItemCount() ? table_->topLevelItem(index) : new QTreeWidgetItem(table_);
            if (item->text(Verdict) != row.cells[Verdict]) {
                set_verdict_ink(*item, row.cells[Verdict]);
            }
            for (int col = 0; col < row.cells.size(); ++col) {
                if (item->text(col) != row.cells[col]) {
                    item->setText(col, row.cells[col]);
                    if (col >= Measured) { item->setTextAlignment(col, Qt::AlignRight | Qt::AlignVCenter); }
                    if (col >= Summary) { item->setToolTip(col, row.cells[col]); }
                }
            }
            if (item->toolTip(Round) != row.tooltip) { item->setToolTip(Round, row.tooltip); }
            show_summary |= row.cells[Summary] != dash();
            show_elapsed |= row.cells[Elapsed] != dash();
            addresses_.push_back({id, row.scope, row.invocation});
            if (selected && !ambiguous && selected_index < 0 && (!pinned_row_ || (pinned_row_->scope == row.scope
                && (!pinned_row_->invocation || pinned_row_->invocation == row.invocation)))) {
                selected_index = index;
                if (row.published) { selected_call_ = row.invocation; }
                message_ = row.published ? QString{} : row.cells[Summary] != dash() ? row.cells[Summary]
                    : QStringLiteral("模块%1，当前没有已发布输出").arg(row.cells[Execution]);
                details_ += QStringLiteral("\n%1 · 模块耗时 %2 · 算法耗时 %3")
                    .arg(summary.node_name, row.cells[Elapsed], row.algorithm_time);
            }
        }
    }
    while (table_->topLevelItemCount() > static_cast<int>(addresses_.size())) {
        delete table_->takeTopLevelItem(table_->topLevelItemCount() - 1);
    }
    table_->setColumnHidden(Summary, !show_summary);
    table_->setColumnHidden(Measured, !show_range);
    table_->setColumnHidden(Range, !show_range);
    table_->setColumnHidden(Elapsed, !show_elapsed);
    table_->headerItem()->setText(Round, image_source_ ? tr("执行序号") : tr("轮次"));
    table_->headerItem()->setText(Execution, image_source_ ? tr("时间") : tr("执行"));
    table_->headerItem()->setText(Summary, image_source_ ? tr("模块数据") : tr("结果摘要"));
    table_->setColumnHidden(Verdict, image_source_);
    if (image_source_) { table_->setColumnHidden(Elapsed, true); }
    const auto mode = show_summary ? QHeaderView::ResizeToContents : QHeaderView::Stretch;
    for (const auto col : {Execution, Verdict, Measured}) {
        if (table_->header()->sectionResizeMode(col) != mode) {
            table_->header()->setSectionResizeMode(col, mode);
        }
    }
    auto* selection = selected_index >= 0 ? table_->topLevelItem(selected_index) : nullptr;
    if (table_->currentItem() != selection) { table_->setCurrentItem(selection); }
    if (title.isEmpty()) { title = node_ ? QStringLiteral("所选模块") : QStringLiteral("请选择一个模块"); }
    if (module_->text() != title) {
        module_->setText(title);
        module_->setToolTip(title);
    }
    round_->setText(round_caption);
    round_->setVisible(!round_caption.isEmpty());
    if (previous_run != identity(selected_run_) || previous_call != selected_call_
        || previous_stats != statistics_ || previous_details != details_ || previous_message != message_) { emit selection_changed(); }
}

std::optional<ExecutionScopeId> ModuleHistoryPanel::selected_scope() const
{
    if (selected_call_) { return selected_call_->scope; }
    return pinned_row_ ? pinned_row_->scope : std::nullopt;
}

QString ModuleHistoryPanel::current_result() const
{
    const auto* row = table_->currentItem();
    if (!row || !selected_call_) { return message_; }
    const auto verdict = row->text(Verdict);
    QStringList lines{QStringLiteral("%1 · %2").arg(module_->text(), row->text(Round)),
        tr("执行 %1　判定 %2").arg(row->text(Execution), verdict == dash() ? tr("不适用") : verdict)};
    if (row->text(Measured) != dash()) { lines << tr("实测 %1　允许范围 %2").arg(row->text(Measured), row->text(Range)); }
    return lines.join(u'\n');
}

} // namespace visionpro::ui
