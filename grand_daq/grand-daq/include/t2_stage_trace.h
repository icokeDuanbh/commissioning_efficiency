#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace t2_stage_trace {

struct T2BodySignature {
    std::size_t message_size = 0;
    std::size_t body_size = 0;
    std::uint64_t body_hash64 = 0;
};

struct FrontendTraceRecord {
    std::uint32_t du_id = 0;
    std::uint64_t steady_ns = 0;
    std::uint64_t wall_ns = 0;
    bool accepted_to_ring = false;
    bool ring_accepts_inbound = false;
    std::size_t ring_size_before = 0;
    std::size_t ring_free_before = 0;
};

struct HandlerTraceRecord {
    std::uint32_t du_id = 0;
    std::uint64_t steady_ns = 0;
    std::uint64_t wall_ns = 0;
    std::size_t ts_count = 0;
    std::uint64_t first_tm_id = 0;
    std::uint64_t last_tm_id = 0;
    std::uint64_t first_ts = 0;
    std::uint64_t last_ts = 0;
    std::uint64_t batch_monotonic_violation_count = 0;
    std::uint64_t forwarded_ts_count = 0;
    std::uint64_t dropped_regressed_ts_count = 0;
};

bool enabled();
std::uint64_t steadyNowNs();
std::uint64_t wallNowNs();

std::optional<T2BodySignature> inspectT2Message(const std::uint8_t* data, std::size_t len);
T2BodySignature signatureFromT2Body(const std::uint8_t* body, std::size_t body_size);

void writeFrontendRx(const FrontendTraceRecord& record, const T2BodySignature& signature);
void writeHandlerParse(const HandlerTraceRecord& record, const T2BodySignature& signature);

} // namespace t2_stage_trace
