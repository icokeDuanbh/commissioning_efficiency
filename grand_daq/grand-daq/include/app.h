#pragma once

#include <cstddef>
#include <cstdint>
#include <frontend.h>
#include <data_handler.h>
#include <builder.h>
#include <t3_trigger.h>
#include <data_store.h>

#include <condition_variable>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cs_yaml_config.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <nlohmann/json.hpp>

enum class ControlPlaneSendOutcome {
    Success = 0,
    RetryableFailure,
    FatalFailure,
};

struct ControlPlaneSendSummary {
    std::string command;
    std::size_t success_count = 0;
    std::size_t failure_count = 0;
    std::size_t fatal_failure_count = 0;
    std::vector<uint32_t> failed_du_ids;
    std::vector<uint32_t> fatal_failed_du_ids;
    bool should_abort = false;

    bool allSucceeded() const {
        return failure_count == 0 && fatal_failure_count == 0;
    }
};

/**
 * Startup control-plane send with retry (aligned with old-cs-daq-cw `sendCmdOneWithRetry` semantics).
 * @return true if `send_once` completed without throwing; false if all attempts were exhausted.
 */
// 控制面 ZMQ 发送重试封装，供 App::start/stop 发 STAR/STOP 等
bool startupControlPlaneSendWithRetry(const std::function<void()>& send_once,
                                      int max_attempts,
                                      int retry_delay_ms);

/**
 * @brief 应用程序类
 */
// 聚合 Config、Frontend、DataHandler、Builder、DataStore、T3Trigger 与 do_trigger 工作线程
class App {
public:
    App(const std::string& config_file);
    ~App();
    
    /**
     * @brief 初始化应用程序
     */
    void initialize();
    
    /**
     * @brief 启动应用程序
     * @note 任务 6 收口：legacy 控制序列在 `App::start()` 内显式编排；`Frontend` 为固定生产类。
     */
    void start();
    
    /**
     * @brief 停止应用程序
     */
    void stop();

    void terminate();

    /**
     * 停止态重读 `config_file_` 并原子替换 initialized 子图（Builder/DataStore/Handler/Frontend/T3 参数）。
     * 仅当 `status_ == "initialized"`；`running` 等其它状态会抛 `SoftwareException`。
     */
    void reloadYamlConfig();

    std::string status();
    nlohmann::json collectHealthSnapshot() const;

private:
    std::map<uint32_t, std::unique_ptr<DataHandler>> data_handlers_;
    std::map<uint32_t, std::unique_ptr<Frontend>> frontends_;
    std::unique_ptr<Builder> builder_;
    std::unique_ptr<DataStore> data_store_;
    std::unique_ptr<grand::T3Trigger> t3_trigger_; // 每轮 run 重建
    grand::T3Config t3_config_{};
    mutable std::mutex t3_dispatch_mutex_; // 保护 t3_trigger_ 指针与 tag
    std::atomic<uint32_t> t3_trigger_tag_{0};

    struct DotriggerWorkItem {
        grand::TriggerResult result;
        uint32_t tag = 0; // 与 beginEvent tag 对齐
    };
    // 2026-04-16：256 在峰值速率下会先于 Builder pending 满（观测到 5562 次
    // overflow → inline fallback），和 Builder 16384 保持同量级，避免成为前置
    // 瓶颈把 T3 回调线程拖进 inline sendTCP。
    static constexpr std::size_t kDotriggerQueueCapacity = 4096;
    std::deque<DotriggerWorkItem> dotrigger_queue_;
    mutable std::mutex dotrigger_queue_mutex_;
    std::condition_variable dotrigger_cv_;
    std::thread dotrigger_worker_;
    std::atomic<bool> dotrigger_accept_{true};
    std::atomic<bool> dotrigger_shutdown_{false};
    /** Work items popped by the worker but not yet finished in `processDotriggerDispatch` (queue mutex). */
    std::size_t dotrigger_inflight_{0};
    /** Test seam: artificial delay at the start of `processDotriggerDispatch` (milliseconds). */
    std::atomic<int> dotrigger_test_pause_before_send_ms_{0};
    /** Count of items enqueued while `dotrigger_accept_` is false (shutdown drain path). For tests/diagnostics. */
    std::atomic<int> dotrigger_shutdown_enqueue_count_{0};
    /** Count of queue-overflow inline-send fallbacks. */
    std::atomic<int> dotrigger_overflow_count_{0};
    /** Total individual DU sends that succeeded. */
    std::atomic<uint64_t> dotrigger_send_success_count_{0};
    /** Total individual DU sends that failed. */
    std::atomic<uint64_t> dotrigger_send_fail_count_{0};
    /** Total DOTRIGGER dispatches (one per trigger event). */
    std::atomic<uint64_t> dotrigger_dispatch_count_{0};
    /** P4 tests: nonzero DU id → `start()` unconfigures that frontend after TERM gap, before INIT. */
    std::atomic<uint32_t> control_plane_test_invalidate_before_init_du_{0};
    /** P4 tests: nonzero DU id → `start()` unconfigures that frontend after data-plane start, before STOP. */
    std::atomic<uint32_t> control_plane_test_invalidate_before_stop_du_{0};
    /** P4 tests: nonzero DU id → `start()` unconfigures that frontend after CONF gap, before STAR. */
    std::atomic<uint32_t> control_plane_test_invalidate_before_star_du_{0};
    std::mutex dotrigger_send_mutex_;

    struct DotriggerLatencySnapshot {
        std::size_t pending = 0;
        uint64_t response_count = 0;
        uint64_t response_missing_send_count = 0;
        uint64_t response_slow_count = 0;
        double response_last_ms = 0.0;
        double response_avg_ms = 0.0;
        double response_max_ms = 0.0;
        double response_slow_threshold_ms = 0.0;
    };

    struct DotriggerDuLatencyStats {
        uint64_t sent_count = 0;
        uint64_t response_count = 0;
        uint64_t response_missing_send_count = 0;
        uint64_t response_slow_count = 0;
        uint64_t pending_count = 0;
        double response_last_ms = 0.0;
        double response_sum_ms = 0.0;
        double response_max_ms = 0.0;
    };

    mutable std::mutex dotrigger_latency_mutex_;
    std::map<uint64_t, std::chrono::steady_clock::time_point> dotrigger_send_times_;
    mutable std::map<uint32_t, DotriggerDuLatencyStats> dotrigger_du_latency_stats_;
    uint64_t dotrigger_response_count_ = 0;
    uint64_t dotrigger_response_missing_send_count_ = 0;
    uint64_t dotrigger_response_slow_count_ = 0;
    double dotrigger_response_last_ms_ = 0.0;
    double dotrigger_response_sum_ms_ = 0.0;
    double dotrigger_response_max_ms_ = 0.0;
    double dotrigger_response_slow_threshold_ms_ = 0.0;

    struct CdEventFragment {
        uint32_t du_id = 0;
        std::vector<uint8_t> payload;
    };

    struct CdWriteWorkItem {
        EventTag event_key = 0;
        std::vector<CdEventFragment> fragments;
    };
    // Capacity is counted in completed CD events, not individual DU fragments.
    static constexpr std::size_t kCdWriteQueueCapacity = 2048;
    std::deque<CdWriteWorkItem> cd_write_queue_;
    std::mutex cd_write_queue_mutex_;
    std::condition_variable cd_write_cv_;
    std::thread cd_write_worker_;
    std::atomic<bool> cd_write_accept_{true};
    std::atomic<bool> cd_write_shutdown_{false};
    std::size_t cd_write_inflight_{0};

    std::string status_;
    std::string config_file_;

    /** 最近一次成功 `initialize`/`reloadYamlConfig` 的快照，供 reload 失败时回滚 `ConfigManager`。 */
    std::optional<grand::LegacyYamlConfig> last_applied_legacy_yaml_;

    void rebuildInitializedMembersFromLegacyYaml(const grand::LegacyYamlConfig& legacy);

    void stopNetworkLegacyCommandBestEffort(const char* cmd, const uint8_t* param, std::size_t param_size);
    void stopLocalPipelineNoexcept();

    /** `start()` 失败时回滚已启动的本地资源；不抛异常。 */
    void rollbackStartedLocalNoexcept();

    void sendLegacyCommandAllThrowing(const char* cmd, const uint8_t* param, std::size_t param_size);
    void handleT3TriggerResult(const grand::TriggerResult& result);
    void processDotriggerDispatch(const grand::TriggerResult& result, uint32_t tag);
    static uint64_t dotriggerLatencyKey(EventTag tag, uint32_t du_id);
    void recordDotriggerSend(EventTag tag,
                             uint32_t du_id,
                             std::chrono::steady_clock::time_point sent_at);
    void recordDotriggerResponse(const SubFragmentHeader& header);
    void resetDotriggerLatencyTracking(double slow_threshold_ms);
    DotriggerLatencySnapshot dotriggerLatencySnapshot() const;
    void dotriggerWorkerLoop();
    void joinDotriggerWorkerDrain();
    void startDotriggerWorkerIfNeeded();
    void waitDotriggerQuiescent();
    /** Wait until queue empty and inflight==0; `lk` must hold `dotrigger_queue_mutex_`. Logs on timeout. */
    void waitDotriggerQueueInflightDrainUnderLock(std::unique_lock<std::mutex>& lk);
    void resetT3TriggerNoexcept();
    void createAndBindT3TriggerForRun(uint32_t cd_run_number);
    void onSubFragmentFromDataHandler(SubFragmentHeader& header, uint8_t* data);
    void drainBuilderCompletedToStore();
    void drainBuilderDiagnosticsToLog();
    void logStopObservabilitySummary();

    void cdWriteWorkerLoop();
    void startCdWriteWorkerIfNeeded();
    void joinCdWriteWorkerDrainNoexcept() noexcept;
    void waitCdWriteQueueInflightDrainUnderLock(std::unique_lock<std::mutex>& lk);
    /** After last `drainBuilderCompletedToStore()` enqueue; drain queue + join worker before `DataStore::close()`. */
    void quiesceCdWritesBeforeCloseStoreNoexcept() noexcept;
};
