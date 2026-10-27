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
# 4. 将现有项目文档加入工程文件视图
# ------------------------------------------------------------------
set(VISIONPRO_PROJECT_DOCUMENTS
    "${PROJECT_SOURCE_DIR}/README.md"
    "${PROJECT_SOURCE_DIR}/AGENTS.md"

    "${PROJECT_SOURCE_DIR}/docs/requirements.md"
    "${PROJECT_SOURCE_DIR}/docs/architecture.md"
    "${PROJECT_SOURCE_DIR}/docs/data-model.md"
    "${PROJECT_SOURCE_DIR}/docs/development.md"
)

add_custom_target(
    visionpro_project_files
    SOURCES
        ${VISIONPRO_PROJECT_DOCUMENTS}
)

# ------------------------------------------------------------------
# 5. 当前阶段状态显示
# ------------------------------------------------------------------
message(STATUS "========== VisionPro Configuration ==========")
message(STATUS "Project version : ${PROJECT_VERSION}")
message(STATUS "C++ standard    : C++${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler        : ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "Compiler version: ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "Source directory: ${PROJECT_SOURCE_DIR}")
message(STATUS "Build directory : ${PROJECT_BINARY_DIR}")
message(STATUS "Qt              : Not configured")
message(STATUS "OpenCV          : Not configured")
message(STATUS "Business code   : Not configured")
message(STATUS "=============================================")