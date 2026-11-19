#pragma once

#include <visionpro/core/result.hpp>

#include <string>
#include <vector>

namespace visionpro::infrastructure {

struct MvsCameraInfo {
    std::string serial;
    std::string model;
    std::string firmware;
    std::string transport;
};
struct MvsCameraInventory {
    std::string sdk_version;
    std::vector<MvsCameraInfo> devices;
};

// Enumerates the supported transports without opening or reconfiguring a camera.
[[nodiscard]] core::Result<MvsCameraInventory> query_mvs_cameras();

} // namespace visionpro::infrastructure
