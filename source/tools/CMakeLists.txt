add_library(visionpro_simulation_tools STATIC simulation.cpp simulation.hpp)
target_include_directories(visionpro_simulation_tools PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(visionpro_simulation_tools
    PUBLIC visionpro_core
    PRIVATE visionpro_infrastructure visionpro_build_options
)

if(VISIONPRO_WITH_OPENCV)
    add_library(visionpro_vision_tools STATIC
        vision/vision_tools.cpp vision/vision_tools.hpp
        vision/opencv_image.cpp vision/opencv_image.hpp
        vision/read_file.cpp vision/read_file.hpp
        vision/roi.cpp vision/roi.hpp
        vision/roi_mask.cpp vision/roi_mask.hpp
        vision/threshold.cpp vision/threshold.hpp
    )
    target_include_directories(visionpro_vision_tools PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
    target_link_libraries(visionpro_vision_tools
        PUBLIC visionpro_core
        PRIVATE opencv_core opencv_imgproc opencv_imgcodecs visionpro_infrastructure visionpro_build_options
    )
endif()
