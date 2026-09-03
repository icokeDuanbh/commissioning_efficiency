#pragma once

#include "gate_logic.h"
#include "duplicate_reject_logger.h"
#include "t3_filter.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace grand {

struct TriggerPipelineConfig {
    uint64_t time_cut_ns = 1000000000ULL;   // T2 时间戳按此粒度分桶得到 tm_id
    uint64_t timeout_ns = 1000000000ULL;      // Gate 桶超时 (ns)
    uint64_t timeout_cap_ns = 1000000000ULL; // 桶 deadline 硬上限
    uint64_t idle_gap_ns = 0ULL;             // 空闲间隙阈值（与 GateDeadlineMode 配合）
    uint64_t closed_bucket_retention_ns = 10000000000ULL; // 已关闭桶保留时长，用于去重/防重入
    uint64_t time_window_ns = 15000ULL;      // nhit 滑动窗口宽度
    int nhit_threshold = 5;                // 窗口内 distinct DU 达到此数则候选触发
    int trigger_type = 0;                  // 0/1/2：决定 T3Filter 是否走因果分支（见 pipeline 构造）
    bool duplicate_filter_enable = true;
    int duplicate_time_diff_ns = 100;
    int duplicate_history_size = 10;
    int duplicate_min_pair = 2;
    bool duplicate_reject_log_enable = false;
    std::string duplicate_reject_log_path;
    std::size_t duplicate_reject_log_queue_capacity = 4096;
    bool causal_window_enabled = false;      // 是否启用因果筛选（还需天线表）
    double causal_tolerance_ns = 100.0;
    std::string antenna_distances_file;      // DU 对间距表，因果模式用
    GateDeadlineMode deadline_mode = GateDeadlineMode::LastArrivalLegacy;
    bool extend_on_progress_only = true;
    std::vector<uint32_t> expected_du_ids;   // 传入 GateLogic，用于 advance/shadow
    GateAdvanceCompletionMode advance_completion_mode = GateAdvanceCompletionMode::Disabled;
    std::size_t max_pending_buckets = 1024;
    bool template_match_filter_enable = false;
    double template_match_coefficient = 0.0;
};

// 单桶闭合后经 nhit/过滤得到的判定结果，回调给上层
struct TriggerResult {
    uint64_t tm_id = 0;
    bool triggered = false;       // 是否满足触发条件（过滤后仍有足够 DU）
    std::size_t du_count = 0;   // 参与判定的 DU 数
    uint64_t timestamp_ns = 0;  // 代表性时间（实现取窗口内最早或约定值）
    std::vector<uint32_t> du_ids;
    std::vector<uint64_t> trigger_timestamps; // 与各 DU 对齐的触发时刻（T3Filter 后）
    std::vector<uint64_t> window_timestamps;  // 全窗口 hit timestamps（T3Filter 之前），供 DOTRIGGER 使用
    std::vector<uint32_t> window_du_ids;      // 全窗口 distinct DU IDs，供 DOTRIGGER 发送目标使用
};

using TriggerCallback = std::function<void(const TriggerResult&, const TmIdBucket&)>;
using TriggerResults = std::vector<TriggerResult>;

class TriggerPipeline {
public:
    using Config = TriggerPipelineConfig;

    explicit TriggerPipeline(const Config& config);
    ~TriggerPipeline();

    void start(); // 启动后台 processLoop，从 Gate 取就绪桶
    void stop();  // 停线程并 drain 剩余桶

    void addTimestamp(uint32_t du_id, uint64_t timestamp_ns); // 单点 T2，内部算 tm_id
    void addT2HitsBatch(uint32_t du_id, std::vector<Gp300T2Hit>&& hits);

    void setTriggerCallback(TriggerCallback callback); // 每桶处理完异步回调，非空才调用
    GateLogic::Stats getStats() const;

    struct T3FilterStats {
        uint64_t candidate_window_count = 0;
        uint64_t causal_eval_count = 0;
        uint64_t causal_input_detector_sum = 0;
        uint64_t causal_input_detector_max = 0;
        uint64_t causal_selected_detector_sum = 0;
        uint64_t causal_selected_detector_max = 0;
        uint64_t causal_reject_count = 0;
        uint64_t duplicate_reject_count = 0;
        uint64_t high_multiplicity_bypass_count = 0;
        uint64_t high_multiplicity_bypass_input_detector_sum = 0;
        uint64_t high_multiplicity_bypass_input_detector_max = 0;
        uint64_t high_multiplicity_bypass_unique_du_sum = 0;
        uint64_t high_multiplicity_bypass_unique_du_max = 0;
        uint64_t maximum_clique_time_count = 0;
        uint64_t maximum_clique_time_sum_ns = 0;
        uint64_t maximum_clique_time_max_ns = 0;
        uint64_t template_match_eval_count = 0;
        uint64_t template_match_reject_count = 0;
        uint64_t template_match_avg_sum_scaled = 0;
        uint64_t template_match_avg_max_scaled = 0;
    };

    T3FilterStats getT3FilterStats() const;

    uint64_t bucketWaitCount() const { return bucket_wait_count_.load(std::memory_order_relaxed); }
    uint64_t bucketWaitSumNs() const { return bucket_wait_sum_ns_.load(std::memory_order_relaxed); }
    uint64_t bucketWaitMaxNs() const { return bucket_wait_max_ns_.load(std::memory_order_relaxed); }

private:
    struct Hit {
        uint32_t du_id = 0;
        uint64_t timestamp_ns = 0;
        uint16_t trigger_channel = 0;
        uint16_t related_value = 0;
    };

public:
    using TestHit = Hit;
    // 单测/parity：不启线程，直接对 hits 做 nhit 窗口判定
    bool testOnly_checkNhitTriggerHits(const std::vector<TestHit>& hits,
                                       std::vector<uint32_t>& triggered_dus,
                                       std::vector<uint64_t>& trigger_timestamps);
    TriggerResults testOnly_collectTriggerResults(const std::vector<TestHit>& hits,
                                                  uint64_t tm_id = 0,
                                                  uint64_t bucket_timestamp_ns = 0);

private:
    Config config_;
    std::unique_ptr<GateLogic> gate_logic_;
    std::unique_ptr<T3Filter> t3_filter_;
    std::unique_ptr<DuplicateRejectLogger> duplicate_reject_logger_;

    std::thread process_thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> wake_epoch_{0};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    TriggerCallback trigger_callback_;

    std::atomic<uint64_t> bucket_wait_count_{0};
    std::atomic<uint64_t> bucket_wait_sum_ns_{0};
    std::atomic<uint64_t> bucket_wait_max_ns_{0};

    std::atomic<uint64_t> t3_filter_candidate_window_count_{0};
    std::atomic<uint64_t> t3_filter_causal_eval_count_{0};
    std::atomic<uint64_t> t3_filter_causal_input_detector_sum_{0};
    std::atomic<uint64_t> t3_filter_causal_input_detector_max_{0};
    std::atomic<uint64_t> t3_filter_causal_selected_detector_sum_{0};
    std::atomic<uint64_t> t3_filter_causal_selected_detector_max_{0};
    std::atomic<uint64_t> t3_filter_causal_reject_count_{0};
    std::atomic<uint64_t> t3_filter_duplicate_reject_count_{0};
    std::atomic<uint64_t> t3_filter_high_multiplicity_bypass_count_{0};
    std::atomic<uint64_t> t3_filter_high_multiplicity_bypass_input_detector_sum_{0};
    std::atomic<uint64_t> t3_filter_high_multiplicity_bypass_input_detector_max_{0};
    std::atomic<uint64_t> t3_filter_high_multiplicity_bypass_unique_du_sum_{0};
    std::atomic<uint64_t> t3_filter_high_multiplicity_bypass_unique_du_max_{0};
    std::atomic<uint64_t> t3_filter_maximum_clique_time_count_{0};
    std::atomic<uint64_t> t3_filter_maximum_clique_time_sum_ns_{0};
    std::atomic<uint64_t> t3_filter_maximum_clique_time_max_ns_{0};
    std::atomic<uint64_t> t3_filter_template_match_eval_count_{0};
    std::atomic<uint64_t> t3_filter_template_match_reject_count_{0};
    std::atomic<uint64_t> t3_filter_template_match_avg_sum_scaled_{0};
    std::atomic<uint64_t> t3_filter_template_match_avg_max_scaled_{0};

    void processLoop();
    TriggerResults processBucket(const TmIdBucket& bucket);
    bool checkNhitTriggerHits(const std::vector<Hit>& hits,
                              std::vector<uint32_t>& triggered_dus,
                              std::vector<uint64_t>& trigger_timestamps);
    TriggerResults collectTriggerResults(const std::vector<Hit>& hits,
                                         uint64_t tm_id,
                                         uint64_t bucket_timestamp_ns,
                                         bool stop_after_first_match);

    static uint64_t calculateTmId(uint64_t timestamp_ns, uint64_t time_cut_ns);
    static uint64_t steadyNowNs();
};

} // namespace grand
