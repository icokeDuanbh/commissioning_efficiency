
# include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# if(REAL_DU)
    
#     include_directories(/home/grand/externals/template_flt_online)
#     include_directories(/home/grand/externals/tensorflow-master)
#     include_directories(/home/grand/externals)
# endif()

# link_libraries(grandcommon)

# add_subdirectory(src)
# add_subdirectory(exec)
# # add_subdirectory(tests)

cmake_minimum_required(VERSION 3.15)
project(dudaq)

# -----------------------------
# 设置路径
# -----------------------------
set(TEMPLATE_FLT_DIR /home/grand/externals/template_flt_online)
set(TFLITE_DIR /home/grand/externals/tensorflow-master)
set(EIGEN_DIR /home/grand/externals)

# 添加 include 路径
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${TEMPLATE_FLT_DIR}
    # ${TFLITE_DIR}/tensorflow/lite/c
    ${TFLITE_DIR}
    ${EIGEN_DIR}
    
)

# -----------------------------
# dudaq 源文件列表
# -----------------------------
set(DUDAQ_COMMON_SOURCES
    src/data_manager.cpp
    src/du_fsm.cpp
    src/dudaq_app.cpp
    src/frontend.cpp
    src/scope_a.cpp
    src/scope_dummy.cpp
    src/data_format.cpp
)

list(APPEND DUDAQ_COMMON_SOURCES
        ${TEMPLATE_FLT_DIR}/utils.cpp
        ${TEMPLATE_FLT_DIR}/error_handling.cpp
        ${TEMPLATE_FLT_DIR}/template_FLT.cpp
        /home/grand/workarea/grand-daq/du-daq/src/tflite_inference.cpp
    )

# Template FLT 和 TensorFlow Lite 仅在 REAL_DU 时加入
if(REAL_DU)
    message(STATUS "Building DU-DAQ with Template FLT and TensorFlow Lite")

    # 查找 ARM 或 PC 的 TFLite C 库
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm")
        set(TFLITE_LIB_PATH ${EXTERNAL_DIR_ARM}/lib)
    else()
        set(TFLITE_LIB_PATH /home/grand/arm_a_installed/lib)
    endif()

    find_library(TFLITE_LIB tensorflowlite_c PATHS ${TFLITE_LIB_PATH} NO_DEFAULT_PATH)
    if(NOT TFLITE_LIB)
        message(FATAL_ERROR "Cannot find tensorflowlite_c library in ${TFLITE_LIB_PATH}")
    endif()
endif()

# -----------------------------
# 创建 dudaq 动态库
# -----------------------------
add_library(dudaq SHARED ${DUDAQ_COMMON_SOURCES})

# 链接依赖库
target_link_libraries(dudaq
    PRIVATE
    pthread
    grandcommon
)

if(REAL_DU)
    target_link_libraries(dudaq PRIVATE ${TFLITE_LIB})
endif()

# -----------------------------
# 安装库文件
# -----------------------------
install(TARGETS dudaq LIBRARY DESTINATION lib)

# -----------------------------
# 添加 exec 子目录
# -----------------------------
add_subdirectory(exec)