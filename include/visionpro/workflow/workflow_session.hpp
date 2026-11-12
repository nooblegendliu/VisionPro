#pragma once

#include <visionpro/workflow/workflow_engine.hpp>

namespace visionpro::workflow {

enum class WorkflowSessionStatus { Empty, Active, Switching, Closing, Closed };
enum class SessionOperationKind { Activate, Close };
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
    [[nodiscard]] WorkflowSessionProgress inspect() const;
    [[nodiscard]] RunSubmission submit(const RunRequest& request) override;
    [[nodiscard]] core::Result<RunObservation> observe(std::size_t capacity = 32);
    [[nodiscard]] core::Result<std::optional<NodeResult>> lookup_result(
        const RunIdentity& run, const core::InvocationKey& invocation) const;
    [[nodiscard]] core::Result<std::monostate> reset_node_state(const NodeTarget& target);

    // Blocking management call. Existing handles survive the closed session.
    [[nodiscard]] core::Result<std::monostate> close();

private:
    struct Impl;
    explicit WorkflowSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace visionpro::workflow
