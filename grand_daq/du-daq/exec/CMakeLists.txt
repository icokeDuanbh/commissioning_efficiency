
add_executable(dudaq_run dudaq_run.cpp)

target_link_libraries(dudaq_run dudaq grandcommon)

install(TARGETS dudaq_run RUNTIME DESTINATION bin)
