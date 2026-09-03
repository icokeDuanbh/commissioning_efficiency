#pragma once

#include <sys/types.h>

#include <vector>
#include <cstdint>
#include <string>

struct Gp300T2Hit {
    uint64_t timestamp_ns = 0;
    uint16_t trigger_channel = 0;
    uint16_t related_value = 0;
};

class DataParser {
public:
    // GP300 T2 payload: [du_id(4B LE)][record 12B]×N, record = timestamp + trigger_channel + related_value
    struct Gp300T2ParseResult {
        uint32_t du_id = 0;
        std::vector<Gp300T2Hit> hits;
    };

    static Gp300T2ParseResult parseGp300T2Payload(const uint8_t* payload, size_t payload_size);

    static uint64_t bytesToUint64(const uint8_t* data);
    static void uint64ToBytes(uint64_t value, uint8_t* data);
    static uint32_t bytesToUint32(const uint8_t* data);
    static void uint32ToBytes(uint32_t value, uint8_t* data);
    static uint16_t bytesToUint16(const uint8_t* data);
    static void uint16ToBytes(uint16_t value, uint8_t* data);
};
