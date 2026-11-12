#include "simulation.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/file_resources.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace visionpro::tools {
namespace {

using namespace core;

enum class Operation { ReadByte, Add, Select, ForwardValues };

template<SchemaIdentity Id>
Id key(std::string_view spelling)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(spelling)); }

const DataValue& input_value(const InvocationContextData& invocation, std::string_view id)
{
    // The invocation context has validated every declared input's identity and type.
    return std::ranges::find(invocation.inputs, id, [](const FixedToolInput& input) { return input.id.text(); })->value;
}

class SimulationTool final : public ToolInstance {
public:
    SimulationTool(const ToolExecutionConfiguration& configuration, Operation operation)
        : ToolInstance(configuration), operation_(operation) {}

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        const auto& invocation = context->data();
        const auto& descriptor = invocation.configuration.data().tool;
        ToolCompletion completed{invocation.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated, {}, {}};
        switch (operation_) {
        case Operation::ReadByte: {
            const auto& binding = invocation.resources.front();
            const auto file = std::dynamic_pointer_cast<const infrastructure::FileResourceLease>(binding.lease);
            if (!file || file->bytes().empty()) {
                DiagnosticLocation location;
                location.resource = binding.lease->reference().id;
                return Diagnostic{ErrorCode::InvalidResource, Severity::Error, std::move(location),
                    "The simulated byte reader requires a nonempty binary file."};
            }
            const auto byte = std::to_integer<std::int64_t>(file->bytes().front());
            completed.outputs.push_back({descriptor.outputs.front().id, DataValue::integer(byte)});
            break;
        }
        case Operation::Add: {
            const auto left = *input_value(invocation, "left").as_int64();
            const auto right = *input_value(invocation, "right").as_int64();
            if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right)
                || (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
                return Diagnostic{ErrorCode::InvalidValue, Severity::Error, {}, "Simulated addition exceeds the Int64 range."};
            }
            completed.outputs.push_back({descriptor.outputs.front().id, DataValue::integer(left + right)});
            break;
        }
        case Operation::Select: {
            const auto mask = *input_value(invocation, "mask").as_int64();
            if ((mask & 1) != 0) { completed.selected_control_outputs.push_back(key<ControlOutputId>("left")); }
            if ((mask & 2) != 0) { completed.selected_control_outputs.push_back(key<ControlOutputId>("right")); }
            break;
        }
        case Operation::ForwardValues:
            completed.outputs.push_back({descriptor.outputs.front().id, input_value(invocation, "values")});
            break;
        }
        return InvocationStart{std::move(completed)};
    }

    Operation operation_;
};

class SimulationFactory final : public ToolInstanceFactory {
public:
    explicit SimulationFactory(Operation operation) : operation_(operation) {}

    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<SimulationTool>(configuration, operation_)}; }

private:
    Operation operation_;
};

ToolDescriptor descriptor(std::string_view id, std::string name)
{
    return {key<ToolTypeId>(id), std::move(name), 1, {key<SchemaId>(id), 1, {}},
        {}, {}, {}, {}, {}, {}, {}, {}, {}, {}};
}

InputDescriptor input(std::string_view id, DataType type, std::vector<BindingSourceKind> sources)
{
    InputDescriptor result{key<InputId>(id), std::string(id), std::move(type), true, {}, {}, {}, {}};
    result.binding.allowed_sources = std::move(sources);
    return result;
}

ToolRegistration registration(ToolDescriptor tool, Operation operation)
{
    const auto schema = tool.schema.id;
    return {std::move(tool), {schema, 1, 1, {}}, std::make_shared<SimulationFactory>(operation)};
}

} // namespace

core::Result<core::ToolRegistry> make_simulation_registry()
{
    const auto integer = DataType::integer();
    const auto list = DataType::list(integer);
    const OutputDescriptor value{key<OutputId>("value"), "Value", integer, {}};
    const OutputDescriptor values{key<OutputId>("values"), "Values", list, {}};

    auto reader = descriptor("visionpro.sim.file_byte", "Simulated file byte");
    reader.resources = {{key<SchemaId>("payload"), ResourceKind::Binary}};
    reader.outputs = {value};

    auto adder = descriptor("visionpro.sim.add", "Simulated addition");
    const std::vector integer_sources{BindingSourceKind::Constant, BindingSourceKind::RunOutput, BindingSourceKind::PublicInput};
    adder.inputs = {input("left", integer, integer_sources), input("right", integer, integer_sources)};
    adder.outputs = {value};

    auto selector = descriptor("visionpro.sim.select", "Simulated branch selection");
    auto mask = input("mask", integer, {BindingSourceKind::PublicInput, BindingSourceKind::Constant});
    mask.constraints.integer_range = IntegerRange{0, 3};
    selector.inputs = {std::move(mask)};
    selector.control_outputs = {{key<ControlOutputId>("left"), "Left"}, {key<ControlOutputId>("right"), "Right"}};

    auto collect = descriptor("visionpro.sim.collect", "Simulated collection");
    auto collection = input("values", list, {BindingSourceKind::RunOutput});
    collection.binding.cardinality = BindingCardinality::MultipleSources;
    collect.inputs = {std::move(collection)};
    collect.outputs = {values};

    auto deliver = descriptor("visionpro.sim.deliver", "Simulated delivery");
    deliver.inputs = {input("values", list, {BindingSourceKind::RunOutput})};
    deliver.outputs = {values};

    const std::array definitions{
        registration(std::move(reader), Operation::ReadByte), registration(std::move(adder), Operation::Add),
        registration(std::move(selector), Operation::Select), registration(std::move(collect), Operation::ForwardValues),
        registration(std::move(deliver), Operation::ForwardValues)};
    return ToolRegistry::create(definitions);
}

} // namespace visionpro::tools
