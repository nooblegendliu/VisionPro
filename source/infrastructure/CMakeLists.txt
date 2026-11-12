add_library(visionpro_infrastructure STATIC
    json_codec.cpp
    geometry_codec.cpp
    value_codec.cpp
    resource_codec.cpp
    solution_codec.cpp
    file_resources.cpp
    solution_file.cpp
    solution_storage.cpp
    solution_save.cpp
)
add_library(visionpro::infrastructure ALIAS visionpro_infrastructure)

target_include_directories(visionpro_infrastructure PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_include_directories(visionpro_infrastructure SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include"
)
target_compile_features(visionpro_infrastructure PUBLIC cxx_std_20)
target_compile_definitions(visionpro_infrastructure PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(visionpro_infrastructure PUBLIC visionpro_workflow PRIVATE visionpro_build_options bcrypt)
