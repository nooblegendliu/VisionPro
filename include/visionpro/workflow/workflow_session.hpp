#pragma once

#include <visionpro/workflow/workflow_engine.hpp>

#include <functional>
#include <stop_token>

namespace visionpro::workflow {

enum class WorkflowSessionStatus { Empty, Active, Switching, Closing, Closed };
enum class SessionOperationKind { Activate, Close, Publish };
struct SessionOperationId {
    std::uint64_t value;
    auto operator<=>(const SessionOperationId&) const = default;
};
struct SessionOperation {
    SessionOperationId id;
    SessionOperationKind kind;
};
struct WorkflowSessionProgress {
    WorkflowSessionStatus status{WorkflowSessionStatus::Empty};
    std::optional<ConfigSnapshot> active{};
    std::optional<SessionOperation> operation{};
};

struct RunArguments {
    RunTarget target;
    std::vector<RunInput> inputs;
    RunTrigger trigger;
    std::optional<core::CorrelationId> correlation{};
    std::optional<DebugStatePolicy> debug_state{};
    std::vector<core::GlobalVariableEntry> globals{};
};

struct SolutionDelivery {
    RunIdentity source;
    core::OutputId output;
    RunIdentity target;
    core::InputId input;
};
struct SolutionLinkProgress {
    WorkflowControlLink link;
    ControlLinkStatus status{ControlLinkStatus::Pending};
    std::optional<ControlCloseReason> close_reason{};
};
struct SolutionRunReport {
    std::uint64_t sequence;
    SnapshotIdentity snapshot;
    ExecutionStatus status{ExecutionStatus::Succeeded};
    std::vector<RunHandle> runs{};
    std::vector<SolutionDelivery> deliveries{};
    std::vector<core::WorkflowId> skipped{};
    std::vector<core::Diagnostic> diagnostics{};
    std::vector<SolutionLinkProgress> links{};
};

// Side-effect-free preflight for a draft before replacing an active capture.
[[nodiscard]] core::Result<std::monostate> validate_solution_inputs(const ConfigSnapshot& snapshot,
    std::span<const WorkflowRunInputs> inputs, RunTrigger trigger);

// Owns one active in-memory solution. Configuration and management operations
// are independent of accepted run identities.
class WorkflowSession final : public RunSubmitter {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<WorkflowSession>> create(const core::ToolRegistry& registry,
        std::shared_ptr<InvocationResourceProvider> resources = {}, WorkflowEngineOptions options = {});
    ~WorkflowSession() override;
    WorkflowSession(const WorkflowSession&) = delete;
    WorkflowSession& operator=(const WorkflowSession&) = delete;

    // Candidate validation leaves the old session available. A successful
    // replacement stops admission and drains the old engine before publishing.
    [[nodiscard]] core::Result<ConfigSnapshot> activate(const Solution& candidate);
    // Candidate-owned resources are used for the new engine. A null provider
    // uses the session's default provider.
    [[nodiscard]] core::Result<ConfigSnapshot> activate(const Solution& candidate,
        std::shared_ptr<InvocationResourceProvider> provider);
    // Publish a validated draft without submitting a run. Compatible shared
    // data and node state remain in the same solution session.
    [[nodiscard]] core::Result<ConfigSnapshot> publish(const ConfigSnapshot& base, const Solution& draft,
        std::shared_ptr<InvocationResourceProvider> provider);
    // Prepare a run from the draft against the exact active base capture.
    // Only admission publishes the new snapshot and its provider. A null
    // provider means the draft has no resource provider for this publication.
    [[nodiscard]] RunSubmission publish_and_submit(const ConfigSnapshot& base, const Solution& draft,
        const RunArguments& run, std::shared_ptr<InvocationResourceProvider> provider);
    [[nodiscard]] WorkflowSessionProgress inspect() const;
    [[nodiscard]] core::Result<std::shared_ptr<const core::SharedData>> shared_preview() const;
    [[nodiscard]] core::Result<std::shared_ptr<const core::CorrelationJoinAccess>> correlation_preview() const;
    [[nodiscard]] RunSubmission submit(const RunRequest& request) override;
    // Blocking host entry point. Each admitted child uses this round's capture;
    // its handle remains available for result lookup after physical cleanup.
    [[nodiscard]] core::Result<SolutionRunReport> run_solution(const ConfigSnapshot& snapshot,
        std::span<const WorkflowRunInputs> inputs, RunTrigger trigger, std::stop_token stop = {},
        std::optional<core::CorrelationId> correlation = {});
    [[nodiscard]] core::Result<RunObservation> observe(std::size_t capacity = 32);
    [[nodiscard]] core::Result<std::optional<NodeResult>> lookup_result(
        const RunIdentity& run, const core::InvocationKey& invocation) const;
    [[nodiscard]] core::Result<std::monostate> reset_node_state(const NodeTarget& target);

    // Blocking management call. Existing handles survive the closed session.
    [[nodiscard]] core::Result<std::monostate> close();

private:
    friend class TriggerControl;
    // Called only at final admission, after resource preparation and capture revalidation.
    using AdmissionGate = std::function<RunSubmission(const std::function<RunSubmission()>&)>;
    [[nodiscard]] RunSubmission submit_guarded(const RunRequest& request, const AdmissionGate* gate);
    struct Impl;
    explicit WorkflowSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace visionpro::workflow
