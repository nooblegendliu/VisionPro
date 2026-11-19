add_library(visionpro_infrastructure STATIC
    json_codec.cpp
    geometry_codec.cpp
    value_codec.cpp
    resource_codec.cpp
    solution_codec.cpp
    file_resources.cpp
    camera.cpp
    communication.cpp
    camera_trigger.cpp
    solution_file.cpp
    solution_storage.cpp
    solution_save.cpp
    solution_transfer.cpp
    subgraph_package.cpp
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
target_link_libraries(visionpro_infrastructure PUBLIC visionpro_workflow PRIVATE visionpro_build_options bcrypt ws2_32)

option(VISIONPRO_WITH_MVS "Enable the Hikrobot MVS GigE/USB camera backend" OFF)
target_sources(visionpro_infrastructure PRIVATE mvs_camera.cpp)
if(VISIONPRO_WITH_MVS)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The MVS backend requires Windows x64.")
    endif()
    find_path(VISIONPRO_MVS_INCLUDE_DIR MvCameraControl.h HINTS ENV MVCAM_COMMON_RUNENV PATH_SUFFIXES Includes REQUIRED)
    find_library(VISIONPRO_MVS_LIBRARY NAMES MvCameraControl HINTS ENV MVCAM_COMMON_RUNENV PATH_SUFFIXES Libraries/win64 REQUIRED)
    target_compile_definitions(visionpro_infrastructure PRIVATE VISIONPRO_WITH_MVS=1)
    target_include_directories(visionpro_infrastructure SYSTEM PRIVATE "${VISIONPRO_MVS_INCLUDE_DIR}")
    target_link_libraries(visionpro_infrastructure PRIVATE "${VISIONPRO_MVS_LIBRARY}")
endif()
