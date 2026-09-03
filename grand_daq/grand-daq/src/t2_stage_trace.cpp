#include "t2_stage_trace.h"

#include <common.h>
#include <message.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace t2_stage_trace {

namespace {

constexpr const char* kTraceEnv = "CSDAQ_T2_STAGE_TRACE_FILE";
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct TraceState {
    std::mutex mutex;
    bool initialized = false;
    bool active = false;
    fs::path path;
    std::ofstream stream;
};

TraceState& state() {
    static TraceState s;
    return s;
}

void ensureInitializedLocked(TraceState& s) {
    if (s.initialized) {
        return;
    }
    s.initialized = true;

    const char* raw = std::getenv(kTraceEnv);
    if (raw == nullptr || *raw == '\0') {
        return;
    }

    s.path = fs::path(raw);
    std::error_code ec;
    const fs::path parent = s.path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            std::fprintf(stderr, "t2_stage_trace: failed to create parent dir for %s: %s\n",
                         s.path.string().c_str(), ec.message().c_str());
            return;
        }
    }

    s.stream.open(s.path, std::ios::app);
    if (!s.stream.is_open()) {
        std::fprintf(stderr, "t2_stage_trace: failed to open %s\n", s.path.string().c_str());
        return;
    }
    s.active = true;
}

std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t len) {
    std::uint64_t hash = kFnvOffsetBasis;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= static_cast<std::uint64_t>(data[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

void appendJsonLine(nlohmann::json&& doc) {
    TraceState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    ensureInitializedLocked(s);
    if (!s.active) {
        return;
    }
    s.stream << doc.dump() << '\n';
    s.stream.flush();
}

} // namespace

bool enabled() {
    TraceState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    ensureInitializedLocked(s);
    return s.active;
}

std::uint64_t steadyNowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t wallNowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::optional<T2BodySignature> inspectT2Message(const std::uint8_t* data, std::size_t len) {
    if (!enabled() || data == nullptr || len < grand::kMessageHeaderSize) {
        return std::nullopt;
    }

    grand::MessageHeader mh{};
    std::memcpy(&mh, data, sizeof(mh));
    if (mh.type != grand::MT_T2) {
        return std::nullopt;
    }
    if (mh.size < grand::kMessageHeaderSize || mh.size > len || mh.size > MAX_CH_PACKET_LEN) {
        return std::nullopt;
    }

    const std::size_t body_size = mh.size - grand::kMessageHeaderSize;
    const std::uint8_t* body = data + grand::kMessageHeaderSize;
    return signatureFromT2Body(body, body_size);
}

T2BodySignature signatureFromT2Body(const std::uint8_t* body, std::size_t body_size) {
    T2BodySignature sig;
    sig.message_size = grand::kMessageHeaderSize + body_size;
    sig.body_size = body_size;
    sig.body_hash64 = fnv1a64(body, body_size);
    return sig;
}

void writeFrontendRx(const FrontendTraceRecord& record, const T2BodySignature& signature) {
    nlohmann::json doc = {
        {"stage", "frontend_rx"},
        {"du_id", record.du_id},
        {"steady_ns", record.steady_ns},
        {"wall_ns", record.wall_ns},
        {"message_size", signature.message_size},
        {"body_size", signature.body_size},
        {"body_hash64", signature.body_hash64},
        {"accepted_to_ring", record.accepted_to_ring},
        {"ring_accepts_inbound", record.ring_accepts_inbound},
        {"ring_size_before", record.ring_size_before},
        {"ring_free_before", record.ring_free_before},
    };
    appendJsonLine(std::move(doc));
}

void writeHandlerParse(const HandlerTraceRecord& record, const T2BodySignature& signature) {
    nlohmann::json doc = {
        {"stage", "handler_parse"},
        {"du_id", record.du_id},
        {"steady_ns", record.steady_ns},
        {"wall_ns", record.wall_ns},
        {"body_size", signature.body_size},
        {"body_hash64", signature.body_hash64},
        {"ts_count", record.ts_count},
        {"first_tm_id", record.first_tm_id},
        {"last_tm_id", record.last_tm_id},
        {"first_ts", record.first_ts},
        {"last_ts", record.last_ts},
        {"batch_monotonic_violation_count", record.batch_monotonic_violation_count},
        {"forwarded_ts_count", record.forwarded_ts_count},
        {"dropped_regressed_ts_count", record.dropped_regressed_ts_count},
    };
    appendJsonLine(std::move(doc));
}

} // namespace t2_stage_trace
