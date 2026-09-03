#pragma once

#include <cstddef>
#include <cstdint>

namespace grand {

// 与 DU/前端之间 `grand::Message` 外层信封里的 type 字段对应，决定 body 语义。
using MessageType = std::uint32_t;

const MessageType MT_HSHAKE = 0x10FF; // 握手/能力协商类消息
const MessageType MT_NONE = 0x00;
const MessageType MT_CMD = 0x01;        // 控制命令
const MessageType MT_ACCEPT = 0x02;     // 命令应答
const MessageType MT_T2 = 0x03;         // T2 时间戳等载荷（legacy T2 slice）
const MessageType MT_DAQEVENT = 0x04;   // DAQ 事件路径（与旧 EventStore/CD 对齐）
const MessageType MT_RAWEVENT = 0x05;   // RAW 事件路径（与旧 RAWEVENT/L1/MD 对齐）

// `grand::Message` 固定前缀：紧接在 TCP 字节流里 body 之前，size 含本头 + body。
struct MessageHeader {
    std::uint32_t size;   // 本头 + 后续 body 的总字节数
    MessageType type;     // 载荷类别，见 MT_* 常量
};

inline constexpr std::size_t kMessageHeaderSize = sizeof(MessageHeader); // 定界时跳过此前缀

} // namespace grand
