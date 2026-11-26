add_executable(visionpro_cli_tests cli_tests.cpp)
target_compile_definitions(visionpro_cli_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    VISIONPRO_CALCULATION_SAMPLE="${PROJECT_SOURCE_DIR}/samples/s6-calculation/solution.json"
    VISIONPRO_SOLUTION_GRAPH_SAMPLE="${PROJECT_SOURCE_DIR}/samples/f3-solution-graph/solution.json"
    VISIONPRO_CROSS_DIRECTORY="${PROJECT_SOURCE_DIR}/samples/s6-cross-workflow"
    VISIONPRO_SHARED_SAMPLE="${PROJECT_SOURCE_DIR}/samples/s6-shared/solution.json"
    VISIONPRO_TRIGGER_SAMPLE="${PROJECT_SOURCE_DIR}/samples/s6-trigger/solution.json"
    VISIONPRO_RECOVERY_SAMPLE="${PROJECT_SOURCE_DIR}/samples/s6-recovery/solution.json")
target_include_directories(visionpro_cli_tests SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include")
target_link_libraries(visionpro_cli_tests PRIVATE visionpro_app_tools visionpro_infrastructure visionpro_build_options)
if(MINGW)
    target_link_options(visionpro_cli_tests PRIVATE -municode)
endif()
add_dependencies(visionpro_cli_tests visionpro_cli)
target_compile_definitions(visionpro_cli_tests PRIVATE VISIONPRO_CAMERA_DIRECTORY="${PROJECT_SOURCE_DIR}/samples/s6-camera")
add_test(NAME app.cli_camera COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> camera)
add_executable(visionpro_camera_stream_tests camera_stream_tests.cpp)
target_compile_definitions(visionpro_camera_stream_tests PRIVATE VISIONPRO_CAMERA_DIRECTORY="${PROJECT_SOURCE_DIR}/samples/s6-camera")
target_link_libraries(visionpro_camera_stream_tests PRIVATE visionpro_app_tools visionpro_infrastructure visionpro_build_options)
add_test(NAME app.camera_stream_lifecycle COMMAND visionpro_camera_stream_tests)
set_tests_properties(app.cli_camera app.camera_stream_lifecycle PROPERTIES TIMEOUT 45)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    if(VISIONPRO_WITH_OPENCV)
        foreach(mode IN ITEMS local package ftp)
            add_test(NAME app.image_output_${mode} COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/image_output_cli_tests.py" ${mode}
                $<TARGET_FILE:visionpro_cli> "${PROJECT_SOURCE_DIR}/samples/s4")
            set_tests_properties(app.image_output_${mode} PROPERTIES TIMEOUT 100
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>")
        endforeach()
    endif()
    foreach(protocol IN ITEMS tcp plc)
        add_test(NAME app.cli_${protocol}_communication COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/communication_cli_tests.py" ${protocol}
            $<TARGET_FILE:visionpro_cli> "${PROJECT_SOURCE_DIR}/samples/s6-communication/solution.json")
        set_tests_properties(app.cli_${protocol}_communication PROPERTIES TIMEOUT 60)
        if(VISIONPRO_WITH_OPENCV)
            set_property(TEST app.cli_${protocol}_communication APPEND PROPERTY
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>")
        endif()
    endforeach()
else()
    message(STATUS "S6.11 loopback CLI tests require a Python 3 interpreter")
endif()

add_executable(visionpro_shared_package_tests shared_package_tests.cpp)
target_compile_definitions(visionpro_shared_package_tests PRIVATE VISIONPRO_SHARED_DIRECTORY="${PROJECT_SOURCE_DIR}/samples/s6-shared")
target_link_libraries(visionpro_shared_package_tests PRIVATE visionpro_app_tools visionpro_infrastructure visionpro_build_options)
add_test(NAME app.shared_package COMMAND visionpro_shared_package_tests)
add_test(NAME app.cli_shared_workflows COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> shared)
add_test(NAME app.cli_trigger_replay COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> trigger)
add_test(NAME app.cli_error_recovery COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> recovery)
set_tests_properties(app.cli_trigger_replay app.cli_error_recovery PROPERTIES TIMEOUT 60)
set_tests_properties(app.shared_package app.cli_shared_workflows PROPERTIES TIMEOUT 60)

add_executable(visionpro_structured_loop_tests structured_loop_tests.cpp)
target_link_libraries(visionpro_structured_loop_tests PRIVATE visionpro_app_tools visionpro_workflow visionpro_build_options)
add_test(NAME app.structured_loops COMMAND visionpro_structured_loop_tests)
set_tests_properties(app.structured_loops PROPERTIES TIMEOUT 30)

add_test(NAME app.cli_roundtrip COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> roundtrip)
add_test(NAME app.cli_diagnostics COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> diagnostics)
add_test(NAME app.cli_external_roundtrip COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> external-roundtrip)
add_test(NAME app.cli_external_diagnostics COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> external-diagnostics)
add_test(NAME app.cli_output_failures COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> output-failures)
add_test(NAME app.cli_calculations COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> calculations)
add_test(NAME app.cli_solution_graph COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> solution-graph)
add_test(NAME app.cli_replay COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> replay)
set_tests_properties(app.cli_replay app.cli_solution_graph PROPERTIES TIMEOUT 30)
set_tests_properties(app.cli_roundtrip app.cli_diagnostics app.cli_external_roundtrip app.cli_external_diagnostics app.cli_output_failures app.cli_calculations
    PROPERTIES TIMEOUT 60)

if(VISIONPRO_WITH_OPENCV)
    add_executable(visionpro_image_entry_tests image_entry_tests.cpp)
    target_link_libraries(visionpro_image_entry_tests PRIVATE visionpro_app_tools visionpro_infrastructure visionpro_build_options)
    add_test(NAME app.image_source_entries COMMAND visionpro_image_entry_tests)
    set_tests_properties(app.image_source_entries PROPERTIES TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>")
    target_compile_definitions(visionpro_cli_tests PRIVATE VISIONPRO_WITH_OPENCV
        VISIONPRO_INSPECTION_DIRECTORY="${PROJECT_SOURCE_DIR}/samples/s6-inspection")
    target_link_libraries(visionpro_cli_tests PRIVATE visionpro_vision_tools)
    add_test(NAME app.vision_roundtrip COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> vision-roundtrip)
    add_test(NAME app.vision_diagnostics COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> vision-diagnostics)
    add_test(NAME app.cli_inspection COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> inspection)
    set_tests_properties(app.vision_roundtrip app.vision_diagnostics app.cli_inspection PROPERTIES TIMEOUT 60)

    add_executable(visionpro_image_summary_tests image_summary_tests.cpp ../../source/app/run_summary.cpp)
    target_include_directories(visionpro_image_summary_tests PRIVATE "${PROJECT_SOURCE_DIR}/source/app")
    target_include_directories(visionpro_image_summary_tests SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include")
    target_link_libraries(visionpro_image_summary_tests PRIVATE visionpro_vision_tools visionpro_infrastructure visionpro_build_options)
    add_test(NAME app.vision_image_summary COMMAND visionpro_image_summary_tests)
    set_tests_properties(app.vision_image_summary PROPERTIES TIMEOUT 30)
    set_tests_properties(app.cli_roundtrip app.cli_diagnostics app.cli_external_roundtrip app.cli_external_diagnostics app.cli_output_failures app.cli_calculations
        app.vision_image_summary app.structured_loops app.shared_package app.cli_shared_workflows app.cli_trigger_replay app.cli_error_recovery app.cli_replay app.cli_solution_graph
        app.vision_roundtrip app.vision_diagnostics app.cli_inspection app.cli_camera app.camera_stream_lifecycle PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>"
    )
endif()
