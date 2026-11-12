#pragma once

#include <visionpro/core/timing.hpp>
#include <visionpro/core/tool_descriptor.hpp>

#include <chrono>
#include <atomic>
#include <memory>
#include <span>

namespace visionpro::core {

struct FixedToolParameter { ParameterId id; ConfigValue value; };
struct ToolResourceBinding { SchemaId slot; ResourceReference resource; };
struct ToolExecutionConfigurationData {
    SolutionSessionId session;
    ConfigSnapshotId snapshot;
    WorkflowId workflow;
    NodeId node;
    ToolDescriptor tool;
    // Exactly the SnapshotBuild parameters, already fixed by the workflow layer.
    std::vector<FixedToolParameter> parameters;
    std::vector<ToolResourceBinding> resources;
};
class ToolExecutionConfiguration final {
public:
    ToolExecutionConfiguration(const ToolExecutionConfiguration&) noexcept = default;
    ToolExecutionConfiguration& operator=(const ToolExecutionConfiguration&) noexcept = default;
    ToolExecutionConfiguration(ToolExecutionConfiguration&& other) noexcept : data_(other.data_) {}
    ToolExecutionConfiguration& operator=(ToolExecutionConfiguration&& other) noexcept { data_ = other.data_; return *this; }
    [[nodiscard]] static Result<ToolExecutionConfiguration> create(const ToolExecutionConfigurationData& data);
    [[nodiscard]] const ToolExecutionConfigurationData& data() const noexcept;
    // Immutable node-specific state specification, derived from fixed parameters.
    [[nodiscard]] const std::optional<NodeStateDefinition>& effective_state() const noexcept;
private:
    struct Storage;
    explicit ToolExecutionConfiguration(std::shared_ptr<const Storage> data) : data_(std::move(data)) {}
    std::shared_ptr<const Storage> data_;
};

struct ToolInvocationIdentity {
    SolutionSessionId session;
    ConfigSnapshotId snapshot;
    InvocationKey invocation;
    AttemptId attempt;
    auto operator<=>(const ToolInvocationIdentity&) const = default;
};
struct FixedToolInput { InputId id; DataValue value; };
struct FixedInvocationParameter { ParameterId id; DataValue value; };
using ToolValueTarget = std::variant<InputId, ParameterId>;
struct ResolvedRegionInput {
    ToolValueTarget target;
    InheritedRegion reference;
    RegionDefinition value;
    ToolInvocationIdentity producer;
    std::size_t source_index{0};
    std::vector<DiagnosticPathStep> path;
};

// Adapter-owned RAII lease. Concrete capabilities stay in typed adapter interfaces;
// no opaque device handle or service lookup is exposed through the invocation ABI.
class ResourceLease {
public:
    virtual ~ResourceLease() = default;
    [[nodiscard]] const ResourceReference& reference() const noexcept { return reference_; }
protected:
    explicit ResourceLease(ResourceReference reference) : reference_(std::move(reference)) {}
private:
    ResourceReference reference_;
};
struct InvocationResourceLease { SchemaId slot; std::shared_ptr<const ResourceLease> lease; };
struct InvocationContextData {
    ToolExecutionConfiguration configuration;
    ToolInvocationIdentity identity;
    // Complete resolved values, including materialized defaults. No binding lookup
    // or fallback occurs here. Workflow validates runtime domains and constraints.
    std::vector<FixedToolInput> inputs;
    std::vector<FixedInvocationParameter> parameters;
    std::vector<ResolvedRegionInput> regions;
    std::vector<InvocationResourceLease> resources;
};
class InvocationContext final {
public:
    InvocationContext(const InvocationContext&) noexcept = default;
    InvocationContext& operator=(const InvocationContext&) noexcept = default;
    InvocationContext(InvocationContext&& other) noexcept : data_(other.data_) {}
    InvocationContext& operator=(InvocationContext&& other) noexcept { data_ = other.data_; return *this; }
    [[nodiscard]] static Result<InvocationContext> create(const InvocationContextData& data);
    [[nodiscard]] const InvocationContextData& data() const noexcept { return *data_; }
private:
    explicit InvocationContext(std::shared_ptr<const InvocationContextData> data) : data_(std::move(data)) {}
    std::shared_ptr<const InvocationContextData> data_;
};

enum class ToolCompletionStatus { Succeeded, Failed, Cancelled, TimedOut };
enum class ToolJudgement { NotEvaluated, OK, NG };
struct ToolOutput { OutputId id; OutputContent content; };
struct ToolCompletion {
    ToolInvocationIdentity identity;
    ToolCompletionStatus status;
    ToolJudgement judgement;
    std::vector<ToolOutput> outputs;
    std::vector<Diagnostic> diagnostics;
    std::optional<std::chrono::nanoseconds> elapsed{};
    std::vector<ControlOutputId> selected_control_outputs{};
    std::vector<TimingMeasurement> timings{};
};
[[nodiscard]] Result<std::monostate> validate_tool_completion(const InvocationContext& context,
    const ToolCompletion& result);

namespace detail { struct InvocationState; }
class InvocationOperation;
class InvocationControl;
class ToolInstanceFactory;
class InvocationCompletion final {
public:
    InvocationCompletion(const InvocationCompletion&) noexcept = default;
    InvocationCompletion& operator=(const InvocationCompletion&) noexcept = default;
    InvocationCompletion(InvocationCompletion&& other) noexcept : state_(other.state_) {}
    InvocationCompletion& operator=(InvocationCompletion&& other) noexcept { state_ = other.state_; return *this; }
    // A backend retains this token until it can prove no callback/worker still
    // accesses the context, instance or leases. Completion alone does not release them.
    // Before start returns its observable operation, a startup failure takes
    // precedence over a provisional callback completion from that startup.
    // Register control BEFORE launching asynchronous activity. Until registration,
    // begin failure promises there is no asynchronous activity needing cleanup.
    [[nodiscard]] Result<std::monostate> register_control(std::shared_ptr<InvocationControl> control) const;
    [[nodiscard]] Result<std::monostate> complete(const ToolCompletion& result) const;
    // Report that backend work has stopped. PhysicalExit is published after
    // startup and any in-flight cancellation callback have returned and the
    // operation has released its owned invocation state.
    [[nodiscard]] Result<std::monostate> mark_exited() const;
    [[nodiscard]] bool cancellation_requested() const;
private:
    friend class InvocationOperation;
    explicit InvocationCompletion(std::shared_ptr<detail::InvocationState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::InvocationState> state_;
};
class InvocationControl {
public:
    virtual ~InvocationControl() = default;
    // Called outside operation locks. May report completion and backend exit
    // synchronously; the operation defers cleanup until this callback returns.
    virtual void request_cancel() = 0;
};
struct AsynchronousInvocation { std::shared_ptr<InvocationControl> control; };
using InvocationStart = std::variant<ToolCompletion, AsynchronousInvocation>;
class ToolInstance {
public:
    virtual ~ToolInstance() = default;
    [[nodiscard]] const ToolExecutionConfiguration& configuration() const noexcept { return configuration_; }
protected:
    explicit ToolInstance(ToolExecutionConfiguration configuration) : configuration_(std::move(configuration)) {}
    // Returning ToolCompletion promises physical exit. Asynchronous work first
    // registers control and retains completion; its returned control must match.
    [[nodiscard]] virtual Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context,
        InvocationCompletion completion) = 0;
private:
    friend class InvocationOperation;
    friend class InvocationCompletion;
    friend struct detail::InvocationState;
    friend Result<std::shared_ptr<ToolInstance>> create_tool_instance(ToolInstanceFactory&, const ToolExecutionConfiguration&);
    ToolExecutionConfiguration configuration_;
    std::atomic<std::size_t> active_{0};
    std::atomic<bool> issued_{false};
};
class ToolInstanceFactory {
public:
    virtual ~ToolInstanceFactory() = default;
    // Each successful call creates an independent instance for this fixed configuration.
    [[nodiscard]] virtual Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) = 0;
};
[[nodiscard]] Result<std::shared_ptr<ToolInstance>> create_tool_instance(ToolInstanceFactory& factory,
    const ToolExecutionConfiguration& configuration);

struct InvocationProgress {
    std::optional<ToolCompletion> completion;
    bool physically_exited{false};
    bool cancellation_requested{false};
};
enum class InvocationWaitTarget { LogicalCompletion, PhysicalExit };
enum class InvocationWaitOutcome { Reached, TimedOut };
struct InvocationWaitResult { InvocationWaitOutcome outcome; InvocationProgress progress; };
enum class CancellationRequest { Requested, AlreadyRequested, AlreadyExited, CleanupInProgress };

class InvocationWakeup {
public:
    virtual ~InvocationWakeup() = default;
    // Called outside operation locks on the registering or backend thread.
    // Implementations should enqueue a wakeup and return promptly. Concurrent
    // signals for different targets need not arrive in transition order.
    virtual void signal(const ToolInvocationIdentity& identity, InvocationWaitTarget target) noexcept = 0;
};

class InvocationOperation final {
public:
    InvocationOperation(const InvocationOperation&) noexcept = default;
    InvocationOperation& operator=(const InvocationOperation&) noexcept = default;
    InvocationOperation(InvocationOperation&& other) noexcept : state_(other.state_) {}
    InvocationOperation& operator=(InvocationOperation&& other) noexcept { state_ = other.state_; return *this; }
    [[nodiscard]] static Result<InvocationOperation> start(std::shared_ptr<ToolInstance> instance,
        const InvocationContext& context);
    [[nodiscard]] InvocationProgress poll() const;
    [[nodiscard]] bool physically_exited() const;
    // One weakly held, one-shot notification per target. If the target has
    // already been reached, signal synchronously before returning.
    [[nodiscard]] Result<std::monostate> notify_when(InvocationWaitTarget target,
        std::weak_ptr<InvocationWakeup> wakeup) const;
    [[nodiscard]] Result<InvocationWaitResult> wait_for(InvocationWaitTarget target, std::chrono::milliseconds timeout) const;
    [[nodiscard]] Result<CancellationRequest> request_cancel() const;
private:
    explicit InvocationOperation(std::shared_ptr<detail::InvocationState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::InvocationState> state_;
};

} // namespace visionpro::core
