#pragma once

#include <visionpro/core/tool_registry.hpp>

namespace visionpro::tools::vision {

enum class FileImageDecodeMode { Gray8, Native8 };

[[nodiscard]] core::Result<core::ImageHandle> decode_file_image(std::span<const std::byte> bytes,
    FileImageDecodeMode mode, const core::CoordinateSystemId& coordinates);
[[nodiscard]] core::ToolRegistration read_file_registration(core::ToolDescriptor descriptor);

} // namespace visionpro::tools::vision
