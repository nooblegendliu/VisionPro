add_executable(visionpro_cli_tests cli_tests.cpp)
target_compile_definitions(visionpro_cli_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    VISIONPRO_CALCULATION_SAMPLE="${PROJECT_SOURCE_DIR}/samples/s6-calculation/solution.json"
    VISIONPRO_SHARED_SAMPLE="${PROJECT_SOURCE_DIR}/samples/s6-shared/solution.json"
    VISIONPRO_TRIGGER_SAMPLE="${PROJECT_SOURCE_DIR}/samples/s6-trigger/solution.json"
    VISIONPRO_RECOVERY_SAMPLE="${PROJECT_SOURCE_DIR}/samples/s6-recovery/solution.json")
target_include_directories(visionpro_cli_tests SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include")
target_link_libraries(visionpro_cli_tests PRIVATE visionpro_infrastructure visionpro_build_options)
if(MINGW)
    target_link_options(visionpro_cli_tests PRIVATE -municode)
endif()
add_dependencies(visionpro_cli_tests visionpro_cli)

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
set_tests_properties(app.cli_roundtrip app.cli_diagnostics app.cli_external_roundtrip app.cli_external_diagnostics app.cli_output_failures app.cli_calculations
    PROPERTIES TIMEOUT 60)

if(VISIONPRO_WITH_OPENCV)
    target_compile_definitions(visionpro_cli_tests PRIVATE VISIONPRO_WITH_OPENCV)
    target_link_libraries(visionpro_cli_tests PRIVATE visionpro_vision_tools visionpro_simulation_tools)
    add_test(NAME app.vision_roundtrip COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> vision-roundtrip)
    add_test(NAME app.vision_diagnostics COMMAND visionpro_cli_tests $<TARGET_FILE:visionpro_cli> vision-diagnostics)
    set_tests_properties(app.vision_roundtrip app.vision_diagnostics PROPERTIES TIMEOUT 60)

    add_executable(visionpro_image_summary_tests image_summary_tests.cpp ../../source/app/run_summary.cpp)
    target_include_directories(visionpro_image_summary_tests PRIVATE "${PROJECT_SOURCE_DIR}/source/app")
    target_include_directories(visionpro_image_summary_tests SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include")
    target_link_libraries(visionpro_image_summary_tests PRIVATE visionpro_vision_tools visionpro_infrastructure visionpro_build_options)
    add_test(NAME app.vision_image_summary COMMAND visionpro_image_summary_tests)
    set_tests_properties(app.vision_image_summary PROPERTIES TIMEOUT 30)
    set_tests_properties(app.cli_roundtrip app.cli_diagnostics app.cli_external_roundtrip app.cli_external_diagnostics app.cli_output_failures app.cli_calculations
        app.vision_image_summary app.structured_loops app.shared_package app.cli_shared_workflows app.cli_trigger_replay app.cli_error_recovery
        app.vision_roundtrip app.vision_diagnostics PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>"
    )
endif()
