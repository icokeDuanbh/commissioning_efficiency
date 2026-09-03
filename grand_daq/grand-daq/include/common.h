#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// 这条 <sstream> 不是给 common.h 自己用的，而是显式挂在这里，让所有 include
// common.h 的 TU 都能在任何后续的 `#define private public` 之前先把 sstream
// 处理完。libstdc++ 的 basic_stringbuf 内部嵌套类 __xfer_bufptrs 只要在
// `#define private public` 作用域内被首次实例化，就会触发 "redeclared with
// different access" 的编译错误（见 gcc 9 /usr/include/c++/9/sstream:312）。
//
// 2f99345 之前 common.h 里的 <filesystem> 会传递带入 sstream（<filesystem> →
// <bits/fs_path.h> → <sstream>），所以测试里"先 include common.h 再 define
// private public"的惯例一直是安全的。2f99345 移除 <filesystem> 后这条隐式
// 链被切断，test_p2_frontend_lock_scope.cpp / test_p4_startup_control_plane_
// outcomes.cpp 由于早期包含头里没有别的拉 sstream 的通道，首次处理 sstream
// 落到了 `#define private public` 之后，编译直接失败。
//
// 这里显式 include 一次，把依赖从"transitive + 脆弱"变成"explicit + 稳定"。
#include <sstream>

const size_t MAX_CH_PACKET_LEN = 1024 * 1024; // 单通道单次读缓冲上限，防异常大包撑爆内存

using TriggerID = uint16_t; // 硬件/事例侧触发序号，与波形、触发信号里字段一致
using EventTag = uint32_t;  // Builder 侧事例标签，用于多 DU 拼事件

/// Builder 输入沿：GP300 格式波形片段来源。
enum class WaveformEnvelopeKind : uint8_t {
    GP300DaqEvent = 0,   // MT_DAQEVENT 响应（DOTRIGGER 回传）
    GP300RawEvent = 1,   // MT_RAWEVENT（DU 主动上报）
};

/** Builder：数据源为完整逻辑 DU ID（与 `ElectronicsConfig::du_id` / channel tag 一致）。 */
struct SubFragmentHeader {
    uint32_t du_id;                      // 逻辑 DU，与电子学配置一致
    TriggerID trigger_number;           // 与本段波形对应的触发号
    uint32_t data_size;                 // `data` 指向的载荷字节数
    WaveformEnvelopeKind waveform_envelope; // 片段来自哪条封装路径，下游区分 RAW/DAQ
    uint32_t event_tag = 0;             // MT_DAQEVENT body[0:4] 提取的 DOTRIGGER tag
} __attribute__((packed));

// 与某类线上块头对齐的紧凑头：trigger + 载荷长度（具体包类型见使用处）
struct FullPacketHead {
    TriggerID trigger_number;
    uint32_t data_size; // 紧随其后的波形/二进制块长度
} __attribute__((packed));

// 返回已消费字节数；供 Comm 等读循环喂数据（与 `DataHandler::handle_data` 签名一致）
using DataHandleCallback = std::function<ssize_t(uint8_t*, size_t)>;
// Builder 入口：解析出一条子片段后填入 header 并指向载荷
using AddToBuildCallback = std::function<void(SubFragmentHeader& header, uint8_t* data)>;

/**
 * @brief 异常基类
 */
class DAQException : public std::exception {
public:
    DAQException(int code, const std::string& message) : std::exception(), code(code), message(message) {}
    int code;
    std::string message;
};

#define DECLARE_EXCEPTION(name, code) \
    class name : public DAQException { \
    public: \
        name(const std::string& message) : DAQException(code, message) {} \
    };

DECLARE_EXCEPTION(LogicException, 4000);
DECLARE_EXCEPTION(InitializationException, 4001);
DECLARE_EXCEPTION(ConfigException, 4004);
DECLARE_EXCEPTION(DataFormatException, 4005);
DECLARE_EXCEPTION(DataProcessingException, 4006);
DECLARE_EXCEPTION(SoftwareException, 4007);
DECLARE_EXCEPTION(FrontendException, 4008);