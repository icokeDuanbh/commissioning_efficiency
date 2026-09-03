#include "gate_logic.h"

#include <algorithm>
#include <chrono>
#include <iostream>

namespace grand {

namespace {

uint64_t saturatingAdd(uint64_t lhs, uint64_t rhs) {
    return (UINT64_MAX - lhs < rhs) ? UINT64_MAX : (lhs + rhs);
}

void logLateArrival(uint64_t late_count, uint64_t tm_id, uint32_t du_id,
                    uint64_t age_ns, uint64_t close_at_ns, uint64_t ts_count,
                    const char* path) {
    if (late_count <= 100 || (late_count % 1000 == 0)) {
        std::cerr << "LATE_ARRIVAL"
                  << " path=" << path
                  << " du=" << du_id
                  << " tmId=" << tm_id
                  << " age_ms=" << (age_ns / 1000000)
                  << " close_at_ns=" << close_at_ns
                  << " ts_count=" << ts_count
                  << " total_late=" << late_count
                  << std::endl;
    }
}

} // namespace

// 校正 cap/retention，expected_du_ids 去重排序
GateLogic::GateLogic(const Config& config) : config_(config) {
    if (config_.timeout_cap_ns < config_.timeout_ns) {
        config_.timeout_cap_ns = config_.timeout_ns;
    }
    if (config_.closed_bucket_retention_ns == 0) {
        config_.closed_bucket_retention_ns = 1;
    }
    std::sort(config_.expected_du_ids.begin(), config_.expected_du_ids.end());
    config_.expected_du_ids.erase(
        std::unique(config_.expected_du_ids.begin(), config_.expected_du_ids.end()),
        config_.expected_du_ids.end());
}

uint64_t GateLogic::nowNs() const {
    if (config_.now_ns_fn) {
        return config_.now_ns_fn();
    }
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void GateLogic::initializeBucketTimingLocked(TmIdBucket& bucket, uint64_t now_ns) {
    bucket.first_arrival_ns = now_ns;
    bucket.last_progress_ns = now_ns;
    bucket.base_close_ns = saturatingAdd(now_ns, config_.timeout_ns);
    bucket.hard_cap_ns = saturatingAdd(now_ns, config_.timeout_cap_ns);
    recomputeCloseDeadlineLocked(bucket);
}

void GateLogic::recomputeCloseDeadlineLocked(TmIdBucket& bucket) {
    switch (config_.deadline_mode) {
        case GateDeadlineMode::LastArrivalLegacy:
            bucket.close_at_ns = saturatingAdd(bucket.last_progress_ns, config_.timeout_ns);
            return;
        case GateDeadlineMode::FirstArrivalFixed:
            bucket.close_at_ns = bucket.base_close_ns;
            return;
        case GateDeadlineMode::FirstArrivalBoundedSliding:
            break;
    }

    const uint64_t idle_close_ns = saturatingAdd(bucket.last_progress_ns, config_.idle_gap_ns);
    const uint64_t candidate_ns = std::max(bucket.base_close_ns, idle_close_ns);
    bucket.close_at_ns = std::min(bucket.hard_cap_ns, candidate_ns);
}

void GateLogic::observeDuTmIdLocked(uint32_t du_id, uint64_t tm_id) {
    if (config_.advance_completion_mode == GateAdvanceCompletionMode::Disabled ||
        config_.expected_du_ids.empty()) {
        return;
    }
    if (std::find(config_.expected_du_ids.begin(), config_.expected_du_ids.end(), du_id) ==
        config_.expected_du_ids.end()) {
        return;
    }
    auto it = expected_du_last_tm_id_.find(du_id);
    if (it == expected_du_last_tm_id_.end() || tm_id > it->second) {
        expected_du_last_tm_id_[du_id] = tm_id;
    }
}

bool GateLogic::wouldCompleteByAdvanceLocked(uint64_t tm_id) const {
    if (config_.advance_completion_mode != GateAdvanceCompletionMode::ShadowOnly ||
        config_.expected_du_ids.empty()) {
        return false;
    }
    for (uint32_t du_id : config_.expected_du_ids) {
        auto it = expected_du_last_tm_id_.find(du_id);
        if (it == expected_du_last_tm_id_.end() || it->second <= tm_id) {
            return false;
        }
    }
    return true;
}

void GateLogic::maybeMarkShadowCompletionLocked(uint64_t now_ns) {
    if (config_.advance_completion_mode != GateAdvanceCompletionMode::ShadowOnly ||
        config_.expected_du_ids.empty()) {
        return;
    }
    for (auto& [tm_id, bucket] : pending_) {
        if (bucket.shadow_complete_ns != 0) {
            continue;
        }
        if (!wouldCompleteByAdvanceLocked(tm_id)) {
            continue;
        }
        bucket.shadow_complete_ns = now_ns;
        stats_.shadow_complete_count++;
    }
}

void GateLogic::cleanupClosedGuardLocked(uint64_t now_ns) {
    for (auto it = timeout_closed_guard_expires_ns_.begin();
         it != timeout_closed_guard_expires_ns_.end();) {
        if (it->second <= now_ns) {
            it = timeout_closed_guard_expires_ns_.erase(it);
        } else {
            ++it;
        }
    }
}

void GateLogic::rememberTimeoutClosedBucketLocked(const TmIdBucket& bucket) {
    if (bucket.close_at_ns == 0) {
        return;
    }
    timeout_closed_guard_expires_ns_[bucket.tm_id] =
        saturatingAdd(bucket.close_at_ns, config_.closed_bucket_retention_ns);
}

bool GateLogic::closeBucketIfTimedOutForIngressLocked(uint64_t tm_id, uint64_t now_ns,
                                                       uint64_t* out_close_at_ns) {
    auto it = pending_.find(tm_id);
    if (it == pending_.end()) {
        return false;
    }
    TmIdBucket& bucket = it->second;
    if (bucket.close_at_ns == 0 || now_ns < bucket.close_at_ns) {
        return false;
    }
    if (out_close_at_ns) {
        *out_close_at_ns = bucket.close_at_ns;
    }
    ready_tm_ids_.insert(tm_id);
    rememberTimeoutClosedBucketLocked(bucket);
    stats_.late_arrival_after_close_count++;
    return true;
}

void GateLogic::ensureCapacityLocked() {
    if (pending_.size() <= config_.max_pending_buckets) {
        return;
    }

    bool found_oldest = false;
    uint64_t oldest_tm_id = 0;
    uint64_t oldest_arrival = UINT64_MAX;
    for (const auto& [tm_id, bucket] : pending_) {
        if (!found_oldest || bucket.first_arrival_ns < oldest_arrival) {
            found_oldest = true;
            oldest_arrival = bucket.first_arrival_ns;
            oldest_tm_id = tm_id;
        }
    }
    if (found_oldest) {
        ready_tm_ids_.insert(oldest_tm_id);
        forced_ready_tm_ids_.insert(oldest_tm_id);
        stats_.force_ready_count++;
    }
}

void GateLogic::setT2Hits(uint64_t tm_id, uint32_t du_id, std::vector<Gp300T2Hit>&& hits) {
    if (hits.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t now_ns = nowNs();
    cleanupClosedGuardLocked(now_ns);
    {
        auto guard_it = timeout_closed_guard_expires_ns_.find(tm_id);
        if (guard_it != timeout_closed_guard_expires_ns_.end()) {
            stats_.late_arrival_after_close_count++;
            stats_.late_arrival_guard_count++;
            const uint64_t approx_close = (guard_it->second > config_.closed_bucket_retention_ns)
                ? (guard_it->second - config_.closed_bucket_retention_ns) : 0;
            const uint64_t age_ns = (now_ns > approx_close) ? (now_ns - approx_close) : 0;
            if (age_ns > stats_.late_arrival_age_max_ns) {
                stats_.late_arrival_age_max_ns = age_ns;
            }
            stats_.late_arrival_age_sum_ns += age_ns;
            logLateArrival(stats_.late_arrival_after_close_count, tm_id, du_id,
                           age_ns, approx_close, hits.size(), "guard");
            return;
        }
    }
    {
        uint64_t bucket_close_at = 0;
        if (closeBucketIfTimedOutForIngressLocked(tm_id, now_ns, &bucket_close_at)) {
            stats_.late_arrival_ingress_close_count++;
            const uint64_t age_ns = (now_ns > bucket_close_at) ? (now_ns - bucket_close_at) : 0;
            if (age_ns > stats_.late_arrival_age_max_ns) {
                stats_.late_arrival_age_max_ns = age_ns;
            }
            stats_.late_arrival_age_sum_ns += age_ns;
            logLateArrival(stats_.late_arrival_after_close_count, tm_id, du_id,
                           age_ns, bucket_close_at, hits.size(), "ingress_close");
            return;
        }
    }
    observeDuTmIdLocked(du_id, tm_id);

    auto [it, inserted] = pending_.try_emplace(tm_id);
    TmIdBucket& bucket = it->second;
    if (inserted) {
        bucket.tm_id = tm_id;
        initializeBucketTimingLocked(bucket, now_ns);
    }

    auto& seen = bucket.seen_timestamps_by_du[du_id];
    std::unordered_set<uint64_t> batch_seen;
    std::vector<Gp300T2Hit> canonical;
    canonical.reserve(hits.size());
    bool progress = false;
    for (Gp300T2Hit& hit : hits) {
        if (!batch_seen.insert(hit.timestamp_ns).second) {
            continue;
        }
        canonical.push_back(hit);
        if (seen.insert(hit.timestamp_ns).second) {
            progress = true;
        }
    }
    if (canonical.empty()) {
        bucket.t2_hits_by_du.erase(du_id);
    } else {
        bucket.t2_hits_by_du[du_id] = std::move(canonical);
    }

    if (config_.deadline_mode == GateDeadlineMode::LastArrivalLegacy ||
        !config_.extend_on_progress_only || progress) {
        bucket.last_progress_ns = now_ns;
        recomputeCloseDeadlineLocked(bucket);
    }
    maybeMarkShadowCompletionLocked(now_ns);
    if (bucket.isReady(now_ns)) {
        ready_tm_ids_.insert(tm_id);
    }
    ensureCapacityLocked();
}

void GateLogic::addT2Timestamp(uint64_t tm_id, uint32_t du_id, uint64_t timestamp_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t now_ns = nowNs();
    cleanupClosedGuardLocked(now_ns);
    {
        auto guard_it = timeout_closed_guard_expires_ns_.find(tm_id);
        if (guard_it != timeout_closed_guard_expires_ns_.end()) {
            stats_.late_arrival_after_close_count++;
            stats_.late_arrival_guard_count++;
            const uint64_t approx_close = (guard_it->second > config_.closed_bucket_retention_ns)
                ? (guard_it->second - config_.closed_bucket_retention_ns) : 0;
            const uint64_t age_ns = (now_ns > approx_close) ? (now_ns - approx_close) : 0;
            if (age_ns > stats_.late_arrival_age_max_ns) {
                stats_.late_arrival_age_max_ns = age_ns;
            }
            stats_.late_arrival_age_sum_ns += age_ns;
            logLateArrival(stats_.late_arrival_after_close_count, tm_id, du_id,
                           age_ns, approx_close, 1, "guard");
            return;
        }
    }
    {
        uint64_t bucket_close_at = 0;
        if (closeBucketIfTimedOutForIngressLocked(tm_id, now_ns, &bucket_close_at)) {
            stats_.late_arrival_ingress_close_count++;
            const uint64_t age_ns = (now_ns > bucket_close_at) ? (now_ns - bucket_close_at) : 0;
            if (age_ns > stats_.late_arrival_age_max_ns) {
                stats_.late_arrival_age_max_ns = age_ns;
            }
            stats_.late_arrival_age_sum_ns += age_ns;
            logLateArrival(stats_.late_arrival_after_close_count, tm_id, du_id,
                           age_ns, bucket_close_at, 1, "ingress_close");
            return;
        }
    }
    observeDuTmIdLocked(du_id, tm_id);

    auto [it, inserted] = pending_.try_emplace(tm_id);
    TmIdBucket& bucket = it->second;
    if (inserted) {
        bucket.tm_id = tm_id;
        initializeBucketTimingLocked(bucket, now_ns);
    }

    auto& seen = bucket.seen_timestamps_by_du[du_id];
    bool progress = false;
    if (seen.insert(timestamp_ns).second) {
        bucket.t2_hits_by_du[du_id].push_back(Gp300T2Hit{timestamp_ns, 0, 0});
        progress = true;
    }

    if (config_.deadline_mode == GateDeadlineMode::LastArrivalLegacy ||
        !config_.extend_on_progress_only || progress) {
        bucket.last_progress_ns = now_ns;
        recomputeCloseDeadlineLocked(bucket);
    }
    maybeMarkShadowCompletionLocked(now_ns);
    if (bucket.isReady(now_ns)) {
        ready_tm_ids_.insert(tm_id);
    }
    ensureCapacityLocked();
}

std::vector<uint64_t> GateLogic::pollReady() {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t now_ns = nowNs();
    maybeMarkShadowCompletionLocked(now_ns);
    for (const auto& [tm_id, bucket] : pending_) {
        if (bucket.isReady(now_ns)) {
            ready_tm_ids_.insert(tm_id);
        }
    }
    cleanupClosedGuardLocked(now_ns);
    return std::vector<uint64_t>(ready_tm_ids_.begin(), ready_tm_ids_.end());
}

std::vector<TmIdBucket> GateLogic::popAllReadySorted() {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t now_ns = nowNs();
    maybeMarkShadowCompletionLocked(now_ns);
    for (const auto& [tm_id, bucket] : pending_) {
        if (bucket.isReady(now_ns)) {
            ready_tm_ids_.insert(tm_id);
        }
    }

    std::vector<TmIdBucket> out;
    out.reserve(ready_tm_ids_.size());
    for (uint64_t tm_id : ready_tm_ids_) {
        auto it = pending_.find(tm_id);
        if (it == pending_.end()) {
            forced_ready_tm_ids_.erase(tm_id);
            continue;
        }
        const bool forced_ready = forced_ready_tm_ids_.erase(tm_id) > 0;
        if (!forced_ready && !it->second.isReady(now_ns)) {
            continue;
        }
        it->second.closed = true;
        if (it->second.close_at_ns != 0 && now_ns >= it->second.close_at_ns) {
            stats_.bucket_timeout_count++;
            rememberTimeoutClosedBucketLocked(it->second);
        }
        out.push_back(std::move(it->second));
        pending_.erase(it);
    }
    ready_tm_ids_.clear();
    cleanupClosedGuardLocked(now_ns);
    return out;
}

std::optional<TmIdBucket> GateLogic::forcePopOldest() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.empty()) {
        return std::nullopt;
    }
    const uint64_t now_ns = nowNs();

    bool found_oldest = false;
    uint64_t oldest_tm_id = 0;
    uint64_t oldest_arrival = UINT64_MAX;
    for (const auto& [tm_id, bucket] : pending_) {
        if (!found_oldest || bucket.first_arrival_ns < oldest_arrival) {
            found_oldest = true;
            oldest_arrival = bucket.first_arrival_ns;
            oldest_tm_id = tm_id;
        }
    }
    if (!found_oldest) {
        return std::nullopt;
    }

    auto it = pending_.find(oldest_tm_id);
    if (it == pending_.end()) {
        return std::nullopt;
    }
    it->second.closed = true;
    if (it->second.close_at_ns != 0 && now_ns >= it->second.close_at_ns) {
        rememberTimeoutClosedBucketLocked(it->second);
    }
    TmIdBucket out = std::move(it->second);
    pending_.erase(it);
    ready_tm_ids_.erase(oldest_tm_id);
    forced_ready_tm_ids_.erase(oldest_tm_id);
    stats_.force_ready_count++;
    return out;
}

std::optional<TmIdBucket> GateLogic::getBucketSnapshot(uint64_t tm_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_.find(tm_id);
    if (it == pending_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<uint64_t> GateLogic::nextDeadlineNs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_tm_ids_.empty()) {
        return 0ULL;
    }
    uint64_t next = UINT64_MAX;
    for (const auto& [tm_id, bucket] : pending_) {
        (void)tm_id;
        if (bucket.close_at_ns != 0 && bucket.close_at_ns < next) {
            next = bucket.close_at_ns;
        }
    }
    if (next == UINT64_MAX) {
        return std::nullopt;
    }
    return next;
}

GateLogic::Stats GateLogic::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats out = stats_;
    out.pending_bucket_count = pending_.size();
    return out;
}

void GateLogic::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();
    ready_tm_ids_.clear();
    forced_ready_tm_ids_.clear();
    timeout_closed_guard_expires_ns_.clear();
    expected_du_last_tm_id_.clear();
    stats_ = Stats{};
}

} // namespace grand
