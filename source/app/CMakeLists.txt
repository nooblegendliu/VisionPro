add_library(visionpro_app_tools STATIC tool_catalog.cpp tool_catalog.hpp)
target_include_directories(visionpro_app_tools PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(visionpro_app_tools
    PUBLIC visionpro_core
    PRIVATE visionpro_simulation_tools visionpro_build_options
)
if(VISIONPRO_WITH_OPENCV)
    target_compile_definitions(visionpro_app_tools PRIVATE VISIONPRO_WITH_OPENCV)
    target_link_libraries(visionpro_app_tools PRIVATE visionpro_vision_tools)
endif()

if(VISIONPRO_WITH_GUI AND VISIONPRO_WITH_OPENCV)
    add_library(visionpro_visual_preview STATIC visual_preview.cpp visual_preview.hpp)
    target_include_directories(visionpro_visual_preview PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
    target_link_libraries(visionpro_visual_preview
        PUBLIC visionpro_infrastructure
        PRIVATE visionpro_vision_tools visionpro_build_options
    )
endif()

add_executable(visionpro_cli main.cpp run_summary.cpp)
target_include_directories(visionpro_cli SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include")
target_compile_definitions(visionpro_cli PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(visionpro_cli PRIVATE visionpro_app_tools visionpro_infrastructure visionpro_build_options)
if(VISIONPRO_WITH_OPENCV)
    target_compile_definitions(visionpro_cli PRIVATE VISIONPRO_WITH_OPENCV)
endif()
if(MINGW)
    target_link_options(visionpro_cli PRIVATE -municode)
endif()
