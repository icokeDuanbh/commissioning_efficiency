#include <utils.h>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

/**
 * @brief 实际的UDP发送函数实现
 * @param data 要发送的数据
 * @param len 数据长度
 * @return 发送的字节数，失败返回负数
 */
int udp_send_func(const uint8_t* data, size_t len) {
    static int sock = -1;
    static struct sockaddr_in server_addr;
    static bool initialized = false;
    
    // 初始化socket（仅第一次调用时）
    if (!initialized) {
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            std::cerr << "创建socket失败" << std::endl;
            return -1;
        }
        
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(4660);  // RBCP默认端口
        server_addr.sin_addr.s_addr = inet_addr("192.168.1.100");  // 目标IP地址
        
        initialized = true;
    }
    
    // 发送数据
    ssize_t sent = sendto(sock, data, len, 0, 
                         (struct sockaddr*)&server_addr, sizeof(server_addr));
    
    if (sent < 0) {
        std::cerr << "发送数据失败: " << strerror(errno) << std::endl;
        return -1;
    }
    
    return sent;
}

/**
 * @brief 实际的UDP接收函数实现
 * @param data 接收数据缓冲区
 * @param len 期望接收的数据长度
 * @param timeout_ms 超时时间(毫秒)
 * @return 接收的字节数，失败返回负数
 */
int udp_recv_func(uint8_t* data, size_t len, int timeout_ms) {
    static int sock = -1;
    static bool initialized = false;
    
    // 初始化socket（仅第一次调用时）
    if (!initialized) {
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            std::cerr << "创建socket失败" << std::endl;
            return -1;
        }
        
        // 设置接收超时
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        // 绑定本地端口
        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(4661);  // 本地接收端口
        local_addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
            std::cerr << "绑定端口失败: " << strerror(errno) << std::endl;
            close(sock);
            return -1;
        }
        
        initialized = true;
    }
    
    // 接收数据
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    
    ssize_t received = recvfrom(sock, data, len, 0, 
                               (struct sockaddr*)&from_addr, &from_len);
    
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            std::cerr << "接收超时" << std::endl;
            return RBCP_ERR_TIMEOUT;
        } else {
            std::cerr << "接收数据失败: " << strerror(errno) << std::endl;
            return -1;
        }
    }
    
    return received;
}

/**
 * @brief 模拟的UDP发送函数（用于测试）
 */
int mock_udp_send_func(const uint8_t* data, size_t len) {
    std::cout << "模拟UDP发送 " << len << " 字节" << std::endl;
    // 这里可以添加实际的数据处理逻辑
    return len;
}

/**
 * @brief 模拟的UDP接收函数（用于测试）
 */
int mock_udp_recv_func(uint8_t* data, size_t len, int timeout_ms) {
    std::cout << "模拟UDP接收 " << len << " 字节，超时 " << timeout_ms << "ms" << std::endl;
    
    // 模拟接收到的RBCP响应
    data[0] = (RBCP_VERSION << 4) | RBCP_TYPE;
    data[1] = RBCP_WRITE | 0x08;  // 写命令 + ACK
    data[2] = 0x01;  // 包ID
    data[3] = len - 8;  // 数据长度
    data[4] = 0x00;  // 地址
    data[5] = 0x00;
    data[6] = 0x00;
    data[7] = 0x00;
    
    // 填充模拟数据
    for (size_t i = 8; i < len; ++i) {
        data[i] = 0xAA + (i % 16);
    }
    
    return len;
}

void example_basic_usage() {
    std::cout << "=== RBCP基本使用示例 ===" << std::endl;
    
    // 创建RBCP实例（使用模拟函数）
    RBCP rbcp(mock_udp_send_func, mock_udp_recv_func);
    
    // 示例1：写入单个字节
    std::cout << "\n1. 写入单个字节到地址0x12345678" << std::endl;
    int ret = rbcp.write(0x12345678, 0xAB);
    if (ret == RBCP_SUCCESS) {
        std::cout << "写入成功" << std::endl;
    } else {
        std::cout << "写入失败，错误码: " << ret << std::endl;
    }
    
    // 示例2：读取单个字节
    std::cout << "\n2. 从地址0x12345678读取单个字节" << std::endl;
    uint8_t read_data;
    ret = rbcp.read(0x12345678, read_data);
    if (ret == RBCP_SUCCESS) {
        std::cout << "读取成功，数据: 0x" << std::hex << (int)read_data << std::dec << std::endl;
    } else {
        std::cout << "读取失败，错误码: " << ret << std::endl;
    }
    
    // 示例3：写入多个字节
    std::cout << "\n3. 写入多个字节到地址0x87654321" << std::endl;
    uint8_t write_data[] = {0x11, 0x22, 0x33, 0x44, 0x55};
    ret = rbcp.write(0x87654321, write_data, 5);
    if (ret == RBCP_SUCCESS) {
        std::cout << "写入成功" << std::endl;
    } else {
        std::cout << "写入失败，错误码: " << ret << std::endl;
    }
    
    // 示例4：读取多个字节
    std::cout << "\n4. 从地址0x87654321读取多个字节" << std::endl;
    uint8_t read_buffer[5];
    ret = rbcp.read(0x87654321, read_buffer, 5);
    if (ret == RBCP_SUCCESS) {
        std::cout << "读取成功，数据: ";
        for (int i = 0; i < 5; ++i) {
            printf("0x%02x ", read_buffer[i]);
        }
        std::cout << std::endl;
    } else {
        std::cout << "读取失败，错误码: " << ret << std::endl;
    }
}

void example_advanced_usage() {
    std::cout << "\n=== RBCP高级使用示例 ===" << std::endl;
    
    // 创建RBCP实例
    RBCP rbcp(mock_udp_send_func, mock_udp_recv_func);
    
    // 示例1：写入并验证（带重试）
    std::cout << "\n1. 写入并验证数据（带重试）" << std::endl;
    int ret = rbcp.writeAndCheck(0x11111111, 0x55, 10, 1000, 5);
    if (ret == RBCP_SUCCESS) {
        std::cout << "写入并验证成功" << std::endl;
    } else {
        std::cout << "写入并验证失败，错误码: " << ret << std::endl;
    }
    
    // 示例2：自定义超时和重试次数
    std::cout << "\n2. 自定义超时和重试次数" << std::endl;
    ret = rbcp.write(0x22222222, 0x66, 500, 2);  // 500ms超时，重试2次
    if (ret == RBCP_SUCCESS) {
        std::cout << "写入成功" << std::endl;
    } else {
        std::cout << "写入失败，错误码: " << ret << std::endl;
    }
    
    // 示例3：批量操作
    std::cout << "\n3. 批量写入操作" << std::endl;
    uint8_t config_data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    for (int i = 0; i < 4; ++i) {
        uint32_t addr = 0x30000000 + i * 4;
        ret = rbcp.write(addr, &config_data[i * 2], 2);
        if (ret == RBCP_SUCCESS) {
            std::cout << "地址0x" << std::hex << addr << std::dec << " 写入成功" << std::endl;
        } else {
            std::cout << "地址0x" << std::hex << addr << std::dec << " 写入失败" << std::endl;
        }
    }
}

void example_real_udp_usage() {
    std::cout << "\n=== 实际UDP使用示例（注释掉以避免网络错误） ===" << std::endl;
    
    // 注意：以下代码被注释掉，因为需要实际的网络环境
    /*
    // 创建使用实际UDP的RBCP实例
    RBCP rbcp_real(udp_send_func, udp_recv_func);
    
    // 配置设备寄存器
    std::cout << "配置设备寄存器..." << std::endl;
    
    // 启用数据采集
    int ret = rbcp_real.write(0x10000000, 0x01);  // 启用位
    if (ret == RBCP_SUCCESS) {
        std::cout << "数据采集已启用" << std::endl;
    }
    
    // 设置采样率
    uint8_t sample_rate[] = {0x00, 0x01, 0x00, 0x00};  // 65536 Hz
    ret = rbcp_real.write(0x10000004, sample_rate, 4);
    if (ret == RBCP_SUCCESS) {
        std::cout << "采样率设置成功" << std::endl;
    }
    
    // 读取状态寄存器
    uint8_t status;
    ret = rbcp_real.read(0x10000008, status);
    if (ret == RBCP_SUCCESS) {
        std::cout << "设备状态: 0x" << std::hex << (int)status << std::dec << std::endl;
    }
    */
    
    std::cout << "实际UDP使用示例已注释（需要网络环境）" << std::endl;
}

int main() {
    std::cout << "RBCP类使用示例" << std::endl;
    std::cout << "==================" << std::endl;
    
    try {
        example_basic_usage();
        example_advanced_usage();
        example_real_udp_usage();
        
        std::cout << "\n所有示例执行完成！" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "示例执行失败: " << e.what() << std::endl;
        return 1;
    }
} 