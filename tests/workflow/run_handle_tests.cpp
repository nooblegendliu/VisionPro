#include <visionpro/workflow/run_handle.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
int checks = 0;
int failures = 0;
void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template<class T> T take(Result<T> value)
{
    if (const auto* failure = std::get_if<Diagnostic>(&value)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(value));
}
template<class T> void error(const Result<T>& value, ErrorCode code)
{
    const auto* failure = std::get_if<Diagnostic>(&value);
    check(failure && failure->code == code, "typed failure has the expected code");
}
template<UuidIdentity T> T uuid(int digit = 1)
{ return take(IdFactory::parse<T>(std::to_string(digit) + "0112233-4455-4677-8899-aabbccddeeff")); }
template<SchemaIdentity T> T token(std::string_view value)
{ return take(SchemaIdFactory::parse<T>(value)); }
template<RuntimeIdentity T> T count(std::uint64_t value)
{ return take(RuntimeIdFactory::from_value<T>(value)); }
Diagnostic diagnostic() { return {ErrorCode::InvalidValue, Severity::Error, {}, "controlled failure"}; }
InputId input_id() { return token<InputId>("input"); }
OutputId output_id() { return token<OutputId>("output"); }

RunRequest request(std::int64_t value = 7, bool single_node = false)
{
    ToolDescriptor tool{token<ToolTypeId>("test.handle"), "Handle", 1,
        {token<SchemaId>("test.handle.schema"), 1}, {}, {}, {{output_id(), "Output", DataType::integer()}}};
    Workflow flow{uuid<WorkflowId>(), "Flow", {token<SchemaId>("test.public"), 1,
        {{input_id(), "Input", DataType::integer()}}},
        {{uuid<NodeId>(), "First", ToolConfiguration{tool.id, 1}},
         {uuid<NodeId>(2), "Second", ToolConfiguration{tool.id, 1}}}, {}, {}, {uuid<NodeId>()}};
    Solution solution{uuid<SolutionId>(), "Solution", 1, 1, {flow}};
    const auto snapshot = take(ConfigSnapshot::capture(solution, std::vector{tool},
        {uuid<SolutionSessionId>(), count<ConfigSnapshotId>(1)}));
    if (single_node) {
        return take(RunRequest::create(snapshot, NodeTarget{flow.id, uuid<NodeId>()}, {},
            {TriggerKind::Debug, token<SchemaId>("test.debug")}));
    }
    return take(RunRequest::create(snapshot, WorkflowTarget{flow.id}, {{input_id(), DataValue::integer(value)}},
        {TriggerKind::Manual, token<SchemaId>("test.manual")}));
}

RunRequest group_request()
{
    const auto baseline = request();
    auto solution = baseline.snapshot().solution();
    auto& flow = solution.workflows.front();
    PublicInterface ports{token<SchemaId>("test.group"), 1, {}, {{output_id(), "Output", DataType::integer()}}};
    flow.nodes.push_back({uuid<NodeId>(3), "Outer group", GroupConfiguration{ports}});
    flow.nodes.push_back({uuid<NodeId>(4), "Inner group", GroupConfiguration{ports}});
    flow.containment = {{uuid<NodeId>(3), uuid<NodeId>(4)}, {uuid<NodeId>(4), uuid<NodeId>()}};
    flow.entries = {uuid<NodeId>(3)};
    const auto snapshot = take(ConfigSnapshot::capture(solution, baseline.snapshot().tools(), baseline.snapshot().identity()));
    return take(RunRequest::create(snapshot, NodeTarget{flow.id, uuid<NodeId>(3)}, {},
        {TriggerKind::Debug, token<SchemaId>("test.debug")}));
}

class ControlledRun final : public RunControl {
public:
    ControlledRun(const RunRequest& prepared, std::uint64_t run) : RunControl(prepared, count<WorkflowRunId>(run)) {}
    RunProgress progress;
    std::optional<NodeResult> record;
    bool throw_now = false;
    bool fail_now = false;
    bool cancelled = false;
    bool bad_wait = false;
    bool bad_cancel = false;
    mutable int lookups = 0;
    int cancels = 0;
    Result<RunProgress> inspect() const override
    {
        if (throw_now) { throw std::runtime_error("backend detail"); }
        if (fail_now) { return diagnostic(); }
        return progress;
    }
    Result<RunWaitResult> wait(RunWaitTarget target, std::chrono::steady_clock::time_point) override
    {
        if (throw_now) { throw 42; }
        if (fail_now) { return diagnostic(); }
        const bool complete = progress.status != ExecutionStatus::Pending && progress.status != ExecutionStatus::Running;
        bool ready = target == RunWaitTarget::Completion ? complete : progress.quiescent;
        if (bad_wait) { ready = !ready; }
        // Deterministic deadline check only; no actual worker or blocking wait.
        return RunWaitResult{ready ? RunWaitOutcome::Satisfied : RunWaitOutcome::DeadlineExpired, progress};
    }
    Result<RunCancelDisposition> cancel() override
    {
        ++cancels;
        if (throw_now) { throw std::runtime_error("cancel failed"); }
        if (fail_now) { return diagnostic(); }
        if (bad_cancel) { return static_cast<RunCancelDisposition>(99); }
        if (progress.status != ExecutionStatus::Pending && progress.status != ExecutionStatus::Running) {
            return RunCancelDisposition::AlreadyCompleted;
        }
        if (cancelled) { return RunCancelDisposition::AlreadyRequested; }
        cancelled = true;
        return RunCancelDisposition::Requested;
    }
    Result<std::optional<NodeResult>> lookup(const InvocationKey&) const override
    {
        ++lookups;
        if (throw_now) { throw std::runtime_error("lookup failed"); }
        if (fail_now) { return diagnostic(); }
        return record;
    }
};

NodeResult result(const RunIdentity& run, NodeId node = uuid<NodeId>(), std::uint64_t invocation = 1,
    bool bad_schema = false)
{
    return take(NodeResult::create({{run, {run.run, node, count<ExecutionScopeId>(0), count<InvocationId>(invocation)}, count<AttemptId>(0)},
        ExecutionStatus::Succeeded, Judgement::NG,
        {{output_id(), bad_schema ? DataType::boolean() : DataType::integer(),
            bad_schema ? DataValue::boolean(false) : DataValue::integer(12)}}}));
}

// A single-slot contract double, not the production admission/Busy engine.
class ControlledSubmitter final : public RunSubmitter {
public:
    std::shared_ptr<ControlledRun> active;
    bool reject = false;
    std::uint64_t next = 1;
    RunSubmission submit(const RunRequest& prepared) override
    {
        if (reject) { return RunRejected{diagnostic()}; }
        if (active && !active->progress.quiescent) { return RunBusy{active->identity()}; }
        active = std::make_shared<ControlledRun>(prepared, next++);
        return take(RunHandle::from_accepted(active));
    }
};

static_assert(!std::is_default_constructible_v<RunHandle>);
static_assert(!std::is_convertible_v<RunRequest, RunHandle>);
static_assert(!std::is_convertible_v<RunHandle, ConfigValue>);
static_assert(std::is_abstract_v<RunControl> && std::is_abstract_v<RunSubmitter>);

void handles()
{
    error(RunHandle::from_accepted({}), ErrorCode::InvalidRunHandle);
    ControlledSubmitter backend;
    auto first = backend.submit(request(7));
    check(std::holds_alternative<RunHandle>(first), "accepted request has a handle, not a bool");
    auto handle = std::get<RunHandle>(first);
    check(handle.identity().snapshot == handle.request().snapshot().identity(), "accepted identity owns the request snapshot");
    check(handle.identity().workflow == uuid<WorkflowId>() && handle.identity().run == count<WorkflowRunId>(1), "accepted run identity is fixed");
    check(*handle.request().input(input_id())->value.as_int64() == 7, "request lives after temporary input owners leave");
    check(take(handle.inspect()).status == ExecutionStatus::Pending, "acceptance does not claim tool execution");
    const auto deadline = std::chrono::steady_clock::now();
    auto expired = take(handle.wait(RunWaitTarget::Completion, deadline));
    check(expired.outcome == RunWaitOutcome::DeadlineExpired && expired.progress.status == ExecutionStatus::Pending,
        "wait expiry does not manufacture a TimedOut run");
    const auto busy = backend.submit(request(99));
    check(std::holds_alternative<RunBusy>(busy) && std::get<RunBusy>(busy).active == handle.identity(), "Busy identifies the existing run");
    check(*handle.request().input(input_id())->value.as_int64() == 7, "Busy request cannot replace active input");
    backend.reject = true;
    check(std::holds_alternative<RunRejected>(backend.submit(request(100))), "rejection is distinct from Busy and acceptance");
    backend.reject = false;
    check(take(handle.cancel()) == RunCancelDisposition::Requested, "cooperative cancellation reports a request");
    check(take(handle.cancel()) == RunCancelDisposition::AlreadyRequested, "repeat cancellation is distinguishable");
    check(!take(handle.inspect()).quiescent, "cancellation does not release active ownership");
    backend.active->progress = {ExecutionStatus::Cancelled, false, {}};
    check(take(handle.wait(RunWaitTarget::Completion, deadline)).outcome == RunWaitOutcome::Satisfied, "logical completion can be observed first");
    check(take(handle.wait(RunWaitTarget::Quiescence, deadline)).outcome == RunWaitOutcome::DeadlineExpired, "physical exit remains a separate wait");
    check(take(handle.cancel()) == RunCancelDisposition::AlreadyCompleted, "cancel after terminal reports no new request");
    check(std::holds_alternative<RunBusy>(backend.submit(request(101))), "double retains slot until physical exit");
    backend.active->progress.quiescent = true;
    check(take(handle.wait(RunWaitTarget::Quiescence, deadline)).outcome == RunWaitOutcome::Satisfied, "quiescence is explicit");
    const auto second = std::get<RunHandle>(backend.submit(request(8)));
    check(second.identity().run != handle.identity().run && *second.request().input(input_id())->value.as_int64() == 8,
        "new acceptance has a new ID and its own input");
    check(*handle.request().input(input_id())->value.as_int64() == 7, "prior handle retains its fixed request");
    auto copy = handle;
    auto moved = std::move(copy);
    check(copy.identity() == moved.identity(), "handle moves preserve valid owning views like other immutable models");

    std::weak_ptr<ControlledRun> lifetime;
    {
        ControlledSubmitter supervisor;
        {
            auto submitted = supervisor.submit(request(42));
            lifetime = supervisor.active;
        }
        check(!lifetime.expired() && supervisor.active->cancels == 0, "dropping clients neither cancels nor releases backend active ownership");
        supervisor.active->progress = {ExecutionStatus::Succeeded, true, {}};
        supervisor.active.reset();
    }
    check(lifetime.expired(), "backend can release ownership after explicit exit");
}

void boundaries()
{
    auto control = std::make_shared<ControlledRun>(request(), 1);
    auto handle = take(RunHandle::from_accepted(control));
    const auto key = result(handle.identity()).data().identity.invocation;
    check(!take(handle.lookup(key)), "missing exact current invocation remains absent");
    auto other = key; other.workflow_run = count<WorkflowRunId>(2);
    const auto prior = control->lookups;
    error(handle.lookup(other), ErrorCode::ResultIdentityMismatch);
    other = key; other.node = uuid<NodeId>(3);
    error(handle.lookup(other), ErrorCode::ResultIdentityMismatch);
    check(control->lookups == prior, "outside run/node lookup is rejected before calling backend");
    control->record = result(handle.identity());
    const auto saved = *take(handle.lookup(key));
    check(saved.data().judgement == Judgement::NG, "NG remains a successful technical result");
    auto wrong_run = handle.identity(); wrong_run.snapshot.snapshot = count<ConfigSnapshotId>(2);
    control->record = result(wrong_run);
    error(handle.lookup(key), ErrorCode::ResultIdentityMismatch);
    wrong_run = handle.identity(); wrong_run.snapshot.session = uuid<SolutionSessionId>(2);
    control->record = result(wrong_run);
    error(handle.lookup(key), ErrorCode::ResultIdentityMismatch);
    wrong_run = handle.identity(); wrong_run.workflow = uuid<WorkflowId>(2);
    control->record = result(wrong_run);
    error(handle.lookup(key), ErrorCode::ResultIdentityMismatch);
    auto wrong_scope = result(handle.identity()).data();
    wrong_scope.identity.invocation.scope = count<ExecutionScopeId>(1);
    control->record = take(NodeResult::create(wrong_scope));
    error(handle.lookup(key), ErrorCode::ResultIdentityMismatch);
    control->record = result(handle.identity(), uuid<NodeId>(), 2);
    error(handle.lookup(key), ErrorCode::ResultIdentityMismatch);
    control->record = result(handle.identity(), uuid<NodeId>(), 1, true);
    check(std::holds_alternative<Diagnostic>(handle.lookup(key)), "schema-invalid backend output is rejected");
    control->record.reset();
    check(*std::get<DataValue>(saved.output(output_id())->content).as_int64() == 12, "returned immutable result owns its content independently");

    auto node_control = std::make_shared<ControlledRun>(request(7, true), 1);
    auto node_handle = take(RunHandle::from_accepted(node_control));
    other = key; other.node = uuid<NodeId>(2);
    error(node_handle.lookup(other), ErrorCode::ResultIdentityMismatch);
    check(node_control->lookups == 0, "single-node handle cannot query a sibling result");

    auto group_control = std::make_shared<ControlledRun>(group_request(), 1);
    const auto group_handle = take(RunHandle::from_accepted(group_control));
    for (const auto node : {uuid<NodeId>(3), uuid<NodeId>(4), uuid<NodeId>()}) {
        group_control->record = result(group_handle.identity(), node);
        const auto group_key = group_control->record->data().identity.invocation;
        check(take(group_handle.lookup(group_key)).has_value(), "Group query covers self, child Group and recursive descendant");
    }
    const auto group_queries = group_control->lookups;
    other = key; other.node = uuid<NodeId>(2);
    error(group_handle.lookup(other), ErrorCode::ResultIdentityMismatch);
    check(group_control->lookups == group_queries, "Group query excludes siblings before backend access");

    const auto deadline = std::chrono::steady_clock::now();
    control->throw_now = true;
    error(handle.inspect(), ErrorCode::ExecutionBoundaryFailure);
    error(handle.wait(RunWaitTarget::Completion, deadline), ErrorCode::ExecutionBoundaryFailure);
    error(handle.cancel(), ErrorCode::ExecutionBoundaryFailure);
    error(handle.lookup(key), ErrorCode::ExecutionBoundaryFailure);
    control->throw_now = false; control->fail_now = true;
    error(handle.inspect(), ErrorCode::InvalidValue);
    error(handle.wait(RunWaitTarget::Completion, deadline), ErrorCode::InvalidValue);
    error(handle.cancel(), ErrorCode::InvalidValue);
    error(handle.lookup(key), ErrorCode::InvalidValue);
    control->fail_now = false;
    for (const auto status : {ExecutionStatus::Skipped, static_cast<ExecutionStatus>(99)}) {
        control->progress.status = status;
        error(handle.inspect(), ErrorCode::InvalidRunHandle);
    }
    control->progress = {ExecutionStatus::Running, true, {}};
    error(handle.inspect(), ErrorCode::InvalidRunHandle);
    control->progress = {ExecutionStatus::Failed, true, {}};
    error(handle.inspect(), ErrorCode::InvalidRunHandle);
    control->progress.diagnostics = {diagnostic()};
    check(take(handle.inspect()).status == ExecutionStatus::Failed, "failed terminal keeps its diagnostic");
    control->progress.status = ExecutionStatus::Succeeded;
    error(handle.inspect(), ErrorCode::InvalidRunHandle);
    control->progress = {};
    control->bad_wait = true;
    error(handle.wait(RunWaitTarget::Completion, deadline), ErrorCode::InvalidRunHandle);
    control->bad_wait = false; control->bad_cancel = true;
    error(handle.cancel(), ErrorCode::InvalidRunHandle);
    error(handle.wait(static_cast<RunWaitTarget>(99), deadline), ErrorCode::InvalidRunHandle);
    control->progress.diagnostics = {diagnostic()}; control->progress.diagnostics.front().message.clear();
    error(handle.inspect(), ErrorCode::InvalidRunHandle);
}
} // namespace

int main()
{
    try { handles(); boundaries(); }
    catch (const std::exception& exception) { ++failures; std::cerr << "EXCEPTION: " << exception.what() << '\n'; }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
