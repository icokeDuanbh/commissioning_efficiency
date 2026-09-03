#pragma once

#include <data_parser.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace grand {

// 桶「到点关门」时间如何随新 hit 滑动/截断
enum class GateDeadlineMode {
    FirstArrivalFixed = 0,           // 以首包到达 + timeout 为固定截止
    FirstArrivalBoundedSliding = 1,  // 首包基准 + idle_gap 与 hard_cap 联合约束
    LastArrivalLegacy = 2,           // 与旧逻辑一致：按 last_progress + timeout
};

// 是否在关门条件外再算一层「影子凑齐」用于统计/观测
enum class GateAdvanceCompletionMode {
    Disabled = 0,
    ShadowOnly = 1, // 仅打 shadow_complete 标记，不改变正式关门时刻
};

struct HitData {
    uint32_t du_id = 0;
    uint64_t timestamp_ns = 0;
};

// 按 tm_id 分桶后的聚合状态：每个 DU 多条 T2 时间戳 + 关门调度字段
struct TmIdBucket {
    uint64_t tm_id = 0;
    std::map<uint32_t, std::vector<Gp300T2Hit>> t2_hits_by_du;
    std::unordered_map<uint32_t, std::unordered_set<uint64_t>> seen_timestamps_by_du; // 去重用
    uint64_t first_arrival_ns = 0;   // 桶内首条到达的本地时间
    uint64_t last_progress_ns = 0;   // 最近一次向桶内写入的本地时间
    uint64_t base_close_ns = 0;      // FirstArrival 模式下的固定截止基线
    uint64_t hard_cap_ns = 0;        // 绝对最晚关门（timeout_cap）
    uint64_t close_at_ns = 0;        // 当前生效的关门时刻
    uint64_t shadow_complete_ns = 0; // ShadowOnly：预期 DU 都越过本 tm_id 时打点
    bool closed = false;

    bool isReady(uint64_t now_ns) const { return close_at_ns != 0 && now_ns >= close_at_ns; }
};

class GateLogic {
public:
    struct Config {
        uint64_t timeout_ns = 1000000000ULL;              // 桶「宽限期」主尺度 (ns)
        uint64_t timeout_cap_ns = 1000000000ULL;          // 从首到达起硬上限，close 不晚于此
        uint64_t idle_gap_ns = 0ULL;                      // BoundedSliding：无新 hit 多久可提前关
        uint64_t closed_bucket_retention_ns = 10000000000ULL; // 超时强关后防重复判定的保留窗
        GateDeadlineMode deadline_mode = GateDeadlineMode::LastArrivalLegacy;
        bool extend_on_progress_only = true;              // true：仅在有新数据时延长 deadline
        std::vector<uint32_t> expected_du_ids;            // 参与 advance/shadow 判定的 DU 列表
        GateAdvanceCompletionMode advance_completion_mode = GateAdvanceCompletionMode::Disabled;
        std::size_t max_pending_buckets = 1024;           // pending 桶数上限
        std::function<uint64_t()> now_ns_fn;                // 可注入的「当前 ns」，默认可 steady_clock
    };

    struct Stats {
        std::size_t pending_bucket_count = 0;
        uint64_t bucket_timeout_count = 0;
        uint64_t force_ready_count = 0;
        uint64_t shadow_complete_count = 0;
        uint64_t late_arrival_after_close_count = 0;
        uint64_t late_arrival_guard_count = 0;          // 桶已关闭且在 guard 中
        uint64_t late_arrival_ingress_close_count = 0;  // 桶在 pending 中但已超时，由 ingress 触发关闭
        uint64_t late_arrival_age_max_ns = 0;
        uint64_t late_arrival_age_sum_ns = 0;
    };

    explicit GateLogic(const Config& config);
    ~GateLogic() = default;

    // 覆盖写入某 DU 在该 tm_id 下的整批 T2（与 addT2Timestamp 二选一语义由调用方保证）
    void setT2Hits(uint64_t tm_id, uint32_t du_id, std::vector<Gp300T2Hit>&& hits);
    void addT2Timestamp(uint64_t tm_id, uint32_t du_id, uint64_t timestamp_ns);

    // 返回本轮已到关门时刻的 tm_id 列表（不弹出）
    std::vector<uint64_t> pollReady();
    std::vector<TmIdBucket> popAllReadySorted();
    std::optional<TmIdBucket> forcePopOldest();
    std::optional<TmIdBucket> getBucketSnapshot(uint64_t tm_id) const;
    std::optional<uint64_t> nextDeadlineNs() const;

    Stats getStats() const;
    void reset();

private:
    Config config_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, TmIdBucket> pending_;
    std::set<uint64_t> ready_tm_ids_;
    std::unordered_set<uint64_t> forced_ready_tm_ids_;
    std::unordered_map<uint64_t, uint64_t> timeout_closed_guard_expires_ns_; // 强关桶的防重入过期时间
    std::unordered_map<uint32_t, uint64_t> expected_du_last_tm_id_;        // advance 观测用
    Stats stats_{};

    uint64_t nowNs() const;
    void initializeBucketTimingLocked(TmIdBucket& bucket, uint64_t now_ns);
    void recomputeCloseDeadlineLocked(TmIdBucket& bucket);
    bool wouldCompleteByAdvanceLocked(uint64_t tm_id) const;
    void maybeMarkShadowCompletionLocked(uint64_t now_ns);
    void observeDuTmIdLocked(uint32_t du_id, uint64_t tm_id);
    void cleanupClosedGuardLocked(uint64_t now_ns);
    bool closeBucketIfTimedOutForIngressLocked(uint64_t tm_id, uint64_t now_ns,
                                                uint64_t* out_close_at_ns = nullptr);
    void rememberTimeoutClosedBucketLocked(const TmIdBucket& bucket);
    void ensureCapacityLocked();
};

} // namespace grand
