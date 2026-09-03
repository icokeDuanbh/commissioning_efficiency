#include <utils.h>
#include <algorithm>
#include <cstring>

Logger g_logger(LogLevel::DEBUG);

// 见 utils.h；多行时每 16 字节换行
void dump_hex(const uint8_t* data, size_t len, size_t max_len) {
    for (size_t i = 0; i < (len < max_len ? len : max_len); i++) {
        printf("%02X ", data[i]);
        if (i % 16 == 15) {
            printf("\n");
        }
    }
    if (len > max_len) {
        printf("...");
    }
    printf("\n");
}

// 本地日历日，8 位数字字符串
std::string current_date() {
    auto now = std::chrono::system_clock::now();
    time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&time);
    
    char buffer[9]; // 8位日期 + 结束符
    std::strftime(buffer, sizeof(buffer), "%Y%m%d", &tm);
    return buffer;
}