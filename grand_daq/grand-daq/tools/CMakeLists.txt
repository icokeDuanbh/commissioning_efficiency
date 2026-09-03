add_executable(entry_gate_dump_new_conf entry_gate/dump_new_conf_payloads.cpp)
target_include_directories(entry_gate_dump_new_conf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_link_libraries(entry_gate_dump_new_conf PRIVATE daq_core yaml-cpp nlohmann_json::nlohmann_json)

add_executable(entry_gate_replay_new_t2 entry_gate/replay_new_t2.cpp)
target_link_libraries(entry_gate_replay_new_t2 PRIVATE daq_core nlohmann_json::nlohmann_json)
