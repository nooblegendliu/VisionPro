add_executable(visionpro_core_tests core_tests.cpp)
target_link_libraries(visionpro_core_tests PRIVATE visionpro_core visionpro_build_options)

foreach(test_group IN ITEMS parse_valid parse_invalid generate value_semantics diagnostics)
    add_test(NAME core.${test_group} COMMAND visionpro_core_tests ${test_group})
endforeach()

# The same check path must report failure even when NDEBUG is defined.
add_test(NAME core.failure_exit COMMAND visionpro_core_tests --force-failure)
set_tests_properties(core.failure_exit PROPERTIES WILL_FAIL TRUE)

add_executable(visionpro_identity_tests identity_tests.cpp)
target_link_libraries(visionpro_identity_tests PRIVATE visionpro_core visionpro_build_options)
add_test(NAME core.identities COMMAND visionpro_identity_tests)

add_executable(visionpro_value_tests value_tests.cpp)
target_link_libraries(visionpro_value_tests PRIVATE visionpro_core visionpro_build_options)
foreach(test_group IN ITEMS values schema utf8)
    add_test(NAME core.${test_group} COMMAND visionpro_value_tests ${test_group})
endforeach()

add_executable(visionpro_metadata_tests metadata_tests.cpp)
target_link_libraries(visionpro_metadata_tests PRIVATE visionpro_core visionpro_build_options)
foreach(test_group IN ITEMS metadata constraints conversions compatibility)
    add_test(NAME core.${test_group} COMMAND visionpro_metadata_tests ${test_group})
endforeach()

add_executable(visionpro_image_tests image_tests.cpp)
target_link_libraries(visionpro_image_tests PRIVATE visionpro_core visionpro_build_options)
foreach(test_group IN ITEMS image_layout image_ownership)
    add_test(NAME core.${test_group} COMMAND visionpro_image_tests ${test_group})
endforeach()

add_executable(visionpro_geometry_tests geometry_tests.cpp)
target_link_libraries(visionpro_geometry_tests PRIVATE visionpro_core visionpro_build_options)
foreach(test_group IN ITEMS geometry regions masks)
    add_test(NAME core.${test_group} COMMAND visionpro_geometry_tests ${test_group})
endforeach()

add_executable(visionpro_resource_state_tests resource_state_tests.cpp)
target_link_libraries(visionpro_resource_state_tests PRIVATE visionpro_core visionpro_build_options)
foreach(test_group IN ITEMS resources shared_definitions state_models)
    add_test(NAME core.${test_group} COMMAND visionpro_resource_state_tests ${test_group})
endforeach()

add_executable(visionpro_domain_value_tests domain_value_tests.cpp)
target_link_libraries(visionpro_domain_value_tests PRIVATE visionpro_core visionpro_build_options)
foreach(test_group IN ITEMS domain_values domain_metadata)
    add_test(NAME core.${test_group} COMMAND visionpro_domain_value_tests ${test_group})
endforeach()

add_executable(visionpro_tool_execution_tests tool_execution_tests.cpp)
target_link_libraries(visionpro_tool_execution_tests PRIVATE visionpro_core visionpro_build_options)
add_test(NAME core.tool_execution COMMAND visionpro_tool_execution_tests)
