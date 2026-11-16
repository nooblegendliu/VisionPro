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
set_tests_properties(visionpro_canvas_tests visionpro_editor_tests visionpro_inspector_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    TIMEOUT 60
)
if(VISIONPRO_WITH_OPENCV)
    set_property(TEST visionpro_editor_tests APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>")
endif()
