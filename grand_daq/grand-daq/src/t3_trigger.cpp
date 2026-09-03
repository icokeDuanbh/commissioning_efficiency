#include <t3_trigger.h>

#include <stdexcept>
#include <utility>
#include <vector>

namespace grand {

// 把 T3Config 摊平到 TriggerPipelineConfig；triggerType>2 直接抛 invalid_argument
TriggerPipelineConfig buildTriggerPipelineConfig(const T3Config& cfg) {
    if (cfg.triggerType > 2) {
        throw std::invalid_argument("unsupported t3 trigger type");
    }
    TriggerPipelineConfig out;
    out.time_cut_ns = cfg.timeCutNs;
    out.timeout_ns = cfg.timeOutNs;
    out.timeout_cap_ns = cfg.timeoutCapNs;
    out.idle_gap_ns = cfg.idleGapNs;
    out.closed_bucket_retention_ns = cfg.closedBucketRetentionNs;
    out.time_window_ns = cfg.timeWindowNs;
    out.nhit_threshold = static_cast<int>(cfg.triggerThresholdDus);
    out.trigger_type = static_cast<int>(cfg.triggerType);
    out.duplicate_filter_enable = cfg.duplicateFilterEnable;
    out.duplicate_time_diff_ns = cfg.duplicateTimeDiffNs;
    out.duplicate_history_size = cfg.duplicateHistorySize;
    out.duplicate_min_pair = cfg.duplicateMinPair;
    out.duplicate_reject_log_enable = cfg.duplicateRejectLogEnable;
    out.duplicate_reject_log_path = cfg.duplicateRejectLogPath;
    out.duplicate_reject_log_queue_capacity = cfg.duplicateRejectLogQueueCapacity;
    out.causal_window_enabled = cfg.causalWindowEnabled;
    out.causal_tolerance_ns = cfg.causalToleranceNs;
    out.antenna_distances_file = cfg.antennaDistancesFile;
    out.deadline_mode = cfg.deadlineMode;
    out.extend_on_progress_only = cfg.extendOnProgressOnly;
    out.expected_du_ids = cfg.expectedDuIds;
    out.advance_completion_mode = cfg.advanceCompletionMode;
    out.template_match_filter_enable = cfg.templateMatchFilterEnable;
    out.template_match_coefficient = cfg.templateMatchCoefficient;
    return out;
}

// 构造即启动 TriggerPipeline 并挂上本类回调，把桶结果转成单参数 TriggerResult
T3Trigger::T3Trigger(T3Config cfg) : cfg_(std::move(cfg)), pipeline_(buildTriggerPipelineConfig(cfg_)) {
    pipeline_.setTriggerCallback(
        [this](const TriggerResult& result, const TmIdBucket& bucket) {
            onPipelineTriggerResult(result, bucket);
        });
    pipeline_.start();
}

T3Trigger::~T3Trigger() { stop(); }

void T3Trigger::stop() {
    if (stopped_.exchange(true)) {
        return;
    }
    pipeline_.stop();
}

void T3Trigger::onT2(uint32_t duId, std::vector<Gp300T2Hit>&& hits) {
    if (stopped_.load(std::memory_order_relaxed) || hits.empty()) {
        return;
    }
    pipeline_.addT2HitsBatch(duId, std::move(hits));
}

void T3Trigger::setTriggerResultCallback(TriggerResultCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    result_callback_ = std::move(cb);
}

// 忽略 bucket，仅把 result 拷到用户回调（若已 set）
void T3Trigger::onPipelineTriggerResult(const TriggerResult& result, const TmIdBucket& bucket) {
    (void)bucket;
    TriggerResultCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = result_callback_;
    }
    if (cb) {
        cb(result);
    }
}

uint64_t T3Trigger::bucketWaitCount() const { return pipeline_.bucketWaitCount(); }
uint64_t T3Trigger::bucketWaitSumNs() const { return pipeline_.bucketWaitSumNs(); }
uint64_t T3Trigger::bucketWaitMaxNs() const { return pipeline_.bucketWaitMaxNs(); }
GateLogic::Stats T3Trigger::getGateStats() const { return pipeline_.getStats(); }
TriggerPipeline::T3FilterStats T3Trigger::getT3FilterStats() const {
    return pipeline_.getT3FilterStats();
}

} // namespace grand

