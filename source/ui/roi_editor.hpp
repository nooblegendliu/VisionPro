#pragma once

#include <visionpro/core/geometry.hpp>

#include <optional>

class QWidget;

namespace visionpro {
namespace core { class ToolRegistry; }
namespace workflow { struct Solution; struct Workflow; struct Node; }

namespace ui {

struct RoiEditContext {
    const workflow::Solution& solution;
    const core::ToolRegistry& registry;
    const workflow::Workflow* workflow{};
    const workflow::Node* node{};
    const core::RegionCapabilities* capabilities{};
    core::ValueSemantics semantics{};
    const core::ImageHandle* image{};
};

[[nodiscard]] std::optional<core::RegionDefinition> edit_roi(QWidget* parent, const RoiEditContext& context,
    const core::RegionDefinition* current);

} // namespace ui
} // namespace visionpro
