#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/tool_execution.hpp>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

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

template<UuidIdentity T> T uuid(char suffix = '1')
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    text.back() = suffix;
    return take(IdFactory::parse<T>(text));
}

template<SchemaIdentity T> T key(std::string_view text)
{ return take(SchemaIdFactory::parse<T>(text)); }

SnapshotIdentity identity(std::uint64_t revision)
{ return {uuid<SolutionSessionId>(), take(RuntimeIdFactory::from_value<ConfigSnapshotId>(revision))}; }

ToolDescriptor counter()
{
    const auto per_run = key<EnumMemberId>("per-run");
    const auto session = key<EnumMemberId>("session");
    const auto policy = take(DataType::enumeration(key<SchemaId>("counter.state-policy"), 1,
        {{per_run, "Initialize each run"}, {session, "Accumulate across runs"}}));
    ParameterDescriptor initial{key<ParameterId>("initial"), "Initial value", DataType::integer(),
        false, ConfigValue::integer(1)};
    initial.binding.time = BindingTime::SnapshotBuild;
    initial.constraints.integer_range = IntegerRange{-100, 100};
    ParameterDescriptor mode{key<ParameterId>("mode"), "State policy", policy,
        false, take(ConfigValue::enumeration(policy, session))};
    mode.binding.time = BindingTime::SnapshotBuild;
    ToolDescriptor tool{key<ToolTypeId>("test.counter"), "Counter", 1,
        {key<SchemaId>("test.counter.schema"), 1}, {initial, mode}};
    tool.state = NodeStateDefinition{DataType::integer(), ConfigValue::integer(0),
        StateScope::Invocation, StateResetPolicy::DomainStart, StateCompatibilityPolicy::PreserveCompatible,
        StateMutationTiming::OnResultAccepted, StateConcurrencyPolicy::Serialized};
    tool.state_parameters = {initial.id, StatePolicyParameter{mode.id,
        {{per_run, StateScope::WorkflowRun, StateResetPolicy::DomainStart},
         {session, StateScope::SolutionSession, StateResetPolicy::DomainStartAndExplicit}}}};
    return tool;
}

Solution two_counters(const ToolDescriptor& tool)
{
    const auto& initial = tool.parameters[0];
    const auto& mode = tool.parameters[1];
    Node per_run{uuid<NodeId>('1'), "Per run", ToolConfiguration{tool.id, 1},
        {{initial.id, {ConfigValue::integer(-5)}},
         {mode.id, {take(ConfigValue::enumeration(mode.type, key<EnumMemberId>("per-run")))}}}};
    Node accumulating{uuid<NodeId>('2'), "Accumulating", ToolConfiguration{tool.id, 1},
        {{initial.id, {ConfigValue::integer(17)}}}};
    Workflow flow{uuid<WorkflowId>(), "Counters", {key<SchemaId>("counters.public"), 1},
        {per_run, accumulating}, {}, {}, {per_run.id, accumulating.id}};
    return {uuid<SolutionId>(), "Counters", 1, 1, {flow}};
}

void check_state(const ToolExecutionConfiguration& configuration, std::int64_t initial,
    StateScope scope, StateResetPolicy reset, std::string_view message)
{
    const auto& state = configuration.effective_state();
    check(state && state->initial_value == ConfigValue::integer(initial) && state->scope == scope
        && state->reset == reset && state->mutation == StateMutationTiming::OnResultAccepted
        && state->compatibility == StateCompatibilityPolicy::PreserveCompatible
        && state->concurrency == StateConcurrencyPolicy::Serialized, message);
}

void projected_state()
{
    auto tools = std::vector{counter()};
    auto solution = two_counters(tools.front());
    const auto before = take(ConfigSnapshot::capture(solution, tools, identity(1)));
    const auto first = take(PreparedTool::create(before, uuid<WorkflowId>(), uuid<NodeId>('1')));
    const auto second = take(PreparedTool::create(before, uuid<WorkflowId>(), uuid<NodeId>('2')));
    check_state(first.configuration(), -5, StateScope::WorkflowRun, StateResetPolicy::DomainStart,
        "first counter selects its own initial value and per-run initialization");
    check_state(second.configuration(), 17, StateScope::SolutionSession, StateResetPolicy::DomainStartAndExplicit,
        "same tool type selects a different initial value and default session policy");
    check(first.configuration().data().tool.id == second.configuration().data().tool.id
        && first.configuration().data().tool.state->initial_value == ConfigValue::integer(0),
        "effective state does not modify the type's state definition");

    auto& changed = solution.workflows.front().nodes.front();
    changed.bindings.front().sources.front() = ConfigValue::integer(24);
    changed.bindings.pop_back();
    ++solution.revision;
    tools.front().state_parameters.policy->choices.front().reset = StateResetPolicy::DomainStartAndExplicit;
    const auto after = take(ConfigSnapshot::capture(solution, tools, identity(2)));
    const auto updated = take(PreparedTool::create(after, uuid<WorkflowId>(), uuid<NodeId>('1')));
    const auto old = take(PreparedTool::create(before, uuid<WorkflowId>(), uuid<NodeId>('1')));
    check_state(updated.configuration(), 24, StateScope::SolutionSession, StateResetPolicy::DomainStartAndExplicit,
        "new snapshot projects the edited initial value and policy");
    check_state(old.configuration(), -5, StateScope::WorkflowRun, StateResetPolicy::DomainStart,
        "old snapshot retains the configured selection and original state metadata");

    auto legacy_data = first.configuration().data();
    legacy_data.tool.state_parameters = {};
    const auto legacy = take(ToolExecutionConfiguration::create(legacy_data));
    check_state(legacy, 0, StateScope::Invocation, StateResetPolicy::DomainStart,
        "a legacy descriptor keeps its static state definition");

    changed.bindings.front().sources.front() = ConfigValue::integer(101);
    const auto invalid = take(ConfigSnapshot::capture(solution, tools, identity(3)));
    const auto prepare = PreparedTool::create(invalid, uuid<WorkflowId>(), uuid<NodeId>('1'));
    const auto* failure = std::get_if<Diagnostic>(&prepare);
    check(failure && failure->code == ErrorCode::ConstraintViolation && failure->location.node == changed.id
        && failure->location.parameter == tools.front().state_parameters.initial_value,
        "invalid initial value is rejected at the affected node and parameter");
}

void invalid_state_parameters()
{
    const auto tool = counter();
    const auto rejects = [&](const ToolDescriptor& descriptor, ParameterId parameter, std::string_view message) {
        const auto result = validate_descriptor(descriptor);
        const auto* failure = std::get_if<Diagnostic>(&result);
        check(failure && failure->code == ErrorCode::InvalidDescriptor
            && failure->location.tool == tool.id && failure->location.parameter == parameter, message);
    };
    const auto initial = tool.parameters[0].id;
    const auto policy = tool.parameters[1].id;
    auto invalid = tool;
    invalid.state.reset();
    rejects(invalid, initial, "projection without a tool state definition is rejected");
    invalid = tool;
    invalid.parameters[0].binding.time = BindingTime::BeforeInvocation;
    rejects(invalid, initial, "initial value must be fixed at snapshot build");
    invalid = tool;
    invalid.state_parameters.initial_value = policy;
    rejects(invalid, policy, "state initial parameter must have the exact state type");
    invalid = tool;
    invalid.state_parameters.policy->id = initial;
    rejects(invalid, initial, "state policy parameter must have a typed Enum");
    invalid = tool;
    invalid.state_parameters.policy->choices.erase(invalid.state_parameters.policy->choices.begin());
    rejects(invalid, policy, "every selectable member needs a scope and reset policy");
    invalid.parameters[1].constraints.allowed_values = {*invalid.parameters[1].default_value};
    check(std::holds_alternative<std::monostate>(validate_descriptor(invalid)),
        "an Enum member excluded by the parameter constraints needs no mapping");
    invalid = tool;
    invalid.state_parameters.policy->choices.back().choice = invalid.state_parameters.policy->choices.front().choice;
    rejects(invalid, policy, "each selectable member has exactly one mapping");
    invalid = tool;
    invalid.state->concurrency = StateConcurrencyPolicy::IsolatedPerInvocation;
    rejects(invalid, policy, "mapped persistent state cannot bypass the tool concurrency contract");
}

static_assert(std::is_same_v<decltype(std::declval<ToolExecutionConfiguration&>().effective_state()),
    const std::optional<NodeStateDefinition>&>);
} // namespace

int main()
{
    try {
        projected_state();
        invalid_state_parameters();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }
    std::cout << "state_projection: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
