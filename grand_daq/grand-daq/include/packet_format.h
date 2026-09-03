#pragma once

#include <cstdint>
#include <vector>
#include <common.h>

const uint32_t T2_TIMESTAMPS_TAG = 0xEE12;    // legacy T2/T1 时间戳 slice（EE12）

enum class PacketType {
    UNKNOWN,
    /// DU→CS 与 `old-cs-daq` 一致：`grand::MessageHeader` + body；body 为纯 legacy T2 载荷（非 EE12 头）
    GRAND_MESSAGE_T2,
    GP300_DAQEVENT,   // MT_DAQEVENT，body = [event_tag(4B)][prefix(4B)][ElecEvent]
    GP300_RAWEVENT,   // MT_RAWEVENT，body = [ElecEvent uint16_t array]
};

// `check_packet` 等定界结果：sz 为本次应推进的字节数（含头），ptype 为解析出的包类别
struct PacketCheckResult {
    ssize_t sz;           // 本轮应消费的字节数；错误时可能为负或 0，见调用方约定
    PacketType ptype;     // 识别到的包类型
};

