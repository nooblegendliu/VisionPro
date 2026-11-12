#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/execution_plan.hpp>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;

int checks = 0;
int failures = 0;

void check(bool condition, std::string_view message)
{
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}

template<class T> Diagnostic rejects(const Result<T>& result, ErrorCode expected)
{
    const auto* failure = std::get_if<Diagnostic>(&result);
    check(failure && failure->code == expected, error_code_name(expected));
    if (!failure) { throw std::runtime_error("Expected a diagnostic."); }
    return *failure;
}

template<UuidIdentity T> T uuid(char digit = '1')
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    text.back() = digit;
    return take(IdFactory::parse<T>(text));
}
template<SchemaIdentity T> T key(std::string_view text)
{ return take(SchemaIdFactory::parse<T>(text)); }
template<RuntimeIdentity T> T number(std::uint64_t value)
{ return take(RuntimeIdFactory::from_value<T>(value)); }

struct Calls { int created{0}; int started{0}; };

class Increment final : public ToolInstance {
public:
    Increment(ToolExecutionConfiguration config, std::shared_ptr<Calls> calls)
        : ToolInstance(std::move(config)), calls_(std::move(calls)) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        ++calls_->started;
        const auto input = *context->data().inputs.front().value.as_int64();
        const auto offset = *context->data().parameters.front().value.as_int64();
        return InvocationStart{ToolCompletion{context->data().identity, ToolCompletionStatus::Succeeded,
            ToolJudgement::NotEvaluated, {{key<OutputId>("value"), DataValue::integer(input + offset)}}, {}}};
    }
    std::shared_ptr<Calls> calls_;
};

class IncrementFactory final : public ToolInstanceFactory {
public:
    explicit IncrementFactory(std::shared_ptr<Calls> calls) : calls_(std::move(calls)) {}
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& config) override
    {
        ++calls_->created;
        return std::shared_ptr<ToolInstance>{std::make_shared<Increment>(config, calls_)};
    }
private:
    std::shared_ptr<Calls> calls_;
};

InputDescriptor input()
{
    InputDescriptor value{key<InputId>("value"), "Value", DataType::integer()};
    value.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::RunOutput, BindingSourceKind::PublicInput};
    return value;
}

ToolDescriptor descriptor()
{
    ParameterDescriptor offset{key<ParameterId>("offset"), "Offset", DataType::integer(),
        false, ConfigValue::integer(2)};
    offset.binding.time = BindingTime::SnapshotBuild;
    return {key<ToolTypeId>("test.increment"), "Increment", 1, {key<SchemaId>("test.increment.schema"), 1},
        {offset}, {input()}, {{key<OutputId>("value"), "Value", DataType::integer()}}};
}

OutputReference output(char producer)
{ return {uuid<WorkflowId>(), uuid<NodeId>(producer), key<OutputId>("value"), {}}; }

Node node(const ToolDescriptor& tool, char id, BindingSource source)
{ return {uuid<NodeId>(id), "Increment", ToolConfiguration{tool.id, 1}, {{input().id, {std::move(source)}}}}; }

struct Fixture {
    ToolDescriptor tool{descriptor()};
    std::shared_ptr<Calls> calls{std::make_shared<Calls>()};
    Solution solution{uuid<SolutionId>(), "Planning", 1, 1, {
        {uuid<WorkflowId>(), "Pipeline", {key<SchemaId>("test.pipeline"), 1, {input()}, tool.outputs},
            {node(tool, '1', ScopedInputReference{uuid<WorkflowId>(), {}, input().id, {}}), node(tool, '2', output('1'))},
            {{uuid<ControlLinkId>(), uuid<NodeId>('1'), {}, uuid<NodeId>('2')}}, {}, {uuid<NodeId>('1')},
            {{key<OutputId>("value"), output('2')}}}
    }};

    Workflow& flow() { return solution.workflows.front(); }
    ToolRegistry registry() const
    {
        const ToolRegistration registration{tool,
            {tool.schema.id, 1, 1, {}}, std::make_shared<IncrementFactory>(calls)};
        return take(ToolRegistry::create(std::span{&registration, 1}));
    }
    ConfigSnapshot snapshot(const ToolRegistry& registry) const
    {
        return take(ConfigSnapshot::capture(solution, registry.descriptors(),
            {uuid<SolutionSessionId>(), number<ConfigSnapshotId>(solution.revision)}));
    }
    RunRequest request(const ToolRegistry& registry, RunTarget target = WorkflowTarget{uuid<WorkflowId>()},
        std::int64_t value = 5, TriggerKind trigger = TriggerKind::Manual) const
    {
        return take(RunRequest::create(snapshot(registry), target, {{input().id, DataValue::integer(value)}},
            {trigger, key<SchemaId>("test.request")}));
    }
};

ResolutionContext context(const ExecutionPlan& plan, char consumer)
{
    const auto scope = number<ExecutionScopeId>(0);
    const RunIdentity run{plan.request().snapshot().identity(), uuid<WorkflowId>(), number<WorkflowRunId>(1)};
    return {run, {run.run, uuid<NodeId>(consumer), scope, number<InvocationId>(consumer - '0')},
        {{scope, {}, {}, {}}}, {}, {}, {}};
}

const PreparedInvocation& invocation(const InvocationPreparation& prepared)
{
    if (const auto* failure = std::get_if<InvocationFailure>(&prepared)) {
        throw std::runtime_error(failure->diagnostic.message);
    }
    return std::get<PreparedInvocation>(prepared);
}

void scoped_planning()
{
    Fixture fixture;
    const auto registry = fixture.registry();
    const auto unknown = key<ToolTypeId>("test.unavailable");
    fixture.flow().nodes.push_back({uuid<NodeId>('3'), "Disconnected", ToolConfiguration{unknown, 1}});
    auto other = fixture.flow();
    other.id = uuid<WorkflowId>('2');
    other.nodes = {{uuid<NodeId>('4'), "", ToolConfiguration{unknown, 1}}};
    other.entries = {other.nodes.front().id};
    other.control_links.clear();
    fixture.solution.workflows.push_back(other);
    rejects(validate_model_shape(fixture.solution), ErrorCode::InvalidModel);
    const auto diagnostics = take(diagnose_solution(fixture.solution, registry.descriptors()));
    check(diagnostics.size() == 2 && diagnostics[0].code == ErrorCode::UnknownTool
        && diagnostics[0].location.node == uuid<NodeId>('3') && diagnostics[1].location.workflow == other.id,
        "editable validation reports local faults in both workflows");
    const auto plan = take(ExecutionPlan::create(fixture.request(registry), registry));
    const auto& range = plan.dependencies().scopes.front();
    check(plan.request().snapshot().solution().workflows.size() == 2
        && plan.request().snapshot().solution().workflows.front().nodes.size() == 3,
        "plan retains the full editable snapshot");
    check(range.reachable.size() == 2 && range.control_links.size() == 1 && range.data.size() == 1
        && range.data.front().producer == uuid<NodeId>('1') && plan.entries().size() == 1,
        "plan retains exactly the entry closure and the derived input dependency");
    rejects(plan.create_instance(uuid<NodeId>('3')), ErrorCode::IncompleteExecutionScope);

    const auto original = fixture.solution;
    fixture.flow().control_links.push_back({uuid<ControlLinkId>('2'), uuid<NodeId>('3'), {}, uuid<NodeId>('2')});
    const auto predecessor = rejects(ExecutionPlan::create(fixture.request(registry), registry), ErrorCode::IncompleteExecutionScope);
    check(predecessor.location.control_link == uuid<ControlLinkId>('2'), "range violation identifies the incoming control link");
    fixture.solution = original;
    fixture.flow().nodes[1].bindings.front().sources = {output('3')};
    const auto outside = rejects(ExecutionPlan::create(fixture.request(registry), registry), ErrorCode::IncompleteExecutionScope);
    check(outside.location.input == input().id && outside.location.node == uuid<NodeId>('2'), "out-of-range data source identifies its consumer");
    fixture.solution = original;
    fixture.flow().nodes[0].bindings.front().sources = {output('2')};
    rejects(ExecutionPlan::create(fixture.request(registry), registry), ErrorCode::ControlOrderConflict);
    fixture.solution = original;
    fixture.flow().nodes[1].configuration = ToolConfiguration{unknown, 1};
    rejects(ExecutionPlan::create(fixture.request(registry), registry), ErrorCode::UnknownTool);
    fixture.solution = original;
    fixture.flow().outputs.front().source = output('3');
    rejects(ExecutionPlan::create(fixture.request(registry), registry), ErrorCode::IncompleteExecutionScope);
    fixture.solution = original;
    fixture.flow().entries.clear();
    rejects(ExecutionPlan::create(fixture.request(registry), registry), ErrorCode::IncompleteExecutionScope);
    fixture.solution = original;
    const auto empty_registry = take(ToolRegistry::create({}));
    rejects(ExecutionPlan::create(fixture.request(registry), empty_registry), ErrorCode::UnknownTool);
    rejects(ExecutionPlan::create(fixture.request(registry, WorkflowTarget{fixture.flow().id}, 5, TriggerKind::External),
        registry), ErrorCode::MissingCapability);

    fixture.solution.workflows.back().nodes.front().id = uuid<NodeId>('1');
    rejects(diagnose_solution(fixture.solution, registry.descriptors()), ErrorCode::DuplicateIdentity);
    rejects(ConfigSnapshot::capture(fixture.solution, registry.descriptors(), plan.request().snapshot().identity()),
        ErrorCode::DuplicateIdentity);
    check(fixture.calls->created == 0 && fixture.calls->started == 0, "all preflight paths avoid factory and tool side effects");
}

void capability_checks()
{
    Fixture fixture;
    auto registry = fixture.registry();
    fixture.flow().nodes.push_back(node(fixture.tool, '3', output('1')));
    fixture.flow().nodes.push_back(node(fixture.tool, '4', output('3')));
    fixture.flow().control_links.push_back({uuid<ControlLinkId>('2'), uuid<NodeId>('1'), {}, uuid<NodeId>('3')});
    fixture.flow().control_links.push_back({uuid<ControlLinkId>('3'), uuid<NodeId>('3'), {}, uuid<NodeId>('4')});
    fixture.flow().nodes[1].bindings.front().sources = {output('4')};
    rejects(ExecutionPlan::create(fixture.request(registry), registry), ErrorCode::MissingCapability);
    ExecutionCapabilities graph;
    graph.control_graph = true;
    const auto plan = take(ExecutionPlan::create(fixture.request(registry), registry, graph));
    check(plan.dependencies().scopes.front().data.size() == 3 && plan.capabilities().control_graph,
        "sibling dependencies coexist with the original control graph");
    fixture.flow().nodes[2].bindings.front().sources = {output('2')};
    rejects(ExecutionPlan::create(fixture.request(registry), registry, graph), ErrorCode::DependencyCycle);

    Fixture asynchronous;
    asynchronous.tool.execution.mode = ToolExecutionMode::Asynchronous;
    registry = asynchronous.registry();
    const auto request = asynchronous.request(registry);
    rejects(ExecutionPlan::create(request, registry), ErrorCode::MissingCapability);
    ExecutionCapabilities async_support;
    async_support.asynchronous = true;
    const auto async_plan = take(ExecutionPlan::create(request, registry, async_support));
    check(async_plan.tool(uuid<NodeId>())->configuration().data().tool.execution.mode == ToolExecutionMode::Asynchronous,
        "supported asynchronous tools retain their declared mode");
    check(fixture.calls->created == 0 && asynchronous.calls->created == 0, "capability checks do not instantiate tools");
}

void planned_bindings()
{
    Fixture fixture;
    const auto registry = fixture.registry();
    const auto plan = take(ExecutionPlan::create(fixture.request(registry), registry));
    fixture.flow().nodes.front().bindings.clear();
    ResultStore results(plan.request().snapshot().identity().session);
    const auto attempt = number<AttemptId>(0);
    auto second_context = context(plan, '2');
    const auto scope = second_context.consumer.scope;
    second_context.sources = {{uuid<NodeId>(), scope, AwaitingSource{}}};
    const auto& second = *plan.tool(uuid<NodeId>('2'));
    check(std::holds_alternative<PendingInvocation>(second.prepare(second_context, attempt, results)),
        "planned consumer waits for the explicitly selected producer");

    const auto first_context = context(plan, '1');
    const auto first_preparation = plan.tool(uuid<NodeId>())->prepare(first_context, attempt, results);
    const auto& first = invocation(first_preparation);
    const auto& input_resolution = std::get<ResolvedBinding>(first.bindings.front().resolution);
    const auto* public_value = input_resolution.value.as_int64();
    check(public_value && *public_value == 5
        && std::get<PublicInputTrace>(input_resolution.sources.front().origin).origin == InputOrigin::Provided,
        "workflow resolves the request input from the plan after the editable configuration changes");
    auto replacement = first_context;
    replacement.scopes.front().inputs = {{input().id, DataValue::integer(99)}};
    check(std::holds_alternative<InvocationFailure>(plan.tool(uuid<NodeId>())->prepare(replacement, attempt, results)),
        "runtime context cannot replace the fixed request input");

    const auto instance = take(plan.create_instance(uuid<NodeId>()));
    const auto operation = take(InvocationOperation::start(instance, first.context));
    const auto progress = operation.poll();
    check(progress.physically_exited && progress.completion.has_value(), "synchronous tool returns a completed operation");
    const auto result = take(checked_node_result(first, *progress.completion));
    take(results.insert(plan.request().snapshot(), result));
    second_context.sources.front().state = first_context.consumer.invocation;
    const auto second_preparation = second.prepare(second_context, attempt, results);
    const auto* upstream_value = invocation(second_preparation).context.data().inputs.front().value.as_int64();
    check(upstream_value && *upstream_value == 7
        && fixture.calls->created == 1 && fixture.calls->started == 1,
        "same plan prepares downstream data from the explicit current invocation without starting the consumer");
}

void independent_plan()
{
    Fixture fixture;
    auto& flow = fixture.flow();
    const auto node_id = uuid<NodeId>('2');
    const auto group_id = uuid<NodeId>('3');
    flow.nodes.push_back({group_id, "Owner", GroupConfiguration{{key<SchemaId>("test.group"), 1, {input()}}, {node_id}}});
    flow.containment.push_back({group_id, node_id});
    const auto registry = fixture.registry();
    const NodeTarget target{flow.id, node_id};
    const auto plan = take(ExecutionPlan::create(fixture.request(registry, target, 41, TriggerKind::Debug), registry));
    check(plan.entries().size() == 1 && plan.entries().front() == node_id
        && plan.dependencies().scopes.front().group == group_id
        && plan.dependencies().scopes.front().control_links.empty() && plan.dependencies().scopes.front().data.empty(),
        "independent node plan retains its static ownership and excludes external control and data links");
    ResultStore results(plan.request().snapshot().identity().session);
    auto runtime = context(plan, '2');
    const auto attempt = number<AttemptId>(0);
    const auto prepared = plan.tool(node_id)->prepare(runtime, attempt, results);
    const auto& call = invocation(prepared);
    const auto& source = std::get<ResolvedBinding>(call.bindings.front().resolution).sources.front();
    const auto* provided_value = call.context.data().inputs.front().value.as_int64();
    check(provided_value && *provided_value == 41
        && std::get<IndependentInputTrace>(source.origin).origin == InputOrigin::Provided
        && fixture.calls->created == 0,
        "direct preparation uses the injected value and a distinct trace without upstream calls");
    runtime.sources = {{uuid<NodeId>(), runtime.consumer.scope, number<InvocationId>(1)}};
    check(std::holds_alternative<InvocationFailure>(plan.tool(node_id)->prepare(runtime, attempt, results)),
        "independent preparation rejects an upstream source selection");
    rejects(ExecutionPlan::create(fixture.request(registry, NodeTarget{flow.id, group_id}), registry),
        ErrorCode::MissingCapability);

    fixture.tool.parameters.front().binding.time = BindingTime::BeforeInvocation;
    fixture.tool.parameters.front().subscribable = true;
    fixture.tool.parameters.front().binding.allowed_sources.push_back(BindingSourceKind::RunOutput);
    flow.nodes[1].bindings.push_back({fixture.tool.parameters.front().id, {output('1')}});
    const auto subscribed = fixture.registry();
    const auto failure = rejects(ExecutionPlan::create(fixture.request(subscribed, target), subscribed), ErrorCode::InputUnavailable);
    check(failure.location.parameter == fixture.tool.parameters.front().id, "independent plan locates the unavailable parameter subscription");
    check(fixture.calls->created == 0 && fixture.calls->started == 0, "independent validation makes no tool calls");
}
} // namespace

int main(int argc, char** argv)
{
    const std::string_view group = argc == 2 ? argv[1] : "";
    try {
        if (group == "execution_scope") { scoped_planning(); capability_checks(); }
        else if (group == "planned_bindings") { planned_bindings(); }
        else if (group == "independent_plan") { independent_plan(); }
        else { throw std::runtime_error("Expected execution_scope, planned_bindings or independent_plan."); }
    } catch (const std::exception& failure) {
        check(false, failure.what());
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
