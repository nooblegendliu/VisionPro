#include "tool_catalog.hpp"

#include "simulation.hpp"
#include "camera_tools.hpp"
#include "list_item.hpp"
#include "logic_tools.hpp"
#include "shared_tools.hpp"
#include "vision/integer_range.hpp"
#ifdef VISIONPRO_WITH_OPENCV
#include "vision/vision_tools.hpp"
#endif

#include <utility>

namespace visionpro::app {

core::Result<core::ToolRegistry> make_tool_registry()
{
    auto registrations = tools::simulation_registrations();
    for (auto& registration : tools::camera_registrations()) { registrations.push_back(std::move(registration)); }
    for (auto& registration : tools::logic_registrations()) { registrations.push_back(std::move(registration)); }
    for (auto& registration : tools::shared_registrations()) { registrations.push_back(std::move(registration)); }
    registrations.push_back(tools::list_item_registration("visionpro.list.int64_at", "Integer list item",
        core::DataType::integer()));
#ifdef VISIONPRO_WITH_OPENCV
    registrations.push_back(tools::list_item_registration("visionpro.list.blob_at", "Blob component by index",
        tools::vision::blob_component_type()));
    for (auto& registration : tools::vision::registrations()) {
        registrations.push_back(std::move(registration));
    }
#else
    registrations.push_back(tools::vision::integer_range_registration(tools::vision::integer_range_descriptor()));
    registrations.push_back(tools::vision::float_range_registration(tools::vision::float_range_descriptor()));
#endif
    return core::ToolRegistry::create(registrations);
}

} // namespace visionpro::app
