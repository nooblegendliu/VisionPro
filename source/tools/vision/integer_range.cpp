#include "integer_range.hpp"

#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>

namespace visionpro::tools::vision {
namespace {
using namespace core;

template<SchemaIdentity Id> Id key(std::string_view spelling)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(spelling)); }

class IntegerRangeTool final : public ToolInstance {
public:
    IntegerRangeTool(const ToolExecutionConfiguration& configuration, IntegerRange range)
        : ToolInstance(configuration), range_(range) {}

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        const auto& invocation = context->data();
        const auto input = std::ranges::find(invocation.inputs, key<InputId>("value"), &FixedToolInput::id);
        const auto value = *input->value.as_int64();
        const bool passed = value >= range_.minimum && value <= range_.maximum;

        return InvocationStart{ToolCompletion{invocation.identity, ToolCompletionStatus::Succeeded,
            passed ? ToolJudgement::OK : ToolJudgement::NG, {{key<OutputId>("passed"), DataValue::boolean(passed)}}, {}}};
    }

    const IntegerRange range_;
};

class IntegerRangeFactory final : public ToolInstanceFactory {
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    {
        const auto& parameters = configuration.data().parameters;
        const auto minimum = std::ranges::find(parameters, key<ParameterId>("minimum"), &FixedToolParameter::id);
        const auto maximum = std::ranges::find(parameters, key<ParameterId>("maximum"), &FixedToolParameter::id);
        const IntegerRange range{*minimum->value.as_int64(), *maximum->value.as_int64()};
        if (range.minimum > range.maximum) {
            DiagnosticLocation location;
            location.parameter = maximum->id;
            return Diagnostic{ErrorCode::ConstraintViolation, Severity::Error, std::move(location),
                "Integer range maximum must be greater than or equal to minimum."};
        }
        return std::shared_ptr<ToolInstance>{std::make_shared<IntegerRangeTool>(configuration, range)};
    }
};
} // namespace

core::ToolDescriptor integer_range_descriptor()
{
    const auto integer = DataType::integer();
    const auto id = "visionpro.judge.integer_range";
    ToolDescriptor tool{key<ToolTypeId>(id), "Judge integer range", 1, {key<SchemaId>(id), 1, {}},
        {}, {}, {}, {}, {}, {}, {}, {}, {}, {}};
    tool.execution.retry = ToolRetry::Idempotent;
    for (auto name : {"minimum", "maximum"}) {
        ParameterDescriptor parameter{key<ParameterId>(name), name, integer, true, {}, {}, {}, {}};
        parameter.binding.time = BindingTime::SnapshotBuild;
        tool.parameters.push_back(std::move(parameter));
    }
    InputDescriptor value{key<InputId>("value"), "value", integer, true, {}, {}, {}, {}};
    value.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::RunOutput, BindingSourceKind::PublicInput};
    tool.inputs.push_back(std::move(value));
    tool.outputs = {{key<OutputId>("passed"), "passed", DataType::boolean(), {}}};
    return tool;
}

core::ToolRegistration integer_range_registration(core::ToolDescriptor descriptor)
{
    const auto schema = descriptor.schema.id;
    return {std::move(descriptor), {schema, 1, 1, {}}, std::make_shared<IntegerRangeFactory>()};
}

} // namespace visionpro::tools::vision
