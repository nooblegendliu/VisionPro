if(NOT WIN32)
    message(FATAL_ERROR "The UUID entropy backend currently requires Windows. Add a platform backend before building on other systems.")
endif()

add_library(visionpro_core STATIC
    id.cpp
    id_factory.cpp
    diagnostic.cpp
    schema_id.cpp
    runtime_id.cpp
    text.cpp
    value.cpp
    tool_descriptor.cpp
    tool_execution.cpp
    image.cpp
    geometry.cpp
    resource.cpp
    state.cpp
)
add_library(visionpro::core ALIAS visionpro_core)

target_include_directories(visionpro_core PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_compile_features(visionpro_core PUBLIC cxx_std_20)
target_compile_definitions(visionpro_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(visionpro_core PRIVATE visionpro_build_options bcrypt)
