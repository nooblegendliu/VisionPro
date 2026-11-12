add_library(visionpro_simulation_tools STATIC simulation.cpp simulation.hpp)
target_include_directories(visionpro_simulation_tools PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(visionpro_simulation_tools
    PUBLIC visionpro_core
    PRIVATE visionpro_infrastructure visionpro_build_options
)
