
# #add_library(dudaq SHARED 
# #    IFrontend.cpp
# #    ScopeA.cpp
# #    ScopeDummy.cpp
# #)

# add_library(dudaq SHARED
#         data_manager.cpp
# 	du_fsm.cpp
# 	#data_manager.cpp
#         dudaq_app.cpp
#         tflite_inference.cpp
#         /home/grand/externals/template_flt_online/template_FLT.cpp
#         frontend.cpp
#         scope_a.cpp
#         scope_dummy.cpp
#         data_format.cpp
# )

# install(TARGETS dudaq LIBRARY DESTINATION lib)
cmake_minimum_required(VERSION 3.15)
project(dudaq)

# -----------------------------
# 设置路径
# -----------------------------
set(TEMPLATE_FLT_DIR /home/grand/externals/template_flt_online)
set(TFLITE_DIR /home/grand/externals/tensorflow-master)

# -----------------------------
# 添加 include 路径
# -----------------------------
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}       # 当前源文件目录
    ${TEMPLATE_FLT_DIR}               # TemplateFLT
    ${TFLITE_DIR}                      # TensorFlow Lite
)

# -----------------------------
# dudaq 源文件列表
# -----------------------------
set(DUDAQ_COMMON_SOURCES
    data_manager.cpp
    du_fsm.cpp
    dudaq_app.cpp
    frontend.cpp
    scope_a.cpp
    scope_dummy.cpp
    data_format.cpp
    tflite_inference.cpp
    ${TEMPLATE_FLT_DIR}/utils.cpp
    ${TEMPLATE_FLT_DIR}/error_handling.cpp
    ${TEMPLATE_FLT_DIR}/template_FLT.cpp
)

# -----------------------------
# 创建 dudaq 动态库
# -----------------------------
add_library(dudaq SHARED ${DUDAQ_COMMON_SOURCES})

# -----------------------------
# 链接依赖库
# -----------------------------
# 注意：确保系统上有 tensorflowlite_c 库
find_library(TFLITE_LIB tensorflowlite_c PATHS /usr/local/lib /usr/lib /home/grand/externals/tensorflow-master)
if(NOT TFLITE_LIB)
    message(FATAL_ERROR "Cannot find tensorflowlite_c library. Please build or install TFLite C API.")
endif()

target_link_libraries(dudaq PRIVATE
    grandcommon
    pthread
    ${TFLITE_LIB}
)

# -----------------------------
# target_include_directories 再指定一次
# -----------------------------
target_include_directories(dudaq PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${TEMPLATE_FLT_DIR}
    ${TFLITE_DIR}
    ${EIGEN_DIR}
)

# -----------------------------
# 安装库文件
# -----------------------------
install(TARGETS dudaq LIBRARY DESTINATION lib)