#pragma once

#include "run_inputs.hpp"

#include <visionpro/infrastructure/solution_file.hpp>
#include <visionpro/infrastructure/camera_trigger.hpp>
#include <visionpro/infrastructure/image_directory_trigger.hpp>
#include <visionpro/infrastructure/image_archive.hpp>
#include <visionpro/workflow/trigger_control.hpp>

#include <QChronoTimer>
#include <QObject>
#include <QTimer>

#include <map>

namespace visionpro::ui {

struct RunDocument {
    std::optional<infrastructure::SolutionDocument> source;
    std::optional<std::filesystem::path> directory;
    std::vector<infrastructure::NamedResourceRoot> roots;
    std::uint64_t edit_generation{};
    std::shared_ptr<infrastructure::ImageArchivePool> archives{};
};

struct SessionSharedPreview {
    workflow::ConfigSnapshot snapshot;
    std::shared_ptr<const core::SharedData> data;
    std::shared_ptr<const core::CorrelationJoinAccess> joins;
};

struct SolutionEntryInputs {
    core::WorkflowId workflow;
    EnteredRunInputs entered;
};

enum class SolutionActivity { Idle, Running, CancelRequested, CleaningUp };

class RunController final : public QObject {
    Q_OBJECT

public:
    explicit RunController(core::ToolRegistry registry, QObject* parent = nullptr);
    ~RunController() override;

    void start(workflow::Solution draft, RunDocument document, workflow::RunArguments arguments,
        std::vector<RunImageInput> images);
    void cancel(const workflow::RunHandle& run);
    void start_solution(workflow::Solution draft, RunDocument document, std::vector<SolutionEntryInputs> inputs);
    void cancel_solution();
    bool can_cancel_solution() const noexcept { return solution_stop_ && !solution_stop_->stop_requested(); }
    SolutionActivity solution_activity() const noexcept;
    std::vector<workflow::RunHandle> cancellable_workflow_runs(core::WorkflowId workflow) const;
    const std::optional<workflow::SolutionRunReport>& solution_report() const noexcept { return solution_report_; }
    void arm_trigger(workflow::Solution draft, RunDocument document, core::WorkflowId workflow);
    void send_frame(core::WorkflowId workflow, std::int64_t frame);
    void stop_trigger(core::WorkflowId workflow, workflow::TriggerStopMode mode);
    bool can_arm_trigger(const workflow::Workflow& flow) const;
    bool can_send_frame(core::WorkflowId workflow) const;
    bool can_stop_trigger(core::WorkflowId workflow) const;
    QString trigger_status(core::WorkflowId workflow) const;
    QString camera_status(core::WorkflowId workflow) const;
    core::Result<std::optional<SessionSharedPreview>> shared_preview() const;
    bool can_reset_state(const workflow::NodeTarget& target) const;
    core::Result<std::monostate> reset_state(const workflow::NodeTarget& target);
    void reset_document();
    void flush_archive();
    bool can_flush_archive() const noexcept { return archives_ && can_start() && triggers_.empty(); }
    void set_observing(bool visible);
    void set_limits(std::size_t history, std::size_t notifications);

    bool can_start() const noexcept { return !preparing_ && !retiring_ && active_.empty(); }
    bool can_submit(const workflow::Workflow& flow) const noexcept;
    bool can_cancel(const workflow::RunHandle& run) const noexcept;
    bool has_session_work() const noexcept { return session_ || preparing_ || retiring_; }
    const std::vector<workflow::RunHandle>& runs() const noexcept { return runs_; }
    std::uint64_t dropped() const noexcept { return dropped_; }
    std::size_t history_limit() const noexcept { return history_limit_; }
    std::size_t observation_capacity() const noexcept { return observation_capacity_; }

signals:
    void snapshot_published(const visionpro::workflow::SnapshotIdentity& snapshot, std::uint64_t edit_generation);
    void changed();
    void operation_failed(const visionpro::core::Diagnostic& diagnostic);
    void trigger_notice(const QString& message);
    void run_finished(const visionpro::workflow::RunHandle& run, visionpro::workflow::ExecutionStatus status);

private:
    void refresh_progress();
    void close_session();
    void subscribe();
    void unsubscribe();
    void trim_history();
    bool workflow_active(core::WorkflowId workflow) const noexcept;
    bool ensure_session();
    enum class RunOrigin { Manual, Trigger, Solution };
    void record_submission(const workflow::RunSubmission& submission, RunOrigin origin, std::string description = {});
    void dispatch_trigger(core::WorkflowId workflow, std::optional<std::int64_t> frame);
    void poll_directory(core::WorkflowId workflow);
    void disarm_triggers();

    core::ToolRegistry registry_;
    std::shared_ptr<workflow::WorkflowSession> session_;
    std::shared_ptr<infrastructure::ImageArchivePool> archives_;
    struct ArmedTrigger {
        std::shared_ptr<workflow::TriggerControl> control;
        std::shared_ptr<infrastructure::CameraTriggerStream> camera;
        std::shared_ptr<infrastructure::ImageDirectoryTrigger> directory;
        std::unique_ptr<QChronoTimer> directory_timer;
        std::unique_ptr<QChronoTimer> timer;
        QString camera_fault;
        QString camera_last_frame;
        bool dispatching{};
        bool stopping{};
    };
    std::map<core::WorkflowId, ArmedTrigger> triggers_;
    std::map<core::WorkflowId, QString> camera_status_;
    std::uint64_t document_generation_{};
    std::optional<workflow::RunObservation> observation_;
    struct ActiveRun {
        workflow::RunHandle handle;
        RunOrigin origin;
        workflow::ExecutionStatus status{workflow::ExecutionStatus::Pending};
        bool cancelling{};
        bool inspection_failed{};
        bool cancellable() const noexcept {
            return !cancelling && (status == workflow::ExecutionStatus::Pending || status == workflow::ExecutionStatus::Running);
        }
    };
    std::map<workflow::RunIdentity, ActiveRun> active_; // Retained until physical quiescence, independently of the panel.
    std::vector<workflow::RunHandle> runs_;
    QTimer refresh_;
    std::size_t history_limit_{16};
    std::size_t observation_capacity_{32};
    std::uint64_t observation_generation_{0};
    std::uint64_t subscription_dropped_{0};
    std::uint64_t dropped_{0};
    bool observing_{false};
    bool preparing_{false};
    bool retiring_{false};
    std::optional<std::stop_source> solution_stop_;
    std::optional<workflow::SolutionRunReport> solution_report_;
};

} // namespace visionpro::ui
