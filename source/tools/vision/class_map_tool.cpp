#include "class_map_tool.hpp"

#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace visionpro::tools::vision {
namespace {
using namespace core;
constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};

template<SchemaIdentity Id> Id key(std::string_view value)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(value)); }

Diagnostic parameter_error(std::string_view parameter, ErrorCode code, std::string message)
{
    DiagnosticLocation location;
    location.parameter = key<ParameterId>(parameter);
    return {code, Severity::Error, std::move(location), std::move(message)};
}

ParameterDescriptor parameter(std::string_view name, const ConfigValue& initial)
{
    ParameterDescriptor parameter{key<ParameterId>(name), std::string(name), initial.type(), false, initial, {}, {}, {}};
    parameter.binding.time = BindingTime::SnapshotBuild;
    return parameter;
}

class ClassMapTool final : public ToolInstance {
public:
    explicit ClassMapTool(const ToolExecutionConfiguration& configuration) : ToolInstance(configuration)
    {
        const auto& parameters = configuration.data().parameters;
        const auto values = [&](std::string_view name) {
            return std::ranges::find(parameters, name, [](const auto& p) { return p.id.text(); })->value.elements();
        };
        for (const auto& v : *values("thresholds")) { thresholds_.push_back(*v.as_int64()); }
        for (const auto& v : *values("class_names")) { names_.push_back(*v.as_string()); }
    }
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        const auto& call = context->data();
        const auto& input = *std::ranges::find(call.inputs, key<InputId>("image"), &FixedToolInput::id);
        auto map = classify_intensity(*input.value.as_image(), thresholds_, names_);
        if (auto* failure = std::get_if<Diagnostic>(&map)) { return std::move(*failure); }
        return InvocationStart{ToolCompletion{call.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated,
            {{key<OutputId>("class_map"), std::get<DataValue>(std::move(map))}}, {}}};
    }
    std::vector<std::int64_t> thresholds_;
    std::vector<std::string> names_;
};

class ClassMapFactory final : public ToolInstanceFactory {
public:
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<ClassMapTool>(configuration)}; }
};
} // namespace

Result<DataValue> classify_intensity(const ImageFrame& source, std::span<const std::int64_t> thresholds,
    std::span<const std::string> class_names)
{
    if (source.image.format() != mono8) {
        DiagnosticLocation where;
        where.input = key<InputId>("image");
        return Diagnostic{ErrorCode::UnsupportedPixelFormat, Severity::Error, std::move(where), "Intensity classification requires UInt8 Mono."};
    }
    if ((!thresholds.empty() && (thresholds.front() < 1 || thresholds.back() > 255))
        || std::ranges::adjacent_find(thresholds, std::greater_equal<>{}) != thresholds.end()) {
        return parameter_error("thresholds", ErrorCode::ConstraintViolation, "Class thresholds must increase strictly within [1,255].");
    }
    if (class_names.empty() || class_names.size() > 256 || std::ranges::any_of(class_names, &std::string::empty)) {
        return parameter_error("class_names", ErrorCode::ConstraintViolation, "Declare 1 to 256 nonempty class names in class ID order.");
    }
    std::array<std::uint8_t, 256> ids{};
    for (std::size_t level = 0; level < ids.size(); ++level) {
        ids[level] = static_cast<std::uint8_t>(std::ranges::upper_bound(thresholds, static_cast<std::int64_t>(level)) - thresholds.begin());
    }

    const auto width = source.image.width();
    const auto height = source.image.height();
    if (height > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / width) {
        return Diagnostic{ErrorCode::SizeOverflow, Severity::Error, {}, "Class pixel counts exceed Int64."};
    }
    auto labels = source.image.copy_pixels();
    std::array<std::int64_t, 256> counts{};
    for (auto& sample : labels) {
        const auto id = ids[std::to_integer<std::uint8_t>(sample)];
        if (id >= class_names.size()) {
            return parameter_error("class_names", ErrorCode::MissingField, "Class table is missing pixel ID " + std::to_string(id) + ".");
        }
        ++counts[id];
        sample = static_cast<std::byte>(id);
    }
    auto result = source.image.copy_with_pixels({width, height, width, mono8}, labels);
    if (auto* failure = std::get_if<Diagnostic>(&result)) { return std::move(*failure); }
    ClassMap map{{std::get<ImageHandle>(std::move(result)), source.frame, source.captured_at}, {}};
    for (std::size_t id = 0; id < class_names.size(); ++id) {
        map.classes.push_back({static_cast<std::uint8_t>(id), class_names[id], counts[id]});
    }
    return make_class_map(map);
}

ToolDescriptor class_map_descriptor()
{
    constexpr auto id = "visionpro.image.class_map";
    ToolDescriptor tool{key<ToolTypeId>(id), "Generate intensity class map", 1, {key<SchemaId>(id), 1, {}},
        {}, {}, {}, {}, {}, {}, {}, {}, {}, {}};
    tool.execution.retry = ToolRetry::Idempotent;
    InputDescriptor image{key<InputId>("image"), "Image", DataType::image(), true, {}, {}, {}, {MeasurementUnit::Pixel, {}}};
    image.binding.allowed_sources = {BindingSourceKind::RunOutput, BindingSourceKind::PublicInput};
    tool.inputs = {std::move(image)};
    std::vector<ConfigValue> names;
    for (const auto name : {"Background", "Class 1", "Class 2"}) { names.push_back(std::get<ConfigValue>(ConfigValue::string(name))); }
    tool.parameters = {
        parameter("thresholds", std::get<ConfigValue>(ConfigValue::list(DataType::integer(), {ConfigValue::integer(1), ConfigValue::integer(2)}))),
        parameter("class_names", std::get<ConfigValue>(ConfigValue::list(DataType::string(), names)))};
    tool.outputs = {{key<OutputId>("class_map"), "Class map", class_map_type(), {}}};
    tool.image_formats = {mono8};
    return tool;
}

ToolRegistration class_map_registration(ToolDescriptor descriptor)
{
    const auto schema = descriptor.schema.id;
    return {std::move(descriptor), {schema, 1, 1, {}}, std::make_shared<ClassMapFactory>()};
}
} // namespace visionpro::tools::vision
