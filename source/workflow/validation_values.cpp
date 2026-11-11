#include "validation_values.hpp"

#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <string_view>
#include <type_traits>
#include <utility>

namespace visionpro::workflow::detail {
namespace {
using Check = core::Result<std::monostate>;
using InheritedCheck = std::function<Check(const core::InheritedRegion&)>;

bool failed(const Check& checked) { return std::holds_alternative<core::Diagnostic>(checked); }

Check at(Check checked, core::DiagnosticPathStep step)
{
    if (auto* invalid = std::get_if<core::Diagnostic>(&checked)) {
        invalid->location.path.insert(invalid->location.path.begin(), std::move(step));
    }
    return checked;
}

core::FieldId field(std::string_view name)
{
    // Only fixed, valid implementation tokens are passed here.
    return std::get<core::FieldId>(core::SchemaIdFactory::parse<core::FieldId>(name));
}

Check frame(const core::CoordinateSystemId& id, core::MeasurementUnit unit, const Solution& solution)
{
    const auto found = std::ranges::find(solution.coordinates, id, &core::CoordinateFrameDefinition::id);
    if (found == solution.coordinates.end()) {
        return core::Diagnostic{core::ErrorCode::InvalidCoordinate, core::Severity::Error, {},
            "Configured geometry refers to an undeclared coordinate frame."};
    }
    if (found->unit != unit) {
        return core::Diagnostic{core::ErrorCode::CoordinateMismatch, core::Severity::Error, {},
            "Configured geometry unit differs from its declared coordinate frame."};
    }
    return core::validate_coordinate_frame(*found);
}

Check transform(const core::Transform2D& value, const Solution& solution)
{
    if (auto checked = core::validate_transform(value); failed(checked)) { return checked; }
    if (auto checked = frame(value.from, value.from_unit, solution); failed(checked)) {
        return at(std::move(checked), field("from"));
    }
    if (auto checked = frame(value.to, value.to_unit, solution); failed(checked)) {
        return at(std::move(checked), field("to"));
    }
    if (value.calibration) {
        return at(core::validate_resource_reference(*value.calibration, solution.resources), field("calibration"));
    }
    return std::monostate{};
}

Check mask(const core::MaskConfig& value, const Solution& solution)
{
    if (auto checked = core::validate_mask_config(value); failed(checked)) { return checked; }
    if (auto checked = core::validate_resource_reference(value.resource, solution.resources); failed(checked)) {
        return at(std::move(checked), field("resource"));
    }
    if (auto checked = frame(value.mask_frame, core::MeasurementUnit::Pixel, solution); failed(checked)) {
        return at(std::move(checked), field("mask_frame"));
    }
    if (auto checked = frame(value.mapping.target_frame, core::MeasurementUnit::Pixel, solution); failed(checked)) {
        return at(std::move(checked), field("target_frame"));
    }
    if (value.mapping.transform) {
        return at(transform(*value.mapping.transform, solution), field("transform"));
    }
    return std::monostate{};
}

Check region_source(const core::RegionSource& source, const Solution& solution, const InheritedCheck& inherited)
{
    return std::visit([&](const auto& value) -> Check {
        if (auto checked = frame(value.coordinate_frame, value.unit, solution); failed(checked)) { return checked; }
        if (value.transform) {
            if (auto checked = transform(*value.transform, solution); failed(checked)) {
                return at(std::move(checked), field("transform"));
            }
        }
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, core::InheritedRegion>) {
            if (!inherited) {
                return core::Diagnostic{core::ErrorCode::InvalidBinding, core::Severity::Error, {},
                    "Inherited regions require an explicit reference validation context."};
            }
            return inherited(value);
        } else {
            if (const auto* configured_mask = std::get_if<core::MaskConfig>(&value.geometry)) {
                return at(mask(*configured_mask, solution), field("geometry"));
            }
            return std::monostate{};
        }
    }, source);
}

Check region(const core::RegionDefinition& value, const Solution& solution,
    const core::RegionCapabilities* capabilities, const InheritedCheck& inherited)
{
    const auto contextualize = [&](Check checked) {
        if (auto* invalid = std::get_if<core::Diagnostic>(&checked)) { invalid->location.region = value.id; }
        return checked;
    };
    if (auto checked = capabilities ? core::validate_region(value, *capabilities) : core::validate_region(value);
        failed(checked)) { return contextualize(std::move(checked)); }
    if (auto checked = frame(value.coordinate_frame, value.unit, solution); failed(checked)) {
        return contextualize(std::move(checked));
    }
    if (auto checked = region_source(value.source, solution, inherited); failed(checked)) {
        return contextualize(at(std::move(checked), field("source")));
    }
    for (std::size_t index = 0; index < value.exclusions.size(); ++index) {
        if (auto checked = region_source(value.exclusions[index], solution, inherited); failed(checked)) {
            return contextualize(at(at(std::move(checked), core::DiagnosticElementIndex{index}), field("exclusions")));
        }
    }
    return std::monostate{};
}
} // namespace

Check validate_config_references(const core::ConfigValue& value, const Solution& solution,
    const core::RegionCapabilities* capabilities, const InheritedCheck& inherited)
{
    if (const auto* optional = value.optional_value()) {
        return validate_config_references(*optional, solution, capabilities, inherited);
    }
    if (const auto* elements = value.elements()) {
        for (std::size_t index = 0; index < elements->size(); ++index) {
            if (auto checked = validate_config_references((*elements)[index], solution, capabilities, inherited); failed(checked)) {
                return at(std::move(checked), core::DiagnosticElementIndex{index});
            }
        }
    }
    if (const auto* fields = value.fields()) {
        for (const auto& member : *fields) {
            if (auto checked = validate_config_references(member.value, solution, capabilities, inherited); failed(checked)) {
                return at(std::move(checked), member.id);
            }
        }
    }
    if (const auto* reference = value.as_resource_reference()) {
        return core::validate_resource_reference(*reference, solution.resources);
    }
    if (const auto* configured_region = value.as_region2d()) {
        return region(*configured_region, solution, capabilities, inherited);
    }
    if (const auto* configured_transform = value.as_transform2d()) {
        return transform(*configured_transform, solution);
    }
    const auto semantics = config_value_semantics(value);
    if (semantics.coordinate_system) { return frame(*semantics.coordinate_system, semantics.unit, solution); }
    return std::monostate{};
}

core::ValueSemantics config_value_semantics(const core::ConfigValue& value)
{
    if (const auto* optional = value.optional_value()) { return config_value_semantics(*optional); }
    if (const auto* point = value.as_point2d()) { return {point->unit, point->coordinate_frame}; }
    if (const auto* line = value.as_line2d()) { return {line->start.unit, line->start.coordinate_frame}; }
    if (const auto* circle = value.as_circle2d()) { return {circle->center.unit, circle->center.coordinate_frame}; }
    if (const auto* pose = value.as_pose2d()) { return {pose->position.unit, pose->position.coordinate_frame}; }
    if (const auto* configured_region = value.as_region2d()) {
        return {configured_region->unit, configured_region->coordinate_frame};
    }
    return {};
}

} // namespace visionpro::workflow::detail
