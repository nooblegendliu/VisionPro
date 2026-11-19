#pragma once

#include <visionpro/core/result.hpp>
#include <visionpro/core/runtime_id.hpp>

#include <optional>

namespace visionpro {
namespace workflow { class RunHandle; struct Solution; }
namespace ui {

class ImagePanel;

[[nodiscard]] core::Result<std::monostate> show_run_image(ImagePanel& panel, const workflow::RunHandle& run,
    const core::InvocationKey& invocation, const std::optional<core::OutputId>& output,
    const workflow::Solution* display_config = nullptr);

} // namespace ui
} // namespace visionpro
