#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
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

template<UuidIdentity T> T uuid(char digit = '1')
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    text.back() = digit;
    return take(IdFactory::parse<T>(text));
}

template<SchemaIdentity T> T key(std::string_view text)
{ return take(SchemaIdFactory::parse<T>(text)); }

struct Calls {
    std::vector<NodeId> created;
    std::vector<NodeId> started;
};

class Adder final : public ToolInstance {
public:
    Adder(const ToolExecutionConfiguration& configuration, std::shared_ptr<Calls> calls)
        : ToolInstance(configuration), calls_(std::move(calls)) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        const auto& call = context->data();
        calls_->started.push_back(call.identity.invocation.node);
        const auto amount = *call.inputs.at(0).value.as_int64();
        const auto adjustment = *call.inputs.at(1).value.as_int64();
        const auto offset = *call.parameters.at(0).value.as_int64();
        return InvocationStart{ToolCompletion{call.identity, ToolCompletionStatus::Succeeded, ToolJudgement::OK,
            {{key<OutputId>("sum"), DataValue::integer(amount + adjustment + offset)}}, {}}};
    }
    std::shared_ptr<Calls> calls_;
};

class AdderFactory final : public ToolInstanceFactory {
public:
    explicit AdderFactory(std::shared_ptr<Calls> calls) : calls_(std::move(calls)) {}
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    {
        calls_->created.push_back(configuration.data().node);
        return std::shared_ptr<ToolInstance>{std::make_shared<Adder>(configuration, calls_)};
    }
private:
    std::shared_ptr<Calls> calls_;
};

ToolRegistry registry_for(const std::shared_ptr<Calls>& calls, ToolEffects effects)
{
    const auto integer = DataType::integer();
    InputDescriptor amount{key<InputId>("amount"), "Amount", integer};
    amount.binding.allowed_sources.push_back(BindingSourceKind::RunOutput);
    const InputDescriptor adjustment{key<InputId>("adjustment"), "Adjustment", integer, false, ConfigValue::integer(7)};
    ParameterDescriptor offset{key<ParameterId>("offset"), "Offset", integer, false, ConfigValue::integer(0)};
    offset.subscribable = true;
    offset.binding.allowed_sources.push_back(BindingSourceKind::RunOutput);
    ToolDescriptor tool{key<ToolTypeId>("s35.adder"), "Adder", 1, {key<SchemaId>("s35.adder.schema"), 1},
        {offset}, {amount, adjustment}, {{key<OutputId>("sum"), "Sum", integer}}};
    tool.execution.effects = effects;
    const ToolRegistration registration{tool, {tool.schema.id, 1, 1, {}}, std::make_shared<AdderFactory>(calls)};
    return take(ToolRegistry::create(std::span{&registration, 1}));
}

OutputReference sum_from(char node)
{ return {uuid<WorkflowId>(), uuid<NodeId>(node), key<OutputId>("sum"), {}}; }

Solution solution_for(const ToolDescriptor& tool)
{
    const auto first = uuid<NodeId>('1');
    const auto target = uuid<NodeId>('2');
    const auto last = uuid<NodeId>('3');
    const auto amount = key<InputId>("amount");
    const auto configuration = ToolConfiguration{tool.id, tool.config_version};
    const Workflow flow{uuid<WorkflowId>(), "Additions", {key<SchemaId>("s35.additions"), 1},
        {{first, "Source", configuration, {{amount, {ConfigValue::integer(4)}}}},
         {target, "Target", configuration, {{amount, {sum_from('1')}},
             {key<InputId>("adjustment"), {ConfigValue::integer(90)}},
             {key<ParameterId>("offset"), {ConfigValue::integer(3)}}}},
         {last, "Successor", configuration, {{amount, {sum_from('2')}}}}},
        {{uuid<ControlLinkId>('1'), first, {}, target}, {uuid<ControlLinkId>('2'), target, {}, last}},
        {}, {first}};
    return {uuid<SolutionId>(), "Independent execution", 1, 1, {flow}};
}

struct Fixture {
    explicit Fixture(ToolEffects effects = ToolEffects::None) : registry(registry_for(calls, effects)) {}

    std::shared_ptr<Calls> calls{std::make_shared<Calls>()};
    ToolRegistry registry;
    std::unique_ptr<WorkflowEngine> engine{take(WorkflowEngine::create(registry))};
    Solution solution{solution_for(registry.descriptors().front())};
    IdSequence<ConfigSnapshotId> snapshots;

    ConfigSnapshot capture()
    { return take(ConfigSnapshot::capture(solution, registry.descriptors(), {engine->session(), take(snapshots.next())})); }
};

NodeTarget target(char node = '2')
{ return {uuid<WorkflowId>(), uuid<NodeId>(node)}; }

RunTrigger debug()
{ return {TriggerKind::Debug, key<SchemaId>("s35.test")}; }

std::vector<RunInput> amount(std::int64_t value)
{ return {{key<InputId>("amount"), DataValue::integer(value)}}; }

RunHandle accepted(WorkflowEngine& engine, const RunRequest& request)
{
    auto submitted = engine.submit(request);
    if (const auto* refused = std::get_if<RunRejected>(&submitted)) { throw std::runtime_error(refused->diagnostic.message); }
    check(std::holds_alternative<RunHandle>(submitted), "A quiescent workflow must accept the new request.");
    return std::get<RunHandle>(std::move(submitted));
}

RunProgress finished(const RunHandle& run)
{
    auto waited = take(run.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
    check(waited.outcome == RunWaitOutcome::Satisfied && waited.progress.quiescent
        && waited.progress.status == ExecutionStatus::Succeeded, "The accepted request must succeed and release its slot.");
    return std::move(waited.progress);
}

void sum_is(const RunHandle& run, const RunNodeProgress& node, std::int64_t expected)
{
    check(node.status == ExecutionStatus::Succeeded && node.invocation && node.invocation->node == node.node,
        "The completed target reports its actual invocation.");
    const auto result = take(run.lookup(*node.invocation));
    check(result && result->data().identity.run == run.identity() && result->data().identity.invocation == *node.invocation,
        "The result belongs to the exact accepted run and invocation.");
    const auto* output = result->output(key<OutputId>("sum"));
    const auto* value = output ? std::get_if<DataValue>(&output->content) : nullptr;
    check(value && *value == DataValue::integer(expected), "The invocation must use the prepared values.");
}

void independent_input(const RunHandle& run, const RunNodeProgress& node, std::string_view name, InputOrigin origin)
{
    const BindingTarget input{key<InputId>(name)};
    const auto binding = std::ranges::find(node.bindings, input, &RunBindingTrace::target);
    check(binding != node.bindings.end() && binding->sources.size() == 1, "The input reports its single source.");
    const auto* trace = std::get_if<IndependentInputTrace>(&binding->sources.front().origin);
    check(trace && trace->input == std::get<InputId>(input) && trace->run == run.identity() && trace->origin == origin,
        "The independent trace names this input and run without a fabricated upstream result.");
}

void independent_calls()
{
    Fixture fixture;
    const auto snapshot = fixture.capture();
    auto inputs = amount(41);
    const auto request = take(RunRequest::create(snapshot, target(), inputs, debug()));
    inputs.front().value = DataValue::integer(120);
    const auto first = accepted(*fixture.engine, request);
    const auto progress = finished(first);
    check(progress.nodes.size() == 1 && progress.nodes.front().node == target().node && progress.control_links.empty(),
        "The isolated run contains only the target, with no external control links.");
    check(fixture.calls->created == std::vector{target().node} && fixture.calls->started == fixture.calls->created,
        "No upstream or downstream instance is created or started.");
    const auto& node = progress.nodes.front();
    sum_is(first, node, 51);
    independent_input(first, node, "amount", InputOrigin::Provided);
    independent_input(first, node, "adjustment", InputOrigin::Default);

    auto supplied = amount(-1);
    supplied.push_back({key<InputId>("adjustment"), DataValue::integer(2)});
    const auto second = accepted(*fixture.engine, take(RunRequest::create(snapshot, target(), supplied, debug())));
    const auto second_progress = finished(second);
    check(second.identity() != first.identity(), "Each independent request has a separate run identity.");
    sum_is(second, second_progress.nodes.front(), 4);
    independent_input(second, second_progress.nodes.front(), "adjustment", InputOrigin::Provided);
    const auto previous = second.lookup(*node.invocation);
    check(std::get<Diagnostic>(previous).code == ErrorCode::ResultIdentityMismatch,
        "A previous invocation cannot address the independent run.");
    sum_is(first, node, 51);

    const auto missing = RunRequest::create(snapshot, target(), {}, debug());
    const auto* failure = std::get_if<Diagnostic>(&missing);
    check(failure && failure->code == ErrorCode::MissingRequiredValue && failure->location.node == target().node
        && failure->location.input == key<InputId>("amount"), "Missing input cannot reuse the previous call's value.");
    check(fixture.calls->created.size() == 2 && fixture.calls->started.size() == 2,
        "A rejected request cannot create or invoke a tool.");

    const auto ordinary = accepted(*fixture.engine, take(RunRequest::create(snapshot, WorkflowTarget{target().workflow},
        {}, {TriggerKind::Manual, key<SchemaId>("s35.test")})));
    const auto flow = finished(ordinary);
    check(flow.nodes.size() == 3 && flow.control_links.size() == 2, "The workflow still uses its original control graph.");
    sum_is(ordinary, flow.nodes.back(), 111);

    auto& target_node = fixture.solution.workflows.front().nodes[1];
    target_node.bindings.back().sources = {sum_from('1')};
    const auto subscribed = take(RunRequest::create(fixture.capture(), target(), amount(41), debug()));
    const auto rejected = fixture.engine->submit(subscribed);
    const auto* refusal = std::get_if<RunRejected>(&rejected);
    check(refusal && refusal->diagnostic.code == ErrorCode::InputUnavailable
        && refusal->diagnostic.location.node == target().node && refusal->diagnostic.location.parameter == key<ParameterId>("offset"),
        "An upstream parameter subscription reports the target parameter before any tool can run.");
    check(fixture.calls->created.size() == 5 && fixture.calls->started.size() == 5,
        "Independent parameter resolution neither executes the producer nor takes a historical result.");
}

void group_boundary()
{
    Fixture fixture;
    auto& flow = fixture.solution.workflows.front();
    const auto group = uuid<NodeId>('4');
    for (const auto& node : flow.nodes) { flow.containment.push_back({group, node.id}); }
    flow.nodes.push_back({group, "Owner", GroupConfiguration{{key<SchemaId>("s35.group"), 1}, flow.entries}});
    flow.entries = {group};
    const auto snapshot = fixture.capture();

    const auto submitted = fixture.engine->submit(take(RunRequest::create(snapshot, target('4'), {}, debug())));
    const auto* refusal = std::get_if<RunRejected>(&submitted);
    check(refusal && refusal->diagnostic.code == ErrorCode::MissingCapability
        && refusal->diagnostic.location.node == group && fixture.calls->created.empty(),
        "An independent Group target reports its missing execution capability before any factory call.");

    const auto child = accepted(*fixture.engine, take(RunRequest::create(snapshot, target(), amount(25), debug())));
    const auto progress = finished(child);
    check(progress.nodes.size() == 1 && progress.control_links.empty()
        && fixture.calls->created == std::vector{target().node} && fixture.calls->started == fixture.calls->created,
        "A child tool may run independently of its enabled Group's control graph.");
    sum_is(child, progress.nodes.front(), 35);

    flow.nodes.back().enabled = false;
    const auto disabled = RunRequest::create(fixture.capture(), target(), amount(25), debug());
    const auto* failure = std::get_if<Diagnostic>(&disabled);
    check(failure && failure->code == ErrorCode::InvalidRunRequest && failure->location.node == target().node,
        "Independent calls still require an enabled containing Group.");
}

void state_definition_required()
{
    Fixture fixture(ToolEffects::State);
    const auto request = take(RunRequest::create(fixture.capture(), target(), amount(1), debug()));
    const auto submitted = fixture.engine->submit(request);
    const auto* refusal = std::get_if<RunRejected>(&submitted);
    check(refusal && refusal->diagnostic.code == ErrorCode::MissingCapability
        && refusal->diagnostic.location.node == target().node && fixture.calls->created.empty(),
        "Declared state effects require a typed state definition before execution admission.");
}
} // namespace

int main()
{
    try {
        independent_calls();
        group_boundary();
        state_definition_required();
    } catch (const std::exception& failure) {
        std::cerr << "FAIL after " << checks << " checks: " << failure.what() << '\n';
        return 1;
    }
    std::cout << checks << " checks, 0 failures\n";
    return 0;
}
