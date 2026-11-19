#pragma once

#include "run_inputs.hpp"

#include <visionpro/infrastructure/solution_file.hpp>

#include <QObject>
#include <QTimer>

namespace visionpro::ui {

struct RunDocument {
    std::optional<infrastructure::SolutionDocument> source;
    std::optional<std::filesystem::path> directory;
    std::vector<infrastructure::NamedResourceRoot> roots;
};

class RunController final : public QObject {
    Q_OBJECT

public:
    explicit RunController(core::ToolRegistry registry, QObject* parent = nullptr);

    void start(workflow::Solution draft, RunDocument document, workflow::RunArguments arguments,
        std::vector<RunImageInput> images);
    void cancel();
    bool can_reset_state(const workflow::NodeTarget& target) const;
    core::Result<std::monostate> reset_state(const workflow::NodeTarget& target);
    void reset_document();
    void set_observing(bool visible);
    void set_limits(std::size_t history, std::size_t notifications);

    bool can_start() const noexcept { return !preparing_ && !retiring_ && !active_; }
    bool can_cancel() const noexcept;
    bool has_session_work() const noexcept { return session_ || preparing_ || retiring_; }
    const std::vector<workflow::RunHandle>& runs() const noexcept { return runs_; }
    std::uint64_t dropped() const noexcept { return dropped_; }
    std::size_t history_limit() const noexcept { return history_limit_; }
    std::size_t observation_capacity() const noexcept { return observation_capacity_; }

signals:
    void changed();
    void operation_failed(const visionpro::core::Diagnostic& diagnostic);

private:
    void refresh_progress();
    void close_session();
    void subscribe();
    void unsubscribe();
    void trim_history();

    core::ToolRegistry registry_;
    std::shared_ptr<workflow::WorkflowSession> session_;
    std::optional<workflow::RunObservation> observation_;
    std::optional<workflow::RunHandle> active_;
    std::vector<workflow::RunHandle> runs_;
    QTimer refresh_;
    workflow::ExecutionStatus active_status_{workflow::ExecutionStatus::Pending};
    std::size_t history_limit_{16};
    std::size_t observation_capacity_{32};
    std::uint64_t observation_generation_{0};
    std::uint64_t subscription_dropped_{0};
    std::uint64_t dropped_{0};
    bool observing_{false};
    bool preparing_{false};
    bool cancelling_{false};
    bool inspection_failed_{false};
    bool retiring_{false};
};

} // namespace visionpro::ui
