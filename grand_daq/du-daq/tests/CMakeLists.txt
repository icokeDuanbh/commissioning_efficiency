
add_executable(test_elec_scope_a test_elec_scope_a.cpp)

target_link_libraries(test_elec_scope_a dudaq grandcommon)

module_test(TARGET test_data_manager
        SRCS test_data_manager.cpp
        LINKS grandcommon dudaq)

module_test(TARGET test_scope_dummy
        SRCS test_scope_dummy.cpp
        LINKS grandcommon dudaq)

install(TARGETS test_elec_scope_a RUNTIME DESTINATION bin)
