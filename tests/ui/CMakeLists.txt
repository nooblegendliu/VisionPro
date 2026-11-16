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
    visionpro_ui visionpro_simulation_tools visionpro_build_options Qt6::Test
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
    list(APPEND ui_tests visionpro_visual_tests)
endif()

set_tests_properties(${ui_tests} PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    TIMEOUT 60
)
if(VISIONPRO_WITH_OPENCV)
    set_property(TEST visionpro_editor_tests visionpro_editor_run_tests visionpro_run_lifecycle_tests visionpro_visual_tests
        APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>")
endif()
