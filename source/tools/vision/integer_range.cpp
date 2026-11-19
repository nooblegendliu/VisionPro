#include "integer_range.hpp"

#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>

namespace visionpro::tools::vision {
namespace {
using namespace core;

template<SchemaIdentity Id> Id key(std::string_view spelling)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(spelling)); }

template<class Range, class Value>
auto number(const Value& value)
{
    if constexpr (std::same_as<Range, IntegerRange>) { return *value.as_int64(); }
    else { return *value.as_float64(); }
}

template<class Range>
class RangeTool final : public ToolInstance {
public:
    RangeTool(const ToolExecutionConfiguration& configuration, Range range)
        : ToolInstance(configuration), range_(range) {}

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        const auto& invocation = context->data();
        const auto input = std::ranges::find(invocation.inputs, key<InputId>("value"), &FixedToolInput::id);
        const auto value = number<Range>(input->value);
        const bool passed = value >= range_.minimum && value <= range_.maximum;

        return InvocationStart{ToolCompletion{invocation.identity, ToolCompletionStatus::Succeeded,
            passed ? ToolJudgement::OK : ToolJudgement::NG, {{key<OutputId>("passed"), DataValue::boolean(passed)}}, {}}};
    }

    const Range range_;
};

template<class Range>
class RangeFactory final : public ToolInstanceFactory {
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    {
        const auto& parameters = configuration.data().parameters;
        const auto minimum = std::ranges::find(parameters, key<ParameterId>("minimum"), &FixedToolParameter::id);
        const auto maximum = std::ranges::find(parameters, key<ParameterId>("maximum"), &FixedToolParameter::id);
        const Range range{number<Range>(minimum->value), number<Range>(maximum->value)};
        if (range.minimum > range.maximum) {
            DiagnosticLocation location;
            location.parameter = maximum->id;
            return Diagnostic{ErrorCode::ConstraintViolation, Severity::Error, std::move(location),
                "Range maximum must be greater than or equal to minimum."};
        }
        return std::shared_ptr<ToolInstance>{std::make_shared<RangeTool<Range>>(configuration, range)};
    }
};
ToolDescriptor range_descriptor(std::string_view id, std::string display_name, DataType type)
{
    ToolDescriptor tool{key<ToolTypeId>(id), std::move(display_name), 1, {key<SchemaId>(id), 1, {}},
        {}, {}, {}, {}, {}, {}, {}, {}, {}, {}};
    tool.execution.retry = ToolRetry::Idempotent;
    for (auto name : {"minimum", "maximum"}) {
        ParameterDescriptor parameter{key<ParameterId>(name), name, type, true, {}, {}, {}, {}};
        parameter.binding.time = BindingTime::SnapshotBuild;
        tool.parameters.push_back(std::move(parameter));
    }
    InputDescriptor value{key<InputId>("value"), "value", type, true, {}, {}, {}, {}};
    value.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::RunOutput, BindingSourceKind::PublicInput};
    tool.inputs.push_back(std::move(value));
    tool.outputs = {{key<OutputId>("passed"), "passed", DataType::boolean(), {}}};
    return tool;
}
} // namespace

core::ToolDescriptor integer_range_descriptor()
{ return range_descriptor("visionpro.judge.integer_range", "Judge integer range", DataType::integer()); }

core::ToolDescriptor float_range_descriptor()
{ return range_descriptor("visionpro.judge.float_range", "Judge floating-point range", DataType::real()); }

core::ToolRegistration integer_range_registration(core::ToolDescriptor descriptor)
{
    const auto schema = descriptor.schema.id;
    return {std::move(descriptor), {schema, 1, 1, {}}, std::make_shared<RangeFactory<IntegerRange>>()};
}

core::ToolRegistration float_range_registration(core::ToolDescriptor descriptor)
{
    const auto schema = descriptor.schema.id;
    return {std::move(descriptor), {schema, 1, 1, {}}, std::make_shared<RangeFactory<RealRange>>()};
}

} // namespace visionpro::tools::vision
