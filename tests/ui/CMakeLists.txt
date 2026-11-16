add_executable(visionpro_canvas_tests canvas_tests.cpp)
set_target_properties(visionpro_canvas_tests PROPERTIES AUTOMOC ON)
target_link_libraries(visionpro_canvas_tests PRIVATE
    visionpro_ui visionpro_simulation_tools visionpro_build_options Qt6::Test
)

add_test(NAME visionpro_canvas_tests COMMAND visionpro_canvas_tests -platform offscreen)
set_tests_properties(visionpro_canvas_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)
