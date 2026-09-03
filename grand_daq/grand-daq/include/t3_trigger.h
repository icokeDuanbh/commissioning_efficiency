#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <data_parser.h>

#include <trigger_pipeline.h>

namespace grand {

struct LegacyAppConfig;

// 与 LegacyAppConfig / YAML 对齐的运行时 T3 参数，再映射为 TriggerPipelineConfig
struct T3Config {
    uint64_t timeCutNs = 1000000000ULL;   // 分桶粒度 (ns) → tm_id
    uint64_t timeOutNs = 1000000000ULL;   // Gate 桶超时
    uint64_t timeoutCapNs = 1000000000ULL;
    uint64_t idleGapNs = 0ULL;
    uint64_t closedBucketRetentionNs = 10000000000ULL;
    uint64_t timeWindowNs = 2000ULL;      // nhit 重合窗口
    uint32_t triggerThresholdDus = 2;   // 最少 distinct DU 数（映射到 nhit_threshold）
    uint32_t triggerType = 1;
    bool duplicateFilterEnable = true;
    int duplicateTimeDiffNs = 100;
    int duplicateHistorySize = 10;
    int duplicateMinPair = 2;
    bool duplicateRejectLogEnable = false;
    std::string duplicateRejectLogPath;
    std::size_t duplicateRejectLogQueueCapacity = 4096;
    bool causalWindowEnabled = false;
    double causalToleranceNs = 100.0;
    std::string antennaDistancesFile;
    GateDeadlineMode deadlineMode = GateDeadlineMode::LastArrivalLegacy;
    bool extendOnProgressOnly = true;
    std::vector<uint32_t> expectedDuIds;
    GateAdvanceCompletionMode advanceCompletionMode = GateAdvanceCompletionMode::Disabled;
    bool templateMatchFilterEnable = false;
    double templateMatchCoefficient = 0.0;
};

// 实现见 app.cpp：从 global 段 + active DU 列表填 T3Config，带校验
T3Config buildT3RuntimeConfig(const LegacyAppConfig& app,
                              const std::vector<uint32_t>& active_du_ids);
TriggerPipelineConfig buildTriggerPipelineConfig(const T3Config& cfg);

class T3Trigger {
public:
    using TriggerResultCallback = std::function<void(const TriggerResult&)>;

    explicit T3Trigger(T3Config cfg);
    ~T3Trigger();

    void stop();

    // 该 DU 的一批 T2 hit，与 process_t2_payload 解析结果同形
    void onT2(uint32_t duId, std::vector<Gp300T2Hit>&& hits);

    void setTriggerResultCallback(TriggerResultCallback cb);

    uint64_t bucketWaitCount() const;
    uint64_t bucketWaitSumNs() const;
    uint64_t bucketWaitMaxNs() const;
    GateLogic::Stats getGateStats() const;
    TriggerPipeline::T3FilterStats getT3FilterStats() const;

private:
    T3Config cfg_;
    TriggerPipeline pipeline_;
    mutable std::mutex callback_mutex_;
    TriggerResultCallback result_callback_;
    std::atomic<bool> stopped_{false};
    void onPipelineTriggerResult(const TriggerResult& result, const TmIdBucket& bucket);
};

} // namespace grand

