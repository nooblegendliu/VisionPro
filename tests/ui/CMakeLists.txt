add_executable(visionpro_canvas_tests canvas_tests.cpp)
set_target_properties(visionpro_canvas_tests PROPERTIES AUTOMOC ON)
target_link_libraries(visionpro_canvas_tests PRIVATE
    visionpro_ui visionpro_simulation_tools visionpro_build_options Qt6::Test
)

add_test(NAME visionpro_canvas_tests COMMAND visionpro_canvas_tests -platform offscreen)

add_executable(visionpro_editor_tests editor_tests.cpp "${PROJECT_SOURCE_DIR}/source/ui/canvas_example.cpp")
set_target_properties(visionpro_editor_tests PROPERTIES AUTOMOC ON)
target_link_libraries(visionpro_editor_tests PRIVATE
    visionpro_ui visionpro_simulation_tools visionpro_infrastructure visionpro_build_options Qt6::Test
)
add_dependencies(visionpro_editor_tests visionpro_cli)
target_compile_definitions(visionpro_editor_tests PRIVATE "VISIONPRO_TEST_CLI=\"$<TARGET_FILE:visionpro_cli>\"")
add_test(NAME visionpro_editor_tests COMMAND visionpro_editor_tests -platform offscreen)

add_executable(visionpro_inspector_tests inspector_tests.cpp "${PROJECT_SOURCE_DIR}/source/ui/canvas_example.cpp")
set_target_properties(visionpro_inspector_tests PROPERTIES AUTOMOC ON)
target_link_libraries(visionpro_inspector_tests PRIVATE
    visionpro_ui visionpro_simulation_tools visionpro_app_tools visionpro_infrastructure visionpro_build_options Qt6::Test
)
add_test(NAME visionpro_inspector_tests COMMAND visionpro_inspector_tests -platform offscreen)

add_executable(visionpro_image_view_tests image_view_tests.cpp)
set_target_properties(visionpro_image_view_tests PROPERTIES AUTOMOC ON)
target_link_libraries(visionpro_image_view_tests PRIVATE visionpro_ui visionpro_build_options Qt6::Test)
add_test(NAME visionpro_image_view_tests COMMAND visionpro_image_view_tests -platform offscreen)

add_executable(visionpro_editor_run_tests editor_run_tests.cpp)
set_target_properties(visionpro_editor_run_tests PROPERTIES AUTOMOC ON)
target_link_libraries(visionpro_editor_run_tests PRIVATE
    visionpro_ui visionpro_app_tools visionpro_infrastructure visionpro_build_options Qt6::Test
)
target_compile_definitions(visionpro_editor_run_tests PRIVATE
    "VISIONPRO_S66_DIRECTORY=\"${PROJECT_SOURCE_DIR}/samples/s6-trigger\""
    "VISIONPRO_TEST_CLI=\"$<TARGET_FILE:visionpro_cli>\"")
add_dependencies(visionpro_editor_run_tests visionpro_cli)
add_test(NAME visionpro_editor_run_tests COMMAND visionpro_editor_run_tests -platform offscreen)

add_executable(visionpro_run_lifecycle_tests run_lifecycle_tests.cpp)
set_target_properties(visionpro_run_lifecycle_tests PROPERTIES AUTOMOC ON)
target_link_libraries(visionpro_run_lifecycle_tests PRIVATE
    visionpro_ui visionpro_infrastructure visionpro_build_options Qt6::Test
)
add_test(NAME visionpro_run_lifecycle_tests COMMAND visionpro_run_lifecycle_tests -platform offscreen)

set(ui_tests visionpro_canvas_tests visionpro_editor_tests visionpro_inspector_tests visionpro_image_view_tests
    visionpro_editor_run_tests visionpro_run_lifecycle_tests)
if(VISIONPRO_WITH_OPENCV)
    add_executable(visionpro_visual_tests visual_tests.cpp)
    set_target_properties(visionpro_visual_tests PROPERTIES AUTOMOC ON)
    target_compile_definitions(visionpro_visual_tests PRIVATE "VISIONPRO_S4_DIRECTORY=\"${PROJECT_SOURCE_DIR}/samples/s4\"")
    target_link_libraries(visionpro_visual_tests PRIVATE
        visionpro_ui visionpro_app_tools visionpro_visual_preview visionpro_build_options Qt6::Test
    )
    add_test(NAME visionpro_visual_tests COMMAND visionpro_visual_tests -platform offscreen)

    add_executable(visionpro_acceptance_parity_tests
        acceptance_parity_tests.cpp "${PROJECT_SOURCE_DIR}/source/app/run_summary.cpp")
    set_target_properties(visionpro_acceptance_parity_tests PROPERTIES AUTOMOC ON)
    target_include_directories(visionpro_acceptance_parity_tests PRIVATE "${PROJECT_SOURCE_DIR}/source/app")
    target_include_directories(visionpro_acceptance_parity_tests SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/nlohmann_json/include")
    target_compile_definitions(visionpro_acceptance_parity_tests PRIVATE
        "VISIONPRO_S4_DIRECTORY=\"${PROJECT_SOURCE_DIR}/samples/s4\""
        "VISIONPRO_S61_DIRECTORY=\"${PROJECT_SOURCE_DIR}/samples/s6-loop\""
        "VISIONPRO_S62_DIRECTORY=\"${PROJECT_SOURCE_DIR}/samples/s6-group\""
        "VISIONPRO_S63_DIRECTORY=\"${PROJECT_SOURCE_DIR}/samples/s6-calculation\""
        "VISIONPRO_S64_DIRECTORY=\"${PROJECT_SOURCE_DIR}/samples/s6-shared\""
        "VISIONPRO_S65_DIRECTORY=\"${PROJECT_SOURCE_DIR}/samples/s6-concurrency\""
        "VISIONPRO_TEST_CLI=\"$<TARGET_FILE:visionpro_cli>\"")
    target_link_libraries(visionpro_acceptance_parity_tests PRIVATE
        visionpro_ui visionpro_app_tools visionpro_infrastructure visionpro_build_options Qt6::Test)
    add_dependencies(visionpro_acceptance_parity_tests visionpro_cli)
    add_test(NAME visionpro_acceptance_parity_tests COMMAND visionpro_acceptance_parity_tests -platform offscreen
        compares_saved_s4_workflows -o acceptance-parity.log,txt)
    add_test(NAME visionpro_loop_parity_tests COMMAND visionpro_acceptance_parity_tests -platform offscreen
        compares_saved_blob_loop)
    add_test(NAME visionpro_group_parity_tests COMMAND visionpro_acceptance_parity_tests -platform offscreen
        imports_and_runs_blob_groups -o group-parity.log,txt)
    add_test(NAME visionpro_calculation_parity_tests COMMAND visionpro_acceptance_parity_tests -platform offscreen
        reproduces_saved_calculations)
    add_test(NAME visionpro_shared_parity_tests COMMAND visionpro_acceptance_parity_tests -platform offscreen
        reproduces_saved_shared_workflows)
    add_test(NAME visionpro_concurrency_parity_tests COMMAND visionpro_acceptance_parity_tests -platform offscreen
        reproduces_saved_reentrant_workflows)
    list(APPEND ui_tests visionpro_visual_tests visionpro_acceptance_parity_tests visionpro_loop_parity_tests
        visionpro_group_parity_tests visionpro_calculation_parity_tests visionpro_shared_parity_tests visionpro_concurrency_parity_tests)

    if(WIN32)
        add_executable(visionpro_acceptance_load acceptance_load.cpp)
        target_compile_definitions(visionpro_acceptance_load PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(visionpro_acceptance_load PRIVATE
            visionpro_ui visionpro_app_tools visionpro_infrastructure visionpro_build_options Qt6::Test psapi)
    endif()
endif()

set_tests_properties(${ui_tests} PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    TIMEOUT 60
)
if(VISIONPRO_WITH_OPENCV)
    set_property(TEST visionpro_editor_tests visionpro_inspector_tests visionpro_editor_run_tests visionpro_run_lifecycle_tests visionpro_visual_tests
        visionpro_acceptance_parity_tests visionpro_loop_parity_tests visionpro_group_parity_tests visionpro_calculation_parity_tests visionpro_shared_parity_tests visionpro_concurrency_parity_tests
        APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>")
endif()
