#pragma once

#include <visionpro/workflow/resolver.hpp>
#include <visionpro/workflow/run.hpp>

namespace visionpro::workflow {

enum class RunWaitTarget { Completion, Quiescence };
enum class RunWaitOutcome { Satisfied, DeadlineExpired };
enum class RunCancelDisposition { Requested, AlreadyRequested, AlreadyCompleted };
enum class RunAdmission { Immediate, Queued };

enum class NodeSkipReason { Disabled, UpstreamClosed, FailFast, RunCancelled, RunTimedOut, NotSelected };
struct RunBindingTrace {
    BindingTarget target;
    std::vector<SourceTrace> sources;
    // Frozen expressions and exact producer identities from successful resolution.
    std::vector<ResolvedRegionReference> regions{};
    // Scalar actually delivered by the resolver, including selectors and conversions.
    std::variant<std::monostate, std::int64_t, double> numeric_value{};
};
struct RunResourceTrace {
    core::SchemaId slot;
    core::ResourceReference reference;
};
struct RunAttempt {
    core::AttemptId attempt;
    ExecutionStatus status;
    std::vector<core::Diagnostic> diagnostics;
};
struct RunNodeProgress {
    core::NodeId node;
    ExecutionStatus status{ExecutionStatus::Pending};
    // Started calls and preparation failures sent to an error outlet have an addressable result.
    std::optional<core::InvocationKey> invocation{};
    std::optional<NodeSkipReason> skip_reason{};
    std::vector<RunBindingTrace> bindings{};
    std::vector<RunResourceTrace> resources{}; // References from leases acquired for the call.
    std::optional<core::ExecutionScopeId> scope{};
    std::vector<RunAttempt> attempts{};
    bool error_handled{false};
};

enum class ControlLinkStatus { Pending, Arrived, Closed };
enum class ControlCloseReason { NotSelected, Disabled, Failed, Cancelled, TimedOut };
// Each body call tracks the state of its own control links.
struct RunControlLinkProgress {
    core::ControlLinkId link;
    ControlLinkStatus status{ControlLinkStatus::Pending};
    std::optional<ControlCloseReason> close_reason{};
    std::optional<core::ExecutionScopeId> scope{};
};

struct RunExportTrace {
    core::OutputId output;
    OutputReference source;
    ResultIdentity producer;
};
struct WorkflowPublicResult {
    std::vector<OutputSlot> outputs;
    std::vector<RunExportTrace> exports;
    std::vector<core::ControlOutputId> selected_control_outputs;
};
using RunLoopExportTrace = RunExportTrace;
struct RunLoopIteration {
    core::ExecutionScopeId scope;
    core::InvocationKey loop;
    std::int64_t index;
    std::vector<RunLoopExportTrace> exports{};
};

struct RunGroupCall {
    core::ExecutionScopeId scope;
    core::InvocationKey group;
    std::vector<RunExportTrace> exports{};
};

// A logical terminal status does not imply that callbacks, leases or workers
// have exited. Pending is an accepted run which has not started yet.
struct RunProgress {
    ExecutionStatus status{ExecutionStatus::Pending};
    bool quiescent{false};
    std::vector<core::Diagnostic> diagnostics;
    std::vector<RunNodeProgress> nodes{}; // Root in planned control order, followed by each body call in its own planned order.
    std::vector<RunControlLinkProgress> control_links{};
    std::vector<core::NodeId> call_order{}; // Actual dispatch order, after input and lease preparation.
    std::vector<RunLoopIteration> iterations{};
    std::vector<RunGroupCall> groups{};
    // From first dispatch step to logical completion. Queuing and physical cleanup are excluded.
    std::optional<std::chrono::nanoseconds> elapsed{};
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
    [[nodiscard]] RunAdmission admission() const noexcept { return admission_; }
    [[nodiscard]] virtual core::Result<RunProgress> inspect() const = 0;
    [[nodiscard]] virtual core::Result<RunWaitResult> wait(RunWaitTarget target,
        std::chrono::steady_clock::time_point deadline) = 0;
    [[nodiscard]] virtual core::Result<RunCancelDisposition> cancel() = 0;
    [[nodiscard]] virtual core::Result<std::optional<NodeResult>> lookup(
        const core::InvocationKey& invocation) const = 0;
    [[nodiscard]] virtual core::Result<WorkflowPublicResult> public_result() const;
protected:
    RunControl(const RunRequest& request, core::WorkflowRunId run, RunAdmission admission = RunAdmission::Immediate);
private:
    const RunRequest request_;
    const RunIdentity identity_;
    const RunAdmission admission_;
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
    // Records whether this request initially occupied a slot or entered the workflow FIFO.
    [[nodiscard]] RunAdmission admission() const noexcept { return control_->admission(); }
    [[nodiscard]] core::Result<RunProgress> inspect() const;
    [[nodiscard]] core::Result<RunWaitResult> wait(RunWaitTarget target,
        std::chrono::steady_clock::time_point deadline) const;
    [[nodiscard]] core::Result<RunCancelDisposition> cancel() const;
    [[nodiscard]] core::Result<std::optional<NodeResult>> lookup(const core::InvocationKey& invocation) const;
    // Root WorkflowTarget requests publish their complete public interface at logical completion.
    // Failed, cancelled and timed-out runs expose only typed absence at their data ports.
    [[nodiscard]] core::Result<WorkflowPublicResult> public_result() const;
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
