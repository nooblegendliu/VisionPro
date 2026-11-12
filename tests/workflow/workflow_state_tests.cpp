#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace std::chrono_literals;

int checks = 0;

void check(bool condition, std::string_view message)
{
    ++checks;
    if (!condition) { throw std::runtime_error(std::string(message)); }
}

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) {
        throw std::runtime_error(std::string(error_code_name(failure->code)) + ": " + failure->message);
    }
    return std::get<T>(std::move(result));
}

template<UuidIdentity T> T uuid(char suffix = '1')
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    text.back() = suffix;
    return take(IdFactory::parse<T>(text));
}

template<SchemaIdentity T> T key(std::string_view text)
{ return take(SchemaIdFactory::parse<T>(text)); }

struct Sample {
    NodeStateKey state;
    std::int64_t before;
    std::int64_t after;
};

struct PendingCall {
    InvocationCompletion completion;
    ToolInvocationIdentity identity;
    std::shared_ptr<ToolStateAccess> state;
    Sample sample;
};

ToolCompletion success(const ToolInvocationIdentity& identity, std::int64_t value)
{
    return {identity, ToolCompletionStatus::Succeeded, ToolJudgement::OK,
        {{key<OutputId>("total"), DataValue::integer(value)}}, {}};
}

struct Counts {
    unsigned created{0};
    unsigned initialized{0};
    unsigned started{0};
    std::optional<Sample> latest;
};

class Driver {
public:
    void created(NodeId node)
    {
        const std::lock_guard lock(mutex_);
        ++counts_[node].created;
    }

    void initialized(NodeId node)
    {
        const std::lock_guard lock(mutex_);
        ++counts_[node].initialized;
    }

    void started(NodeId node, const Sample& sample)
    {
        const std::lock_guard lock(mutex_);
        auto& stats = counts_.at(node);
        ++stats.started;
        stats.latest = sample;
    }

    Counts counts(NodeId node)
    {
        const std::lock_guard lock(mutex_);
        return counts_.at(node);
    }

    bool publish(const PendingCall& call)
    {
        const std::lock_guard lock(mutex_);
        if (draining_) { return false; }
        pending_ = call;
        changed_.notify_all();
        return true;
    }

    PendingCall await_call(WorkflowRunId run)
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 5s, [&] { return pending_ && pending_->identity.invocation.workflow_run == run; })) {
            throw std::runtime_error("The stateful asynchronous invocation did not start.");
        }
        return *pending_;
    }

    void request_cancel()
    {
        const std::lock_guard lock(mutex_);
        ++cancellations_;
        changed_.notify_all();
    }

    unsigned await_cancellation()
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 5s, [&] { return cancellations_ > 0; })) {
            throw std::runtime_error("The stateful asynchronous invocation did not receive cancellation.");
        }
        return cancellations_;
    }

    static void close(const PendingCall& call)
    {
        static_cast<void>(call.completion.complete({call.identity, ToolCompletionStatus::Cancelled,
            ToolJudgement::NotEvaluated, {}, {}}));
        static_cast<void>(call.completion.mark_exited());
    }

    void drain()
    {
        std::optional<PendingCall> pending;
        {
            const std::lock_guard lock(mutex_);
            draining_ = true;
            pending.swap(pending_);
        }
        if (pending) { close(*pending); }
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<NodeId, Counts> counts_;
    std::optional<PendingCall> pending_;
    unsigned cancellations_{0};
    bool draining_{false};
};

class CounterControl final : public InvocationControl {
public:
    explicit CounterControl(const std::shared_ptr<Driver>& driver) : driver_(driver) {}
    void request_cancel() override
    {
        if (const auto driver = driver_.lock()) { driver->request_cancel(); }
    }
private:
    std::weak_ptr<Driver> driver_;
};

class Counter final : public ToolInstance {
public:
    Counter(const ToolExecutionConfiguration& configuration, const std::shared_ptr<Driver>& driver)
        : ToolInstance(configuration), driver_(driver)
    {
        driver->created(configuration.data().node);
    }
private:
    Result<DataValue> initialize_state(const InvocationContext& context, const DataValue& initial) override
    {
        const auto driver = driver_.lock();
        if (!driver || context.data().state || !initial.as_int64()) {
            return Diagnostic{ErrorCode::InvalidState, Severity::Error, {},
                "Counter Init requires a fixed integer initial value and no state access."};
        }
        driver->initialized(configuration().data().node);
        return initial;
    }

    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context,
        InvocationCompletion completion) override
    {
        const auto driver = driver_.lock();
        const auto& call = context->data();
        if (!driver || !call.state) {
            return Diagnostic{ErrorCode::InvalidState, Severity::Error, {}, "Counter invocation requires typed state."};
        }
        const auto current = take(call.state->read());
        const auto before = *current.as_int64();
        const auto after = before + *call.inputs.front().value.as_int64();
        take(call.state->write(DataValue::integer(after)));
        const Sample sample{call.state->key(), before, after};
        driver->started(call.identity.invocation.node, sample);
        if (configuration().data().tool.execution.mode == ToolExecutionMode::Synchronous) {
            return InvocationStart{success(call.identity, after)};
        }
        auto control = std::make_shared<CounterControl>(driver);
        if (auto registered = completion.register_control(control); const auto* failure = std::get_if<Diagnostic>(&registered)) {
            return *failure;
        }
        const PendingCall pending{completion, call.identity, call.state, sample};
        if (!driver->publish(pending)) { Driver::close(pending); }
        return InvocationStart{AsynchronousInvocation{std::move(control)}};
    }

    std::weak_ptr<Driver> driver_;
};

class CounterFactory final : public ToolInstanceFactory {
public:
    explicit CounterFactory(std::shared_ptr<Driver> driver) : driver_(std::move(driver)) {}
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    {
        return std::shared_ptr<ToolInstance>{std::make_shared<Counter>(configuration, driver_)};
    }
private:
    std::shared_ptr<Driver> driver_;
};

ToolDescriptor counter_descriptor(ToolExecutionMode execution, StateMutationTiming mutation,
    StateCompatibilityPolicy compatibility)
{
    const auto per_run = key<EnumMemberId>("per-run");
    const auto session = key<EnumMemberId>("session");
    const auto mode = take(DataType::enumeration(key<SchemaId>("s35.counter.mode"), 1,
        {{per_run, "Per run"}, {session, "Session"}}));
    const auto integer = DataType::integer();
    ParameterDescriptor initial{key<ParameterId>("initial"), "Initial value", integer,
        false, ConfigValue::integer(0)};
    initial.binding.time = BindingTime::SnapshotBuild;
    ParameterDescriptor scope{key<ParameterId>("mode"), "Scope", mode,
        false, take(ConfigValue::enumeration(mode, session))};
    scope.binding.time = BindingTime::SnapshotBuild;
    InputDescriptor delta{key<InputId>("delta"), "Delta", integer};
    delta.binding.allowed_sources = {BindingSourceKind::PublicInput, BindingSourceKind::Constant};
    ToolDescriptor tool{key<ToolTypeId>("s35.counter"), "Counter", 1,
        {key<SchemaId>("s35.counter.schema"), 1}, {initial, scope}, {delta},
        {{key<OutputId>("total"), "Total", integer}}};
    tool.state = NodeStateDefinition{integer, ConfigValue::integer(0), StateScope::SolutionSession,
        StateResetPolicy::DomainStartAndExplicit, compatibility,
        mutation, StateConcurrencyPolicy::Serialized};
    tool.state_parameters = {initial.id, StatePolicyParameter{scope.id,
        {{per_run, StateScope::WorkflowRun, StateResetPolicy::DomainStart},
         {session, StateScope::SolutionSession, StateResetPolicy::DomainStartAndExplicit}}}};
    tool.execution.mode = execution;
    tool.execution.effects = ToolEffects::State;
    if (execution == ToolExecutionMode::Asynchronous) { tool.execution.cancellation = ToolCancellation::Cooperative; }
    return tool;
}

Node counter_node(const ToolDescriptor& tool, char suffix, std::int64_t initial, std::string_view mode)
{
    const auto flow = uuid<WorkflowId>();
    const auto delta = tool.inputs.front().id;
    return {uuid<NodeId>(suffix), "Counter " + std::string(1, suffix), ToolConfiguration{tool.id, tool.config_version},
        {{tool.parameters[0].id, {ConfigValue::integer(initial)}},
         {tool.parameters[1].id, {take(ConfigValue::enumeration(tool.parameters[1].type, key<EnumMemberId>(mode)))}},
         {delta, {ScopedInputReference{flow, {}, delta, {}}}}}};
}

Solution counters(const ToolDescriptor& tool, std::vector<Node> nodes)
{
    std::vector<NodeId> entries;
    for (const auto& node : nodes) { entries.push_back(node.id); }
    Workflow flow{uuid<WorkflowId>(), "Counters", {key<SchemaId>("s35.counters"), 1, tool.inputs},
        std::move(nodes), {}, {}, std::move(entries)};
    return {uuid<SolutionId>(), "State service", 1, 1, {std::move(flow)}};
}

struct Fixture {
    std::shared_ptr<Driver> driver{std::make_shared<Driver>()};
    const ToolDescriptor descriptor;
    const ToolRegistry registry;
    const std::unique_ptr<WorkflowEngine> engine;
    IdSequence<ConfigSnapshotId> snapshots;

    Fixture(ToolExecutionMode mode = ToolExecutionMode::Synchronous,
        StateMutationTiming mutation = StateMutationTiming::OnResultAccepted,
        StateCompatibilityPolicy compatibility = StateCompatibilityPolicy::PreserveCompatible)
        : descriptor(counter_descriptor(mode, mutation, compatibility)),
          registry(take(ToolRegistry::create(std::array{ToolRegistration{descriptor,
              {descriptor.schema.id, 1, 1, {}}, std::make_shared<CounterFactory>(driver)}}))),
          engine(take(WorkflowEngine::create(registry))) {}

    ~Fixture() { driver->drain(); }

    ConfigSnapshot capture(const Solution& solution)
    { return take(ConfigSnapshot::capture(solution, registry.descriptors(), {engine->session(), take(snapshots.next())})); }

    RunSubmission submit(const ConfigSnapshot& snapshot, std::int64_t delta,
        std::optional<NodeTarget> node = {}, std::optional<DebugStatePolicy> policy = {})
    {
        RunTarget target = node ? RunTarget{*node} : RunTarget{WorkflowTarget{uuid<WorkflowId>()}};
        const auto request = take(RunRequest::create(snapshot, std::move(target), {{key<InputId>("delta"), DataValue::integer(delta)}},
            {node ? TriggerKind::Debug : TriggerKind::Manual, key<SchemaId>("s35.test")}, {}, policy));
        return engine->submit(request);
    }
};

RunHandle accepted(RunSubmission submission)
{
    if (const auto* rejected = std::get_if<RunRejected>(&submission)) {
        throw std::runtime_error("Run rejected: " + rejected->diagnostic.message);
    }
    check(std::holds_alternative<RunHandle>(submission), "Idle workflow accepts the stateful request.");
    return std::get<RunHandle>(std::move(submission));
}

RunProgress wait_for(const RunHandle& run, ExecutionStatus status = ExecutionStatus::Succeeded,
    RunWaitTarget target = RunWaitTarget::Quiescence)
{
    auto waited = take(run.wait(target, std::chrono::steady_clock::now() + 5s));
    if (waited.progress.status != status && !waited.progress.diagnostics.empty()) {
        throw std::runtime_error("Unexpected run status: " + waited.progress.diagnostics.front().message);
    }
    check(waited.outcome == RunWaitOutcome::Satisfied && waited.progress.status == status, "Run reached the requested terminal state.");
    if (target == RunWaitTarget::Quiescence) { check(waited.progress.quiescent, "All invocation work has exited."); }
    return std::move(waited.progress);
}

void expect_total(const RunHandle& run, const RunProgress& progress, NodeId node, std::int64_t total)
{
    const auto entry = std::ranges::find(progress.nodes, node, &RunNodeProgress::node);
    check(entry != progress.nodes.end() && entry->invocation, "Counter progress reports its invocation.");
    const auto result = take(run.lookup(*entry->invocation));
    check(result && result->data().identity.run == run.identity(), "Counter lookup returns this run's result.");
    const auto* output = result->output(key<OutputId>("total"));
    const auto* value = output ? std::get_if<DataValue>(&output->content) : nullptr;
    check(value && *value == DataValue::integer(total), "Counter published the expected total.");
}

void check_counts(Fixture& fixture, NodeId node, unsigned calls, unsigned initializations)
{
    const auto counts = fixture.driver->counts(node);
    check(counts.created == calls && counts.started == calls && counts.initialized == initializations,
        "The factory rebuilds the instance per invocation and Init follows state-domain lifetime.");
}

void lifetimes()
{
    Fixture fixture;
    const auto per_run = counter_node(fixture.descriptor, '1', -5, "per-run");
    const auto accumulating = counter_node(fixture.descriptor, '2', 17, "session");
    const auto neighbor = counter_node(fixture.descriptor, '3', 31, "session");
    auto solution = counters(fixture.descriptor, {per_run, accumulating, neighbor});
    const auto flow = solution.workflows.front().id;
    const NodeTarget target{flow, accumulating.id};
    const auto original = fixture.capture(solution);
    const auto run = [&](const ConfigSnapshot& snapshot, std::int64_t delta,
                         std::int64_t per_run_total, std::int64_t session_total, std::int64_t neighbor_total) {
        const auto handle = accepted(fixture.submit(snapshot, delta));
        const auto progress = wait_for(handle);
        expect_total(handle, progress, per_run.id, per_run_total);
        expect_total(handle, progress, accumulating.id, session_total);
        expect_total(handle, progress, neighbor.id, neighbor_total);
        return handle;
    };

    const auto first = run(original, 3, -2, 20, 34);
    const auto first_per_run_key = fixture.driver->counts(per_run.id).latest->state;
    const auto session_key = fixture.driver->counts(accumulating.id).latest->state;
    const auto* run_domain = std::get_if<WorkflowRunStateDomain>(&first_per_run_key.domain);
    check(run_domain && run_domain->session == fixture.engine->session() && run_domain->run == first.identity().run,
        "The per-run state has the accepted run's exact domain.");
    const auto* session_domain = std::get_if<SessionStateDomain>(&session_key.domain);
    check(session_domain && session_domain->session == fixture.engine->session() && session_key.node == accumulating.id,
        "Accumulating state belongs to its node in the engine session.");
    const auto second = run(original, 4, -1, 24, 38);
    const auto second_per_run_key = fixture.driver->counts(per_run.id).latest->state;
    check(std::get<WorkflowRunStateDomain>(second_per_run_key.domain).run == second.identity().run,
        "The next run initializes in its own domain.");
    check(fixture.driver->counts(accumulating.id).latest->state == session_key,
        "Instance recreation preserves the accumulating state key.");

    solution.workflows.front().nodes[1].name = "Renamed counter";
    solution.layouts = {{flow, {{accumulating.id, 12, 18}}}};
    ++solution.revision;
    const auto renamed = fixture.capture(solution);
    run(renamed, 1, -4, 25, 39);
    check(fixture.driver->counts(accumulating.id).latest->state == session_key,
        "Presentation edits preserve the state generation.");
    check(std::holds_alternative<Diagnostic>(fixture.engine->reset_node_state({flow, per_run.id})),
        "The per-run counter has no retained state to reset explicitly.");
    take(fixture.engine->reset_node_state(target));
    run(renamed, 2, -3, 19, 41);
    check(fixture.driver->counts(accumulating.id).latest->state == session_key,
        "Explicit reset restores the configured initial value in the same generation.");
    check_counts(fixture, per_run.id, 4, 4);
    check_counts(fixture, accumulating.id, 4, 1);
    check_counts(fixture, neighbor.id, 4, 1);

    solution.workflows.front().nodes[1].bindings[0].sources[0] = ConfigValue::integer(50);
    ++solution.revision;
    const auto updated = fixture.capture(solution);
    run(updated, 4, -1, 54, 45);
    const auto new_session_key = fixture.driver->counts(accumulating.id).latest->state;
    check(new_session_key.generation != session_key.generation && new_session_key.domain == session_key.domain,
        "An incompatible initial value starts a new session-state generation.");
    check_counts(fixture, accumulating.id, 5, 2);
    expect_total(first, take(first.inspect()), accumulating.id, 20);

    solution.workflows.front().nodes[1].bindings.back().sources = {ConfigValue::integer(8)};
    ++solution.revision;
    run(fixture.capture(solution), 2, -3, 62, 47);
    check(fixture.driver->counts(accumulating.id).latest->state == new_session_key,
        "PreserveCompatible retains the generation after a binding-only edit.");
    check_counts(fixture, accumulating.id, 6, 2);
}

void generation_on_configuration_edit()
{
    Fixture fixture(ToolExecutionMode::Synchronous, StateMutationTiming::OnResultAccepted,
        StateCompatibilityPolicy::AlwaysNewGeneration);
    const auto node = counter_node(fixture.descriptor, '2', 10, "session");
    auto solution = counters(fixture.descriptor, {node});
    const auto run = [&](const ConfigSnapshot& snapshot, std::int64_t delta, std::int64_t expected) {
        const auto handle = accepted(fixture.submit(snapshot, delta));
        expect_total(handle, wait_for(handle), node.id, expected);
        return fixture.driver->counts(node.id).latest->state;
    };
    const auto original = fixture.capture(solution);
    const auto generation = run(original, 3, 13);
    check(run(original, 2, 15) == generation, "An unchanged configuration keeps its generation.");
    auto& flow = solution.workflows.front();
    flow.nodes.front().name = "Renamed strict counter";
    solution.layouts = {{flow.id, {{node.id, 12, 18}}}};
    ++solution.revision;
    check(run(fixture.capture(solution), 1, 16) == generation, "Renaming and layout edits keep a strict generation.");
    flow.nodes.front().bindings.back().sources = {ConfigValue::integer(8)};
    ++solution.revision;
    check(run(fixture.capture(solution), 1, 18) != generation, "Changing the binding starts a new strict generation.");
    check_counts(fixture, node.id, 4, 2);
}

void debug_isolation()
{
    Fixture fixture;
    const auto per_run = counter_node(fixture.descriptor, '1', -5, "per-run");
    const auto accumulating = counter_node(fixture.descriptor, '2', 17, "session");
    auto solution = counters(fixture.descriptor, {per_run, accumulating});
    const NodeTarget target{solution.workflows.front().id, accumulating.id};
    const auto original = fixture.capture(solution);
    const auto first = accepted(fixture.submit(original, 3));
    expect_total(first, wait_for(first), target.node, 20);
    const auto active_key = fixture.driver->counts(target.node).latest->state;

    solution.workflows.front().nodes[1].bindings[0].sources[0] = ConfigValue::integer(80);
    ++solution.revision;
    const auto alternate = fixture.capture(solution);
    const auto isolated = accepted(fixture.submit(alternate, 5, target));
    check(isolated.request().debug_state() == DebugStatePolicy::Isolated, "Independent requests default to isolated state.");
    const auto isolated_progress = wait_for(isolated);
    check(isolated_progress.nodes.size() == 1 && isolated_progress.control_links.empty(),
        "The isolated run executes one tool with no workflow control edges.");
    expect_total(isolated, isolated_progress, target.node, 85);
    const auto isolated_key = fixture.driver->counts(target.node).latest->state;
    check(isolated_key != active_key && isolated.identity().run != first.identity().run,
        "The isolated invocation has separate state and result identities.");
    const auto isolated_again = accepted(fixture.submit(alternate, 1, target));
    expect_total(isolated_again, wait_for(isolated_again), target.node, 81);
    check(fixture.driver->counts(target.node).latest->state != isolated_key, "A new isolated request starts from a new state.");
    check_counts(fixture, per_run.id, 1, 1);

    const auto resumed = accepted(fixture.submit(original, 2));
    expect_total(resumed, wait_for(resumed), target.node, 22);
    check(fixture.driver->counts(target.node).latest->state == active_key, "Debugging preserved the active generation.");
    const auto active_debug = accepted(fixture.submit(original, 7, target, DebugStatePolicy::Active));
    const auto active_progress = wait_for(active_debug);
    check(active_progress.nodes.size() == 1, "Explicit active-state debugging still invokes only the target.");
    expect_total(active_debug, active_progress, target.node, 29);
    check(fixture.driver->counts(target.node).latest->state == active_key, "Explicit active debug uses the same active state.");
    const auto final = accepted(fixture.submit(original, 1));
    expect_total(final, wait_for(final), target.node, 30);
    check_counts(fixture, per_run.id, 3, 3);
    check_counts(fixture, target.node, 6, 3);
}

void report_success(const PendingCall& pending)
{ take(pending.completion.complete(success(pending.identity, pending.sample.after))); }

void cancel_while_active(const RunHandle& handle, Fixture& fixture)
{
    check(take(handle.cancel()) == RunCancelDisposition::Requested, "The client requested cancellation once.");
    check(!wait_for(handle, ExecutionStatus::Cancelled, RunWaitTarget::Completion).quiescent,
        "Logical cancellation retains the active state access through backend cleanup.");
    check(fixture.driver->await_cancellation() == 1, "The tool received one cooperative cancellation.");
}

void state_acceptance()
{
    Fixture fixture(ToolExecutionMode::Asynchronous);
    const auto node = counter_node(fixture.descriptor, '2', 10, "session");
    auto solution = counters(fixture.descriptor, {node});
    const NodeTarget target{solution.workflows.front().id, node.id};
    const auto original = fixture.capture(solution);

    const auto first = accepted(fixture.submit(original, 2));
    const auto pending = fixture.driver->await_call(first.identity().run);
    check(pending.sample.before == 10 && pending.sample.after == 12, "The first asynchronous call stages a new state value.");
    check(take(pending.state->read()) == DataValue::integer(12), "The invocation can read its staged state.");
    check(std::holds_alternative<Diagnostic>(pending.state->write(DataValue::boolean(true))),
        "State writes reject values outside the declared schema.");

    solution.workflows.front().nodes.front().bindings[0].sources[0] = ConfigValue::integer(30);
    ++solution.revision;
    const auto updated = fixture.capture(solution);
    const auto busy = fixture.submit(updated, 10, target, DebugStatePolicy::Active);
    const auto* occupied = std::get_if<RunBusy>(&busy);
    check(occupied && occupied->active == first.identity(), "Explicit active-state debugging shares the workflow slot.");
    check(std::holds_alternative<Diagnostic>(fixture.engine->reset_node_state(target)), "An active state domain refuses reset.");

    report_success(pending);
    const auto completed = wait_for(first, ExecutionStatus::Succeeded, RunWaitTarget::Completion);
    expect_total(first, completed, target.node, 12);
    check(!completed.quiescent && pending.state->key() == pending.sample.state
        && take(pending.state->read()) == DataValue::integer(12),
        "The old generation remains readable after logical completion.");
    check(std::holds_alternative<Diagnostic>(pending.state->write(DataValue::integer(900))),
        "Logical completion seals the invocation's state writes.");
    check(std::holds_alternative<Diagnostic>(fixture.engine->reset_node_state(target)),
        "Reset still waits for physical exit after logical success.");
    take(pending.completion.mark_exited());
    wait_for(first);
    check(std::holds_alternative<Diagnostic>(pending.state->read())
        && std::holds_alternative<Diagnostic>(pending.state->write(DataValue::integer(901))),
        "An escaped state handle cannot read or write after physical exit.");

    const auto failed = accepted(fixture.submit(original, 5));
    const auto invalid = fixture.driver->await_call(failed.identity().run);
    check(invalid.sample.before == 12 && invalid.sample.state == pending.sample.state,
        "A successful accepted result published its staged state to the session.");
    auto bad_completion = success(invalid.identity, 17);
    bad_completion.outputs.front().content = DataValue::boolean(true);
    check(std::holds_alternative<Diagnostic>(invalid.completion.complete(bad_completion)),
        "Invalid outputs cannot complete the stateful invocation successfully.");
    take(invalid.completion.complete({invalid.identity, ToolCompletionStatus::Failed, ToolJudgement::NotEvaluated, {},
        {{ErrorCode::InvalidValue, Severity::Error, {}, "The asynchronous fixture failed."}}}));
    take(invalid.completion.mark_exited());
    wait_for(failed, ExecutionStatus::Failed);

    const auto cancelled = accepted(fixture.submit(original, 6));
    const auto late = fixture.driver->await_call(cancelled.identity().run);
    check(late.sample.before == 12 && late.sample.after == 18, "Failure discarded the preceding staged value.");
    cancel_while_active(cancelled, fixture);
    report_success(late);
    take(late.completion.mark_exited());
    wait_for(cancelled, ExecutionStatus::Cancelled);
    const auto resumed = accepted(fixture.submit(original, 1));
    const auto next = fixture.driver->await_call(resumed.identity().run);
    check(next.sample.before == 12 && next.sample.after == 13 && next.sample.state == pending.sample.state,
        "Cancellation discarded the staged update despite a late success.");
    report_success(next);
    take(next.completion.mark_exited());
    wait_for(resumed);

    const auto new_run = accepted(fixture.submit(updated, 2));
    const auto current = fixture.driver->await_call(new_run.identity().run);
    check(current.sample.before == 30 && current.sample.after == 32
        && current.sample.state.generation != pending.sample.state.generation,
        "The edited initial value applies to a new generation after cleanup.");
    report_success(current);
    take(current.completion.mark_exited());
    wait_for(new_run);
    check_counts(fixture, target.node, 5, 2);
}

void immediate_mutation()
{
    Fixture fixture(ToolExecutionMode::Asynchronous, StateMutationTiming::DuringComputation);
    const auto node = counter_node(fixture.descriptor, '2', 4, "session");
    const auto snapshot = fixture.capture(counters(fixture.descriptor, {node}));
    const auto cancelled = accepted(fixture.submit(snapshot, 9));
    const auto pending = fixture.driver->await_call(cancelled.identity().run);
    check(pending.sample.after == 13, "The tool wrote the value during computation.");
    cancel_while_active(cancelled, fixture);
    Driver::close(pending);
    wait_for(cancelled, ExecutionStatus::Cancelled);

    const auto resumed = accepted(fixture.submit(snapshot, 1));
    const auto next = fixture.driver->await_call(resumed.identity().run);
    check(next.sample.before == 13 && next.sample.after == 14, "Cancellation preserves completed DuringComputation writes.");
    report_success(next);
    take(next.completion.mark_exited());
    wait_for(resumed);
    check_counts(fixture, node.id, 2, 1);
}
} // namespace

int main(int argc, char** argv)
{
    const std::string_view test = argc > 1 ? argv[1] : "";
    try {
        if (test == "lifetimes") { lifetimes(); generation_on_configuration_edit(); }
        else if (test == "debug") { debug_isolation(); }
        else if (test == "acceptance") { state_acceptance(); immediate_mutation(); }
        else { throw std::runtime_error("Unknown state-service test case."); }
        std::cout << test << ": " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << test << ": " << error.what() << '\n';
        return 1;
    }
}
