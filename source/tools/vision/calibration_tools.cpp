#include "calibration.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/file_resources.hpp>

#include <algorithm>

namespace visionpro::tools::vision {
namespace {
using namespace core;
using Outputs = Result<std::vector<ToolOutput>>;
using Execute = Outputs (*)(const InvocationContextData&);

OutputId output(std::string_view name)
{ return std::get<OutputId>(SchemaIdFactory::parse<OutputId>(name)); }

const DataValue& input(const InvocationContextData& context, std::string_view name)
{ return std::ranges::find(context.inputs, name, [](const auto& item) { return item.id.text(); })->value; }

Outputs fit(const InvocationContextData& context)
{
    const auto slot = std::ranges::find(context.resources, "calibration", [](const auto& item) { return item.slot.text(); });
    if (slot == context.resources.end()) {
        return Diagnostic{ErrorCode::InvalidResource, Severity::Error, {}, "Planar fit requires its calibration resource."};
    }
    const auto& reference = slot->lease->reference();
    DiagnosticLocation location;
    location.resource = reference.id;
    if (reference.kind != ResourceKind::Calibration || !reference.revision) {
        return Diagnostic{ErrorCode::ResourceVersionMismatch, Severity::Error, location,
            "Planar fit requires a calibration resource pinned to an explicit revision."};
    }
    const auto file = std::dynamic_pointer_cast<const infrastructure::FileResourceLease>(slot->lease);
    if (!file) {
        return Diagnostic{ErrorCode::InvalidResource, Severity::Error, location, "Planar fit requires a calibration file lease."};
    }
    auto fitted = fit_calibration(file->bytes());
    if (auto* failure = std::get_if<Diagnostic>(&fitted)) {
        failure->location.resource = reference.id;
        return std::move(*failure);
    }
    const auto& result = std::get<CalibrationFit>(fitted);
    return std::vector<ToolOutput>{
        {output("transform"), std::get<DataValue>(DataValue::transform2d(result.transform))},
        {output("rms_error_mm"), std::get<DataValue>(DataValue::real(result.rms_error_mm))},
        {output("source"), std::get<DataValue>(DataValue::resource_reference(reference))}};
}

Outputs map(const InvocationContextData& context)
{
    auto mapped = pixel_to_mm(*input(context, "transform").as_transform2d(), *input(context, "point").as_point2d());
    if (const auto* failure = std::get_if<Diagnostic>(&mapped)) { return *failure; }
    return std::vector<ToolOutput>{{output("point"), std::get<DataValue>(DataValue::point2d(std::get<Point2D>(mapped)))}};
}

Outputs measure(const InvocationContextData& context)
{
    auto measured = distance_mm(*input(context, "first").as_point2d(), *input(context, "second").as_point2d());
    if (const auto* failure = std::get_if<Diagnostic>(&measured)) { return *failure; }
    return std::vector<ToolOutput>{{output("distance_mm"), std::get<DataValue>(DataValue::real(std::get<double>(measured)))}};
}

class MeasurementTool final : public ToolInstance {
public:
    MeasurementTool(const ToolExecutionConfiguration& configuration, Execute execute) : ToolInstance(configuration), execute_(execute) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        auto outputs = execute_(context->data());
        if (auto* failure = std::get_if<Diagnostic>(&outputs)) { return std::move(*failure); }
        return InvocationStart{ToolCompletion{context->data().identity, ToolCompletionStatus::Succeeded,
            ToolJudgement::NotEvaluated, std::get<std::vector<ToolOutput>>(std::move(outputs)), {}}};
    }
    Execute execute_;
};

class MeasurementFactory final : public ToolInstanceFactory {
public:
    explicit MeasurementFactory(Execute execute) : execute_(execute) {}
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<MeasurementTool>(configuration, execute_)}; }
private:
    Execute execute_;
};

ToolRegistration registration(ToolDescriptor descriptor, Execute execute)
{
    const auto schema = descriptor.schema.id;
    return {std::move(descriptor), {schema, 1, 1, {}}, std::make_shared<MeasurementFactory>(execute)};
}
} // namespace

core::ToolRegistration fit_registration(core::ToolDescriptor descriptor) { return registration(std::move(descriptor), fit); }
core::ToolRegistration map_point_registration(core::ToolDescriptor descriptor) { return registration(std::move(descriptor), map); }
core::ToolRegistration distance_registration(core::ToolDescriptor descriptor) { return registration(std::move(descriptor), measure); }

} // namespace visionpro::tools::vision
