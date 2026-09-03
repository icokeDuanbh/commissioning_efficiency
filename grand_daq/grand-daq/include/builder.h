#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <utils.h>
#include <common.h>

class Builder {
public:
    struct WaveformFragment {
        uint32_t du_id = 0;
        TriggerID trigger_number = 0;
        WaveformEnvelopeKind waveform_envelope = WaveformEnvelopeKind::GP300DaqEvent;
        uint64_t trigger_time = 0;       // 片段头里的触发时刻，用于与 pending 校验
        std::vector<uint8_t> payload;    // 拷贝自上游缓冲，长度与 SubFragmentHeader.data_size 一致
    };

    struct CompletedEvent {
        EventTag tag = 0;
        std::set<uint32_t> expected_du_ids;
        std::set<uint32_t> actual_du_ids;
        std::vector<WaveformFragment> fragments;
        uint64_t trigger_timestamp_ns = 0; // beginEvent 传入的整事例时间
        bool is_partial = false;           // 超时/stop 产出的不完整事件
    };

    enum class DiagnosticType : uint8_t {
        NoFifoForDu = 0,
        HeadTimestampMismatch = 2,
        DuplicateFragment = 3,
        InvalidFragment = 4,
        Timeout = 5,
        StopFlush = 6,
        AdmissionRejected = 7,
    };

    struct DiagnosticEvent {
        DiagnosticType type = DiagnosticType::InvalidFragment;
        EventTag tag = 0;
        uint32_t du_id = 0;
        TriggerID trigger_number = 0;
        uint64_t fragment_trigger_time = 0;
        uint64_t event_trigger_timestamp_ns = 0;
    };

    struct LossDuStats {
        uint64_t expected = 0;
        uint64_t missing = 0;

        double lossPct() const {
            return expected == 0 ? 0.0 : static_cast<double>(missing) / expected;
        }
    };

    struct LossTotals {
        uint64_t expected_fragments = 0;
        uint64_t missing_fragments = 0;
        uint64_t timeout_events = 0;

        double lossPct() const {
            return expected_fragments == 0
                ? 0.0
                : static_cast<double>(missing_fragments) / expected_fragments;
        }
    };

    struct LossBucketStats {
        uint64_t start_unix_ms = 0;
        uint64_t expected_fragments = 0;
        uint64_t missing_fragments = 0;
        uint64_t timeout_events = 0;
        std::map<uint32_t, LossDuStats> per_du;

        double lossPct() const {
            return expected_fragments == 0
                ? 0.0
                : static_cast<double>(missing_fragments) / expected_fragments;
        }
    };

    struct LossRecentEvent {
        EventTag tag = 0;
        uint64_t event_ts_ns = 0;
        uint64_t observed_unix_ms = 0;
        uint64_t expected = 0;
        uint64_t attached = 0;
        std::vector<uint32_t> missing_du_ids;
    };

    struct LossStatsSnapshot {
        uint64_t bucket_sec = 300;
        uint64_t retention_sec = 48 * 60 * 60;
        uint64_t telemetry_dropped = 0;
        LossTotals totals;
        std::vector<LossBucketStats> buckets;
        std::vector<LossRecentEvent> recent_events;
    };

private:
    struct PendingEvent {
        EventTag tag = 0;
        std::set<uint32_t> expected_du_ids;
        std::set<uint64_t> expected_trigger_timestamps; // 与各 DU 期望触发时刻对齐校验
        // 一 DU 一片段的语义由 attached_du_ids 做 O(1) 去重判定；vector 作为 backing store
        // 让 CD 输出能按到达顺序写，保持与 old-cs-daq EventStore 的"来一条写一条"契约一致。
        // 同 event_tag 下同一 DU 的第二条 fragment 会在 addFragment 里被 DuplicateFragment 拒掉。
        std::vector<WaveformFragment> attached_fragments;
        std::unordered_set<uint32_t> attached_du_ids;
        std::chrono::steady_clock::time_point created_at;
        std::chrono::steady_clock::time_point deadline; // created_at + event_timeout_
        uint64_t trigger_timestamp_ns = 0;
        uint64_t loss_bucket_start_unix_ms = 0;
    };

    struct DeadlineEntry {
        std::chrono::steady_clock::time_point deadline;
        EventTag tag = 0;
    };

    struct DeadlineGreater {
        bool operator()(const DeadlineEntry& a, const DeadlineEntry& b) const {
            return a.deadline > b.deadline; // min-heap by deadline
        }
    };

public:
    // channel_tags - 允许参与组装的 DU 集合；event_timeout - 单 pending 最长等待
    explicit Builder(std::set<uint32_t> &channel_tags,
                     std::chrono::milliseconds event_timeout = std::chrono::milliseconds(5000),
                     std::size_t max_pending_events = 100000,
                     std::size_t loss_recent_event_limit = 5000);
    ~Builder() = default;

    void start();
    void stop();

    void beginEvent(EventTag tag,
                    const std::vector<uint32_t>& expected_du_ids,
                    const std::vector<uint64_t>& expected_trigger_timestamps,
                    uint64_t trigger_timestamp_ns);

    void addFragment(const SubFragmentHeader& header, uint8_t* data);

    std::vector<CompletedEvent> drainCompleted();
    std::vector<DiagnosticEvent> drainDiagnostics();

    uint64_t completedCount() const;
    uint64_t unmatchedCount() const;
    uint64_t timeoutCount() const;
    uint64_t pendingCount() const;
    std::size_t maxPendingEvents() const { return max_pending_events_; }

    uint64_t statRegisteredEvents() const { return stat_registered_events_.load(std::memory_order_relaxed); }
    uint64_t statExpectedFragments() const { return stat_expected_fragments_.load(std::memory_order_relaxed); }
    uint64_t statReceivedFragments() const { return stat_received_fragments_.load(std::memory_order_relaxed); }
    uint64_t statCompleteEvents() const { return stat_complete_events_.load(std::memory_order_relaxed); }
    uint64_t statTimeoutEvents() const { return stat_timeout_events_.load(std::memory_order_relaxed); }
    uint64_t statMissingOnTimeout() const { return stat_missing_on_timeout_.load(std::memory_order_relaxed); }
    uint64_t statPartialEvents() const { return stat_partial_events_.load(std::memory_order_relaxed); }
    uint64_t statPartialMissingFragments() const { return stat_partial_missing_fragments_.load(std::memory_order_relaxed); }
    std::map<uint32_t, uint64_t> statMissingOnTimeoutPerDu() const;
    LossStatsSnapshot lossStatsSnapshot(std::size_t top_du_per_bucket = 10,
                                        std::size_t recent_event_limit = 200) const;

    uint64_t diagNoFifoCount() const { return diag_no_fifo_.load(std::memory_order_relaxed); }
    uint64_t diagTimestampMismatchCount() const { return diag_timestamp_mismatch_.load(std::memory_order_relaxed); }
    uint64_t diagDuplicateCount() const { return diag_duplicate_.load(std::memory_order_relaxed); }
    uint64_t diagAdmissionRejectedCount() const { return diag_admission_rejected_.load(std::memory_order_relaxed); }

private:
    std::set<uint32_t> channel_tags_;
    std::chrono::milliseconds event_timeout_;
    std::size_t max_pending_events_;
    std::size_t loss_recent_event_limit_;
    mutable std::mutex state_mutex_;
    bool is_stop_ = true;
    std::unordered_map<EventTag, PendingEvent> pending_events_;
    std::vector<CompletedEvent> completed_events_;
    std::vector<DiagnosticEvent> diagnostics_;
    std::priority_queue<DeadlineEntry, std::vector<DeadlineEntry>, DeadlineGreater> deadline_heap_;
    uint64_t completed_count_ = 0;
    uint64_t unmatched_count_ = 0;
    uint64_t timeout_count_ = 0;

    std::atomic<uint64_t> stat_registered_events_{0};
    std::atomic<uint64_t> stat_expected_fragments_{0};
    std::atomic<uint64_t> stat_received_fragments_{0};
    std::atomic<uint64_t> stat_complete_events_{0};
    std::atomic<uint64_t> stat_timeout_events_{0};
    std::atomic<uint64_t> stat_missing_on_timeout_{0};
    std::atomic<uint64_t> stat_partial_events_{0};
    std::atomic<uint64_t> stat_partial_missing_fragments_{0};
    std::unordered_map<uint32_t, uint64_t> stat_missing_on_timeout_per_du_;

    LossTotals loss_totals_;
    std::map<uint64_t, LossBucketStats> loss_buckets_;
    std::deque<LossRecentEvent> loss_recent_events_;
    uint64_t loss_telemetry_dropped_ = 0;

    std::atomic<uint64_t> diag_no_fifo_{0};
    std::atomic<uint64_t> diag_timestamp_mismatch_{0};
    std::atomic<uint64_t> diag_duplicate_{0};
    std::atomic<uint64_t> diag_admission_rejected_{0};

private:
    void harvestExpiredFromHeapLocked();
    void emitDiagnosticLocked(DiagnosticType type,
                              EventTag tag,
                              uint32_t du_id,
                              TriggerID trigger_number,
                              uint64_t fragment_trigger_time,
                              uint64_t event_trigger_timestamp_ns);
    void emitTimeoutLocked(PendingEvent& event);
    void recordLossExpectedLocked(uint64_t bucket_start_unix_ms,
                                  const std::set<uint32_t>& expected_du_ids);
    void recordLossTimeoutLocked(const PendingEvent& event,
                                 const std::vector<uint32_t>& missing_du_ids,
                                 uint64_t observed_unix_ms);
    void pruneLossBucketsLocked(uint64_t newest_bucket_start_unix_ms);
    CompletedEvent makeCompletedEvent(PendingEvent&& event);
    bool isEventCompleted(const PendingEvent& event) const;
};
