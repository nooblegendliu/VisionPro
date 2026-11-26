#pragma once

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/model.hpp>

#include <algorithm>
#include <array>

namespace visionpro::ui {

inline constexpr std::array<std::string_view, 6> camera_diagnostic_outputs{
    "frame_number", "serial", "timestamp_source", "device_ticks", "tick_frequency_hz", "device_frame_number"};

inline bool is_image_source(const workflow::Node* node)
{
    const auto* tool = node ? std::get_if<workflow::ToolConfiguration>(&node->configuration) : nullptr;
    return tool && tool->type.text() == "visionpro.image.source";
}

inline bool is_image_output(const workflow::Node* node)
{
    const auto* tool = node ? std::get_if<workflow::ToolConfiguration>(&node->configuration) : nullptr;
    return tool && tool->type.text() == "visionpro.image.output";
}

template<class Id>
Id source_key(std::string_view text)
{
    return std::get<Id>(core::SchemaIdFactory::parse<Id>(text));
}

inline const core::ConfigValue* source_value(const workflow::Node& node, std::string_view name)
{
    const workflow::BindingTarget target{source_key<core::ParameterId>(name)};
    const auto it = std::ranges::find(node.bindings, target, &workflow::Binding::target);
    if (it == node.bindings.end() || it->sources.size() != 1) { return nullptr; }
    return std::get_if<core::ConfigValue>(&it->sources.front());
}

inline bool source_flag(const workflow::Node& node, std::string_view name, bool fallback = false)
{
    const auto* value = source_value(node, name);
    return value && value->as_bool() ? *value->as_bool() : fallback;
}

inline std::string_view source_enum(const workflow::Node& node, std::string_view name, std::string_view fallback)
{
    const auto* value = source_value(node, name);
    return value && value->as_enum() ? value->as_enum()->text() : fallback;
}

inline void set_source_value(workflow::Node& node, std::string_view name, core::ConfigValue value)
{
    const workflow::BindingTarget target{source_key<core::ParameterId>(name)};
    auto it = std::ranges::find(node.bindings, target, &workflow::Binding::target);
    workflow::Binding next{target, {std::move(value)}};
    if (it == node.bindings.end()) { node.bindings.push_back(std::move(next)); }
    else { *it = std::move(next); }
}

inline core::Result<std::monostate> ensure_source_frame(workflow::Solution& solution, workflow::Node& node, bool stitch)
{
    const auto field = stitch ? "stitch_origin" : "frame_origin";
    if (source_value(node, field)) { return std::monostate{}; }
    const auto* original = source_value(node, "frame_origin");
    const auto* original_point = original ? original->as_point2d() : nullptr;
    const auto existing = std::ranges::find_if(solution.coordinates, [&](const auto& frame) {
        return !stitch && frame.kind == core::CoordinateKind::Image && frame.unit == core::MeasurementUnit::Pixel;
    });
    auto id = existing != solution.coordinates.end() ? existing->id : source_key<core::CoordinateSystemId>(
        std::string(stitch ? "stitched." : "image.") + node.id.to_string());
    if (stitch && original_point && original_point->coordinate_frame == id) {
        return core::Diagnostic{core::ErrorCode::InvalidCoordinate, core::Severity::Error, {}, "Stitched and source coordinate systems must differ."};
    }
    if (std::ranges::find(solution.coordinates, id, &core::CoordinateFrameDefinition::id) == solution.coordinates.end()) {
        solution.coordinates.push_back({id, core::CoordinateKind::Image, core::MeasurementUnit::Pixel, {0,0}, {1,0}, {0,-1}, core::Handedness::LeftHanded});
    }
    auto point = core::ConfigValue::point2d({id, core::MeasurementUnit::Pixel, 0, 0});
    if (const auto* failure = std::get_if<core::Diagnostic>(&point)) { return *failure; }
    set_source_value(node, field, std::get<core::ConfigValue>(std::move(point)));
    return std::monostate{};
}

} // namespace visionpro::ui
