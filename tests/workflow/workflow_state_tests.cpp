#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/workflow_engine.hpp>
#include <visionpro/workflow/execution_plan.hpp>
#include <visionpro/workflow/workflow_session.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <future>
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
        const auto found = counts_.find(node);
        return found == counts_.end() ? Counts{} : found->second;
    }

    bool publish(const PendingCall& call)
    {
        const std::lock_guard lock(mutex_);
        if (draining_) { return false; }
        pending_.insert_or_assign(call.identity.invocation.workflow_run, call);
        changed_.notify_all();
        return true;
    }

    PendingCall await_call(WorkflowRunId run)
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 5s, [&] { return pending_.contains(run); })) {
            throw std::runtime_error("The stateful asynchronous invocation did not start.");
        }
        return pending_.at(run);
    }

    PendingCall await_node(NodeId node, std::optional<WorkflowRunId> after = {})
    {
        std::unique_lock lock(mutex_);
        const auto next = [&] {
            return std::ranges::find_if(pending_, [&](const auto& item) {
                return item.second.identity.invocation.node == node && (!after || item.first > *after);
            });
        };
        if (!changed_.wait_for(lock, 5s, [&] { return next() != pending_.end(); })) {
            throw std::runtime_error("Expected solution child did not enter its asynchronous node.");
        }
        return next()->second;
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
        std::map<WorkflowRunId, PendingCall> pending;
        {
            const std::lock_guard lock(mutex_);
            draining_ = true;
            pending.swap(pending_);
        }
        for (const auto& [id, call] : pending) { close(call); }
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<NodeId, Counts> counts_;
    std::map<WorkflowRunId, PendingCall> pending_;
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

ToolRegistry counter_registry(const ToolDescriptor& descriptor, const std::shared_ptr<Driver>& driver)
{
    return take(ToolRegistry::create(std::array{ToolRegistration{descriptor,
        {descriptor.schema.id, 1, 1, {}}, std::make_shared<CounterFactory>(driver)}}));
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
          registry(counter_registry(descriptor, driver)),
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

void loop_state_and_plan_capability()
{
    Fixture fixture;
    const auto counter = counter_node(fixture.descriptor, '1', 0, "per-run");
    auto solution = counters(fixture.descriptor, {counter});
    auto& flow = solution.workflows.front();
    const auto request = [&](const ConfigSnapshot& snapshot) {
        return take(RunRequest::create(snapshot, WorkflowTarget{flow.id}, {{key<InputId>("delta"), DataValue::integer(1)}},
            {TriggerKind::Manual, key<SchemaId>("s61.state-test")}));
    };
    // This aggregate is the pre-loop graph consumer's complete capability declaration.
    ExecutionCapabilities graph{false, true, true, {}};
    const auto ordinary = take(ExecutionPlan::create(request(fixture.capture(solution)), fixture.registry, graph));
    check(ordinary.dependencies().scopes.size() == 1 && ordinary.tool(counter.id),
        "A DAG-only consumer can continue treating reachable nodes as ordinary tool configurations.");

    const auto loop = uuid<NodeId>('2');
    const auto default_integer = [&](std::string_view name, std::int64_t value) {
        InputDescriptor port{key<InputId>(name), std::string(name), DataType::integer(), false, ConfigValue::integer(value)};
        return port;
    };
    const auto delta = fixture.descriptor.inputs.front().id;
    const auto totals = key<OutputId>("totals");
    GroupConfiguration body{{key<SchemaId>("s61.state-loop"), 1,
        {default_integer("start", 0), default_integer("end", 2), default_integer("index", 0), fixture.descriptor.inputs.front()},
        {{totals, "Totals", DataType::list(DataType::integer())}}, {}}, {counter.id},
        {{totals, {flow.id, counter.id, key<OutputId>("total"), {}}}}, {},
        LoopConfiguration{1, false, 0, {{totals, LoopExportMode::Collect}}}};
    flow.nodes.front().bindings.back().sources = {ScopedInputReference{flow.id, loop, delta, {}}};
    flow.nodes.push_back({loop, "Count twice", body, {{delta, {ScopedInputReference{flow.id, {}, delta, {}}}}}});
    flow.containment = {{loop, counter.id}};
    flow.entries = {loop};
    const auto snapshot = fixture.capture(solution);
    const auto run_request = request(snapshot);

    const auto refused = ExecutionPlan::create(run_request, fixture.registry, graph);
    const auto* diagnostic = std::get_if<Diagnostic>(&refused);
    check(diagnostic && diagnostic->code == ErrorCode::MissingCapability && diagnostic->location.node == loop,
        "A DAG-only consumer must be refused a structured node before it dereferences a missing prepared tool.");
    graph.structured_loops = true;
    const auto plan = take(ExecutionPlan::create(run_request, fixture.registry, graph));
    check(plan.capabilities().structured_loops && !plan.tool(loop) && plan.tool(counter.id),
        "A loop-aware plan retains its structural region and prepares its body tool.");

    const auto expected = take(DataValue::list(DataType::integer(), {DataValue::integer(1), DataValue::integer(2)}));
    for (unsigned round = 1; round <= 2; ++round) {
        const auto handle = accepted(fixture.engine->submit(run_request));
        const auto progress = wait_for(handle);
        const auto entry = std::ranges::find(progress.nodes, loop, &RunNodeProgress::node);
        check(entry != progress.nodes.end() && entry->invocation, "The loop published its own logical invocation.");
        const auto result = take(handle.lookup(*entry->invocation));
        const auto* output = result ? result->output(totals) : nullptr;
        const auto* values = output ? std::get_if<DataValue>(&output->content) : nullptr;
        check(values && *values == expected, "Each run must collect [1,2] from the same per-run counter across its two iterations.");
        check_counts(fixture, counter.id, round * 2, round);
    }
}

void group_state_domains()
{
    Fixture fixture;
    auto solution = counters(fixture.descriptor, {});
    auto& flow = solution.workflows.front();
    const auto delta = fixture.descriptor.inputs.front();
    const auto totals = key<OutputId>("totals");
    const OutputDescriptor output{totals, "Totals", DataType::list(DataType::integer())};
    const auto number = [](std::string_view name, std::int64_t value) {
        return InputDescriptor{key<InputId>(name), std::string(name), DataType::integer(), false, ConfigValue::integer(value)};
    };
    const auto add_group = [&](char outer_id, char loop_id, char child_id) {
        const auto outer = uuid<NodeId>(outer_id);
        const auto loop = uuid<NodeId>(loop_id);
        auto counter = counter_node(fixture.descriptor, child_id, 0, "session");
        counter.bindings.back().sources = {ScopedInputReference{flow.id, loop, delta.id, {}}};
        GroupConfiguration body{{key<SchemaId>("s62.counter-loop"), 1,
            {number("start", 0), number("end", 2), number("index", 0), delta}, {output}}, {counter.id},
            {{totals, {flow.id, counter.id, key<OutputId>("total"), {}}}}, {},
            LoopConfiguration{1, false, 0, {{totals, LoopExportMode::Collect}}}};
        GroupConfiguration group{{key<SchemaId>("s62.counter-group"), 1, {delta}, {output}}, {loop},
            {{totals, {flow.id, loop, totals, {}}}}, {}};
        flow.entries.push_back(outer);
        flow.containment.push_back({outer, loop});
        flow.containment.push_back({loop, counter.id});
        flow.nodes.push_back({outer, "Counter group", group, {{delta.id, {ScopedInputReference{flow.id, {}, delta.id, {}}}}}});
        flow.nodes.push_back({loop, "Count twice", body, {{delta.id, {ScopedInputReference{flow.id, outer, delta.id, {}}}}}});
        flow.nodes.push_back(std::move(counter));
        return NodeTarget{flow.id, outer};
    };
    const auto first = add_group('1', '2', '3');
    const auto other = add_group('4', '5', '6');
    const auto snapshot = fixture.capture(solution);
    const auto expect_pair = [&](const RunHandle& handle, const RunProgress& progress, NodeId group, std::int64_t begin) {
        const auto call = std::ranges::find(progress.groups, group,
            [](const RunGroupCall& trace) { return trace.group.node; });
        check(call != progress.groups.end(), "An executed group has an invocation in this run.");
        const auto result = take(handle.lookup(call->group));
        const auto* entry = result ? result->output(totals) : nullptr;
        const auto* value = entry ? std::get_if<DataValue>(&entry->content) : nullptr;
        check(value && *value == take(DataValue::list(DataType::integer(), {DataValue::integer(begin), DataValue::integer(begin + 1)})),
            "The group's session counter accumulates twice in the expected state domain.");
    };
    const auto first_handle = accepted(fixture.submit(snapshot, 1));
    const auto both = wait_for(first_handle);
    expect_pair(first_handle, both, first.node, 1);
    expect_pair(first_handle, both, other.node, 1);
    const auto active = accepted(fixture.submit(snapshot, 1, first, DebugStatePolicy::Active));
    expect_pair(active, wait_for(active), first.node, 3);
    const auto isolated = accepted(fixture.submit(snapshot, 1, first));
    const auto debug = wait_for(isolated);
    check(debug.groups.size() == 1 && debug.iterations.size() == 2, "Group debugging calls only the selected subtree.");
    expect_pair(isolated, debug, first.node, 1);
    const auto resumed = accepted(fixture.submit(snapshot, 1));
    const auto final = wait_for(resumed);
    expect_pair(resumed, final, first.node, 5);
    expect_pair(resumed, final, other.node, 3);
    check_counts(fixture, uuid<NodeId>('3'), 8, 2);
    check_counts(fixture, uuid<NodeId>('6'), 4, 1);
}

void enabled_workflow_requests()
{
    Fixture fixture;
    auto solution = counters(fixture.descriptor, {});
    Workflow other{uuid<WorkflowId>('2'), "Other", {key<SchemaId>("s62.other"), 1}};
    other.enabled = false;
    const auto first = solution.workflows.front().id;
    solution.workflows.push_back(other);
    const auto original = fixture.capture(solution);
    const RunTrigger trigger{TriggerKind::Manual, key<SchemaId>("s62.batch")};
    const auto delta = key<InputId>("delta");
    const std::array inputs{WorkflowRunInputs{first, {{delta, DataValue::integer(2)}}}};
    const auto started = take(RunRequest::enabled_workflows(original, inputs, trigger));
    check(started.size() == 1 && std::get<WorkflowTarget>(started.front().target()).workflow == first,
        "Enabled workflow preparation freezes the enabled target set.");
    check(std::holds_alternative<Diagnostic>(RunRequest::enabled_workflows(original, {}, trigger))
        && std::holds_alternative<Diagnostic>(RunRequest::enabled_workflows(original,
            std::array{inputs.front(), WorkflowRunInputs{other.id, {}}}, trigger)),
        "Missing participant inputs and inputs for disabled workflows fail preparation.");
    solution.workflows.back().enabled = true;
    const auto next = fixture.capture(solution);
    const auto prepared = take(RunRequest::enabled_workflows(next, inputs, trigger));
    check(prepared.size() == 2 && std::get<WorkflowTarget>(prepared.back().target()).workflow == other.id
        && prepared.front().snapshot().same_capture(next) && prepared.back().snapshot().same_capture(next)
        && prepared.front().input(delta)->value == DataValue::integer(2)
        && started.front().snapshot().same_capture(original) && !started.front().snapshot().solution().workflows.back().enabled,
        "All newly prepared requests use the same new snapshot while old targets and inputs stay frozen.");
    check(std::holds_alternative<Diagnostic>(RunRequest::enabled_workflows(next,
        std::array{inputs.front(), inputs.front()}, trigger)), "Duplicate participant inputs fail the entire batch.");
    for (auto& workflow : solution.workflows) { workflow.enabled = false; }
    check(take(RunRequest::enabled_workflows(fixture.capture(solution), {}, trigger)).empty(),
        "An empty enabled set prepares no requests.");
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

void session_publication_preserves_compatible_state()
{
    const auto driver = std::make_shared<Driver>();
    const auto descriptor = counter_descriptor(ToolExecutionMode::Synchronous, StateMutationTiming::OnResultAccepted,
        StateCompatibilityPolicy::AlwaysNewGeneration);
    const auto node = counter_node(descriptor, '2', 17, "session");
    auto draft = counters(descriptor, {node});
    auto& flow = draft.workflows.front();
    const auto session = take(WorkflowSession::create(counter_registry(descriptor, driver)));
    const auto base = take(session->activate(draft));
    RunArguments args{WorkflowTarget{flow.id}, {{key<InputId>("delta"), DataValue::integer(3)}},
        {TriggerKind::Manual, key<SchemaId>("s35.test")}};
    const auto original = take(RunRequest::create(base, args.target, args.inputs, args.trigger));
    const auto first = accepted(session->submit(original));
    expect_total(first, wait_for(first), node.id, 20);
    const auto original_state = driver->counts(node.id).latest->state;

    flow.nodes.front().name = "Renamed counter";
    draft.layouts = {{flow.id, {{node.id, 12, 18}}}};
    args.inputs.front().value = DataValue::integer(4);
    const auto second = accepted(session->publish_and_submit(base, draft, args, {}));
    expect_total(second, wait_for(second), node.id, 24);
    const auto second_counts = driver->counts(node.id);
    check(second_counts.latest->state == original_state && second_counts.initialized == 1,
        "Renaming and positioning preserve the accumulated state after publication.");
    const auto& renamed = second.request().snapshot();
    check(renamed.identity().session == base.identity().session && renamed.identity().snapshot > base.identity().snapshot
        && renamed.solution().revision == base.solution().revision
        && renamed.solution().workflows.front().nodes.front().name == "Renamed counter"
        && renamed.solution().layouts.front().nodes.front().x == 12
        && first.request().snapshot().solution().workflows.front().nodes.front().name == node.name,
        "The new run sees the unsaved edit while the original run's configuration is detached.");

    flow.nodes.front().bindings.back().sources = {ConfigValue::integer(8)};
    const auto third = accepted(session->publish_and_submit(renamed, draft, args, {}));
    expect_total(third, wait_for(third), node.id, 25);
    const auto third_counts = driver->counts(node.id);
    check(third_counts.latest->state.domain == original_state.domain
        && third_counts.latest->state.generation != original_state.generation && third_counts.initialized == 2,
        "An incompatible binding creates a new generation in the same session-state domain.");
    expect_total(first, take(first.inspect()), node.id, 20);
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

void solution_round_across_publication()
{
    const auto driver = std::make_shared<Driver>();
    const auto tool = counter_descriptor(ToolExecutionMode::Asynchronous, StateMutationTiming::OnResultAccepted,
        StateCompatibilityPolicy::PreserveCompatible);
    const auto a = counter_node(tool, 'a', 0, "per-run");
    auto b = counter_node(tool, 'b', 10, "session");
    const auto first = uuid<WorkflowId>();
    const auto second = uuid<WorkflowId>('2');
    const auto delta = key<InputId>("delta");
    const auto total = key<OutputId>("total");
    b.bindings.back().sources = {ScopedInputReference{second, {}, delta, {}}};
    auto draft = counters(tool, {a});
    auto& source = draft.workflows.front();
    source.interface.outputs = tool.outputs;
    source.outputs = {{total, OutputReference{first, a.id, total, {}}}};
    draft.workflows.push_back({second, "Receiver", {key<SchemaId>("s35.receiver"), 1, tool.inputs, tool.outputs},
        {b}, {}, {}, {b.id}, {{total, OutputReference{second, b.id, total, {}}}}});
    draft.format_version = solution_graph_manifest_version;
    draft.graph = SolutionGraph{{first}, {{first, second}}, {{first, total, second, delta}}};
    const auto session = take(WorkflowSession::create(counter_registry(tool, driver)));
    const auto base = take(session->activate(draft));
    const RunTrigger trigger{TriggerKind::Manual, key<SchemaId>("s35.round")};
    const std::array args{WorkflowRunInputs{first, {{delta, DataValue::integer(3)}}}};
    std::stop_source stop;
    std::future<Result<SolutionRunReport>> running;
    struct DrainOnExit {
        std::stop_source& stop;
        Driver& driver;
        ~DrainOnExit() { stop.request_stop(); driver.drain(); }
    } cleanup{stop, *driver}; // Unblock a held backend before an exceptional future join.
    const auto run_receiver = [&](const ConfigSnapshot& snapshot, std::int64_t value) {
        const auto request = take(RunRequest::create(snapshot, WorkflowTarget{second}, {{delta, DataValue::integer(value)}}, trigger));
        const auto handle = accepted(session->submit(request));
        const auto pending = driver->await_call(handle.identity().run);
        report_success(pending);
        take(pending.completion.mark_exited());
        expect_total(handle, wait_for(handle), b.id, pending.sample.after);
        return pending;
    };
    running = std::async(std::launch::async, [&, base] { return session->run_solution(base, args, trigger, stop.get_token()); });
    const auto cold_source = driver->await_node(a.id);
    check(driver->counts(b.id).created == 0 && driver->counts(b.id).initialized == 0,
        "Pinning a cold downstream state must not construct or initialize its backend.");
    draft.workflows.back().nodes.front().name = "Renamed receiver";
    const auto compatible = take(session->publish(base, draft, {}));
    const auto previous = run_receiver(compatible, 5);
    report_success(cold_source);
    take(cold_source.completion.mark_exited());
    const auto cold_target = driver->await_node(b.id, previous.identity.invocation.workflow_run);
    check(cold_target.sample.state == previous.sample.state && cold_target.sample.before == 15 && cold_target.sample.after == 18,
        "Two compatible snapshots share a cold session node even when its first invocation uses the newer snapshot.");
    report_success(cold_target);
    take(cold_target.completion.mark_exited());
    const auto cold_round = take(running.get());
    check(cold_round.status == ExecutionStatus::Succeeded && driver->counts(b.id).initialized == 1,
        "Both compatible invocations initialize their shared session cell exactly once.");
    for (const auto& child : cold_round.runs) { wait_for(child); }

    running = std::async(std::launch::async, [&, compatible] { return session->run_solution(compatible, args, trigger, stop.get_token()); });
    const auto held = driver->await_node(a.id, cold_source.identity.invocation.workflow_run);
    draft.workflows.back().nodes.front().bindings.front().sources = {ConfigValue::integer(100)};
    const auto current = take(session->publish(compatible, draft, {}));
    const auto fresh = run_receiver(current, 7);
    check(fresh.sample.before == 100 && fresh.sample.after == 107 && fresh.sample.state != previous.sample.state,
        "A changed downstream configuration starts its new generation while the old upstream is held.");
    report_success(held);
    take(held.completion.mark_exited());
    const auto delayed = driver->await_node(b.id, fresh.identity.invocation.workflow_run);
    check(delayed.sample.state == previous.sample.state && delayed.sample.before == 18 && delayed.sample.after == 21,
        "A late child of the pinned solution round resumes the old downstream generation.");
    report_success(delayed);
    take(delayed.completion.mark_exited());
    const auto result = take(running.get());
    check(result.status == ExecutionStatus::Succeeded && result.runs.size() == 2 && result.deliveries.size() == 1
        && result.runs.back().identity().snapshot == compatible.identity(),
        "The admitted solution retains its original capture through downstream delivery.");
    for (const auto& child : result.runs) { wait_for(child); }

    draft.workflows.back().admission.max_in_flight = 2;
    const auto expanded = take(session->publish(current, draft, {}));
    running = std::async(std::launch::async, [&, expanded] { return session->run_solution(expanded, args, trigger, stop.get_token()); });
    const auto second_source = driver->await_node(a.id, held.identity.invocation.workflow_run);
    draft.workflows.back().admission.max_in_flight = 1;
    const auto narrow = take(session->publish(expanded, draft, {}));
    const auto occupied = accepted(session->submit(take(RunRequest::create(narrow, WorkflowTarget{second},
        {{delta, DataValue::integer(0)}}, trigger))));
    const auto occupying = driver->await_call(occupied.identity().run);
    report_success(second_source);
    take(second_source.completion.mark_exited());
    check(running.wait_for(5s) == std::future_status::ready,
        "A pinned child must not occupy a second slot forbidden by the live admission policy.");
    const auto refused = take(running.get());
    check(refused.status == ExecutionStatus::Failed && refused.runs.size() == 1 && refused.deliveries.empty()
        && refused.skipped == std::vector<WorkflowId>{second} && !refused.diagnostics.empty()
        && refused.diagnostics.front().location.workflow == second,
        "A late child reports live capacity exhaustion without dispatch or duplicate delivery.");
    report_success(occupying);
    take(occupying.completion.mark_exited());
    wait_for(occupied);
    for (const auto& child : refused.runs) { wait_for(child); }

    running = std::async(std::launch::async, [&, narrow] { return session->run_solution(narrow, args, trigger, stop.get_token()); });
    const auto cancelled = driver->await_node(a.id, second_source.identity.invocation.workflow_run);
    stop.request_stop();
    driver->await_cancellation();
    Driver::close(cancelled);
    const auto aborted = take(running.get());
    check(aborted.status == ExecutionStatus::Cancelled && aborted.runs.size() == 1 && aborted.deliveries.empty()
        && aborted.skipped == std::vector<WorkflowId>{second} && driver->counts(b.id).started == 5,
        "A cancelled second round cannot reuse the previous public result to start its receiver.");
    const auto resumed = run_receiver(narrow, 1);
    check(resumed.sample.state == fresh.sample.state && resumed.sample.before == 107 && resumed.sample.after == 108,
        "The delayed child and the cancelled round leave the current downstream generation intact.");
}

void overlapping_state_generations()
{
    Fixture fixture(ToolExecutionMode::Asynchronous);
    auto node = counter_node(fixture.descriptor, '2', 0, "session");
    auto solution = counters(fixture.descriptor, {node});
    auto& workflow = solution.workflows.front();
    workflow.admission.max_in_flight = 3;
    const auto original = fixture.capture(solution);
    const auto first = accepted(fixture.submit(original, 1));
    const auto holding = fixture.driver->await_call(first.identity().run);
    const auto second = accepted(fixture.submit(original, 1));
    report_success(holding);
    wait_for(first, ExecutionStatus::Succeeded, RunWaitTarget::Completion);

    // This independent generation can finish while the first still owns its
    // old cell and the second run is entitled to inherit that same cell.
    workflow.nodes.front().bindings.front().sources.front() = ConfigValue::integer(100);
    ++solution.revision;
    const auto revised = fixture.capture(solution);
    const auto third = accepted(fixture.submit(revised, 1));
    const auto separate = fixture.driver->await_call(third.identity().run);
    check(separate.sample.before == 100 && separate.sample.state.generation != holding.sample.state.generation,
        "A revised state definition can run against its own generation before older cleanup.");
    report_success(separate);
    take(separate.completion.mark_exited());
    expect_total(third, wait_for(third), node.id, 101);
    check(fixture.driver->counts(node.id).started == 2 && take(second.inspect()).call_order.empty(),
        "The compatible second invocation cannot start after logical completion but before actual exit.");

    take(holding.completion.mark_exited());
    wait_for(first);
    const auto successor = fixture.driver->await_call(second.identity().run);
    check(successor.sample.state == holding.sample.state && successor.sample.before == 1,
        "An admitted run retains its original shared generation across an intervening publication.");
    report_success(successor);
    take(successor.completion.mark_exited());
    expect_total(second, wait_for(second), node.id, 2);

    const auto fourth = accepted(fixture.submit(revised, 1));
    const auto current = fixture.driver->await_call(fourth.identity().run);
    check(current.sample.state == separate.sample.state && current.sample.before == 101,
        "Completion of an older admitted snapshot cannot replace the new generation.");
    report_success(current);
    take(current.completion.mark_exited());
    expect_total(fourth, wait_for(fourth), node.id, 102);
    check_counts(fixture, node.id, 4, 2);
}

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
        else if (test == "loop-state") { loop_state_and_plan_capability(); }
        else if (test == "group-state") { group_state_domains(); }
        else if (test == "enabled-set") { enabled_workflow_requests(); }
        else if (test == "debug") { debug_isolation(); }
        else if (test == "acceptance") { state_acceptance(); immediate_mutation(); overlapping_state_generations(); }
        else if (test == "publication") { session_publication_preserves_compatible_state(); solution_round_across_publication(); }
        else { throw std::runtime_error("Unknown state-service test case."); }
        std::cout << test << ": " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << test << ": " << error.what() << '\n';
        return 1;
    }
}
