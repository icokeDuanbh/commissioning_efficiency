#include "trigger_pipeline.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

namespace {

constexpr uint64_t kTemplateMatchAvgScale = 10000ULL;
constexpr uint64_t kTemplateMatchSampleLogInterval = 10000ULL;

void updateAtomicMax(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

} // namespace

namespace grand {

// steady_clock 纳秒，与 Gate 默认 now 无关时可对照用
uint64_t TriggerPipeline::steadyNowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint64_t TriggerPipeline::calculateTmId(uint64_t timestamp_ns, uint64_t time_cut_ns) {
    if (time_cut_ns == 0) {
        return 0;
    }
    return timestamp_ns / time_cut_ns;
}

// 组装 GateLogic.Config 与 T3FilterConfig（trigger_type==2 时开因果滤波）
TriggerPipeline::TriggerPipeline(const Config& config) : config_(config) {
    GateLogic::Config gate_cfg;
    gate_cfg.timeout_ns = config.timeout_ns;
    gate_cfg.timeout_cap_ns = config.timeout_cap_ns;
    gate_cfg.idle_gap_ns = config.idle_gap_ns;
    gate_cfg.closed_bucket_retention_ns = config.closed_bucket_retention_ns;
    gate_cfg.deadline_mode = config.deadline_mode;
    gate_cfg.extend_on_progress_only = config.extend_on_progress_only;
    gate_cfg.expected_du_ids = config.expected_du_ids;
    gate_cfg.advance_completion_mode = config.advance_completion_mode;
    gate_cfg.max_pending_buckets = config.max_pending_buckets;
    gate_logic_ = std::make_unique<GateLogic>(gate_cfg);

    T3FilterConfig filter_cfg;
    filter_cfg.duplicate_filter_enable = config.duplicate_filter_enable;
    filter_cfg.duplicate_time_diff_ns = config.duplicate_time_diff_ns;
    filter_cfg.duplicate_history_size = static_cast<std::size_t>(config.duplicate_history_size);
    filter_cfg.duplicate_min_pair = config.duplicate_min_pair;
    filter_cfg.causal_filter_enable = config.causal_window_enabled && (config.trigger_type == 2);
    filter_cfg.causal_tolerance_ns = config.causal_tolerance_ns;
    filter_cfg.antenna_distances_file = config.antenna_distances_file;
    filter_cfg.nhit_threshold = config.nhit_threshold;
    filter_cfg.template_match_filter_enable = config.template_match_filter_enable;
    filter_cfg.template_match_coefficient = config.template_match_coefficient;
    t3_filter_ = std::make_unique<T3Filter>(filter_cfg);
    if (config.duplicate_reject_log_enable) {
        duplicate_reject_logger_ = std::make_unique<DuplicateRejectLogger>(
            config.duplicate_reject_log_path,
            config.duplicate_reject_log_queue_capacity);
    }
}

TriggerPipeline::~TriggerPipeline() { stop(); }

void TriggerPipeline::start() {
    if (running_.exchange(true)) {
        return;
    }
    process_thread_ = std::thread(&TriggerPipeline::processLoop, this);
}

void TriggerPipeline::stop() {
    const bool was_running = running_.exchange(false);
    if (was_running) {
        wake_epoch_.fetch_add(1, std::memory_order_relaxed);
        cv_.notify_all();
        if (process_thread_.joinable()) {
            process_thread_.join();
        }

        while (auto bucket = gate_logic_->forcePopOldest()) {
            TriggerResults results = processBucket(*bucket);
            TriggerCallback cb;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cb = trigger_callback_;
            }
            if (!cb) continue;
            for (const auto& result : results) {
                cb(result, *bucket);
            }
        }
    }
    if (duplicate_reject_logger_) {
        duplicate_reject_logger_->stop();
    }
}

void TriggerPipeline::addTimestamp(uint32_t du_id, uint64_t timestamp_ns) {
    const uint64_t tm_id = calculateTmId(timestamp_ns, config_.time_cut_ns);
    gate_logic_->addT2Timestamp(tm_id, du_id, timestamp_ns);
    wake_epoch_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_one();
}

void TriggerPipeline::addT2HitsBatch(uint32_t du_id, std::vector<Gp300T2Hit>&& hits) {
    if (hits.empty()) {
        return;
    }

    const uint64_t first_tm_id = calculateTmId(hits.front().timestamp_ns, config_.time_cut_ns);
    bool all_same = true;
    for (std::size_t i = 1; i < hits.size(); ++i) {
        if (calculateTmId(hits[i].timestamp_ns, config_.time_cut_ns) != first_tm_id) {
            all_same = false;
            break;
        }
    }

    if (all_same) {
        gate_logic_->setT2Hits(first_tm_id, du_id, std::move(hits));
    } else {
        std::map<uint64_t, std::vector<Gp300T2Hit>> by_tm_id;
        for (Gp300T2Hit& hit : hits) {
            by_tm_id[calculateTmId(hit.timestamp_ns, config_.time_cut_ns)].push_back(hit);
        }
        for (auto& [tm_id, batch] : by_tm_id) {
            gate_logic_->setT2Hits(tm_id, du_id, std::move(batch));
        }
    }

    wake_epoch_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_one();
}

void TriggerPipeline::setTriggerCallback(TriggerCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    trigger_callback_ = std::move(callback);
}

GateLogic::Stats TriggerPipeline::getStats() const { return gate_logic_->getStats(); }

TriggerPipeline::T3FilterStats TriggerPipeline::getT3FilterStats() const {
    T3FilterStats out{};
    out.candidate_window_count = t3_filter_candidate_window_count_.load(std::memory_order_relaxed);
    out.causal_eval_count = t3_filter_causal_eval_count_.load(std::memory_order_relaxed);
    out.causal_input_detector_sum =
        t3_filter_causal_input_detector_sum_.load(std::memory_order_relaxed);
    out.causal_input_detector_max =
        t3_filter_causal_input_detector_max_.load(std::memory_order_relaxed);
    out.causal_selected_detector_sum =
        t3_filter_causal_selected_detector_sum_.load(std::memory_order_relaxed);
    out.causal_selected_detector_max =
        t3_filter_causal_selected_detector_max_.load(std::memory_order_relaxed);
    out.causal_reject_count = t3_filter_causal_reject_count_.load(std::memory_order_relaxed);
    out.duplicate_reject_count =
        t3_filter_duplicate_reject_count_.load(std::memory_order_relaxed);
    out.high_multiplicity_bypass_count =
        t3_filter_high_multiplicity_bypass_count_.load(std::memory_order_relaxed);
    out.high_multiplicity_bypass_input_detector_sum =
        t3_filter_high_multiplicity_bypass_input_detector_sum_.load(std::memory_order_relaxed);
    out.high_multiplicity_bypass_input_detector_max =
        t3_filter_high_multiplicity_bypass_input_detector_max_.load(std::memory_order_relaxed);
    out.high_multiplicity_bypass_unique_du_sum =
        t3_filter_high_multiplicity_bypass_unique_du_sum_.load(std::memory_order_relaxed);
    out.high_multiplicity_bypass_unique_du_max =
        t3_filter_high_multiplicity_bypass_unique_du_max_.load(std::memory_order_relaxed);
    out.maximum_clique_time_count =
        t3_filter_maximum_clique_time_count_.load(std::memory_order_relaxed);
    out.maximum_clique_time_sum_ns =
        t3_filter_maximum_clique_time_sum_ns_.load(std::memory_order_relaxed);
    out.maximum_clique_time_max_ns =
        t3_filter_maximum_clique_time_max_ns_.load(std::memory_order_relaxed);
    out.template_match_eval_count =
        t3_filter_template_match_eval_count_.load(std::memory_order_relaxed);
    out.template_match_reject_count =
        t3_filter_template_match_reject_count_.load(std::memory_order_relaxed);
    out.template_match_avg_sum_scaled =
        t3_filter_template_match_avg_sum_scaled_.load(std::memory_order_relaxed);
    out.template_match_avg_max_scaled =
        t3_filter_template_match_avg_max_scaled_.load(std::memory_order_relaxed);
    return out;
}

bool TriggerPipeline::testOnly_checkNhitTriggerHits(const std::vector<TestHit>& hits,
                                                    std::vector<uint32_t>& triggered_dus,
                                                    std::vector<uint64_t>& trigger_timestamps) {
    return checkNhitTriggerHits(hits, triggered_dus, trigger_timestamps);
}

TriggerResults TriggerPipeline::testOnly_collectTriggerResults(const std::vector<TestHit>& hits,
                                                               uint64_t tm_id,
                                                               uint64_t bucket_timestamp_ns) {
    return collectTriggerResults(hits, tm_id, bucket_timestamp_ns, false);
}

// 等 pollReady 非空则 popAllReadySorted，每桶 processBucket 再回调
void TriggerPipeline::processLoop() {
    while (running_.load(std::memory_order_relaxed)) {
        const uint64_t prev_epoch = wake_epoch_.load(std::memory_order_relaxed);
        const std::vector<uint64_t> ready = gate_logic_->pollReady();
        if (ready.empty()) {
            std::unique_lock<std::mutex> lock(mutex_);
            const std::optional<uint64_t> next_deadline = gate_logic_->nextDeadlineNs();
            if (!next_deadline.has_value()) {
                cv_.wait(lock, [&]() {
                    return !running_.load(std::memory_order_relaxed) ||
                           wake_epoch_.load(std::memory_order_relaxed) != prev_epoch;
                });
            } else if (*next_deadline == 0ULL) {
                cv_.wait_for(lock, std::chrono::milliseconds(1), [&]() {
                    return !running_.load(std::memory_order_relaxed) ||
                           wake_epoch_.load(std::memory_order_relaxed) != prev_epoch;
                });
            } else {
                const auto tp = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(*next_deadline));
                cv_.wait_until(lock, tp, [&]() {
                    return !running_.load(std::memory_order_relaxed) ||
                           wake_epoch_.load(std::memory_order_relaxed) != prev_epoch;
                });
            }
            continue;
        }

        std::vector<TmIdBucket> ready_buckets = gate_logic_->popAllReadySorted();
        const uint64_t pop_ns = steadyNowNs();
        for (const auto& bucket : ready_buckets) {
            if (bucket.first_arrival_ns > 0 && pop_ns > bucket.first_arrival_ns) {
                const uint64_t wait_ns = pop_ns - bucket.first_arrival_ns;
                bucket_wait_count_.fetch_add(1, std::memory_order_relaxed);
                bucket_wait_sum_ns_.fetch_add(wait_ns, std::memory_order_relaxed);
                uint64_t cur_max = bucket_wait_max_ns_.load(std::memory_order_relaxed);
                while (wait_ns > cur_max && !bucket_wait_max_ns_.compare_exchange_weak(cur_max, wait_ns, std::memory_order_relaxed)) {}
            }
            TriggerResults results = processBucket(bucket);
            TriggerCallback cb;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cb = trigger_callback_;
            }
            if (!cb) continue;
            for (const auto& result : results) {
                cb(result, bucket);
            }
        }
    }
}

// 展平 bucket 内各 DU 的 T2 为 Hit 向量，交给 collectTriggerResults
TriggerResults TriggerPipeline::processBucket(const TmIdBucket& bucket) {
    std::vector<Hit> hits;
    std::size_t count = 0;
    for (const auto& [du_id, du_hits] : bucket.t2_hits_by_du) {
        (void)du_id;
        count += du_hits.size();
    }
    hits.reserve(count);
    for (const auto& [du_id, du_hits] : bucket.t2_hits_by_du) {
        for (const Gp300T2Hit& t2_hit : du_hits) {
            hits.push_back(Hit{du_id, t2_hit.timestamp_ns, t2_hit.trigger_channel, t2_hit.related_value});
        }
    }
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) {
                  return a.timestamp_ns < b.timestamp_ns ||
                         (a.timestamp_ns == b.timestamp_ns && a.du_id < b.du_id);
              });
    return collectTriggerResults(hits, bucket.tm_id, bucket.first_arrival_ns, false);
}

bool TriggerPipeline::checkNhitTriggerHits(const std::vector<Hit>& hits,
                                           std::vector<uint32_t>& triggered_dus,
                                           std::vector<uint64_t>& trigger_timestamps) {
    TriggerResults results = collectTriggerResults(hits, 0, 0, true);
    if (results.empty()) {
        return false;
    }
    triggered_dus = results.front().du_ids;
    trigger_timestamps = results.front().trigger_timestamps;
    return true;
}

// trigger_type 0：直通；否则滑动窗口 + nhit + T3Filter；stop_after_first_match 供 test 短路
TriggerResults TriggerPipeline::collectTriggerResults(const std::vector<Hit>& hits,
                                                      uint64_t tm_id,
                                                      uint64_t bucket_timestamp_ns,
                                                      bool stop_after_first_match) {
    TriggerResults results;
    if (hits.empty()) {
        return results;
    }

    if (config_.trigger_type == 0) {
        TriggerResult out;
        out.tm_id = tm_id;
        out.timestamp_ns = bucket_timestamp_ns;
        out.triggered = true;
        std::set<uint32_t> du_set;
        for (const Hit& hit : hits) {
            du_set.insert(hit.du_id);
            out.trigger_timestamps.push_back(hit.timestamp_ns);
        }
        out.du_ids.assign(du_set.begin(), du_set.end());
        out.du_count = out.du_ids.size();
        // trigger_type==0 直通：全窗口与过滤后相同
        out.window_timestamps = out.trigger_timestamps;
        out.window_du_ids = out.du_ids;
        results.push_back(std::move(out));
        return results;
    }

    std::vector<const Hit*> sorted_hits;
    sorted_hits.reserve(hits.size());
    for (const Hit& hit : hits) {
        sorted_hits.push_back(&hit);
    }
    std::sort(sorted_hits.begin(), sorted_hits.end(), [](const Hit* lhs, const Hit* rhs) {
        if (lhs->timestamp_ns != rhs->timestamp_ns) {
            return lhs->timestamp_ns < rhs->timestamp_ns;
        }
        return lhs->du_id < rhs->du_id;
    });

    const std::size_t n = sorted_hits.size();
    std::unordered_map<uint32_t, int> du_counts;
    du_counts.reserve(256);
    std::deque<const Hit*> window_hits;
    std::vector<Detector> detectors;
    detectors.reserve(512);

    std::size_t j_end = 0;
    int unique_du = 0;

    auto addHitToWindow = [&](const Hit* hit) {
        window_hits.push_back(hit);
        int& count = du_counts[hit->du_id];
        if (count == 0) {
            ++unique_du;
        }
        ++count;
    };

    auto removeHitFromWindowFront = [&](const Hit* hit) {
        auto it = du_counts.find(hit->du_id);
        if (it == du_counts.end()) {
            return;
        }
        --it->second;
        if (it->second == 0) {
            du_counts.erase(it);
            --unique_du;
        }
    };

    for (std::size_t i = 0; i < n; ++i) {
        const uint64_t t0 = sorted_hits[i]->timestamp_ns;

        if (j_end < i) {
            j_end = i;
            window_hits.clear();
            du_counts.clear();
            unique_du = 0;
        }

        while (j_end < n && sorted_hits[j_end]->timestamp_ns - t0 <= config_.time_window_ns) {
            addHitToWindow(sorted_hits[j_end]);
            ++j_end;
        }

        if (unique_du >= config_.nhit_threshold) {
            detectors.clear();
            detectors.reserve(window_hits.size());
            std::vector<std::pair<uint64_t, uint32_t>> detector_hits;
            detector_hits.reserve(window_hits.size());
            for (const Hit* hit : window_hits) {
                Detector detector;
                detector.id = static_cast<int>(hit->du_id);
                detector.time = static_cast<int>(hit->timestamp_ns % 1000000000ULL);
                detector.related_value = hit->related_value;
                detectors.push_back(detector);
                detector_hits.emplace_back(hit->timestamp_ns, hit->du_id);
            }

            const T3FilterDecision filter_decision = t3_filter_->filterDecision(detectors);
            t3_filter_candidate_window_count_.fetch_add(1, std::memory_order_relaxed);
            if (filter_decision.causal_evaluated) {
                t3_filter_causal_eval_count_.fetch_add(1, std::memory_order_relaxed);
                t3_filter_causal_input_detector_sum_.fetch_add(
                    filter_decision.causal_input_count, std::memory_order_relaxed);
                t3_filter_causal_selected_detector_sum_.fetch_add(
                    filter_decision.causal_selected_count, std::memory_order_relaxed);
                updateAtomicMax(t3_filter_causal_input_detector_max_,
                                static_cast<uint64_t>(filter_decision.causal_input_count));
                updateAtomicMax(t3_filter_causal_selected_detector_max_,
                                static_cast<uint64_t>(filter_decision.causal_selected_count));
                if (filter_decision.causal_bypassed_high_multiplicity) {
                    t3_filter_high_multiplicity_bypass_count_.fetch_add(
                        1, std::memory_order_relaxed);
                    t3_filter_high_multiplicity_bypass_input_detector_sum_.fetch_add(
                        filter_decision.causal_input_count, std::memory_order_relaxed);
                    t3_filter_high_multiplicity_bypass_unique_du_sum_.fetch_add(
                        filter_decision.causal_unique_du_count, std::memory_order_relaxed);
                    updateAtomicMax(
                        t3_filter_high_multiplicity_bypass_input_detector_max_,
                        static_cast<uint64_t>(filter_decision.causal_input_count));
                    updateAtomicMax(
                        t3_filter_high_multiplicity_bypass_unique_du_max_,
                        static_cast<uint64_t>(filter_decision.causal_unique_du_count));
                } else {
                    t3_filter_maximum_clique_time_count_.fetch_add(1, std::memory_order_relaxed);
                    t3_filter_maximum_clique_time_sum_ns_.fetch_add(
                        filter_decision.causal_elapsed_ns, std::memory_order_relaxed);
                    updateAtomicMax(t3_filter_maximum_clique_time_max_ns_,
                                    filter_decision.causal_elapsed_ns);
                }
            }
            if (filter_decision.reject_reason == T3FilterRejectReason::CausalSubsetTooSmall) {
                t3_filter_causal_reject_count_.fetch_add(1, std::memory_order_relaxed);
            }
            if (filter_decision.template_match_evaluated) {
                const uint64_t avg_scaled = static_cast<uint64_t>(
                    filter_decision.template_match_avg_related_value * static_cast<double>(kTemplateMatchAvgScale)
                    + 0.5);
                t3_filter_template_match_avg_sum_scaled_.fetch_add(avg_scaled,
                                                                   std::memory_order_relaxed);
                updateAtomicMax(t3_filter_template_match_avg_max_scaled_, avg_scaled);
                const uint64_t eval_count =
                    t3_filter_template_match_eval_count_.fetch_add(1, std::memory_order_relaxed) + 1;
                if (filter_decision.reject_reason == T3FilterRejectReason::TemplateMatchBelowThreshold) {
                    t3_filter_template_match_reject_count_.fetch_add(1, std::memory_order_relaxed);
                    std::cerr << "TEMPLATE_MATCH_REJECT avg="
                              << filter_decision.template_match_avg_related_value
                              << " threshold=" << config_.template_match_coefficient
                              << " detectors=" << filter_decision.template_match_detector_count
                              << std::endl;
                }
                if (eval_count % kTemplateMatchSampleLogInterval == 0) {
                    const uint64_t sum_scaled =
                        t3_filter_template_match_avg_sum_scaled_.load(std::memory_order_relaxed);
                    const uint64_t max_scaled =
                        t3_filter_template_match_avg_max_scaled_.load(std::memory_order_relaxed);
                    std::cerr << "TEMPLATE_MATCH_SAMPLE eval=" << eval_count
                              << " running_avg="
                              << (static_cast<double>(sum_scaled)
                                  / static_cast<double>(eval_count)
                                  / static_cast<double>(kTemplateMatchAvgScale))
                              << " max="
                              << (static_cast<double>(max_scaled)
                                  / static_cast<double>(kTemplateMatchAvgScale))
                              << " threshold=" << config_.template_match_coefficient
                              << std::endl;
                }
            }
            if (filter_decision.duplicateRejected()) {
                t3_filter_duplicate_reject_count_.fetch_add(1, std::memory_order_relaxed);
            }
            const std::vector<std::size_t>& selected_indices = filter_decision.selected_indices;
            if (filter_decision.duplicateRejected() && duplicate_reject_logger_) {
                if (!duplicate_reject_logger_->likelyAccepting()) {
                    duplicate_reject_logger_->noteDropped();
                } else {
                    try {
                        std::vector<DuplicateRejectLogHit> log_hits;
                        log_hits.reserve(filter_decision.duplicate_log_indices.size());
                        for (std::size_t idx : filter_decision.duplicate_log_indices) {
                            if (idx >= detector_hits.size()) {
                                continue;
                            }
                            const auto& [ts, du_id] = detector_hits[idx];
                            log_hits.push_back(DuplicateRejectLogHit{du_id, ts});
                        }
                        duplicate_reject_logger_->tryLog(std::move(log_hits));
                    } catch (...) {
                        // Best-effort diagnostics must never affect trigger processing.
                        duplicate_reject_logger_->noteDropped();
                    }
                }
            }
            std::set<uint32_t> selected_dus;
            std::vector<uint64_t> selected_ts;
            selected_ts.reserve(selected_indices.size());
            for (std::size_t idx : selected_indices) {
                if (idx >= detector_hits.size()) {
                    continue;
                }
                const auto& [ts, du_id] = detector_hits[idx];
                selected_dus.insert(du_id);
                selected_ts.push_back(ts);
            }

            if (!selected_indices.empty() &&
                static_cast<int>(selected_dus.size()) >= config_.nhit_threshold) {
                TriggerResult out;
                out.tm_id = tm_id;
                out.timestamp_ns = bucket_timestamp_ns;
                out.triggered = true;
                out.du_ids.assign(selected_dus.begin(), selected_dus.end());
                out.trigger_timestamps = std::move(selected_ts);
                out.du_count = out.du_ids.size();
                // 全窗口数据（与 old-cs-daq t3_trigger.cpp:1476-1491 一致）
                out.window_timestamps.reserve(detector_hits.size());
                std::set<uint32_t> window_du_set;
                for (const auto& [ts, du_id] : detector_hits) {
                    out.window_timestamps.push_back(ts);
                    window_du_set.insert(du_id);
                }
                out.window_du_ids.assign(window_du_set.begin(), window_du_set.end());
                results.push_back(std::move(out));
                if (stop_after_first_match) {
                    return results;
                }

                const std::size_t last_idx = j_end - 1;
                for (std::size_t k = i; k <= last_idx && k < n; ++k) {
                    removeHitFromWindowFront(sorted_hits[k]);
                }
                window_hits.clear();
                i = last_idx;
                continue;
            }

            const std::size_t last_idx = j_end - 1;
            for (std::size_t k = i; k <= last_idx && k < n; ++k) {
                removeHitFromWindowFront(sorted_hits[k]);
            }
            window_hits.clear();
            i = last_idx;
            continue;
        }

        removeHitFromWindowFront(sorted_hits[i]);
        if (!window_hits.empty()) {
            window_hits.pop_front();
        }
    }

    return results;
}

} // namespace grand
