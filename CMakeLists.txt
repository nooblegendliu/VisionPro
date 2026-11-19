cmake_minimum_required(VERSION 3.25)

# ------------------------------------------------------------------
# VisionPro
# Windows x64 / MSVC / C++20 / Cmake
# 
# - 子依赖项：QT / OpenCV
# - 第三方依赖项：
# ------------------------------------------------------------------
project(
    VisionPro
    VERSION 0.1.0
    DESCRIPTION "Visual workflow-based machine vision software"
    LANGUAGES CXX
)


# ------------------------------------------------------------------
# 1. 全局C++标准
# ------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VISIONPRO_WITH_OPENCV "Build the OpenCV vision tools (Windows x64 Release)" OFF)
option(VISIONPRO_WITH_GUI "Build the Qt Widgets desktop application (Windows x64 Release)" OFF)

# Keep the core and tests on the same MSVC runtime.
if(VISIONPRO_WITH_OPENCV OR VISIONPRO_WITH_GUI)
    if(NOT WIN32 OR NOT MSVC OR NOT MSVC_CXX_ARCHITECTURE_ID STREQUAL "x64")
        message(FATAL_ERROR "OpenCV and GUI builds require Windows x64 and MSVC.")
    endif()
    get_property(visionpro_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(visionpro_multi_config)
        set(visionpro_config "${CMAKE_CONFIGURATION_TYPES}")
    else()
        set(visionpro_config "${CMAKE_BUILD_TYPE}")
    endif()
    string(TOUPPER "${visionpro_config}" visionpro_config)
    if(NOT visionpro_config STREQUAL "RELEASE")
        message(FATAL_ERROR "OpenCV and GUI builds require a Release-only build tree. Set CMAKE_CONFIGURATION_TYPES=Release (multi-config) or CMAKE_BUILD_TYPE=Release (single-config).")
    endif()
    if(DEFINED CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreadedDLL")
        message(FATAL_ERROR "OpenCV and GUI builds require /MD. Set CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL.")
    endif()
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
elseif(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

if(VISIONPRO_WITH_OPENCV)
    find_package(OpenCV 4.14.0 EXACT CONFIG REQUIRED COMPONENTS core imgproc imgcodecs)
    if(NOT OpenCV_VERSION_STATUS STREQUAL "-pre")
        message(FATAL_ERROR "VISIONPRO_WITH_OPENCV requires the verified OpenCV 4.14.0-pre package. Set OpenCV_DIR to its OpenCVConfig.cmake directory.")
    endif()
    foreach(component IN ITEMS opencv_core opencv_imgproc opencv_imgcodecs)
        get_target_property(import_library ${component} IMPORTED_IMPLIB_RELEASE)
        get_target_property(runtime_library ${component} IMPORTED_LOCATION_RELEASE)
        if(NOT EXISTS "${import_library}" OR NOT EXISTS "${runtime_library}")
            message(FATAL_ERROR "The OpenCV package must export a Release import library and DLL for ${component}.")
        endif()
    endforeach()
endif()

# ------------------------------------------------------------------
# 2. 工程目录
# ------------------------------------------------------------------
set(VISIONPRO_ROOT_DIR "${PROJECT_SOURCE_DIR}")
set(VISIONPRO_SOURCE_DIR "${PROJECT_SOURCE_DIR}/source")
set(VISIONPRO_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include")
set(VISIONPRO_TESTS_DIR "${PROJECT_SOURCE_DIR}/tests")
set(VISIONPRO_PLUGINS_DIR "${PROJECT_SOURCE_DIR}/plugins")
set(VISIONPRO_THIRD_PARTY_DIR "${PROJECT_SOURCE_DIR}/third_party")

# ------------------------------------------------------------------
# 3. 统一编译标准
# ------------------------------------------------------------------
add_library(visionpro_build_options INTERFACE)

target_compile_features(
    visionpro_build_options
    INTERFACE 
        cxx_std_20
)

# MSVC编译选项
if (MSVC)
    target_compile_options(
        visionpro_build_options 
        INTERFACE 
            "$<$<COMPILE_LANGUAGE:CXX>:/W4>"
            "$<$<COMPILE_LANGUAGE:CXX>:/permissive->"
            "$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>"
            "$<$<COMPILE_LANGUAGE:CXX>:/utf-8>"
    )
endif()

# GCC / Clang 编译选项
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
        visionpro_build_options
        INTERFACE
            "$<$<COMPILE_LANGUAGE:CXX>:-Wall>"
            "$<$<COMPILE_LANGUAGE:CXX>:-Wextra>"
            "$<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>"
    )
endif()

# ------------------------------------------------------------------
# 4. 核心库、应用与测试
# ------------------------------------------------------------------
find_package(Threads REQUIRED)
add_subdirectory(source/core)
add_subdirectory(source/workflow)
add_subdirectory(source/infrastructure)
add_subdirectory(source/tools)
add_subdirectory(source/app)

include(CTest)
if(VISIONPRO_WITH_GUI)
    find_package(Qt6 6.11.1 EXACT REQUIRED CONFIG COMPONENTS Widgets OpenGL)
    if(BUILD_TESTING)
        find_package(Qt6 6.11.1 EXACT REQUIRED CONFIG COMPONENTS Test)
    endif()
    include(cmake/QtNodes.cmake)
    add_subdirectory(source/ui)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests/core)
    add_subdirectory(tests/workflow)
    add_subdirectory(tests/infrastructure)
    add_subdirectory(tests/app)
    add_subdirectory(tests/tools)
    if(VISIONPRO_WITH_GUI)
        add_subdirectory(tests/ui)
    endif()
endif()

# ------------------------------------------------------------------
# 5. 将现有项目文档加入工程文件视图
# ------------------------------------------------------------------
set(VISIONPRO_PROJECT_DOCUMENTS
    "${PROJECT_SOURCE_DIR}/README.md"
    "${PROJECT_SOURCE_DIR}/AGENTS.md"

    "${PROJECT_SOURCE_DIR}/docs/requirements.md"
    "${PROJECT_SOURCE_DIR}/docs/architecture.md"
    "${PROJECT_SOURCE_DIR}/docs/data-model.md"
    "${PROJECT_SOURCE_DIR}/docs/development.md"
    "${PROJECT_SOURCE_DIR}/docs/s3-acceptance.md"
)

add_custom_target(
    visionpro_project_files
    SOURCES
        ${VISIONPRO_PROJECT_DOCUMENTS}
)

# ------------------------------------------------------------------
# 6. 当前阶段状态显示
# ------------------------------------------------------------------
message(STATUS "========== VisionPro Configuration ==========")
message(STATUS "Project version : ${PROJECT_VERSION}")
message(STATUS "C++ standard    : C++${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler        : ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "Compiler version: ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "Source directory: ${PROJECT_SOURCE_DIR}")
message(STATUS "Build directory : ${PROJECT_BINARY_DIR}")
if(VISIONPRO_WITH_GUI)
    message(STATUS "Qt              : ${Qt6_VERSION} (Widgets)")
    message(STATUS "QtNodes         : 3.0.16")
    message(STATUS "Desktop GUI     : visionpro_gui")
else()
    message(STATUS "Qt              : OFF")
endif()
if(VISIONPRO_WITH_OPENCV)
    message(STATUS "OpenCV          : ${OpenCV_VERSION}${OpenCV_VERSION_STATUS} (Release)")
else()
    message(STATUS "OpenCV          : OFF")
endif()
message(STATUS "Core library    : visionpro_core")
message(STATUS "Workflow library: visionpro_workflow")
message(STATUS "Infrastructure  : visionpro_infrastructure")
message(STATUS "Hikrobot MVS    : ${VISIONPRO_WITH_MVS}")
message(STATUS "Headless CLI    : visionpro_cli")
message(STATUS "Tests           : ${BUILD_TESTING}")
message(STATUS "=============================================")
