cmake_minimum_required(VERSION 3.10)
project(seastar_daq_frontend)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -Wall -Wextra -O2")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(yaml-cpp REQUIRED)

pkg_check_modules(ZMQ REQUIRED libzmq)

include(FetchContent)

# cpp-httplib (header-only)
FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
)
FetchContent_MakeAvailable(cpp_httplib)

set(EXTERNAL_LIBS
    nlohmann_json::nlohmann_json
    Threads::Threads
    yaml-cpp
    ${ZMQ_LIBRARIES}
    rt
)

# 源文件（核心库，供 daq_main 与测试共用）
set(LIB_SOURCES
    src/utils.cpp
    src/data_store.cpp
    src/duplicate_reject_logger.cpp
    src/config.cpp
    src/elec_config.cpp
    src/cs_yaml_config.cpp
    src/comm.cpp
    src/frontend.cpp
    src/builder.cpp
    src/data_handler.cpp
    src/data_parser.cpp
    src/t2_stage_trace.cpp
    src/http_api.cpp
    src/params.cpp
    src/app.cpp
    src/t3_trigger.cpp
    src/trigger_processor_pipeline.cpp
    src/trigger_processor_gate_logic.cpp
)

add_library(daq_core ${LIB_SOURCES})
target_include_directories(daq_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${cpp_httplib_SOURCE_DIR}
    ${ZMQ_INCLUDE_DIRS}
)
if(ZMQ_LIBRARY_DIRS)
    link_directories(${ZMQ_LIBRARY_DIRS})
endif()
target_link_libraries(daq_core PUBLIC ${EXTERNAL_LIBS})

add_executable(daq_main src/daq_main.cpp)
target_link_libraries(daq_main PRIVATE daq_core)

# 安装规则
install(TARGETS daq_main
    RUNTIME DESTINATION bin
)

enable_testing()
add_subdirectory(tests)
add_subdirectory(tools)
