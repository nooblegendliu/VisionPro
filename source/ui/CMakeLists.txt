add_library(visionpro_ui STATIC
    background_result.hpp camera_editor.cpp camera_editor.hpp
    binary_editor.cpp binary_editor.hpp endpoint_editor.cpp endpoint_editor.hpp
    class_palette.cpp class_palette.hpp
    control_graph_model.cpp control_graph_model.hpp
    control_scene.cpp control_scene.hpp
    editor_window.cpp editor_window.hpp editor_documents.cpp editor_runs.cpp
    workspace_toolbox.cpp workspace_toolbox.hpp
    workspace_style.cpp workspace_style.hpp
    workspace_appearance.cpp workspace_appearance.hpp
    workspace_icons.cpp workspace_icons.hpp
    toolbox_catalog.cpp toolbox_catalog.hpp
    editor_inspector.cpp editor_inspector.hpp
    image_panel.cpp image_panel.hpp
    image_view.cpp image_view.hpp
    module_history_panel.cpp module_history_panel.hpp
    roi_editor.cpp roi_editor.hpp
    run_controller.cpp run_controller.hpp run_triggers.cpp
    solution_run_actions.cpp solution_run_actions.hpp
    run_image.cpp run_image.hpp
    run_inputs.cpp run_inputs.hpp
    run_panel.cpp run_panel.hpp
)
set_target_properties(visionpro_ui PROPERTIES AUTOMOC ON)
target_include_directories(visionpro_ui PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(visionpro_ui
    PUBLIC visionpro_workflow Qt6::Widgets QtNodes::QtNodes
    PRIVATE visionpro_build_options visionpro_infrastructure
)
if(VISIONPRO_WITH_OPENCV)
    target_compile_definitions(visionpro_ui PRIVATE VISIONPRO_WITH_OPENCV)
    target_link_libraries(visionpro_ui PRIVATE visionpro_visual_preview)
endif()

qt_add_executable(visionpro_gui WIN32
    main.cpp
    canvas_example.cpp canvas_example.hpp
)
target_link_libraries(visionpro_gui PRIVATE visionpro_ui visionpro_app_tools visionpro_build_options)

include(GNUInstallDirs)
install(TARGETS visionpro_gui RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT VisionProGui)
if(VISIONPRO_WITH_OPENCV)
    install(IMPORTED_RUNTIME_ARTIFACTS opencv_core opencv_imgproc opencv_imgcodecs
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT VisionProGui)
endif()
qt_generate_deploy_app_script(
    TARGET visionpro_gui
    OUTPUT_SCRIPT visionpro_gui_deploy_script
    NO_TRANSLATIONS
    NO_COMPILER_RUNTIME
)
install(SCRIPT "${visionpro_gui_deploy_script}" COMPONENT VisionProGui)
