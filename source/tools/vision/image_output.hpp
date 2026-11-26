#pragma once

#include <visionpro/core/tool_registry.hpp>

namespace visionpro::tools::vision
{
[[nodiscard]] core::ToolDescriptor image_output_descriptor();
[[nodiscard]] core::ToolRegistration image_output_registration(core::ToolDescriptor descriptor);
namespace image_output
{
[[nodiscard]] const core::DataValue& parameter(const core::InvocationContextData& call, std::string_view name);
[[nodiscard]] const core::DataValue* input(const core::InvocationContextData& call, std::string_view name);
[[nodiscard]] core::Result<core::ImageFrame> render(const core::InvocationContextData& call,
                                                    const core::ImageFrame& source);
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode(const core::ImageHandle& image, std::string_view extension,
                                                             int jpeg_quality);
} // namespace image_output
} // namespace visionpro::tools::vision
