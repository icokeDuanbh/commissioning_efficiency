#include <data_handler.h>
#include <data_parser.h>
#include <utils.h>
#include <common.h>
#include <message.h>
#include <t2_stage_trace.h>
#include <t3_trigger.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>
#include <mutex>
#include <cstdlib>

#include <nlohmann/json.hpp>

#define WAVEFORM_EXPAND_COUNT 8

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct CaptureConfig {
    fs::path root;
    bool capture_t2 = false;
    bool capture_waveform = false;
    std::size_t max_t2 = 0;
    std::size_t max_waveform = 0;
};

struct CaptureState {
    std::mutex mu;
    std::size_t t2_seq = 0;
    std::size_t waveform_seq = 0;
};

CaptureState& captureState() {
    static CaptureState state;
    return state;
}

bool readEnvFlag(const char* name, bool default_value) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return default_value;
    }
    return std::string(raw) != "0";
}

std::size_t readEnvLimit(const char* name, std::size_t default_value) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return default_value;
    }
    return static_cast<std::size_t>(std::strtoull(raw, nullptr, 10));
}

CaptureConfig loadCaptureConfig() {
    CaptureConfig cfg;
    const char* raw_dir = std::getenv("CSDAQ_CAPTURE_DIR");
    if (raw_dir == nullptr || *raw_dir == '\0') {
        return cfg;
    }
    cfg.root = fs::path(raw_dir);
    cfg.capture_t2 = readEnvFlag("CSDAQ_CAPTURE_T2_ENABLE", true);
    cfg.capture_waveform = readEnvFlag("CSDAQ_CAPTURE_WAVEFORM_ENABLE", true);
    cfg.max_t2 = readEnvLimit("CSDAQ_CAPTURE_T2_MAX", 256);
    cfg.max_waveform = readEnvLimit("CSDAQ_CAPTURE_WAVEFORM_MAX", 256);
    return cfg;
}

uint64_t captureNowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

const char* packetTypeName(PacketType type) {
    switch (type) {
        case PacketType::GRAND_MESSAGE_T2:
            return "GRAND_MESSAGE_T2";
        case PacketType::GP300_DAQEVENT:
            return "GP300_DAQEVENT";
        case PacketType::GP300_RAWEVENT:
            return "GP300_RAWEVENT";
        case PacketType::UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

const char* waveformEnvelopeName(WaveformEnvelopeKind kind) {
    switch (kind) {
        case WaveformEnvelopeKind::GP300DaqEvent:
            return "GP300DaqEvent";
        case WaveformEnvelopeKind::GP300RawEvent:
            return "GP300RawEvent";
        default:
            return "Unknown";
    }
}

void updateMaxRelaxed(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

void writeBytes(const fs::path& path, const uint8_t* data, std::size_t size) {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) {
        throw std::runtime_error("failed to open capture output: " + path.string());
    }
    ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!ofs.good()) {
        throw std::runtime_error("failed to write capture output: " + path.string());
    }
}

void writeJson(const fs::path& path, const json& doc) {
    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        throw std::runtime_error("failed to open capture meta: " + path.string());
    }
    ofs << doc.dump(2) << '\n';
    if (!ofs.good()) {
        throw std::runtime_error("failed to write capture meta: " + path.string());
    }
}

void maybeCaptureT2Body(uint32_t du_id,
                        PacketType packet_type,
                        const uint8_t* payload,
                        std::size_t payload_size) {
    const CaptureConfig cfg = loadCaptureConfig();
    if (!cfg.capture_t2 || payload == nullptr || payload_size == 0 || cfg.max_t2 == 0) {
        return;
    }
    auto& state = captureState();
    std::lock_guard<std::mutex> lk(state.mu);
    if (state.t2_seq >= cfg.max_t2) {
        return;
    }
    const std::size_t seq = state.t2_seq++;
    const fs::path dir = cfg.root / "t2";
    fs::create_directories(dir);
    const std::string stem = "t2_" + std::to_string(seq);
    const fs::path payload_path = dir / (stem + ".bin");
    const fs::path meta_path = dir / (stem + ".json");
    writeBytes(payload_path, payload, payload_size);
    json meta;
    meta["kind"] = "t2_body";
    meta["seq"] = seq;
    meta["captured_ns"] = captureNowNs();
    meta["du_id"] = du_id;
    meta["packet_type"] = packetTypeName(packet_type);
    meta["payload_size"] = payload_size;
    meta["payload_file"] = payload_path.filename().string();
    writeJson(meta_path, meta);
}

void maybeCaptureWaveformBytes(uint32_t du_id,
                               PacketType packet_type,
                               const uint8_t* full_bytes,
                               std::size_t full_size,
                               std::size_t body_offset,
                               std::size_t body_size,
                               WaveformEnvelopeKind envelope) {
    const CaptureConfig cfg = loadCaptureConfig();
    if (!cfg.capture_waveform || full_bytes == nullptr || full_size == 0 || cfg.max_waveform == 0) {
        return;
    }
    auto& state = captureState();
    std::lock_guard<std::mutex> lk(state.mu);
    if (state.waveform_seq >= cfg.max_waveform) {
        return;
    }
    const std::size_t seq = state.waveform_seq++;
    const fs::path dir = cfg.root / "waveform";
    fs::create_directories(dir);
    const std::string stem = "waveform_" + std::to_string(seq);
    const fs::path full_path = dir / (stem + "_full.bin");
    const fs::path body_path = dir / (stem + "_body.bin");
    const fs::path meta_path = dir / (stem + ".json");
    writeBytes(full_path, full_bytes, full_size);
    writeBytes(body_path, full_bytes + body_offset, body_size);
    json meta;
    meta["kind"] = "waveform";
    meta["seq"] = seq;
    meta["captured_ns"] = captureNowNs();
    meta["du_id"] = du_id;
    meta["packet_type"] = packetTypeName(packet_type);
    meta["waveform_envelope"] = waveformEnvelopeName(envelope);
    meta["full_size"] = full_size;
    meta["body_offset"] = body_offset;
    meta["body_size"] = body_size;
    meta["full_file"] = full_path.filename().string();
    meta["body_file"] = body_path.filename().string();
    writeJson(meta_path, meta);
}

bool isSupportedGrandMessageHeader(const uint8_t* data, size_t len, grand::MessageHeader* header_out = nullptr) {
    if (len < grand::kMessageHeaderSize) {
        return false;
    }

    grand::MessageHeader header{};
    std::memcpy(&header, data, sizeof(header));
    const bool type_supported =
        (header.type == grand::MT_T2 || header.type == grand::MT_RAWEVENT || header.type == grand::MT_DAQEVENT);
    if (!type_supported) {
        return false;
    }
    if (header.size < grand::kMessageHeaderSize || header.size > MAX_CH_PACKET_LEN) {
        return false;
    }

    if (header_out != nullptr) {
        *header_out = header;
    }
    return true;
}

} // namespace

DataHandler::DataHandler(const ElectronicsConfig &elec_config,
                         AddToBuildCallback add_to_build_callback,
                         RawRecordStoreCallback raw_record_store_callback,
                         uint64_t time_cut_ns)
    : du_id_(elec_config.du_id)
    , waveform_points_(elec_config.waveform_points + WAVEFORM_EXPAND_COUNT)
    , add_to_build_callback_(std::move(add_to_build_callback))
    , raw_record_store_callback_(std::move(raw_record_store_callback))
    , time_cut_ns_(time_cut_ns > 0 ? time_cut_ns : 1000000000ULL) {
}

DataHandler::~DataHandler() {
}

void DataHandler::before_start() {
    last_sample_time_map_.clear();
    channel_counts_map_.clear();
    last_channel_counts_map_.clear();
    channel_rates_map_.clear();
    last_observed_timestamp_ns_.store(0, std::memory_order_relaxed);
    last_observed_tm_id_.store(0, std::memory_order_relaxed);
    monotonic_violation_count_.store(0, std::memory_order_relaxed);
    max_monotonic_backward_ns_.store(0, std::memory_order_relaxed);
    last_violation_prev_timestamp_ns_.store(0, std::memory_order_relaxed);
    last_violation_current_timestamp_ns_.store(0, std::memory_order_relaxed);
    last_violation_prev_tm_id_.store(0, std::memory_order_relaxed);
    last_violation_current_tm_id_.store(0, std::memory_order_relaxed);
}

void DataHandler::after_stop() {
}

// 定界一包并分派：返回值即可消费字节数；<=0 见 `data_handler.h` 中 handle_data 说明
ssize_t DataHandler::handle_data(uint8_t* data, size_t len) {
    // 获取数据包类型
    PacketCheckResult packet_check_result = check_packet(data, len);

    if (packet_check_result.sz <= 0) {
        return packet_check_result.sz; // 数据包空或不完整，等待
    }

    const ssize_t consumed = packet_check_result.sz;

    if (packet_check_result.ptype == PacketType::GRAND_MESSAGE_T2) {
        const size_t hdr = grand::kMessageHeaderSize;
        maybeCaptureT2Body(du_id_,
                           packet_check_result.ptype,
                           data + hdr,
                           static_cast<size_t>(consumed) - hdr);
        process_t2_payload(data + hdr, static_cast<size_t>(consumed) - hdr);
        // 与旧 cs-daq 一致：T2 不入 `rawEventStore` / EventStore 式原始事例落盘链，故不写 `DataStore`。
        return consumed;
    }

    if (packet_check_result.ptype == PacketType::GP300_DAQEVENT) {
        const size_t hdr = grand::kMessageHeaderSize;
        uint8_t* body = data + hdr;
        const size_t body_len = static_cast<size_t>(consumed) - hdr;

        maybeCaptureWaveformBytes(du_id_,
                                  packet_check_result.ptype,
                                  data,
                                  static_cast<size_t>(consumed),
                                  hdr,
                                  body_len,
                                  WaveformEnvelopeKind::GP300DaqEvent);

        // 提取 event_tag — body 前 4 字节，小端
        uint32_t event_tag = 0;
        std::memcpy(&event_tag, body, sizeof(uint32_t));

        SubFragmentHeader sfh{};
        sfh.du_id = du_id_;
        sfh.trigger_number = 0;
        sfh.data_size = static_cast<uint32_t>(body_len);
        sfh.waveform_envelope = WaveformEnvelopeKind::GP300DaqEvent;
        sfh.event_tag = event_tag;

        add_to_build_callback_(sfh, body);
        return consumed;
    }

    if (packet_check_result.ptype == PacketType::GP300_RAWEVENT) {
        const size_t hdr = grand::kMessageHeaderSize;
        const uint8_t* body = data + hdr;
        const size_t body_len = static_cast<size_t>(consumed) - hdr;

        maybeCaptureWaveformBytes(du_id_,
                                  packet_check_result.ptype,
                                  data,
                                  static_cast<size_t>(consumed),
                                  hdr,
                                  body_len,
                                  WaveformEnvelopeKind::GP300RawEvent);

        if (raw_record_store_callback_) {
            raw_record_store_callback_(du_id_, body, body_len,
                                        WaveformEnvelopeKind::GP300RawEvent);
        }
        return consumed;
    }

    throw LogicException("Logic error, invalid packet type");
}

// 识别 grand::MessageHeader，按 type 分派 T2/DAQEVENT/RAWEVENT
PacketCheckResult DataHandler::check_packet(uint8_t* data, size_t len) {
    // --- DU→CS：`grand::MessageHeader` + body（与 `old-cs-daq/cs-daq` `MessageDispatcher` 一致：
    // MT_T2 → T3Trigger；MT_RAWEVENT → rawEventStore；MT_DAQEVENT → EventStore；此处只做类型定界与解析接线。）---
    grand::MessageHeader mh{};
    if (isSupportedGrandMessageHeader(data, len, &mh)) {
        if (len < mh.size) {
            return PacketCheckResult({0, PacketType::UNKNOWN});
        }
        const uint8_t* body = data + grand::kMessageHeaderSize;
        const size_t bl = mh.size - grand::kMessageHeaderSize;

        if (mh.type == grand::MT_T2) {
            if (bl == 0) {
                return PacketCheckResult({-1, PacketType::UNKNOWN});
            }
            // 定界仅信任 MessageHeader.size；语义校验与 T3 入队在 process_t2_payload 单次 parse 中完成。
            return PacketCheckResult({static_cast<ssize_t>(mh.size), PacketType::GRAND_MESSAGE_T2});
        }

        // GP300 格式识别（DU 固件发送的实际格式）
        if (mh.type == grand::MT_DAQEVENT && bl >= 8) {
            return PacketCheckResult({static_cast<ssize_t>(mh.size), PacketType::GP300_DAQEVENT});
        }
        if (mh.type == grand::MT_RAWEVENT && bl >= 2) {
            return PacketCheckResult({static_cast<ssize_t>(mh.size), PacketType::GP300_RAWEVENT});
        }
        return PacketCheckResult({-1, PacketType::UNKNOWN});
    }

    return PacketCheckResult({-1, PacketType::UNKNOWN});
}

void DataHandler::set_t3_trigger(grand::T3Trigger* t3_trigger) {
    t3_trigger_ = t3_trigger;
}

// 处理已解析的 T2 payload，将时间戳批量送入 T3。
// pay 来自 handle_data 在 GRAND_MESSAGE_T2 分支里对 data+kMessageHeaderSize 的切片。
void DataHandler::process_t2_payload(const uint8_t* pay, size_t pl) {
    if (t3_trigger_ == nullptr || pl == 0) {
        return;
    }

    // 调用 DataParser 固定格式解析
    auto pr = DataParser::parseGp300T2Payload(pay, pl);
    if (pr.hits.empty()) {
        return;
    }

    // du_id 校验：DU 固件的 T2 body 前 4 字节并非可靠的 du_id（第二次及后续发送时
    // 该位置是上一个 timestamp 的尾部，参见 du-daq/src/data_manager.cpp:343），
    // 因此这里不再告警。实际 du_id 以连接层 du_id_ 为准。
    (void)pr.du_id;

    // --- tmId 乱序检测 + 可选过滤 ---
    const uint64_t tc = time_cut_ns_.load(std::memory_order_relaxed);
    const bool do_drop = drop_regressed_timestamps_.load(std::memory_order_relaxed);
    std::vector<Gp300T2Hit> filtered;
    if (do_drop) {
        filtered.reserve(pr.hits.size());
    }

    uint64_t batch_monotonic_violation_count = 0;
    uint64_t batch_regression_dropped_count = 0;
    const uint64_t first_ts = pr.hits.front().timestamp_ns;
    const uint64_t last_ts = pr.hits.back().timestamp_ns;
    const uint64_t first_tm_id = tc > 0 ? first_ts / tc : 0;
    const uint64_t last_tm_id = tc > 0 ? last_ts / tc : 0;

    for (const Gp300T2Hit& hit : pr.hits) {
        const uint64_t ts = hit.timestamp_ns;
        const uint64_t tm_id = tc > 0 ? ts / tc : 0;

        const uint64_t prev_observed_ts = last_observed_timestamp_ns_.load(std::memory_order_relaxed);
        const uint64_t prev_observed_tm_id = last_observed_tm_id_.load(std::memory_order_relaxed);
        if (prev_observed_ts > 0 && ts < prev_observed_ts) {
            const uint64_t backward_ns = prev_observed_ts - ts;
            batch_monotonic_violation_count++;
            const uint64_t cnt = monotonic_violation_count_.fetch_add(1, std::memory_order_relaxed) + 1;
            last_violation_prev_timestamp_ns_.store(prev_observed_ts, std::memory_order_relaxed);
            last_violation_current_timestamp_ns_.store(ts, std::memory_order_relaxed);
            last_violation_prev_tm_id_.store(prev_observed_tm_id, std::memory_order_relaxed);
            last_violation_current_tm_id_.store(tm_id, std::memory_order_relaxed);
            updateMaxRelaxed(max_monotonic_backward_ns_, backward_ns);
            if (cnt <= 20 || (cnt % 1000 == 0)) {
                LOG_WARN << "T2_ORDER VIOLATION: DU=" << du_id_
                         << " prev_ts=" << prev_observed_ts
                         << " current_ts=" << ts
                         << " backward_ns=" << backward_ns
                         << " prev_tmId=" << prev_observed_tm_id
                         << " current_tmId=" << tm_id
                         << " total_monotonic_violations=" << cnt;
            }
        }
        last_observed_timestamp_ns_.store(ts, std::memory_order_relaxed);
        last_observed_tm_id_.store(tm_id, std::memory_order_relaxed);

        // Keep the optional legacy drop behavior unchanged; it is not used as the health statistic.
        const uint64_t prev_max_tmid = last_max_tm_id_.load(std::memory_order_relaxed);
        if (prev_max_tmid > 0 && tm_id < prev_max_tmid) {
            if (do_drop) {
                regression_dropped_count_.fetch_add(1, std::memory_order_relaxed);
                batch_regression_dropped_count++;
            }
            continue;
        }
        const uint64_t prev_max_ts = last_max_timestamp_ns_.load(std::memory_order_relaxed);

        if (tm_id > prev_max_tmid)
            last_max_tm_id_.store(tm_id, std::memory_order_relaxed);
        if (ts > prev_max_ts)
            last_max_timestamp_ns_.store(ts, std::memory_order_relaxed);

        if (do_drop) {
            filtered.push_back(hit);
        }
    }

    if (t2_stage_trace::enabled()) {
        const auto signature = t2_stage_trace::signatureFromT2Body(pay, pl);
        t2_stage_trace::writeHandlerParse(
            t2_stage_trace::HandlerTraceRecord{
                du_id_,
                t2_stage_trace::steadyNowNs(),
                t2_stage_trace::wallNowNs(),
                pr.hits.size(),
                first_tm_id,
                last_tm_id,
                first_ts,
                last_ts,
                batch_monotonic_violation_count,
                do_drop ? filtered.size() : pr.hits.size(),
                batch_regression_dropped_count,
            },
            signature);
    }

    if (do_drop) {
        if (!filtered.empty()) {
            t3_trigger_->onT2(du_id_, std::move(filtered));
        }
    } else {
        t3_trigger_->onT2(du_id_, std::move(pr.hits));
    }
}
