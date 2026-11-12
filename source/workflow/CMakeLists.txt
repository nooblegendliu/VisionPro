add_library(visionpro_workflow STATIC
    binding.cpp
    model.cpp
    snapshot.cpp
    copy.cpp
    run.cpp
    run_handle.cpp
    tool_execution.cpp
    execution_plan.cpp
    control_schedule.cpp
    node_state_service.cpp
    workflow_engine.cpp
    validation.cpp
    validation_values.cpp
    runtime_values.cpp
    result_store.cpp
    resolver.cpp
)
add_library(visionpro::workflow ALIAS visionpro_workflow)

target_include_directories(visionpro_workflow PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_compile_features(visionpro_workflow PUBLIC cxx_std_20)
target_link_libraries(visionpro_workflow PUBLIC visionpro_core PRIVATE visionpro_build_options Threads::Threads)
