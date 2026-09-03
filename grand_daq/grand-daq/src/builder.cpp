#include <builder.h>
#include <algorithm>

namespace {
constexpr bool kLogMissingDuDetails = false;
constexpr uint64_t kLossBucketSec = 300;
constexpr uint64_t kLossRetentionSec = 48 * 60 * 60;

uint64_t currentUnixMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

uint64_t lossBucketStartUnixMs(uint64_t unix_ms) {
    constexpr uint64_t bucket_ms = kLossBucketSec * 1000;
    return (unix_ms / bucket_ms) * bucket_ms;
}
}

Builder::Builder(std::set<uint32_t> &channel_tags,
                 std::chrono::milliseconds event_timeout,
                 std::size_t max_pending_events,
                 std::size_t loss_recent_event_limit)
    : channel_tags_(channel_tags)
    , event_timeout_(event_timeout)
    , max_pending_events_(max_pending_events)
    , loss_recent_event_limit_(loss_recent_event_limit) {
    if (event_timeout_ <= std::chrono::milliseconds(0)) {
        event_timeout_ = std::chrono::milliseconds(1);
    }
    if (max_pending_events_ == 0) {
        max_pending_events_ = 1;
    }
    if (loss_recent_event_limit_ == 0) {
        loss_recent_event_limit_ = 1;
    }
}

void Builder::start() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    is_stop_ = false;
    pending_events_.clear();
    completed_events_.clear();
    diagnostics_.clear();
    stat_missing_on_timeout_per_du_.clear();
    loss_totals_ = LossTotals{};
    loss_buckets_.clear();
    loss_recent_events_.clear();
    loss_telemetry_dropped_ = 0;
    while (!deadline_heap_.empty()) {
        deadline_heap_.pop();
    }
    completed_count_ = 0;
    unmatched_count_ = 0;
    timeout_count_ = 0;
}

void Builder::stop() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (is_stop_) {
        return;
    }

    harvestExpiredFromHeapLocked();

    for (auto& [tag, event] : pending_events_) {
        emitDiagnosticLocked(DiagnosticType::StopFlush,
                             tag,
                             0,
                             0,
                             0,
                             event.trigger_timestamp_ns);
        if (!event.attached_fragments.empty()) {
            stat_partial_events_.fetch_add(1, std::memory_order_relaxed);
            const uint64_t expected = event.expected_du_ids.size();
            // 统计口径按"已到达的 distinct DU"而不是 fragment 条数，避免同 DU 多条片段
            // 把 attached 推到超过 expected 造成 unsigned 下溢。
            const uint64_t attached = event.attached_du_ids.size();
            if (expected > attached) {
                stat_partial_missing_fragments_.fetch_add(expected - attached, std::memory_order_relaxed);
            }
            CompletedEvent partial = makeCompletedEvent(std::move(event));
            partial.is_partial = true;
            completed_events_.push_back(std::move(partial));
        }
    }

    is_stop_ = true;
    pending_events_.clear();
    while (!deadline_heap_.empty()) {
        deadline_heap_.pop();
    }
}

void Builder::beginEvent(EventTag tag,
                         const std::vector<uint32_t>& expected_du_ids,
                         const std::vector<uint64_t>& expected_trigger_timestamps,
                         uint64_t trigger_timestamp_ns) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (is_stop_) {
        return;
    }

    harvestExpiredFromHeapLocked();

    if (expected_du_ids.empty()) {
        return;
    }
    if (pending_events_.find(tag) != pending_events_.end()) {
        LOG_WARN << "Builder duplicate beginEvent tag: " << tag;
        return;
    }

    if (pending_events_.size() >= max_pending_events_) {
        emitDiagnosticLocked(DiagnosticType::AdmissionRejected,
                             tag,
                             0,
                             0,
                             0,
                             trigger_timestamp_ns);
        LOG_WARN << "Builder admission rejected: pending limit reached tag=" << tag
                 << " limit=" << max_pending_events_;
        return;
    }

    PendingEvent event;
    event.tag = tag;
    event.trigger_timestamp_ns = trigger_timestamp_ns;
    event.created_at = std::chrono::steady_clock::now();
    event.deadline = event.created_at + event_timeout_;
    event.expected_du_ids.insert(expected_du_ids.begin(), expected_du_ids.end());
    event.expected_trigger_timestamps.insert(expected_trigger_timestamps.begin(),
                                             expected_trigger_timestamps.end());
    event.loss_bucket_start_unix_ms = lossBucketStartUnixMs(currentUnixMs());

    if (event.expected_du_ids.empty()) {
        return;
    }

    auto inserted = pending_events_.emplace(tag, std::move(event));
    deadline_heap_.push(DeadlineEntry{inserted.first->second.deadline, tag});
    stat_registered_events_.fetch_add(1, std::memory_order_relaxed);
    stat_expected_fragments_.fetch_add(expected_du_ids.size(), std::memory_order_relaxed);
    recordLossExpectedLocked(inserted.first->second.loss_bucket_start_unix_ms,
                             inserted.first->second.expected_du_ids);
}

// 收到一个 DU 的波形片段，按 event_tag 直接查找 pending event 匹配。
void Builder::addFragment(const SubFragmentHeader& header, uint8_t* data) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (is_stop_) {
        return;
    }

    harvestExpiredFromHeapLocked();

    if (data == nullptr || header.data_size == 0 ||
        channel_tags_.find(header.du_id) == channel_tags_.end()) {
        ++unmatched_count_;
        emitDiagnosticLocked(DiagnosticType::InvalidFragment,
                             0, header.du_id, header.trigger_number, 0, 0);
        return;
    }

    // 按 event_tag 直接查找 pending event
    const EventTag tag = static_cast<EventTag>(header.event_tag);
    auto event_it = pending_events_.find(tag);
    if (event_it == pending_events_.end()) {
        ++unmatched_count_;
        emitDiagnosticLocked(DiagnosticType::NoFifoForDu,
                             tag, header.du_id, header.trigger_number, 0, 0);
        return;
    }

    PendingEvent& event = event_it->second;

    if (event.expected_du_ids.find(header.du_id) == event.expected_du_ids.end()) {
        ++unmatched_count_;
        return;
    }
    // 保持原 Builder 语义：同一 event_tag 下同一 DU 只接受一条片段。
    // 生产路径 trigger_type==2 下最大因果团保证每个 DU 在一个事例里只触发一次，
    // 因此 DuplicateFragment 在正常流中不应出现；保留这条检查是防御性监控，
    // 一旦触发就说明上游（du-daq 重传 / 测试构造）有不符合预期的行为。
    if (event.attached_du_ids.find(header.du_id) != event.attached_du_ids.end()) {
        ++unmatched_count_;
        // 直接打一条带完整上下文的 WARN，方便追查"为什么同一 tag 下同一 DU 出现了第二条
        // 波形"。除 diagnostics 队列那条通用日志之外额外独立输出，确保即使 diagnostics
        // drain 被延迟或被下游消费者过滤掉，这条线索也不会丢。生产 (trigger_type==2)
        // 下此行永远不应出现；一旦出现代表：
        //   (a) 上游触发链路有问题（T3Filter 让同 DU 进入了因果子集 / DOTRIGGER 重复发送）
        //   (b) du-daq 对同一 DOTRIGGER 回传了多条波形
        //   (c) event_tag 复用冲突
        // 字段语义：
        //   tag         —— 事例标签（event_tag）
        //   du_id       —— 重复片段来源的 DU
        //   trig_num    —— 片段自身带的 trigger_number
        //   data_size   —— 被拒绝的片段 payload 字节数，用于区分"真实二次波形"和"传输重传"
        //   already     —— 事例此刻已到达的 distinct DU 数
        //   expected    —— 事例 expected_du_ids 的总数
        //   event_ts_ns —— 事例 trigger_timestamp（beginEvent 传入）
        LOG_WARN << "[Builder DuplicateFragment dropped] tag=" << tag
                 << " du_id=" << header.du_id
                 << " trig_num=" << header.trigger_number
                 << " data_size=" << header.data_size
                 << " already=" << event.attached_du_ids.size()
                 << " expected=" << event.expected_du_ids.size()
                 << " event_ts_ns=" << event.trigger_timestamp_ns;
        emitDiagnosticLocked(DiagnosticType::DuplicateFragment,
                             tag, header.du_id, header.trigger_number,
                             0, event.trigger_timestamp_ns);
        return;
    }

    WaveformFragment fragment;
    fragment.du_id = header.du_id;
    fragment.trigger_number = header.trigger_number;
    fragment.waveform_envelope = header.waveform_envelope;
    fragment.trigger_time = 0;
    fragment.payload.assign(data, data + header.data_size);
    event.attached_fragments.push_back(std::move(fragment));
    event.attached_du_ids.insert(header.du_id);
    stat_received_fragments_.fetch_add(1, std::memory_order_relaxed);

    if (isEventCompleted(event)) {
        PendingEvent pe = std::move(event_it->second);
        pending_events_.erase(event_it);
        completed_events_.push_back(makeCompletedEvent(std::move(pe)));
        ++completed_count_;
        stat_complete_events_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::vector<Builder::CompletedEvent> Builder::drainCompleted() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!is_stop_) {
        harvestExpiredFromHeapLocked();
    }
    std::vector<CompletedEvent> drained;
    drained.swap(completed_events_);
    return drained;
}

std::vector<Builder::DiagnosticEvent> Builder::drainDiagnostics() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!is_stop_) {
        harvestExpiredFromHeapLocked();
    }
    std::vector<DiagnosticEvent> drained;
    drained.swap(diagnostics_);
    return drained;
}

uint64_t Builder::completedCount() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return completed_count_;
}

uint64_t Builder::unmatchedCount() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return unmatched_count_;
}

uint64_t Builder::timeoutCount() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return timeout_count_;
}

uint64_t Builder::pendingCount() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return pending_events_.size();
}

std::map<uint32_t, uint64_t> Builder::statMissingOnTimeoutPerDu() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    std::map<uint32_t, uint64_t> out;
    for (const auto& [du_id, count] : stat_missing_on_timeout_per_du_) {
        out[du_id] = count;
    }
    return out;
}

Builder::LossStatsSnapshot Builder::lossStatsSnapshot(std::size_t top_du_per_bucket,
                                                      std::size_t recent_event_limit) const {
    std::map<uint64_t, LossBucketStats> bucket_copy;
    LossStatsSnapshot out;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        out.bucket_sec = kLossBucketSec;
        out.retention_sec = kLossRetentionSec;
        out.telemetry_dropped = loss_telemetry_dropped_;
        out.totals = loss_totals_;
        bucket_copy = loss_buckets_;
        const std::size_t limit = recent_event_limit == 0
            ? loss_recent_events_.size()
            : recent_event_limit;
        const std::size_t skip = loss_recent_events_.size() > limit
            ? loss_recent_events_.size() - limit
            : 0;
        out.recent_events.reserve(loss_recent_events_.size() - skip);
        for (std::size_t i = skip; i < loss_recent_events_.size(); ++i) {
            out.recent_events.push_back(loss_recent_events_[i]);
        }
    }

    out.buckets.reserve(bucket_copy.size());
    for (auto& [start_ms, bucket] : bucket_copy) {
        std::vector<std::pair<uint32_t, LossDuStats>> ranked(bucket.per_du.begin(), bucket.per_du.end());
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) {
                      if (a.second.missing != b.second.missing) {
                          return a.second.missing > b.second.missing;
                      }
                      return a.first < b.first;
                  });

        LossBucketStats trimmed;
        trimmed.start_unix_ms = start_ms;
        trimmed.expected_fragments = bucket.expected_fragments;
        trimmed.missing_fragments = bucket.missing_fragments;
        trimmed.timeout_events = bucket.timeout_events;
        for (const auto& [du_id, stats] : ranked) {
            trimmed.per_du.emplace(du_id, stats);
            if (top_du_per_bucket > 0 && trimmed.per_du.size() >= top_du_per_bucket) {
                break;
            }
        }
        out.buckets.push_back(std::move(trimmed));
    }

    return out;
}

void Builder::harvestExpiredFromHeapLocked() {
    const auto now = std::chrono::steady_clock::now();
    while (!deadline_heap_.empty() && deadline_heap_.top().deadline <= now) {
        const DeadlineEntry top = deadline_heap_.top();
        deadline_heap_.pop();
        auto it = pending_events_.find(top.tag);
        if (it == pending_events_.end()) {
            continue;
        }
        if (it->second.deadline != top.deadline) {
            continue;
        }

        // 产出 partial event（makeCompletedEvent 会 move expected_du_ids）
        emitTimeoutLocked(it->second);

        pending_events_.erase(it);
    }
}

void Builder::emitDiagnosticLocked(DiagnosticType type,
                                   EventTag tag,
                                   uint32_t du_id,
                                   TriggerID trigger_number,
                                   uint64_t fragment_trigger_time,
                                   uint64_t event_trigger_timestamp_ns) {
    diagnostics_.push_back(DiagnosticEvent{
        type,
        tag,
        du_id,
        trigger_number,
        fragment_trigger_time,
        event_trigger_timestamp_ns,
    });
    switch (type) {
        case DiagnosticType::NoFifoForDu: diag_no_fifo_.fetch_add(1, std::memory_order_relaxed); break;
        case DiagnosticType::HeadTimestampMismatch: diag_timestamp_mismatch_.fetch_add(1, std::memory_order_relaxed); break;
        case DiagnosticType::DuplicateFragment: diag_duplicate_.fetch_add(1, std::memory_order_relaxed); break;
        case DiagnosticType::AdmissionRejected: diag_admission_rejected_.fetch_add(1, std::memory_order_relaxed); break;
        default: break;
    }
}

void Builder::emitTimeoutLocked(PendingEvent& event) {
    ++timeout_count_;
    stat_timeout_events_.fetch_add(1, std::memory_order_relaxed);
    // 统计口径按 distinct DU，避免同 DU 多条片段把 attached 推到超过 expected。
    const uint64_t attached = event.attached_du_ids.size();
    const uint64_t expected = event.expected_du_ids.size();
    if (expected > attached) {
        stat_missing_on_timeout_.fetch_add(expected - attached, std::memory_order_relaxed);
    }

    // 在 move 之前保存诊断所需的值
    const EventTag tag = event.tag;
    const uint64_t ts = event.trigger_timestamp_ns;
    std::vector<uint32_t> missing_du_ids;

    // 把缺失 DU 逐条写日志，用于离线聚合到每个 DU 的缺失次数。
    // 格式固定：MISSING_DU tag=<tag> du_id=<id> event_ts_ns=<ts>
    //          expected=<N> attached=<N>
    // 注意：早期版本这里还打印 waited_ms = now - event.created_at，
    //       但该值恰好 = event_timeout_（因为 emitTimeoutLocked 在 deadline 命中时触发），
    //       并不是真实"到达时延"，容易被读成"这个 DU 平均等了 60s 才到" —— 误导性太强，
    //       所以去掉。真实的 per-DU 到达时延需要在 addFragment 里单独追踪（另起 patch）。
    // Per-DU timeout loss is exported through health; detailed per-event logs are
    // disabled by default to avoid timeout-burst log/IO amplification.
    if (expected > attached) {
        for (uint32_t du_id : event.expected_du_ids) {
            if (event.attached_du_ids.find(du_id) == event.attached_du_ids.end()) {
                ++stat_missing_on_timeout_per_du_[du_id];
                missing_du_ids.push_back(du_id);
                if (kLogMissingDuDetails) {
                    LOG_WARN << "MISSING_DU tag=" << tag
                             << " du_id=" << du_id
                             << " event_ts_ns=" << ts
                             << " expected=" << expected
                             << " attached=" << attached;
                }
            }
        }
        recordLossTimeoutLocked(event, missing_du_ids, currentUnixMs());
    }

    // 如果有已到达的 fragment，作为 partial event 产出
    if (!event.attached_fragments.empty()) {
        stat_partial_events_.fetch_add(1, std::memory_order_relaxed);
        stat_partial_missing_fragments_.fetch_add(expected - attached, std::memory_order_relaxed);
        CompletedEvent partial = makeCompletedEvent(std::move(event));
        partial.is_partial = true;
        completed_events_.push_back(std::move(partial));
    }

    emitDiagnosticLocked(DiagnosticType::Timeout, tag, 0, 0, 0, ts);
}

void Builder::recordLossExpectedLocked(uint64_t bucket_start_unix_ms,
                                       const std::set<uint32_t>& expected_du_ids) {
    if (expected_du_ids.empty()) {
        return;
    }
    pruneLossBucketsLocked(bucket_start_unix_ms);
    auto& bucket = loss_buckets_[bucket_start_unix_ms];
    bucket.start_unix_ms = bucket_start_unix_ms;
    const uint64_t expected = expected_du_ids.size();
    bucket.expected_fragments += expected;
    loss_totals_.expected_fragments += expected;
}

void Builder::recordLossTimeoutLocked(const PendingEvent& event,
                                      const std::vector<uint32_t>& missing_du_ids,
                                      uint64_t observed_unix_ms) {
    if (missing_du_ids.empty()) {
        return;
    }
    const uint64_t bucket_start = event.loss_bucket_start_unix_ms != 0
        ? event.loss_bucket_start_unix_ms
        : lossBucketStartUnixMs(observed_unix_ms);
    pruneLossBucketsLocked(bucket_start);
    auto& bucket = loss_buckets_[bucket_start];
    bucket.start_unix_ms = bucket_start;
    bucket.missing_fragments += missing_du_ids.size();
    bucket.timeout_events += 1;
    loss_totals_.missing_fragments += missing_du_ids.size();
    loss_totals_.timeout_events += 1;
    for (uint32_t du_id : missing_du_ids) {
        bucket.per_du[du_id].missing += 1;
    }

    if (loss_recent_events_.size() >= loss_recent_event_limit_) {
        loss_recent_events_.pop_front();
        ++loss_telemetry_dropped_;
    }
    LossRecentEvent recent;
    recent.tag = event.tag;
    recent.event_ts_ns = event.trigger_timestamp_ns;
    recent.observed_unix_ms = observed_unix_ms;
    recent.expected = event.expected_du_ids.size();
    recent.attached = event.attached_du_ids.size();
    recent.missing_du_ids = missing_du_ids;
    loss_recent_events_.push_back(std::move(recent));
}

void Builder::pruneLossBucketsLocked(uint64_t newest_bucket_start_unix_ms) {
    const uint64_t retention_ms = kLossRetentionSec * 1000;
    if (newest_bucket_start_unix_ms <= retention_ms) {
        return;
    }
    const uint64_t cutoff = newest_bucket_start_unix_ms - retention_ms;
    while (!loss_buckets_.empty() && loss_buckets_.begin()->first < cutoff) {
        loss_buckets_.erase(loss_buckets_.begin());
    }
}

Builder::CompletedEvent Builder::makeCompletedEvent(PendingEvent&& event) {
    CompletedEvent completed;
    completed.tag = event.tag;
    completed.expected_du_ids = std::move(event.expected_du_ids);
    completed.trigger_timestamp_ns = event.trigger_timestamp_ns;
    // 保持到达顺序：attached_fragments 是 push_back 构建的 vector，
    // 这里直接整体搬过去，不再做任何重排。与 old-cs-daq EventStore::processData
    // "来一条写一条" 的顺序契约一致。
    completed.actual_du_ids.insert(event.attached_du_ids.begin(), event.attached_du_ids.end());
    completed.fragments = std::move(event.attached_fragments);
    return completed;
}

bool Builder::isEventCompleted(const PendingEvent& event) const {
    // 基于 distinct DU 判定齐活，而不是 fragment 条数：同 DU 多条片段不重复计入齐活进度。
    if (event.attached_du_ids.size() != event.expected_du_ids.size()) {
        return false;
    }
    for (uint32_t du_id : event.expected_du_ids) {
        if (event.attached_du_ids.find(du_id) == event.attached_du_ids.end()) {
            return false;
        }
    }
    return true;
}
