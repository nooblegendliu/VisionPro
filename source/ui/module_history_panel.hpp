#pragma once

#include <visionpro/workflow/run_handle.hpp>

#include <QStringList>
#include <QWidget>

#include <map>

class QCheckBox;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace visionpro::ui {

class ModuleHistoryPanel final : public QWidget {
    Q_OBJECT

public:
    explicit ModuleHistoryPanel(QWidget* parent = nullptr);
    void clear();
    void set_selection(const workflow::Workflow* workflow, const workflow::Node* node);
    void update_runs(std::span<const workflow::RunHandle> runs);

    [[nodiscard]] std::optional<workflow::RunHandle> selected_run() const { return selected_run_; }
    [[nodiscard]] std::optional<core::InvocationKey> selected_invocation() const { return selected_call_; }
    [[nodiscard]] const QString& statistics_text() const { return statistics_; }
    [[nodiscard]] const QString& statistics_details() const { return details_; }
    [[nodiscard]] const QString& selection_message() const { return message_; }
    [[nodiscard]] bool following_latest() const { return !pinned_run_; }

signals:
    void selection_changed();
    void browse_requested();

private:
    struct Row {
        std::optional<core::ExecutionScopeId> scope;
        std::optional<core::InvocationKey> invocation;
        bool published{};
        QStringList cells;
        QString tooltip;
        QString algorithm_time;
    };
    struct RunSummary {
        bool complete{};
        bool range_judge{};
        QString flow_name;
        QString node_name;
        QString run_statistics;
        QString dispatch_count;
        QString diagnostic;
        std::vector<Row> rows;
    };
    struct RowAddress {
        workflow::RunIdentity run;
        std::optional<core::ExecutionScopeId> scope;
        std::optional<core::InvocationKey> invocation;
        bool operator==(const RowAddress&) const = default;
    };
    RunSummary summarize(const workflow::RunHandle& run) const;
    void refresh();
    void pin_item(QTreeWidgetItem* item);

    QLabel* module_{};
    QLabel* round_{};
    QCheckBox* follow_{};
    QTreeWidget* table_{};
    std::optional<core::WorkflowId> workflow_;
    std::optional<core::NodeId> node_;
    QString node_name_;
    bool range_judge_{};
    std::vector<workflow::RunHandle> retained_;
    std::optional<workflow::RunHandle> pinned_run_;
    std::optional<RowAddress> pinned_row_;
    std::vector<RowAddress> addresses_;
    std::map<workflow::RunIdentity, RunSummary> summaries_;
    std::optional<workflow::RunHandle> selected_run_;
    std::optional<core::InvocationKey> selected_call_;
    QString statistics_;
    QString details_;
    QString message_;
};

} // namespace visionpro::ui
