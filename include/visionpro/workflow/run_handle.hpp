#pragma once

#include <visionpro/workflow/resolver.hpp>
#include <visionpro/workflow/run.hpp>

namespace visionpro::workflow {

enum class RunWaitTarget { Completion, Quiescence };
enum class RunWaitOutcome { Satisfied, DeadlineExpired };
enum class RunCancelDisposition { Requested, AlreadyRequested, AlreadyCompleted };

enum class NodeSkipReason { Disabled, UpstreamClosed, FailFast, RunCancelled, RunTimedOut, NotSelected };
struct RunBindingTrace {
    BindingTarget target;
    std::vector<SourceTrace> sources;
};
struct RunResourceTrace {
    core::SchemaId slot;
    core::ResourceReference reference;
};
struct RunNodeProgress {
    core::NodeId node;
    ExecutionStatus status{ExecutionStatus::Pending};
    // Only started calls have a key that can address a NodeResult.
    std::optional<core::InvocationKey> invocation{};
    std::optional<NodeSkipReason> skip_reason{};
    std::vector<RunBindingTrace> bindings{};
    std::vector<RunResourceTrace> resources{}; // References from leases acquired for the call.
};

enum class ControlLinkStatus { Pending, Arrived, Closed };
enum class ControlCloseReason { NotSelected, Disabled, Failed, Cancelled, TimedOut };
// Runtime state of one control link in the run's root scope.
struct RunControlLinkProgress {
    core::ControlLinkId link;
    ControlLinkStatus status{ControlLinkStatus::Pending};
    std::optional<ControlCloseReason> close_reason{};
};

// A logical terminal status does not imply that callbacks, leases or workers
// have exited. Pending is an accepted run which has not started yet.
struct RunProgress {
    ExecutionStatus status{ExecutionStatus::Pending};
    bool quiescent{false};
    std::vector<core::Diagnostic> diagnostics;
    std::vector<RunNodeProgress> nodes{}; // In planned control order when reported by the backend.
    std::vector<RunControlLinkProgress> control_links{};
    std::vector<core::NodeId> call_order{}; // Actual dispatch order, after input and lease preparation.
};
struct RunWaitResult { RunWaitOutcome outcome; RunProgress progress; };

// Backend extension point, not an engine or an admission service. The backend
// must keep its own owning reference until quiescence, even if all clients drop
// their handles. It serializes state transitions and any callback delivery.
// Implementations use condition/event waits (never a polling loop), permit
// concurrent wait/cancel/query, and keep the prepared request fixed.
class RunControl {
public:
    virtual ~RunControl() = default;
    RunControl(const RunControl&) = delete;
    RunControl& operator=(const RunControl&) = delete;
    [[nodiscard]] const RunRequest& request() const noexcept { return request_; }
    [[nodiscard]] const RunIdentity& identity() const noexcept { return identity_; }
    [[nodiscard]] virtual core::Result<RunProgress> inspect() const = 0;
    [[nodiscard]] virtual core::Result<RunWaitResult> wait(RunWaitTarget target,
        std::chrono::steady_clock::time_point deadline) = 0;
    [[nodiscard]] virtual core::Result<RunCancelDisposition> cancel() = 0;
    [[nodiscard]] virtual core::Result<std::optional<NodeResult>> lookup(
        const core::InvocationKey& invocation) const = 0;
protected:
    RunControl(const RunRequest& request, core::WorkflowRunId run);
private:
    const RunRequest request_;
    const RunIdentity identity_;
};

// Copyable accepted-run view. Destruction neither blocks nor implicitly cancels.
// Wait deadlines report wait expiry, never synthesize a TimedOut run status.
class RunHandle final {
public:
    RunHandle(const RunHandle&) noexcept = default;
    RunHandle& operator=(const RunHandle&) noexcept = default;
    RunHandle(RunHandle&& other) noexcept : control_(other.control_) {}
    RunHandle& operator=(RunHandle&& other) noexcept { control_ = other.control_; return *this; }
    // Called by an admission backend after reserving this identity and request.
    // This constructor does not reserve a slot, allocate an ID or start work.
    [[nodiscard]] static core::Result<RunHandle> from_accepted(std::shared_ptr<RunControl> control);
    [[nodiscard]] const RunRequest& request() const noexcept { return control_->request(); }
    [[nodiscard]] const RunIdentity& identity() const noexcept { return control_->identity(); }
    [[nodiscard]] core::Result<RunProgress> inspect() const;
    [[nodiscard]] core::Result<RunWaitResult> wait(RunWaitTarget target,
        std::chrono::steady_clock::time_point deadline) const;
    [[nodiscard]] core::Result<RunCancelDisposition> cancel() const;
    [[nodiscard]] core::Result<std::optional<NodeResult>> lookup(const core::InvocationKey& invocation) const;
private:
    explicit RunHandle(std::shared_ptr<RunControl> control) : control_(std::move(control)) {}
    std::shared_ptr<RunControl> control_;
};

struct RunBusy { RunIdentity active; };
struct RunRejected { core::Diagnostic diagnostic; };
using RunSubmission = std::variant<RunHandle, RunBusy, RunRejected>;

// Busy/rejection must not modify active inputs
// or retain the new request as an implicit future run. Accepted returns the exact
// prepared request with a newly allocated run ID. Implementations translate
// unexpected exceptions before leaving this boundary; no throwing-after-accept.
class RunSubmitter {
public:
    virtual ~RunSubmitter() = default;
    [[nodiscard]] virtual RunSubmission submit(const RunRequest& request) = 0;
};

} // namespace visionpro::workflow
