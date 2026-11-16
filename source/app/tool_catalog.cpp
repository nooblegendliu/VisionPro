#include "tool_catalog.hpp"

#include "simulation.hpp"
#ifdef VISIONPRO_WITH_OPENCV
#include "vision/vision_tools.hpp"
#endif

#include <utility>

namespace visionpro::app {

core::Result<core::ToolRegistry> make_tool_registry()
{
    auto registrations = tools::simulation_registrations();
#ifdef VISIONPRO_WITH_OPENCV
    for (auto& registration : tools::vision::registrations()) {
        registrations.push_back(std::move(registration));
    }
#endif
    return core::ToolRegistry::create(registrations);
}

} // namespace visionpro::app
