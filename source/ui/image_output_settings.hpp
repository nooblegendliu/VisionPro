#pragma once

#include <visionpro/core/tool_registry.hpp>
#include <visionpro/workflow/model.hpp>
#include <QString>
#include <functional>
#include <set>

class QFormLayout;
class QVBoxLayout;

namespace visionpro::ui {
std::set<core::NodeId> preceding_nodes(const workflow::Workflow& flow, core::NodeId target);
struct OutputEditorActions {
    std::function<void(QFormLayout&, const core::InputDescriptor&)> input;
    std::function<void(QFormLayout&, const core::ParameterDescriptor&)> parameter;
    std::function<void(QFormLayout&, const core::ResourceRequirement&, const QString&)> directory;
    std::function<void(const core::InputId&, const workflow::OutputReference&, bool)> toggle_item;
    std::function<void(const core::InputId&, const workflow::OutputReference&, const core::DisplayItemStyle&)> style;
    std::function<void(const std::string&)> select_credential;
};
void image_output_settings(QVBoxLayout& basic, QVBoxLayout& render, QVBoxLayout& layers,
    const workflow::Solution& solution, const workflow::Workflow& flow, const workflow::Node& node,
    const core::ToolRegistry& registry, const core::ToolDescriptor& descriptor, const OutputEditorActions& edit);
} // namespace visionpro::ui
