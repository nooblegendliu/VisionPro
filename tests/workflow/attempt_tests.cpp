#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/tool_execution.hpp>
#include <visionpro/workflow/workflow_session.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void check(bool condition, std::string_view message)
{ if (!condition) { throw std::runtime_error(std::string(message)); } }

template<class T> T take(Result<T> result)
{
    if (const auto* diagnostic = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(diagnostic->message); }
    return std::get<T>(std::move(result));
}
template<SchemaIdentity T> T key(std::string_view name) { return take(SchemaIdFactory::parse<T>(name)); }
template<UuidIdentity T> T fresh() { return take(IdFactory::generate<T>()); }

enum class Backend { Async, Blocking, Failure, NegativeJudgement, UnconfirmedSend, MisdeclaredSend, InvalidReply, MissingResponse, DeduplicatedSend };
bool asynchronous(Backend backend) { return backend == Backend::Async || backend == Backend::MissingResponse; }
struct Started {
    ToolInvocationIdentity identity;
    std::int64_t input;
    InvocationCompletion completion;
};

ToolCompletion success(const Started& call, ToolJudgement judgement = ToolJudgement::OK)
{
    return {call.identity, ToolCompletionStatus::Succeeded, judgement,
        {{key<OutputId>("value"), DataValue::integer(call.input)}}, {}};
}
ToolCompletion failed(const Started& call, ErrorCode code, std::string message)
{
    return {call.identity, ToolCompletionStatus::Failed, ToolJudgement::NotEvaluated, {},
        {{code, Severity::Error, {}, std::move(message)}}};
}

// A backend reports physical exit only when the test releases it. The calls
// retain notification tokens and copied values, never the lease or context.
class Device {
public:
    explicit Device(Backend behavior) : behavior(behavior) {}

    void record(const Started& call)
    {
        std::unique_lock lock(mutex_);
        calls_.push_back(call);
        changed_.notify_all();
        if (behavior == Backend::Blocking) { changed_.wait(lock, [&] { return closing_; }); }
        else if (closing_ && asynchronous(behavior)) { lock.unlock(); exit_async(call); }
    }
    Started await(std::size_t count)
    {
        std::unique_lock lock(mutex_);
        check(changed_.wait_for(lock, 5s, [&] { return calls_.size() >= count; }), "Tool did not reach the attempt barrier.");
        return calls_.at(count - 1);
    }
    std::size_t count() const { const std::lock_guard lock(mutex_); return calls_.size(); }
    void hold_cancel(NodeId node)
    {
        const std::lock_guard lock(mutex_);
        blocked_cancel_ = node;
    }
    void request_cancel(NodeId node)
    {
        std::unique_lock lock(mutex_);
        ++cancellations_;
        if (blocked_cancel_ == node) { cancel_entered_ = true; }
        changed_.notify_all();
        if (blocked_cancel_ == node) { changed_.wait(lock, [&] { return closing_; }); }
    }
    void await_blocked_cancel()
    {
        std::unique_lock lock(mutex_);
        check(changed_.wait_for(lock, 5s, [&] { return cancel_entered_; }), "First cancellation did not reach its gate.");
    }
    void await_cancel(unsigned count = 1)
    {
        std::unique_lock lock(mutex_);
        check(changed_.wait_for(lock, 5s, [&] { return cancellations_ >= count; }), "Timed-out attempt did not request cancellation.");
    }
    void block_sibling()
    {
        std::unique_lock lock(mutex_);
        sibling_entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return closing_; });
    }
    void await_sibling()
    {
        std::unique_lock lock(mutex_);
        check(changed_.wait_for(lock, 5s, [&] { return sibling_entered_; }), "Synchronous sibling never entered begin.");
    }
    void send(const Started& call)
    {
        const std::lock_guard lock(mutex_);
        const auto operation = operation_key(call.identity);
        // Model a remote endpoint that applies the instruction before its acknowledgement is lost.
        if (behavior != Backend::DeduplicatedSend || applied_.insert(operation).second) { ++effects; }
    }
    void close()
    {
        std::vector<Started> calls;
        {
            const std::lock_guard lock(mutex_);
            closing_ = true;
            calls = calls_;
        }
        changed_.notify_all();
        if (asynchronous(behavior)) { for (const auto& call : calls) { exit_async(call); } }
    }
    static void exit_async(const Started& call)
    {
        static_cast<void>(call.completion.complete(success(call)));
        static_cast<void>(call.completion.mark_exited());
    }
    const Backend behavior;
    std::atomic<unsigned> effects{};
private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Started> calls_;
    std::set<SessionInvocationKey> applied_;
    bool closing_{};
    bool sibling_entered_{};
    unsigned cancellations_{};
    std::optional<NodeId> blocked_cancel_;
    bool cancel_entered_{};
};

class Control final : public InvocationControl {
public:
    Control(const std::shared_ptr<Device>& device, NodeId node) : device_(device), node_(node) {}
    void request_cancel() override { if (const auto device = device_.lock()) { device->request_cancel(node_); } }
private:
    std::weak_ptr<Device> device_;
    NodeId node_;
};
class Probe final : public ToolInstance {
public:
    Probe(const ToolExecutionConfiguration& config, const std::shared_ptr<Device>& device)
        : ToolInstance(config), device_(device) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion completion) override
    {
        const auto& data = context->data();
        const Started call{data.identity, *data.inputs.front().value.as_int64(), completion};
        if (data.configuration.data().tool.id.text() == "s67.marker") { return InvocationStart{success(call)}; }
        const auto device = device_.lock();
        if (!device) { throw std::runtime_error("The controlled backend expired."); }
        if (data.configuration.data().tool.id.text() == "s67.blocking") {
            device->block_sibling();
            return InvocationStart{success(call)};
        }
        if (asynchronous(device->behavior)) {
            auto control = std::make_shared<Control>(device, call.identity.invocation.node);
            take(completion.register_control(control));
            if (device->behavior == Backend::MissingResponse) { device->send(call); }
            device->record(call);
            return InvocationStart{AsynchronousInvocation{std::move(control)}};
        }
        device->record(call);
        switch (device->behavior) {
        case Backend::Failure: return InvocationStart{failed(call, ErrorCode::InvalidValue, "Injected computation failure.")};
        case Backend::NegativeJudgement: return InvocationStart{success(call, ToolJudgement::NG)};
        case Backend::UnconfirmedSend:
        case Backend::MisdeclaredSend:
        case Backend::InvalidReply:
        case Backend::DeduplicatedSend:
            device->send(call);
            if (call.identity.attempt.value() == 0) {
                auto error = failed(call, ErrorCode::ExternalEffectUnknown, "No acknowledgement from the output endpoint.");
                if (device->behavior == Backend::InvalidReply) {
                    error.outputs.push_back({key<OutputId>("value"), InvalidOutput{error.diagnostics.front()}});
                    error.diagnostics.clear();
                }
                return InvocationStart{std::move(error)};
            }
            return InvocationStart{success(call)};
        default: return InvocationStart{success(call)};
        }
    }
    std::weak_ptr<Device> device_;
};
class Factory final : public ToolInstanceFactory {
public:
    explicit Factory(std::shared_ptr<Device> device) : device_(std::move(device)) {}
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& config) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<Probe>(config, device_)}; }
private:
    std::shared_ptr<Device> device_;
};

struct LeaseCounts { std::atomic<unsigned> held{}, acquired{}, overlaps{}; };
class Lease final : public ResourceLease {
public:
    Lease(const ResourceReference& resource, std::shared_ptr<LeaseCounts> counts) : ResourceLease(resource), counts_(std::move(counts))
    {
        if (counts_->held.fetch_add(1) != 0) { ++counts_->overlaps; }
        ++counts_->acquired;
    }
    ~Lease() override { --counts_->held; }
private:
    std::shared_ptr<LeaseCounts> counts_;
};
class Provider final : public InvocationResourceProvider {
public:
    std::span<const ResourceKind> kinds() const noexcept override
    { static constexpr std::array supported{ResourceKind::Binary}; return supported; }
    Result<std::shared_ptr<const ResourceLease>> acquire(const ToolExecutionConfiguration&, const ToolResourceBinding& binding) override
    {
        if (unavailable) { return Diagnostic{ErrorCode::ResourceNotFound, Severity::Error, {}, "Instrument is unavailable."}; }
        return std::shared_ptr<const ResourceLease>{std::make_shared<Lease>(binding.resource, counts)};
    }
    const std::shared_ptr<LeaseCounts> counts = std::make_shared<LeaseCounts>();
    bool unavailable{};
};

InputDescriptor input()
{
    InputDescriptor desc{key<InputId>("sample"), "Sample", DataType::integer()};
    desc.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::PublicInput};
    return desc;
}
ToolDescriptor descriptor(std::string_view name)
{
    return {key<ToolTypeId>(name), std::string(name), 1, {key<SchemaId>(name), 1}, {}, {input()},
        {{key<OutputId>("value"), "Value", DataType::integer()}}};
}
ToolRegistry registry(const std::shared_ptr<Device>& device)
{
    auto probe = descriptor("s67.probe");
    probe.resources = {{key<SchemaId>("device"), ResourceKind::Binary, true, true}};
    probe.execution.retry = ToolRetry::Idempotent;
    if (asynchronous(device->behavior)) {
        probe.execution.mode = ToolExecutionMode::Asynchronous;
        probe.execution.cancellation = ToolCancellation::Cooperative;
    }
    if (device->behavior == Backend::UnconfirmedSend || device->behavior == Backend::MissingResponse
        || device->behavior == Backend::DeduplicatedSend) {
        probe.execution.effects = ToolEffects::External;
        probe.execution.retry = device->behavior == Backend::DeduplicatedSend ? ToolRetry::ProtocolDeduplicated : ToolRetry::Never;
    }
    const auto marker = descriptor("s67.marker");
    const auto blocker = descriptor("s67.blocking");
    const auto factory = std::make_shared<Factory>(device);
    const std::array<ToolRegistration, 3> tools{{
        {probe, {probe.schema.id, 1, 1, {}}, factory}, {marker, {marker.schema.id, 1, 1, {}}, factory},
        {blocker, {blocker.schema.id, 1, 1, {}}, factory}}};
    return take(ToolRegistry::create(tools));
}

struct Fixture {
    Fixture(Backend behavior, std::uint32_t timeout, std::uint32_t retries, bool recovery)
        : device(std::make_shared<Device>(behavior)), catalog(registry(device)), session(take(WorkflowSession::create(catalog)))
    {
        const auto sample = input();
        Node probe{source, "Probe", ToolConfiguration{key<ToolTypeId>("s67.probe"), 1},
            {{sample.id, {ScopedInputReference{flow_id, {}, sample.id, {}}}}},
            {{key<SchemaId>("device"), {resource, ResourceKind::Binary, {}}}}};
        probe.execution_policy.timeout_ms = timeout;
        probe.execution_policy.max_retries = retries;
        const auto outlet = key<ControlOutputId>("error");
        if (recovery) { probe.execution_policy.error_output = outlet; }
        const auto marker = [&](NodeId node, std::int64_t value) {
            return Node{node, "Marker", ToolConfiguration{key<ToolTypeId>("s67.marker"), 1},
                {{sample.id, {ConfigValue::integer(value)}}}};
        };
        Workflow flow{flow_id, "Attempt tests", {key<SchemaId>("s67.flow"), 1, {sample}}, {probe, marker(normal, 1)},
            {{fresh<ControlLinkId>(), source, {}, normal}}, {}, {source}};
        if (recovery) {
            flow.nodes.push_back(marker(handler, -1));
            flow.control_links.push_back({fresh<ControlLinkId>(), source, outlet, handler});
        }
        solution.workflows.push_back(std::move(flow));
        solution.resources.push_back({resource, "Test lease", ResourceKind::Binary, ExternalFileResource{
            {PathBase::SolutionDirectory, "probe.bin"}, ResourceReadPolicy::PerInvocation}});
    }
    ~Fixture() { device->close(); }
    RunHandle start()
    {
        auto active = session->inspect().active;
        if (!active) { active = take(session->activate(solution, provider)); }
        const auto request = take(RunRequest::create(*active, WorkflowTarget{flow_id},
            {{key<InputId>("sample"), DataValue::integer(73)}}, {TriggerKind::Manual, key<SchemaId>("s67.test")}));
        auto submitted = session->submit(request);
        if (const auto* refusal = std::get_if<RunRejected>(&submitted)) { throw std::runtime_error(refusal->diagnostic.message); }
        return std::get<RunHandle>(std::move(submitted));
    }
    const std::shared_ptr<Device> device;
    const ToolRegistry catalog;
    const std::unique_ptr<WorkflowSession> session;
    const std::shared_ptr<Provider> provider = std::make_shared<Provider>();
    const WorkflowId flow_id = fresh<WorkflowId>();
    const NodeId source = fresh<NodeId>(), normal = fresh<NodeId>(), handler = fresh<NodeId>();
    const ResourceId resource = fresh<ResourceId>();
    Solution solution{fresh<SolutionId>(), "Attempt fixture", 1, 1};
};

RunProgress wait(const RunHandle& run, RunWaitTarget target = RunWaitTarget::Quiescence)
{
    const auto result = take(run.wait(target, Clock::now() + 5s));
    check(result.outcome == RunWaitOutcome::Satisfied, "Run did not reach the required milestone.");
    return result.progress;
}
const RunNodeProgress& node(const RunProgress& progress, NodeId id)
{
    const auto found = std::ranges::find(progress.nodes, id, &RunNodeProgress::node);
    check(found != progress.nodes.end(), "Node is missing from the run report.");
    return *found;
}
void attempts(const RunNodeProgress& node, std::initializer_list<ExecutionStatus> expected)
{
    check(node.attempts.size() == expected.size(), "Attempt count did not match the configured limit.");
    std::size_t i{};
    for (const auto status : expected) {
        const auto& attempt = node.attempts[i];
        check(attempt.attempt.value() == i && attempt.status == status, "Attempt order, ID or terminal status is incorrect.");
        ++i;
    }
}
bool has_code(std::span<const Diagnostic> diagnostics, ErrorCode code)
{ return std::ranges::any_of(diagnostics, [=](const auto& diagnostic) { return diagnostic.code == code; }); }

bool has_unknown_effect(std::span<const Diagnostic> diagnostics, const ToolInvocationIdentity& identity)
{
    return std::ranges::any_of(diagnostics, [&](const auto& diagnostic) {
        return diagnostic.code == ErrorCode::ExternalEffectUnknown && diagnostic.operation_key == operation_key(identity);
    });
}
NodeResult record(const RunHandle& run, const RunNodeProgress& node)
{
    check(node.invocation.has_value(), "Expected the node to enter its invocation.");
    return take(run.lookup(*node.invocation)).value();
}

void timeout_retry()
{
    Fixture f(Backend::Async, 300, 1, true);
    take(f.session->activate(f.solution, f.provider));
    auto observer = take(f.session->observe(16));
    const auto run = f.start();
    const auto first = f.device->await(1);
    f.device->await_cancel();
    check(f.device->count() == 1 && f.provider->counts->held == 1 && !take(run.lookup(first.identity.invocation)),
        "Intermediate timeout published a result, released a lease or started a concurrent retry.");
    check(take(run.inspect()).call_order == std::vector{f.source}, "A successor ran before the attempt actually exited.");
    auto edited = f.solution;
    edited.workflows.front().nodes.front().bindings.front().sources = {ConfigValue::integer(900)};
    const auto updated = take(f.session->publish(*f.session->inspect().active, edited, f.provider));
    Device::exit_async(first);
    const auto second = f.device->await(2);
    check(first.identity.invocation == second.identity.invocation && first.identity.attempt.value() == 0
        && second.identity.attempt.value() == 1 && second.identity.snapshot == first.identity.snapshot
        && second.identity.snapshot != updated.identity().snapshot && first.input == 73 && second.input == first.input,
        "Retry did not preserve the logical invocation, input and snapshot across an intervening publication.");
    check(f.provider->counts->acquired == 2 && f.provider->counts->held == 1 && f.provider->counts->overlaps == 0,
        "Second attempt must reacquire the exclusive lease after the first has released it.");
    take(second.completion.complete(success(second)));
    take(second.completion.mark_exited());
    const auto done = wait(run);
    check(std::holds_alternative<Diagnostic>(first.completion.complete(success(first))),
        "A duplicate late completion of the retired attempt was accepted.");
    check(done.status == ExecutionStatus::Succeeded && std::ranges::count(done.call_order, f.normal) == 1
        && node(done, f.handler).status == ExecutionStatus::Skipped && f.provider->counts->held == 0,
        "Success must route the normal successor exactly once and skip the error outlet.");
    attempts(node(done, f.source), {ExecutionStatus::TimedOut, ExecutionStatus::Succeeded});
    const auto result = record(run, node(done, f.source));
    check(result.data().identity.attempt == second.identity.attempt && result.data().status == ExecutionStatus::Succeeded
        && *std::get<DataValue>(result.output(key<OutputId>("value"))->content).as_int64() == 73,
        "The logical address must resolve to the final accepted attempt.");
    unsigned publications{};
    while (const auto event = observer.wait(Clock::now()).event) {
        if (event->kind == RunObservationKind::ResultPublished && event->result->invocation == first.identity.invocation) {
            ++publications;
            check(event->result->attempt == second.identity.attempt, "An intermediate attempt was published.");
        }
    }
    check(publications == 1, "There must be one final publication per logical call.");
}

void logical_timeout_and_cancel()
{
    Fixture blocking(Backend::Blocking, 150, 0, false);
    const auto run = blocking.start();
    blocking.device->await(1);
    const auto logical = wait(run, RunWaitTarget::Completion);
    check(logical.status == ExecutionStatus::TimedOut && !logical.quiescent && blocking.provider->counts->held == 1,
        "A synchronous begin blocked past its deadline must become TimedOut before lease return.");
    attempts(node(logical, blocking.source), {ExecutionStatus::TimedOut});
    blocking.device->close();
    check(wait(run).status == ExecutionStatus::TimedOut && blocking.provider->counts->held == 0,
        "Returning late from the synchronous tool must keep TimedOut and release the lease.");

    Fixture exhausted(Backend::Async, 150, 1, false);
    const auto timed = exhausted.start();
    const auto initial = exhausted.device->await(1);
    exhausted.device->await_cancel();
    Device::exit_async(initial);
    const auto last = exhausted.device->await(2);
    exhausted.device->await_cancel(2);
    const auto limit = wait(timed, RunWaitTarget::Completion);
    check(limit.status == ExecutionStatus::TimedOut && !limit.quiescent
        && record(timed, node(limit, exhausted.source)).data().identity.attempt == last.identity.attempt,
        "Exhausted deadlines must commit the second timeout while its backend is still running.");
    attempts(node(limit, exhausted.source), {ExecutionStatus::TimedOut, ExecutionStatus::TimedOut});
    Device::exit_async(last);
    check(wait(timed).status == ExecutionStatus::TimedOut && exhausted.device->count() == 2,
        "Physical exit after an exhausted deadline cannot restart the call.");

    Fixture pending(Backend::Async, 150, 1, true);
    const auto cancelling = pending.start();
    const auto first = pending.device->await(1);
    pending.device->await_cancel();
    take(cancelling.cancel());
    Device::exit_async(first);
    const auto stopped = wait(cancelling);
    check(stopped.status == ExecutionStatus::Cancelled && pending.device->count() == 1
        && std::ranges::count(stopped.call_order, pending.normal) == 0 && std::ranges::count(stopped.call_order, pending.handler) == 0,
        "Cancellation between attempts must suppress both retry and recovery.");
    check(record(cancelling, node(stopped, pending.source)).data().status == ExecutionStatus::Cancelled,
        "The logical invocation must have one cancelled record after the pending retry is cancelled.");

    Fixture siblings(Backend::Async, 300, 0, false);
    auto& flow = siblings.solution.workflows.front();
    Node blocker{fresh<NodeId>(), "Untimed blocking sibling", ToolConfiguration{key<ToolTypeId>("s67.blocking"), 1},
        {{key<InputId>("sample"), {ConfigValue::integer(0)}}}};
    flow.entries.insert(flow.entries.begin(), blocker.id);
    flow.nodes.insert(flow.nodes.begin(), blocker);
    const auto fork = siblings.start();
    siblings.device->await_sibling();
    siblings.device->await(1);
    siblings.device->await_cancel();
    check(node(take(fork.inspect()), siblings.source).status == ExecutionStatus::TimedOut,
        "An untimed blocking sibling in the same workflow must not prevent timeout of the asynchronous call.");
    siblings.device->close();
    check(wait(fork).status == ExecutionStatus::TimedOut, "Both branches must retire under the original timeout.");

    // Both nodes can recover and own different resources. Stall the first driver's
    // cancellation callback through the second deadline; no release is needed for
    // either logical record. Fixture teardown also opens the gate on assertion failure.
    Fixture paired(Backend::Async, 200, 0, true);
    auto& parallel = paired.solution.workflows.front();
    auto second_probe = parallel.nodes.front();
    second_probe.id = fresh<NodeId>();
    second_probe.execution_policy.timeout_ms = 650;
    auto separate_resource = paired.solution.resources.front();
    separate_resource.id = fresh<ResourceId>();
    second_probe.resources.front().resource.id = separate_resource.id;
    paired.solution.resources.push_back(separate_resource);
    parallel.nodes.push_back(second_probe);
    parallel.entries.push_back(second_probe.id);
    parallel.control_links.push_back({fresh<ControlLinkId>(), second_probe.id, second_probe.execution_policy.error_output, paired.handler});
    paired.device->hold_cancel(paired.source);
    const auto dual = paired.start();
    paired.device->await(2);
    paired.device->await_blocked_cancel();
    paired.device->await_cancel(2);
    const auto in_cleanup = take(dual.inspect());
    for (const auto probe_id : {paired.source, second_probe.id}) {
        const auto& timed_out = node(in_cleanup, probe_id);
        check(timed_out.status == ExecutionStatus::TimedOut && record(dual, timed_out).data().status == ExecutionStatus::TimedOut,
            "A stalled cancellation callback must not postpone another active node's logical timeout.");
    }
    check(paired.provider->counts->held == 2, "Both instrument leases stay live through backend exit.");
    paired.device->close();
    const auto handled = wait(dual);
    check(handled.status == ExecutionStatus::Succeeded && std::ranges::count(handled.call_order, paired.handler) == 1
        && paired.provider->counts->held == 0, "Both timed branches should join one recovery after their actual exit.");
}

void recovery_and_judgement()
{
    for (const bool recover : {false, true}) {
        Fixture f(Backend::Failure, 0, 2, recover);
        const auto run = f.start();
        const auto done = wait(run);
        const auto& probe = node(done, f.source);
        attempts(probe, {ExecutionStatus::Failed, ExecutionStatus::Failed, ExecutionStatus::Failed});
        check(f.device->count() == 3 && has_code(done.diagnostics, ErrorCode::InvalidValue)
            && std::ranges::all_of(probe.attempts, [](const auto& attempt) { return has_code(attempt.diagnostics, ErrorCode::InvalidValue); }),
            "Exhausted retries must preserve each failure and the final diagnostic.");
        check(record(run, probe).data().status == ExecutionStatus::Failed && probe.error_handled == recover
            && node(done, f.normal).status == ExecutionStatus::Skipped, "The original failure must remain available after recovery.");
        if (recover) {
            check(done.status == ExecutionStatus::Succeeded && std::ranges::count(done.call_order, f.handler) == 1,
                "Only the final failure should activate the recovery outlet.");
        } else { check(done.status == ExecutionStatus::Failed, "Unhandled failure must end the run."); }
    }
    Fixture f(Backend::NegativeJudgement, 0, 2, true);
    const auto run = f.start();
    const auto done = wait(run);
    check(done.status == ExecutionStatus::Succeeded && f.device->count() == 1 && node(done, f.normal).status == ExecutionStatus::Succeeded
        && record(run, node(done, f.source)).data().judgement == Judgement::NG && node(done, f.handler).status == ExecutionStatus::Skipped,
        "Business NG is a successful call and must neither retry nor activate the error outlet.");

    Fixture missing(Backend::Failure, 0, 2, true);
    missing.provider->unavailable = true;
    const auto missing_run = missing.start();
    const auto recovered = wait(missing_run);
    const auto& preparation = node(recovered, missing.source);
    check(recovered.status == ExecutionStatus::Succeeded && preparation.error_handled && preparation.attempts.empty()
        && preparation.status == ExecutionStatus::Failed && missing.device->count() == 0
        && std::ranges::count(recovered.call_order, missing.handler) == 1
        && has_code(record(missing_run, preparation).data().diagnostics, ErrorCode::ResourceNotFound),
        "Failure acquiring a lease must produce a queryable final record and one recovery dispatch without starting a tool.");

    Fixture disabled(Backend::Failure, 0, 1, true);
    disabled.solution.workflows.front().nodes.back().enabled = false;
    const auto disabled_run = disabled.start();
    const auto unhandled = wait(disabled_run);
    check(unhandled.status == ExecutionStatus::Failed && !node(unhandled, disabled.source).error_handled
        && node(unhandled, disabled.normal).status == ExecutionStatus::Skipped,
        "A disabled error handler cannot count as handling the failure.");
}

void external_deduplication()
{
    Fixture unsafe(Backend::UnconfirmedSend, 0, 0, true);
    auto run = unsafe.start();
    auto done = wait(run);
    const auto& unknown = node(done, unsafe.source);
    const auto original = unsafe.device->await(1).identity;
    attempts(unknown, {ExecutionStatus::Failed});
    check(unsafe.device->effects == 1 && unsafe.device->count() == 1 && unknown.error_handled
        && has_unknown_effect(unknown.attempts.front().diagnostics, original)
        && has_unknown_effect(done.diagnostics, original) && operation_key(original).key == *unknown.invocation,
        "An unknown external effect must keep its operation identity and be delivered only once.");
    unsafe.solution.workflows.front().nodes.front().execution_policy.max_retries = 1;
    check(std::holds_alternative<Diagnostic>(unsafe.session->activate(unsafe.solution, unsafe.provider)) && unsafe.device->effects == 1,
        "An unsafe retry configuration must be rejected before another send.");

    for (const auto reported : {Backend::MisdeclaredSend, Backend::InvalidReply}) {
        Fixture misdeclared(reported, 0, 2, false);
        const auto wrong_run = misdeclared.start();
        const auto wrong = wait(wrong_run);
        const auto& source = node(wrong, misdeclared.source);
        attempts(source, {ExecutionStatus::Failed});
        const auto id = misdeclared.device->await(1).identity;
        check(wrong.status == ExecutionStatus::Failed && misdeclared.device->effects == 1 && misdeclared.device->count() == 1
            && has_unknown_effect(wrong.diagnostics, id) && has_unknown_effect(source.attempts.front().diagnostics, id),
            "An explicit unknown effect must override a pure/idempotent descriptor and prevent automatic resend.");
        if (reported == Backend::InvalidReply) {
            const auto final = record(wrong_run, source);
            const auto& invalid = std::get<InvalidOutput>(final.output(key<OutputId>("value"))->content);
            check(invalid.diagnostic.operation_key == operation_key(id), "An invalid reply must retain its unknown-effect operation key.");
        }
    }

    for (const bool cancel : {false, true}) {
        Fixture silent(Backend::MissingResponse, cancel ? 0 : 150, 0, false);
        const auto silent_run = silent.start();
        const auto applied = silent.device->await(1);
        if (cancel) { take(silent_run.cancel()); }
        const auto expected = cancel ? ExecutionStatus::Cancelled : ExecutionStatus::TimedOut;
        const auto terminal = wait(silent_run, RunWaitTarget::Completion);
        check(terminal.status == expected && silent.device->effects == 1
            && has_unknown_effect(terminal.diagnostics, applied.identity)
            && has_unknown_effect(record(silent_run, node(terminal, silent.source)).data().diagnostics, applied.identity),
            "Local timeout or cancellation after dispatch must identify the unknown effect of the unacknowledged send.");
        Device::exit_async(applied);
        check(wait(silent_run).status == expected && silent.device->count() == 1,
            "Unknown external effects must remain single-delivery after physical cleanup.");
    }

    Fixture safe(Backend::DeduplicatedSend, 0, 1, false);
    for (std::size_t iteration = 0; iteration != 2; ++iteration) {
        run = safe.start();
        done = wait(run);
        const auto& probe = node(done, safe.source);
        attempts(probe, {ExecutionStatus::Failed, ExecutionStatus::Succeeded});
        const auto first = safe.device->await(2 * iteration + 1), retry = safe.device->await(2 * iteration + 2);
        check(done.status == ExecutionStatus::Succeeded && operation_key(first.identity) == operation_key(retry.identity)
            && has_unknown_effect(probe.attempts.front().diagnostics, first.identity)
            && safe.device->effects == iteration + 1 && safe.device->count() == 2 * (iteration + 1),
            "A deduplicating endpoint should apply an unacknowledged instruction once per logical call.");
    }
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const std::string_view group = argc == 2 ? argv[1] : "";
        if (group == "timeout_retry") { timeout_retry(); }
        else if (group == "logical_timeout_and_cancel") { logical_timeout_and_cancel(); }
        else if (group == "recovery_and_judgement") { recovery_and_judgement(); }
        else if (group == "external_deduplication") { external_deduplication(); }
        else { throw std::runtime_error("Unknown attempt test group."); }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
