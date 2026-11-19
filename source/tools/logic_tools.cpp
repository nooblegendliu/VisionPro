#include "logic_tools.hpp"

#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <limits>
#include <set>

namespace visionpro::tools {
namespace {
using namespace core;

template<SchemaIdentity Id> Id key(std::string_view spelling)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(spelling)); }

const DataValue& input(const InvocationContextData& call, std::string_view name)
{
    return std::ranges::find(call.inputs, name, [](const auto& value) { return value.id.text(); })->value;
}

const ConfigValue& parameter(const InvocationContextData& call, std::string_view name)
{
    const auto& fixed = call.configuration.data().parameters;
    return std::ranges::find(fixed, name, [](const auto& value) { return value.id.text(); })->value;
}

Diagnostic parameter_error(std::string_view parameter, ErrorCode code, std::string message)
{
    DiagnosticLocation where;
    where.parameter = key<ParameterId>(parameter);
    return {code, Severity::Error, where, std::move(message)};
}

using Execution = Result<std::monostate>(*)(const InvocationContextData&, ToolCompletion&);

class LogicTool final : public ToolInstance {
public:
    LogicTool(const ToolExecutionConfiguration& configuration, Execution execute) : ToolInstance(configuration), execute_(execute) {}

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        const auto& call = context->data();
        ToolCompletion result{call.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated, {}, {}};
        if (auto executed = execute_(call, result); auto* failure = std::get_if<Diagnostic>(&executed)) { return *failure; }
        return InvocationStart{std::move(result)};
    }
    Execution execute_;
};

class LogicFactory final : public ToolInstanceFactory {
public:
    explicit LogicFactory(Execution execute) : execute_(execute) {}
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<LogicTool>(configuration, execute_)}; }
private:
    Execution execute_;
};

constexpr auto lowest = std::numeric_limits<std::int64_t>::min();
constexpr auto highest = std::numeric_limits<std::int64_t>::max();

Diagnostic overflow()
{ return {ErrorCode::InvalidValue, Severity::Error, {}, "Integer calculation exceeds the Int64 range."}; }

Result<std::int64_t> add(std::int64_t left, std::int64_t right)
{
    if ((right > 0 && left > highest - right) || (right < 0 && left < lowest - right)) { return overflow(); }
    return left + right;
}

Result<std::int64_t> calculate(std::string_view operation, std::int64_t left, std::int64_t right)
{
    if (operation == "Add") { return add(left, right); }
    if (operation == "Subtract") {
        if ((right < 0 && left > highest + right) || (right > 0 && left < lowest + right)) { return overflow(); }
        return left - right;
    }
    if (operation == "Multiply") {
        if (left > 0 && (right > highest / left || right < lowest / left)) { return overflow(); }
        if (left < 0 && ((right > 0 && left < lowest / right) || (right < 0 && left < highest / right))) { return overflow(); }
        return left * right;
    }
    if (operation == "Divide") {
        if (right == 0) { return Diagnostic{ErrorCode::InvalidValue, Severity::Error, {}, "Integer division by zero."}; }
        if (left == lowest && right == -1) { return overflow(); }
        return left / right;
    }
    return parameter_error("operation", ErrorCode::InvalidValue, "Unknown integer operation.");
}

Result<std::monostate> arithmetic(const InvocationContextData& call, ToolCompletion& result)
{
    const auto value = calculate(parameter(call, "operation").as_enum()->text(),
        *input(call, "left").as_int64(), *input(call, "right").as_int64());
    if (const auto* failure = std::get_if<Diagnostic>(&value)) { return *failure; }
    result.outputs.push_back({key<OutputId>("value"), DataValue::integer(std::get<std::int64_t>(value))});
    return std::monostate{};
}

Result<std::monostate> accumulate(const InvocationContextData& call, ToolCompletion& result)
{
    if (!call.state) { return Diagnostic{ErrorCode::MissingCapability, Severity::Error, {}, "Accumulator requires node state."}; }
    const auto previous = call.state->read();
    if (const auto* failure = std::get_if<Diagnostic>(&previous)) { return *failure; }
    const auto* integer = std::get<DataValue>(previous).as_int64();
    if (!integer) { return Diagnostic{ErrorCode::InvalidState, Severity::Error, {}, "Accumulator state requires an Int64 value."}; }
    const auto sum = add(*integer, *input(call, "value").as_int64());
    if (const auto* failure = std::get_if<Diagnostic>(&sum)) { return *failure; }
    const auto value = DataValue::integer(std::get<std::int64_t>(sum));
    const auto written = call.state->write(value);
    if (const auto* failure = std::get_if<Diagnostic>(&written)) { return *failure; }
    result.outputs.push_back({key<OutputId>("value"), value});
    return std::monostate{};
}

Result<std::monostate> conjunction(const InvocationContextData& call, ToolCompletion& result)
{
    const auto& values = *input(call, "values").elements();
    if (values.empty()) {
        DiagnosticLocation where;
        where.input = key<InputId>("values");
        return Diagnostic{ErrorCode::MissingRequiredValue, Severity::Error, where, "Boolean AND requires at least one supplied judgement."};
    }
    const bool passed = std::ranges::all_of(values, [](const DataValue& value) { return *value.as_bool(); });
    result.outputs.push_back({key<OutputId>("passed"), DataValue::boolean(passed)});
    result.judgement = passed ? ToolJudgement::OK : ToolJudgement::NG;
    return std::monostate{};
}

Result<std::monostate> condition(const InvocationContextData& call, ToolCompletion& result)
{
    const bool selected = *input(call, "condition").as_bool();
    result.selected_control_outputs.push_back(key<ControlOutputId>(selected ? "true" : "false"));
    return std::monostate{};
}

Result<std::monostate> format_list(const InvocationContextData& call, ToolCompletion& result)
{
    const auto& list = *input(call, "list").elements();
    const auto& values = *input(call, "values").elements();
    const auto& indices = *parameter(call, "indices").elements();
    const auto& pattern = *parameter(call, "format").as_string();
    const auto& separator = *parameter(call, "separator").as_string();
    constexpr std::string_view placeholder{"{value}"};
    const auto at = pattern.find(placeholder);
    if (at == std::string::npos || pattern.find_first_of("{}") != at
        || pattern.find_first_of("{}", at + placeholder.size()) != std::string::npos) {
        return parameter_error("format", ErrorCode::ConstraintViolation, "The item format requires exactly one {value} placeholder and literal text.");
    }
    std::set<std::size_t> skip;
    for (const auto& permitted : *parameter(call, "skip_out_of_range").elements()) {
        const auto position = *permitted.as_int64();
        if (position < 0 || static_cast<std::uint64_t>(position) >= indices.size()) {
            return parameter_error("skip_out_of_range", ErrorCode::ConstraintViolation, "Each skippable position must name an entry in indices.");
        }
        skip.insert(static_cast<std::size_t>(position));
    }

    const auto source_count = list.size() + values.size();
    const auto count = indices.empty() ? source_count : indices.size();
    std::string text;
    std::vector<DataValue> used_indices;
    std::vector<DataValue> used_values;
    for (std::size_t position = 0; position < count; ++position) {
        const auto index = indices.empty() ? static_cast<std::int64_t>(position) : *indices[position].as_int64();
        if (index < 0 || static_cast<std::uint64_t>(index) >= source_count) {
            if (skip.contains(position)) { continue; }
            return parameter_error("indices", ErrorCode::IndexOutOfRange, "Selection " + std::to_string(position)
                + " refers to the out-of-range index " + std::to_string(index) + ".");
        }
        const auto offset = static_cast<std::size_t>(index);
        const auto& value = offset < list.size() ? list[offset] : values[offset - list.size()];
        if (!used_values.empty()) { text += separator; }
        text.append(pattern, 0, at);
        text += std::to_string(*value.as_int64());
        text.append(pattern, at + placeholder.size());
        used_indices.push_back(DataValue::integer(index));
        used_values.push_back(value);
    }
    result.outputs = {{key<OutputId>("text"), std::get<DataValue>(DataValue::string(text))},
        {key<OutputId>("used_indices"), std::get<DataValue>(DataValue::list(DataType::integer(), used_indices))},
        {key<OutputId>("used_values"), std::get<DataValue>(DataValue::list(DataType::integer(), used_values))}};
    return std::monostate{};
}

ToolDescriptor descriptor(std::string_view id, std::string name)
{
    return {key<ToolTypeId>(id), std::move(name), 1, {key<SchemaId>(id), 1, {}}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}};
}

InputDescriptor port(std::string_view id, const DataType& type, bool allow_index = false)
{
    InputDescriptor port{key<InputId>(id), std::string(id), type, true, {}, {}, {}, {}};
    port.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::RunOutput, BindingSourceKind::PublicInput, BindingSourceKind::GlobalRead};
    port.binding.allow_dynamic_index = allow_index;
    return port;
}

OutputDescriptor output(std::string_view id, const DataType& type)
{ return {key<OutputId>(id), std::string(id), type, {}}; }

ParameterDescriptor fixed(std::string_view id, std::string name, const ConfigValue& initial)
{
    ParameterDescriptor parameter{key<ParameterId>(id), std::move(name), initial.type(), false, initial, {}, {}, {}};
    parameter.binding.time = BindingTime::SnapshotBuild;
    return parameter;
}

ConfigValue enumeration(std::string_view schema, std::initializer_list<std::string_view> choices)
{
    std::vector<EnumMember> members;
    for (auto choice : choices) { members.push_back({key<EnumMemberId>(choice), std::string(choice)}); }
    const auto type = std::get<DataType>(DataType::enumeration(key<SchemaId>(schema), 1, members));
    return std::get<ConfigValue>(ConfigValue::enumeration(type, members.front().id));
}

ToolRegistration registration(ToolDescriptor tool, Execution execute)
{
    const auto schema = tool.schema.id;
    return {std::move(tool), {schema, 1, 1, {}}, std::make_shared<LogicFactory>(execute)};
}
} // namespace

std::vector<core::ToolRegistration> logic_registrations()
{
    const auto integer = DataType::integer();
    const auto integers = DataType::list(integer);
    const auto empty = std::get<ConfigValue>(ConfigValue::list(integer, {}));

    auto calc = descriptor("visionpro.calc.int64", "Integer calculation");
    calc.inputs = {port("left", integer, true), port("right", integer, true)};
    calc.parameters = {fixed("operation", "Operation", enumeration("visionpro.calc.int64.operation", {"Add", "Subtract", "Multiply", "Divide"}))};
    calc.outputs = {output("value", integer)};

    auto accumulator = descriptor("visionpro.calc.int64_accumulate", "Integer accumulator");
    accumulator.inputs = {port("value", integer, true)};
    accumulator.outputs = {output("value", integer)};
    accumulator.parameters = {fixed("initial", "Initial value", ConfigValue::integer(0)),
        fixed("mode", "State mode", enumeration("visionpro.calc.int64_accumulate.mode", {"PerRun", "Session"}))};
    accumulator.execution.effects = ToolEffects::State;
    accumulator.state = NodeStateDefinition{integer, ConfigValue::integer(0), StateScope::WorkflowRun,
        StateResetPolicy::DomainStart, StateCompatibilityPolicy::PreserveCompatible,
        StateMutationTiming::OnResultAccepted, StateConcurrencyPolicy::Serialized};
    accumulator.state_parameters = {key<ParameterId>("initial"), StatePolicyParameter{key<ParameterId>("mode"),
        {{key<EnumMemberId>("PerRun"), StateScope::WorkflowRun, StateResetPolicy::DomainStart},
         {key<EnumMemberId>("Session"), StateScope::SolutionSession, StateResetPolicy::DomainStartAndExplicit}}}};

    auto logic = descriptor("visionpro.logic.and", "Boolean AND");
    logic.inputs = {port("values", DataType::list(DataType::boolean()))};
    logic.inputs.front().binding.cardinality = BindingCardinality::MultipleSources;
    logic.outputs = {output("passed", DataType::boolean())};

    auto branch = descriptor("visionpro.logic.condition", "Conditional branch");
    branch.inputs = {port("condition", DataType::boolean())};
    branch.control_outputs = {{key<ControlOutputId>("true"), "True"}, {key<ControlOutputId>("false"), "False"}};

    auto formatter = descriptor("visionpro.format.int64_list", "Format integer list");
    formatter.inputs = {port("list", integers), port("values", integers)};
    for (auto& port : formatter.inputs) { port.required = false; port.default_value = empty; }
    formatter.inputs.back().binding.cardinality = BindingCardinality::MultipleSources;
    formatter.parameters = {fixed("indices", "Selected indices (in order)", empty),
        fixed("skip_out_of_range", "Skip out-of-range positions in indices (0-based)", empty),
        fixed("format", "Item format", std::get<ConfigValue>(ConfigValue::string("{value}"))),
        fixed("separator", "Separator", std::get<ConfigValue>(ConfigValue::string(",")))};
    formatter.outputs = {output("text", DataType::string()), output("used_indices", integers), output("used_values", integers)};

    return {registration(std::move(calc), arithmetic), registration(std::move(accumulator), accumulate),
        registration(std::move(logic), conjunction), registration(std::move(branch), condition), registration(std::move(formatter), format_list)};
}
} // namespace visionpro::tools
