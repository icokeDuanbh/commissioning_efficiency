#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <functional>
#include <map>
#include <config.h>
#include <packet_format.h>
#include <params.h>
#include <common.h>

namespace grand {
class T3Trigger;
}

/**
 * @brief 数据处理管理器
 */
// 字节流定界 → 波形/触发/T2 分发；Builder 与 T3 通过回调注入
class DataHandler {
public:
    using RawRecordStoreCallback =
        std::function<void(uint32_t du_id, const uint8_t* payload, std::size_t payload_size, WaveformEnvelopeKind envelope)>;

    // elec_config - 本 DU 的 du_id、波形点数等；add_to_build_callback - 交给 Builder
    DataHandler(const ElectronicsConfig &elec_config,
                AddToBuildCallback add_to_build_callback,
                RawRecordStoreCallback raw_record_store_callback = RawRecordStoreCallback{},
                uint64_t time_cut_ns = 1000000000ULL);
    ~DataHandler();

    void before_start();
    void after_stop();
    
    /**
     * @brief 处理数据包
     * @param data 数据包指针
     * @param len 数据包长度
     * @return 0, 无可被解析完整数据；>0, 可被解析完整数据；<0, 格式错误，需扔掉相应长度
     */
    ssize_t handle_data(uint8_t* data, size_t len);

    // legacy T3 trigger hook (non-owning)
    void set_t3_trigger(grand::T3Trigger* t3_trigger);

    uint64_t monotonicViolationCount() const { return monotonic_violation_count_.load(std::memory_order_relaxed); }
    uint64_t maxMonotonicBackwardNs() const { return max_monotonic_backward_ns_.load(std::memory_order_relaxed); }
    uint64_t lastObservedTimestampNs() const { return last_observed_timestamp_ns_.load(std::memory_order_relaxed); }
    uint64_t lastObservedTmId() const { return last_observed_tm_id_.load(std::memory_order_relaxed); }
    uint64_t lastViolationPrevTimestampNs() const { return last_violation_prev_timestamp_ns_.load(std::memory_order_relaxed); }
    uint64_t lastViolationCurrentTimestampNs() const { return last_violation_current_timestamp_ns_.load(std::memory_order_relaxed); }
    uint64_t lastViolationPrevTmId() const { return last_violation_prev_tm_id_.load(std::memory_order_relaxed); }
    uint64_t lastViolationCurrentTmId() const { return last_violation_current_tm_id_.load(std::memory_order_relaxed); }

    void setDropRegressedTimestamps(bool enable) { drop_regressed_timestamps_.store(enable, std::memory_order_relaxed); }

private:
    uint32_t du_id_;           // 本 handler 实例对应的逻辑 DU
    size_t waveform_points_;   // 期望波形点数（来自电子学配置）

    // for builder
    AddToBuildCallback add_to_build_callback_;
    RawRecordStoreCallback raw_record_store_callback_; // 可选：落盘 RAW

private:
    PacketCheckResult check_packet(uint8_t* data, size_t len);
    /// `parseGp300T2Payload` 裸载荷（无 `MessageHeader`）
    void process_t2_payload(const uint8_t* pay, size_t pl);

private: 
    std::map<uint32_t /*channel_id*/, std::chrono::system_clock::time_point> last_sample_time_map_;
    const std::chrono::milliseconds sample_interval_ = std::chrono::milliseconds(1000);

    std::map<uint32_t /*channel_id*/, uint64_t> channel_counts_map_;
    std::map<uint32_t /*channel_id*/, uint64_t> last_channel_counts_map_;
    std::map<uint32_t /*channel_id*/, double> channel_rates_map_;
    std::chrono::system_clock::time_point last_channel_rate_update_time_;
    const std::chrono::milliseconds channel_rate_update_interval_ = std::chrono::milliseconds(1000);

    grand::T3Trigger* t3_trigger_ = nullptr; // App configure 时 set_t3_trigger，非 owning

    std::atomic<uint64_t> time_cut_ns_{1000000000ULL};
    std::atomic<uint64_t> last_max_tm_id_{0};
    std::atomic<uint64_t> last_max_timestamp_ns_{0};
    std::atomic<uint64_t> regression_dropped_count_{0};
    std::atomic<bool> drop_regressed_timestamps_{false};
    std::atomic<uint64_t> last_observed_timestamp_ns_{0};
    std::atomic<uint64_t> last_observed_tm_id_{0};
    std::atomic<uint64_t> monotonic_violation_count_{0};
    std::atomic<uint64_t> max_monotonic_backward_ns_{0};
    std::atomic<uint64_t> last_violation_prev_timestamp_ns_{0};
    std::atomic<uint64_t> last_violation_current_timestamp_ns_{0};
    std::atomic<uint64_t> last_violation_prev_tm_id_{0};
    std::atomic<uint64_t> last_violation_current_tm_id_{0};

}; 
