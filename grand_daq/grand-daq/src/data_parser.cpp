#include <data_parser.h>
#include <packet_format.h>

#include <cstring>

DataParser::Gp300T2ParseResult DataParser::parseGp300T2Payload(const uint8_t* payload, size_t payload_size) {
    Gp300T2ParseResult out;
    constexpr size_t kDuIdPrefixSize = sizeof(uint32_t);
    constexpr size_t kRecordSize = sizeof(uint64_t) + sizeof(uint16_t) + sizeof(uint16_t);

    if (payload == nullptr || payload_size < kDuIdPrefixSize) {
        return out;
    }

    std::memcpy(&out.du_id, payload, sizeof(uint32_t));

    const uint8_t* record_data = payload + kDuIdPrefixSize;
    const size_t record_bytes = payload_size - kDuIdPrefixSize;
    if (record_bytes == 0 || record_bytes % kRecordSize != 0) {
        return out;
    }

    const size_t count = record_bytes / kRecordSize;
    out.hits.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* rec = record_data + i * kRecordSize;
        Gp300T2Hit hit;
        std::memcpy(&hit.timestamp_ns, rec, sizeof(uint64_t));
        std::memcpy(&hit.trigger_channel, rec + sizeof(uint64_t), sizeof(uint16_t));
        std::memcpy(&hit.related_value, rec + sizeof(uint64_t) + sizeof(uint16_t), sizeof(uint16_t));
        out.hits.push_back(hit);
    }

    return out;
}

uint64_t DataParser::bytesToUint64(const uint8_t* data) {
    return (static_cast<uint64_t>(data[0]) & 0xff) << 56 
            | (static_cast<uint64_t>(data[1]) & 0xff) << 48 
            | (static_cast<uint64_t>(data[2]) & 0xff) << 40 
            | (static_cast<uint64_t>(data[3]) & 0xff) << 32 
            | (static_cast<uint64_t>(data[4]) & 0xff) << 24 
            | (static_cast<uint64_t>(data[5]) & 0xff) << 16 
            | (static_cast<uint64_t>(data[6]) & 0xff) << 8 
            | (static_cast<uint64_t>(data[7]) & 0xff);
}

void DataParser::uint64ToBytes(uint64_t value, uint8_t* data) {
    data[0] = (value >> 56) & 0xff;
    data[1] = (value >> 48) & 0xff;
    data[2] = (value >> 40) & 0xff;
    data[3] = (value >> 32) & 0xff;
    data[4] = (value >> 24) & 0xff;
    data[5] = (value >> 16) & 0xff;
    data[6] = (value >> 8) & 0xff;
    data[7] = (value & 0xff);
}

uint32_t DataParser::bytesToUint32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) & 0xff) << 24 
            | (static_cast<uint32_t>(data[1]) & 0xff) << 16 
            | (static_cast<uint32_t>(data[2]) & 0xff) << 8 
            | (static_cast<uint32_t>(data[3]) & 0xff);
}

void DataParser::uint32ToBytes(uint32_t value, uint8_t* data) {
    data[0] = (value >> 24) & 0xff;
    data[1] = (value >> 16) & 0xff;
    data[2] = (value >> 8) & 0xff;
    data[3] = (value & 0xff);
}

uint16_t DataParser::bytesToUint16(const uint8_t* data) {
    return (static_cast<uint16_t>(data[0]) & 0xff) << 8 
            | (static_cast<uint16_t>(data[1]) & 0xff);
}

void DataParser::uint16ToBytes(uint16_t value, uint8_t* data) {
    data[0] = (value >> 8) & 0xff;
    data[1] = (value & 0xff);
}
