#!/bin/bash

# SEASTAR DAQ Frontend 构建脚本

echo "Building SEASTAR DAQ Frontend..."

# 创建构建目录
mkdir -p build
cd build

# 配置项目
echo "Configuring project..."
cmake ..

# 编译项目
echo "Building project..."
make -j$(nproc)

# 检查编译结果
if [ $? -eq 0 ]; then
    echo "Build successful!"
    echo "Executable: build/seastar-daq-frontend"
    echo "Test executable: build/tests/test_seastar_daq"
else
    echo "Build failed!"
    exit 1
fi

# 运行测试
echo "Running tests..."
make test

echo "Build complete!" 