#include "tool_catalog.hpp"

#include "simulation.hpp"
#include "list_item.hpp"
#ifdef VISIONPRO_WITH_OPENCV
#include "vision/vision_tools.hpp"
#endif

#include <utility>

namespace visionpro::app {

core::Result<core::ToolRegistry> make_tool_registry()
{
    auto registrations = tools::simulation_registrations();
    registrations.push_back(tools::list_item_registration("visionpro.list.int64_at", "Integer list item",
        core::DataType::integer()));
#ifdef VISIONPRO_WITH_OPENCV
    registrations.push_back(tools::list_item_registration("visionpro.list.blob_at", "Blob component by index",
        tools::vision::blob_component_type()));
    for (auto& registration : tools::vision::registrations()) {
        registrations.push_back(std::move(registration));
    }
#endif
    return core::ToolRegistry::create(registrations);
}

} // namespace visionpro::app
