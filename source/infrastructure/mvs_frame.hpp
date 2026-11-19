#pragma once

#include <visionpro/infrastructure/camera.hpp>
#include <MvCameraControl.h>

namespace visionpro::infrastructure::mvs_detail {

[[nodiscard]] core::Diagnostic sdk_error(core::ErrorCode fallback, std::string operation, int status);
[[nodiscard]] core::Result<CameraFrame> copy_frame(const CameraCaptureRequest& request, const MV_FRAME_OUT& frame,
    std::optional<std::uint64_t> tick_frequency);

} // namespace visionpro::infrastructure::mvs_detail
