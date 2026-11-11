#include "runtime_values.hpp"
#include "validation_values.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace visionpro::workflow::detail {
namespace {
using core::ConfigValue;
using core::DataValue;
using core::Diagnostic;
using core::ErrorCode;
using core::Result;
using Check = Result<std::monostate>;

Diagnostic error(ErrorCode code, std::string message)
{ return {code, core::Severity::Error, {}, std::move(message)}; }
bool failed(const Check& checked) { return std::holds_alternative<Diagnostic>(checked); }

Check at(Check checked, core::DiagnosticPathStep step)
{
    if (auto* invalid = std::get_if<Diagnostic>(&checked)) {
        invalid->location.path.insert(invalid->location.path.begin(), std::move(step));
    }
    return checked;
}

Check frame(const core::CoordinateSystemId& id, core::MeasurementUnit unit, const Solution& solution)
{
    const auto found = std::ranges::find(solution.coordinates, id, &core::CoordinateFrameDefinition::id);
    if (found == solution.coordinates.end()) {
        return error(ErrorCode::InvalidCoordinate, "Runtime value refers to an undeclared coordinate frame.");
    }
    if (found->unit != unit) {
        return error(ErrorCode::CoordinateMismatch, "Runtime value unit differs from its declared coordinate frame.");
    }
    return core::validate_coordinate_frame(*found);
}

Check compatible(const core::ValueSemantics& actual, const core::ValueSemantics& expected)
{
    if ((actual.unit != core::MeasurementUnit::Unspecified && expected.unit != core::MeasurementUnit::Unspecified
            && actual.unit != expected.unit)
        || (actual.coordinate_system && expected.coordinate_system
            && actual.coordinate_system != expected.coordinate_system)) {
        return error(ErrorCode::CoordinateMismatch, "Runtime payload does not match the declared coordinate frame or unit.");
    }
    return std::monostate{};
}

Check config_domain(Result<ConfigValue> configured, const Solution& solution,
    const core::ValueSemantics& semantics, const core::ToolDescriptor* tool, bool allow_inherited)
{
    if (const auto* invalid = std::get_if<Diagnostic>(&configured)) { return *invalid; }
    const auto& value = std::get<ConfigValue>(configured);
    if (auto checked = compatible(config_value_semantics(value), semantics); failed(checked)) { return checked; }
    return validate_config_references(value, solution,
        tool && tool->region_capabilities ? &*tool->region_capabilities : nullptr,
        [&](const core::InheritedRegion&) -> Check {
            if (!allow_inherited) {
                return error(ErrorCode::InvalidValue, "Runtime source ROI must not contain unresolved inherited output references.");
            }
            return std::monostate{};
        });
}

Check image(const core::ImageHandle& value, const Solution& solution, const core::ToolDescriptor* tool)
{
    if (auto checked = frame(value.coordinate_frame(), core::MeasurementUnit::Pixel, solution); failed(checked)) { return checked; }
    if (auto checked = frame(value.root_frame(), core::MeasurementUnit::Pixel, solution); failed(checked)) { return checked; }
    if (tool) { return core::validate_image_support(*tool, value); }
    return std::monostate{};
}
} // namespace

Result<DataValue> materialize_config_value(const ConfigValue& value)
{
    switch (value.type().kind()) {
    case core::TypeKind::Bool: return DataValue::boolean(*value.as_bool());
    case core::TypeKind::Int64: return DataValue::integer(*value.as_int64());
    case core::TypeKind::Float64: return DataValue::real(*value.as_float64());
    case core::TypeKind::String: return DataValue::string(*value.as_string());
    case core::TypeKind::Binary: return DataValue::binary(*value.as_binary());
    case core::TypeKind::Enum: return DataValue::enumeration(value.type(), *value.as_enum());
    case core::TypeKind::Point2D: return DataValue::point2d(*value.as_point2d());
    case core::TypeKind::Line2D: return DataValue::line2d(*value.as_line2d());
    case core::TypeKind::Circle2D: return DataValue::circle2d(*value.as_circle2d());
    case core::TypeKind::Pose2D: return DataValue::pose2d(*value.as_pose2d());
    case core::TypeKind::Transform2D: return DataValue::transform2d(*value.as_transform2d());
    case core::TypeKind::Region2D: return DataValue::region2d(*value.as_region2d());
    case core::TypeKind::ResourceReference: return DataValue::resource_reference(*value.as_resource_reference());
    case core::TypeKind::Optional:
        if (const auto* payload = value.optional_value()) {
            auto result = materialize_config_value(*payload);
            if (const auto* failure = std::get_if<Diagnostic>(&result)) { return *failure; }
            return DataValue::some(std::get<DataValue>(result));
        }
        return DataValue::none(*value.type().element_type());
    case core::TypeKind::List: {
        std::vector<DataValue> values;
        for (const auto& element : *value.elements()) {
            auto result = materialize_config_value(element);
            if (const auto* failure = std::get_if<Diagnostic>(&result)) { return *failure; }
            values.push_back(std::get<DataValue>(result));
        }
        return DataValue::list(*value.type().element_type(), values);
    }
    case core::TypeKind::Record: {
        std::vector<core::DataFieldValue> values;
        for (const auto& field : *value.fields()) {
            auto result = materialize_config_value(field.value);
            if (const auto* failure = std::get_if<Diagnostic>(&result)) { return *failure; }
            values.push_back({field.id, std::get<DataValue>(result)});
        }
        return DataValue::record(value.type(), values);
    }
    case core::TypeKind::Image:
    case core::TypeKind::RasterMask:
        return error(ErrorCode::InvalidRunRequest, "Runtime pixel buffers cannot be configured defaults.");
    }
    return error(ErrorCode::InvalidRunRequest, "Unsupported configured default type.");
}

Check validate_runtime_value(const core::DataType& type, const core::ValueConstraints& limits, const DataValue& value)
{
    if (type != value.type()) { return error(ErrorCode::TypeMismatch, "Runtime value must match the exact declared type."); }
    const auto* payload = &value;
    while (payload && payload->type().kind() == core::TypeKind::Optional) { payload = payload->optional_value(); }
    if (payload) {
        if (limits.integer_range) {
            const auto* number = payload->as_int64();
            if (!number || *number < limits.integer_range->minimum || *number > limits.integer_range->maximum) {
                return error(ErrorCode::ConstraintViolation, "Runtime integer exceeds the declared inclusive range.");
            }
        }
        if (limits.real_range) {
            const auto* number = payload->as_float64();
            if (!number || !std::isfinite(*number) || *number < limits.real_range->minimum || *number > limits.real_range->maximum) {
                return error(ErrorCode::ConstraintViolation, "Runtime real exceeds the declared finite inclusive range.");
            }
        }
        if (limits.max_length) {
            std::optional<std::size_t> length;
            if (const auto* string = payload->as_string()) { length = string->size(); }
            else if (const auto* bytes = payload->as_binary()) { length = bytes->size(); }
            else if (const auto* values = payload->elements()) { length = values->size(); }
            if (!length || *length > *limits.max_length) { return error(ErrorCode::ConstraintViolation, "Runtime value exceeds its length limit."); }
        }
    }
    if (!limits.allowed_values.empty()) {
        bool allowed = false;
        for (const auto& item : limits.allowed_values) {
            auto candidate = materialize_config_value(item);
            if (const auto* failure = std::get_if<Diagnostic>(&candidate)) { return *failure; }
            if (value == std::get<DataValue>(candidate)) { allowed = true; break; }
        }
        if (!allowed) { return error(ErrorCode::ConstraintViolation, "Runtime value is not explicitly allowed."); }
    }
    return std::monostate{};
}

Check validate_runtime_domain(const DataValue& value, const Solution& solution,
    const core::ValueSemantics& semantics, const core::ToolDescriptor* tool, bool allow_inherited_regions)
{
    if (const auto* optional = value.optional_value()) {
        return validate_runtime_domain(*optional, solution, semantics, tool, allow_inherited_regions);
    }
    if (const auto* elements = value.elements()) {
        for (std::size_t index = 0; index < elements->size(); ++index) {
            if (auto checked = validate_runtime_domain((*elements)[index], solution, {}, tool, allow_inherited_regions); failed(checked)) {
                return at(std::move(checked), core::DiagnosticElementIndex{index});
            }
        }
    }
    if (const auto* fields = value.fields()) {
        for (const auto& member : *fields) {
            if (auto checked = validate_runtime_domain(member.value, solution, {}, tool, allow_inherited_regions); failed(checked)) {
                return at(std::move(checked), member.id);
            }
        }
    }
    const auto domain = [&](Result<ConfigValue> configured) {
        return config_domain(std::move(configured), solution, semantics, tool, allow_inherited_regions);
    };
    if (const auto* point = value.as_point2d()) { return domain(ConfigValue::point2d(*point)); }
    if (const auto* line = value.as_line2d()) { return domain(ConfigValue::line2d(*line)); }
    if (const auto* circle = value.as_circle2d()) { return domain(ConfigValue::circle2d(*circle)); }
    if (const auto* pose = value.as_pose2d()) { return domain(ConfigValue::pose2d(*pose)); }
    if (const auto* transform = value.as_transform2d()) { return domain(ConfigValue::transform2d(*transform)); }
    if (const auto* region = value.as_region2d()) { return domain(ConfigValue::region2d(*region)); }
    if (const auto* reference = value.as_resource_reference()) { return core::validate_resource_reference(*reference, solution.resources); }
    if (const auto* pixels = value.as_image()) {
        if (auto checked = compatible({core::MeasurementUnit::Pixel, pixels->image.coordinate_frame()}, semantics); failed(checked)) { return checked; }
        return image(pixels->image, solution, tool);
    }
    if (const auto* mask = value.as_raster_mask()) {
        if (auto checked = core::validate_raster_mask(*mask); failed(checked)) { return checked; }
        if (auto checked = compatible({core::MeasurementUnit::Pixel, mask->mapping.target_frame}, semantics); failed(checked)) { return checked; }
        if (auto checked = image(mask->pixels, solution, nullptr); failed(checked)) { return checked; }
        if (auto checked = frame(mask->mapping.target_frame, core::MeasurementUnit::Pixel, solution); failed(checked)) { return checked; }
        if (mask->mapping.transform) {
            if (auto checked = config_domain(ConfigValue::transform2d(*mask->mapping.transform), solution, {}, nullptr, false); failed(checked)) { return checked; }
        }
        if (tool && tool->region_capabilities) {
            const auto& capabilities = *tool->region_capabilities;
            if (auto checked = core::validate_region_capabilities(capabilities); failed(checked)) { return checked; }
            if (std::ranges::find(capabilities.shapes, core::RegionShapeKind::RasterMask) == capabilities.shapes.end()
                || std::ranges::find(capabilities.mask_depths, mask->pixels.format().depth) == capabilities.mask_depths.end()
                || (mask->mapping.transform && !capabilities.transforms)) {
                return error(ErrorCode::UnsupportedRegion, "Tool does not support the runtime mask shape, pixel depth or correction.");
            }
        }
        return std::monostate{};
    }
    return std::monostate{};
}

} // namespace visionpro::workflow::detail
