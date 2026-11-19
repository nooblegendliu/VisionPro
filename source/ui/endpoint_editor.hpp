#pragma once

#include <visionpro/core/resource.hpp>

#include <QString>

class QWidget;

namespace visionpro::ui {

std::optional<std::string_view> endpoint_adapter(const core::ToolTypeId& tool);
bool matches_endpoint(const core::ResourceDefinition& device, std::string_view adapter);
QString endpoint_address(const core::ResourceDefinition& resource);
std::optional<core::ResourceDefinition> edit_endpoint(QWidget* parent, std::string_view adapter,
    const core::ResourceDefinition* existing);

} // namespace visionpro::ui
