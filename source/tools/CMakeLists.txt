add_library(visionpro_camera_tools STATIC camera_tools.cpp camera_tools.hpp)
target_include_directories(visionpro_camera_tools PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(visionpro_camera_tools PUBLIC visionpro_core PRIVATE visionpro_infrastructure visionpro_build_options Threads::Threads)

add_library(visionpro_shared_tools STATIC shared_tools.cpp shared_tools.hpp)
target_include_directories(visionpro_shared_tools PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(visionpro_shared_tools PUBLIC visionpro_core PRIVATE visionpro_build_options Threads::Threads)

add_library(visionpro_list_tools STATIC list_item.cpp list_item.hpp)
target_include_directories(visionpro_list_tools PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(visionpro_list_tools PUBLIC visionpro_core PRIVATE visionpro_build_options)

add_library(visionpro_logic_tools STATIC logic_tools.cpp logic_tools.hpp vision/integer_range.cpp vision/integer_range.hpp)
target_include_directories(visionpro_logic_tools PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(visionpro_logic_tools PUBLIC visionpro_core PRIVATE visionpro_build_options)

add_library(visionpro_simulation_tools STATIC simulation.cpp simulation.hpp)
target_include_directories(visionpro_simulation_tools PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(visionpro_simulation_tools
    PUBLIC visionpro_core
    PRIVATE visionpro_infrastructure visionpro_build_options
)

if(VISIONPRO_WITH_OPENCV)
    add_library(visionpro_vision_tools STATIC
        vision/vision_tools.cpp vision/vision_tools.hpp
        vision/calibration.cpp vision/calibration.hpp vision/calibration_tools.cpp
        vision/inspection.cpp vision/inspection.hpp vision/class_map_tool.cpp vision/class_map_tool.hpp
        vision/opencv_image.cpp vision/opencv_image.hpp
        vision/read_file.cpp vision/read_file.hpp
        vision/roi.cpp vision/roi.hpp
        vision/roi_mask.cpp vision/roi_mask.hpp
        vision/threshold.cpp vision/threshold.hpp
        vision/blob.cpp vision/blob.hpp
    )
    target_include_directories(visionpro_vision_tools PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
    target_include_directories(visionpro_vision_tools SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include")
    target_link_libraries(visionpro_vision_tools
        PUBLIC visionpro_core
        PRIVATE visionpro_logic_tools opencv_core opencv_imgproc opencv_imgcodecs visionpro_infrastructure visionpro_build_options
    )
endif()
