#include <app.h>
#include <config.h>
#include <cs_yaml_config.h>
#include <duplicate_reject_logger.h>
#include <elec_config.h>
#include <message.h>
#include <params.h>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <signal.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <exception>
#include <functional>
#include <cstdlib>
#include <set>
#include <builder.h>
#include <thread>
#include <mutex>
#include <algorithm>

bool startupControlPlaneSendWithRetry(const std::function<void()>& send_once,
                                      int max_attempts,
                                      int retry_delay_ms) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        try {
            send_once();
            return true;
        } catch (const std::exception& e) {
            // Use fprintf (not Logger/LogStream) — tight retry loops + iostream/stringstream
            // caused deterministic aborts after ~40 iterations on some platforms (P1 unit test).
            std::fprintf(stderr, "ControlPlane: send_once threw on attempt %d/%d: %s\n",
                         attempt + 1, max_attempts, e.what());
            if (attempt + 1 < max_attempts) {
                std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
            }
        } catch (...) {
            std::fprintf(stderr,
                         "ControlPlane: send_once threw non-std exception on attempt %d/%d\n",
                         attempt + 1, max_attempts);
            if (attempt + 1 < max_attempts) {
                std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
            }
        }
    }
    return false;
}

namespace {

constexpr int kStartupControlPlaneMaxAttempts = 50;
constexpr int kStartupControlPlaneRetryDelayMs = 10;
constexpr int kStopToTermGapMsDefault = 400;
constexpr int kControlPlanePhaseGapMs = 100;
constexpr std::size_t kBuilderMaxPendingEvents = 100000;
constexpr bool kLogPartialEventDetails = false;

size_t readEnvSizeT(const char* name, size_t default_value) {
    const char* p = std::getenv(name);
    if (!p || !*p) {
        return default_value;
    }
    return static_cast<size_t>(std::strtoull(p, nullptr, 10));
}

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void logCaughtExceptionNoexcept(const char* context) noexcept {
    try {
        throw;
    } catch (const std::exception& e) {
        LOG_ERROR << context << ": " << e.what();
    } catch (...) {
        LOG_ERROR << context << ": non-std exception";
    }
}

void logControlPlaneRetryExhausted(const char* cmd, uint32_t du_id) {
    LOG_ERROR << "ControlPlane: send failed cmd=" << cmd << " du_id=" << du_id
              << " after attempts=" << kStartupControlPlaneMaxAttempts;
}

void logControlPlaneSummary(const ControlPlaneSendSummary& s) {
    LOG_INFO << "ControlPlane: cmd=" << s.command << " success=" << s.success_count
             << " retryable_fail=" << s.failure_count << " fatal_fail=" << s.fatal_failure_count;
    if (!s.failed_du_ids.empty()) {
        std::ostringstream oss;
        for (size_t i = 0; i < s.failed_du_ids.size(); ++i) {
            if (i) {
                oss << ',';
            }
            oss << s.failed_du_ids[i];
        }
        LOG_INFO << "ControlPlane: retryable_failed_du_ids=[" << oss.str() << "]";
    }
    if (!s.fatal_failed_du_ids.empty()) {
        std::ostringstream oss;
        for (size_t i = 0; i < s.fatal_failed_du_ids.size(); ++i) {
            if (i) {
                oss << ',';
            }
            oss << s.fatal_failed_du_ids[i];
        }
        LOG_INFO << "ControlPlane: fatal_failed_du_ids=[" << oss.str() << "]";
    }
}

const char* builderDiagnosticTypeName(Builder::DiagnosticType type) {
    switch (type) {
        case Builder::DiagnosticType::NoFifoForDu:
            return "NoFifoForDu";
        case Builder::DiagnosticType::HeadTimestampMismatch:
            return "HeadTimestampMismatch";
        case Builder::DiagnosticType::DuplicateFragment:
            return "DuplicateFragment";
        case Builder::DiagnosticType::InvalidFragment:
            return "InvalidFragment";
        case Builder::DiagnosticType::Timeout:
            return "Timeout";
        case Builder::DiagnosticType::StopFlush:
            return "StopFlush";
        case Builder::DiagnosticType::AdmissionRejected:
            return "AdmissionRejected";
        default:
            return "Unknown";
    }
}

constexpr uint64_t kLegacyDefaultTimeoutCapNs = 1000000000ULL;
constexpr const char* kProgressAwareOnlyError =
    "t3GateExtendOnProgressOnly=false is not supported in Phase 1; only progress-aware extension is implemented";

enum class GateDeadlineModeParseKind {
    Canonical = 0,
    DeprecatedFirstArrivalAlias = 1,
    DeprecatedLastArrivalAlias = 2,
    Invalid = 3,
};

GateDeadlineModeParseKind parseGateDeadlineMode(const std::string& raw,
                                               grand::GateDeadlineMode& mode) {
    if (raw == "first_arrival_fixed") {
        mode = grand::GateDeadlineMode::FirstArrivalFixed;
        return GateDeadlineModeParseKind::Canonical;
    }
    if (raw == "first_arrival_bounded_sliding") {
        mode = grand::GateDeadlineMode::FirstArrivalBoundedSliding;
        return GateDeadlineModeParseKind::Canonical;
    }
    if (raw == "first_arrival") {
        mode = grand::GateDeadlineMode::FirstArrivalFixed;
        return GateDeadlineModeParseKind::DeprecatedFirstArrivalAlias;
    }
    if (raw == "last_arrival") {
        mode = grand::GateDeadlineMode::LastArrivalLegacy;
        return GateDeadlineModeParseKind::DeprecatedLastArrivalAlias;
    }
    return GateDeadlineModeParseKind::Invalid;
}

std::vector<uint8_t> buildLegacyCmd(const std::string& cmd, const uint8_t* param, size_t param_size)
{
    const size_t cmd_size = cmd.size() + 1;
    const size_t total = grand::kMessageHeaderSize + cmd_size + param_size;
    std::vector<uint8_t> buffer(total);
    grand::MessageHeader header{};
    header.size = static_cast<uint32_t>(total);
    header.type = grand::MT_CMD;
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + grand::kMessageHeaderSize, cmd.c_str(), cmd.size());
    buffer[grand::kMessageHeaderSize + cmd.size()] = '\0';
    if (param_size > 0 && param != nullptr) {
        std::memcpy(buffer.data() + grand::kMessageHeaderSize + cmd_size, param, param_size);
    }
    return buffer;
}

ControlPlaneSendSummary sendStartupBroadcastCmd(std::map<uint32_t, std::unique_ptr<Frontend>>& frontends,
                                                const char* cmd,
                                                const uint8_t* param,
                                                std::size_t param_size) {
    ControlPlaneSendSummary summary;
    summary.command = cmd;
    const std::vector<uint8_t> msg = buildLegacyCmd(cmd, param, param_size);
    for (auto& fe : frontends) {
        const uint32_t du_id = fe.first;
        if (!fe.second) {
            LOG_ERROR << "ControlPlane: fatal missing frontend du_id=" << du_id << " cmd=" << cmd;
            summary.fatal_failure_count++;
            summary.fatal_failed_du_ids.push_back(du_id);
            summary.should_abort = true;
            return summary;
        }
        if (!fe.second->isTransportReady()) {
            LOG_ERROR << "ControlPlane: fatal transport not ready du_id=" << du_id << " cmd=" << cmd;
            summary.fatal_failure_count++;
            summary.fatal_failed_du_ids.push_back(du_id);
            summary.should_abort = true;
            return summary;
        }
        Frontend* fe_ptr = fe.second.get();
        const bool ok = startupControlPlaneSendWithRetry(
            [fe_ptr, &msg]() { fe_ptr->sendTCP(const_cast<uint8_t*>(msg.data()), msg.size()); },
            kStartupControlPlaneMaxAttempts,
            kStartupControlPlaneRetryDelayMs);
        if (ok) {
            summary.success_count++;
        } else {
            logControlPlaneRetryExhausted(cmd, du_id);
            summary.failure_count++;
            summary.failed_du_ids.push_back(du_id);
        }
    }
    return summary;
}

ControlPlaneSendSummary sendStartupConfPerDu(std::map<uint32_t, std::unique_ptr<Frontend>>& frontends,
                                             const std::map<uint32_t, std::vector<uint8_t>>& shadows) {
    ControlPlaneSendSummary summary;
    summary.command = "CONF";
    for (auto& fe : frontends) {
        const uint32_t du_id = fe.first;
        const auto it = shadows.find(du_id);
        if (it == shadows.end()) {
            LOG_ERROR << "ControlPlane: fatal missing shadow for du_id=" << du_id << " cmd=CONF";
            throw SoftwareException("Legacy shadowlist: missing compiled shadow for DU");
        }
        const std::vector<uint8_t>& shadow = it->second;
        std::vector<uint8_t> msg = buildLegacyCmd("CONF", shadow.data(), shadow.size());
        if (!fe.second) {
            LOG_ERROR << "ControlPlane: fatal missing frontend du_id=" << du_id << " cmd=CONF";
            summary.fatal_failure_count++;
            summary.fatal_failed_du_ids.push_back(du_id);
            summary.should_abort = true;
            return summary;
        }
        if (!fe.second->isTransportReady()) {
            LOG_ERROR << "ControlPlane: fatal transport not ready du_id=" << du_id << " cmd=CONF";
            summary.fatal_failure_count++;
            summary.fatal_failed_du_ids.push_back(du_id);
            summary.should_abort = true;
            return summary;
        }
        Frontend* fe_ptr = fe.second.get();
        const bool ok = startupControlPlaneSendWithRetry(
            [fe_ptr, msg = std::move(msg)]() mutable {
                fe_ptr->sendTCP(msg.data(), msg.size());
            },
            kStartupControlPlaneMaxAttempts,
            kStartupControlPlaneRetryDelayMs);
        if (ok) {
            summary.success_count++;
        } else {
            logControlPlaneRetryExhausted("CONF", du_id);
            summary.failure_count++;
            summary.failed_du_ids.push_back(du_id);
        }
    }
    return summary;
}

void stopBuilderNoexcept(Builder* builder) {
    try {
        if (builder) {
            builder->stop();
        }
    } catch (...) {
        logCaughtExceptionNoexcept("App: stopBuilderNoexcept");
    }
}

void validateAdvancedGateFields(const grand::LegacyAppConfig& app, uint64_t effective_timeout_ns) {
    uint64_t effective_timeout_cap_ns = kLegacyDefaultTimeoutCapNs;
    if (app.t3TriggerTimeOutCap.has_value()) {
        if (app.t3TriggerTimeOutCap.value() <= 0) {
            throw ConfigException("t3TriggerTimeOutCap must be positive");
        }
        effective_timeout_cap_ns = static_cast<uint64_t>(app.t3TriggerTimeOutCap.value());
    }

    if (app.t3GateIdleGap.has_value() && app.t3GateIdleGap.value() < 0) {
        throw ConfigException("t3GateIdleGap must be non-negative");
    }

    if (app.t3ClosedBucketRetention.has_value()) {
        if (app.t3ClosedBucketRetention.value() <= 0) {
            throw ConfigException("t3ClosedBucketRetention must be positive");
        }
    }

    if (app.t3GateExtendOnProgressOnly.has_value() &&
        !app.t3GateExtendOnProgressOnly.value()) {
        throw ConfigException(kProgressAwareOnlyError);
    }

    grand::GateDeadlineMode deadline_mode = grand::GateDeadlineMode::LastArrivalLegacy;
    if (app.t3GateDeadlineMode.has_value()) {
        if (parseGateDeadlineMode(app.t3GateDeadlineMode.value(), deadline_mode) ==
            GateDeadlineModeParseKind::Invalid) {
            throw ConfigException(
                "t3GateDeadlineMode must be one of "
                "'first_arrival_fixed', 'first_arrival_bounded_sliding', "
                "'first_arrival', or 'last_arrival'");
        }
    }

    if (deadline_mode == grand::GateDeadlineMode::FirstArrivalBoundedSliding &&
        effective_timeout_cap_ns < effective_timeout_ns) {
        throw ConfigException(
            "t3TriggerTimeOutCap must be greater than or equal to t3TriggerTimeOut in "
            "first_arrival_bounded_sliding mode");
    }
}

} // namespace

namespace grand {

// 从 sysconfig global 段 + 当前活跃 DU 列表生成 T3Config；0 值字段用代码默认
T3Config buildT3RuntimeConfig(const LegacyAppConfig& app,
                              const std::vector<uint32_t>& active_du_ids) {
    T3Config cfg;
    const uint64_t effective_timeout_ns =
        app.t3TriggerTimeOut > 0 ? app.t3TriggerTimeOut : cfg.timeOutNs;
    validateAdvancedGateFields(app, effective_timeout_ns);
    cfg.timeCutNs = app.t3TriggerTimeCut > 0 ? app.t3TriggerTimeCut : cfg.timeCutNs;
    cfg.timeOutNs = app.t3TriggerTimeOut > 0 ? app.t3TriggerTimeOut : cfg.timeOutNs;
    cfg.timeWindowNs = app.t3TriggerTimeWindow > 0 ? app.t3TriggerTimeWindow : cfg.timeWindowNs;
    cfg.triggerThresholdDus =
        app.t3TriggerDuNumber > 0 ? app.t3TriggerDuNumber : cfg.triggerThresholdDus;
    cfg.triggerType = app.t3TriggerType;
    cfg.causalWindowEnabled = app.t3CausalWindowEnable;
    cfg.causalToleranceNs = app.t3CausalTimeToleranceNs;
    cfg.duplicateFilterEnable = app.t3DuplicateFilterEnable;
    cfg.duplicateTimeDiffNs = app.t3DuplicateTimeDiffThresholdNs;
    cfg.duplicateHistorySize = app.t3DuplicateHistorySize;
    cfg.duplicateMinPair = app.t3DuplicateMinPair;
    cfg.duplicateRejectLogEnable = app.t3DuplicateRejectLogEnable;
    cfg.duplicateRejectLogPath = app.t3DuplicateRejectLogPath;
    cfg.duplicateRejectLogQueueCapacity = app.t3DuplicateRejectLogQueueCapacity;
    cfg.templateMatchFilterEnable = app.t3TemplateMatchFilterEnable;
    cfg.templateMatchCoefficient = app.t3TemplateMatchCoefficient;
    if (app.t3TriggerTimeOutCap.has_value()) {
        cfg.timeoutCapNs = static_cast<uint64_t>(app.t3TriggerTimeOutCap.value());
    }
    if (app.t3GateIdleGap.has_value()) {
        cfg.idleGapNs = static_cast<uint64_t>(app.t3GateIdleGap.value());
    }
    if (app.t3ClosedBucketRetention.has_value()) {
        cfg.closedBucketRetentionNs =
            static_cast<uint64_t>(app.t3ClosedBucketRetention.value());
    }
    if (app.t3GateDeadlineMode.has_value()) {
        GateDeadlineMode parsed = cfg.deadlineMode;
        if (parseGateDeadlineMode(app.t3GateDeadlineMode.value(), parsed) ==
            GateDeadlineModeParseKind::Invalid) {
            throw ConfigException(
                "t3GateDeadlineMode must be one of "
                "'first_arrival_fixed', 'first_arrival_bounded_sliding', "
                "'first_arrival', or 'last_arrival'");
        }
        cfg.deadlineMode = parsed;
    }
    if (app.t3GateExtendOnProgressOnly.has_value()) {
        cfg.extendOnProgressOnly = app.t3GateExtendOnProgressOnly.value();
    }
    cfg.expectedDuIds = active_du_ids;
    if (!cfg.expectedDuIds.empty()) {
        cfg.advanceCompletionMode = GateAdvanceCompletionMode::ShadowOnly;
    }
    return cfg;
}

} // namespace grand

App::App(const std::string& config_file)
    : status_("idle")
    , config_file_(config_file)
{}

App::~App() {
    try {
        terminate();
    } catch (...) {
        logCaughtExceptionNoexcept("App::~App");
    }
}

// 在 `ConfigManager` 已 apply 与 `legacy` 一致的前提下，重建 Builder/DataStore/Handler/Frontend 与 `t3_config_`。
void App::rebuildInitializedMembersFromLegacyYaml(const grand::LegacyYamlConfig& legacy) {
    auto& config_manager = ConfigManager::instance();
    const uint64_t effective_timeout_ns =
        legacy.app.t3TriggerTimeOut > 0 ? legacy.app.t3TriggerTimeOut : grand::T3Config{}.timeOutNs;
    validateAdvancedGateFields(legacy.app, effective_timeout_ns);

    std::vector<uint32_t> active_du_ids;
    active_du_ids.reserve(config_manager.getElectronicsConfigs().size());
    for (const auto& elec_config : config_manager.getElectronicsConfigs()) {
        active_du_ids.push_back(elec_config.du_id);
    }
    grand::T3Config t3cfg = grand::buildT3RuntimeConfig(legacy.app, active_du_ids);

    if (!legacy.app.antennaDistancesFile.empty()) {
        std::filesystem::path antenna_path(legacy.app.antennaDistancesFile);
        if (antenna_path.is_relative()) {
            const std::filesystem::path config_dir =
                std::filesystem::absolute(std::filesystem::path(config_file_)).parent_path();
            antenna_path = config_dir / antenna_path;
        }
        t3cfg.antennaDistancesFile = antenna_path.lexically_normal().string();
    } else {
        t3cfg.antennaDistancesFile.clear();
    }
    if (t3cfg.triggerType > 2) {
        throw ConfigException("Unsupported t3TriggerType: " + std::to_string(t3cfg.triggerType));
    }
    if (t3cfg.triggerType == 2 && t3cfg.causalWindowEnabled) {
        if (t3cfg.antennaDistancesFile.empty()) {
            throw ConfigException(
                "antennaDistancesFile must be specified when t3TriggerType=2 and t3CausalWindowEnable=true");
        }

        std::ifstream infile(t3cfg.antennaDistancesFile);
        if (!infile) {
            throw ConfigException("antennaDistancesFile is not a usable distance table: " +
                                  t3cfg.antennaDistancesFile);
        }

        bool usable = false;
        std::string line;
        while (std::getline(infile, line)) {
            std::istringstream iss(line);
            uint32_t du_a = 0;
            uint32_t du_b = 0;
            double distance_m = 0.0;
            if (iss >> du_a >> du_b >> distance_m) {
                usable = true;
                break;
            }
        }
        if (!usable) {
            throw ConfigException("antennaDistancesFile is not a usable distance table: " +
                                  t3cfg.antennaDistancesFile);
        }
    }

    std::set<uint32_t> channel_tags;
    for (const auto& elec_config : config_manager.getElectronicsConfigs()) {
        channel_tags.insert(elec_config.du_id);
    }
    // Builder 队列扩容：2026-04-16 观测到 1024 pending 在 >1000 Hz 峰值下
    // 会爆 admission_rejected（一次 run 累计 27973 次 / 3.28% T3 fire 被整个
    // 丢弃）。扩到 16384 给 2000 Hz × 5s 峰值留 1.6 倍 headroom。
    // event_timeout 调至 15000ms：实测观测到最晚回包约 13s，15s 覆盖长尾。
    // 大部分事件在 1-2s 内凑齐，少量晚到的不会长期占用 pending 队列。
    // Field run 2026-04-27: keep pending capacity high enough for short bursts;
    // this is a runtime buffer cap, not a physics selection gate.
    auto new_builder = std::make_unique<Builder>(
        channel_tags,
        std::chrono::milliseconds(legacy.app.builderEventTimeoutMs),
        kBuilderMaxPendingEvents);
    DataStore::Config store_cfg{};
    store_cfg.module = "GP80";
    store_cfg.l1_tag = legacy.app.l1FileName.empty() ? std::string("L1") : legacy.app.l1FileName;
    store_cfg.l3_tag = legacy.app.l3FileName.empty() ? std::string("L3") : legacy.app.l3FileName;
    store_cfg.l1_output_dir = config_manager.getSoftwareConfig().l1_store_path;
    store_cfg.l3_output_dir = config_manager.getSoftwareConfig().l3_store_path;
    store_cfg.max_file_size_bytes = static_cast<std::size_t>(legacy.app.maxFileSizeMB) * 1024U * 1024U;
    store_cfg.daq_mode = legacy.app.daqMode;
    store_cfg.max_event_number_saved = legacy.app.eventNumberSaved;
    auto new_data_store = std::make_unique<DataStore>(store_cfg);

    std::map<uint32_t, std::unique_ptr<DataHandler>> new_handlers;
    std::map<uint32_t, std::unique_ptr<Frontend>> new_frontends;
    for (const auto& elec_config : config_manager.getElectronicsConfigs()) {
        const uint32_t du_id = elec_config.du_id;
        new_handlers[du_id] = std::make_unique<DataHandler>(
            elec_config,
            [this](SubFragmentHeader& header, uint8_t* payload) {
                onSubFragmentFromDataHandler(header, payload);
            },
            [this](uint32_t source_du_id, const uint8_t* payload, std::size_t payload_size, WaveformEnvelopeKind envelope) {
                if (!data_store_ || payload == nullptr || payload_size == 0) {
                    return;
                }
                if (envelope == WaveformEnvelopeKind::GP300RawEvent) {
                    if (!data_store_->writeRawRecord(source_du_id, payload, payload_size)) {
                        LOG_ERROR << "DataStore writeRawRecord failed, du_id=" << source_du_id
                                  << ", errno=" << data_store_->lastIoErrno()
                                  << ", detail=" << data_store_->lastIoError();
                    }
                }
            },
            static_cast<uint64_t>(t3cfg.timeCutNs));

        new_frontends[du_id] = std::make_unique<Frontend>(
            elec_config,
            std::bind(&DataHandler::handle_data, new_handlers[du_id].get(), std::placeholders::_1,
                       std::placeholders::_2));
    }

    for (const auto& frontend : new_frontends) {
        frontend.second->initialize();
    }

    ParamManager::getInstance();

    t3_config_ = std::move(t3cfg);
    builder_ = std::move(new_builder);
    data_store_ = std::move(new_data_store);
    data_handlers_ = std::move(new_handlers);
    frontends_ = std::move(new_frontends);

    // 可选：通过环境变量启用 regression timestamp 过滤
    // CSDAQ_DROP_REGRESSED_T2=1 → 过滤掉乱序 timestamp 不送入 Gate
    if (const char* p = std::getenv("CSDAQ_DROP_REGRESSED_T2"); p && std::string(p) != "0") {
        for (auto& [du_id, handler] : data_handlers_) {
            if (handler) handler->setDropRegressedTimestamps(true);
        }
    }
}

// 读 YAML → ConfigManager、构造 T3 参数、Builder/DataStore 骨架（不启 ZMQ）
void App::initialize() {
    if (status_ != "idle") {
        throw SoftwareException("App is not idle, cannot initialize");
    }

    status_ = "initializing";
    try {
        const grand::LegacyYamlConfig legacy = grand::CsYamlConfigLoader::loadSysConfig(config_file_);
        const uint64_t effective_timeout_ns =
            legacy.app.t3TriggerTimeOut > 0 ? legacy.app.t3TriggerTimeOut : grand::T3Config{}.timeOutNs;
        validateAdvancedGateFields(legacy.app, effective_timeout_ns);

        ConfigManager::instance().applyLegacyYamlConfig(legacy);

        t3_trigger_tag_.store(0, std::memory_order_relaxed);

        resetT3TriggerNoexcept();

        rebuildInitializedMembersFromLegacyYaml(legacy);
        last_applied_legacy_yaml_ = legacy;

        status_ = "initialized";
    } catch (...) {
        frontends_.clear();
        data_handlers_.clear();
        builder_.reset();
        data_store_.reset();
        resetT3TriggerNoexcept();
        last_applied_legacy_yaml_.reset();
        status_ = "idle";
        throw;
    }
}

void App::stopLocalPipelineNoexcept() {
    for (auto& fe : frontends_) {
        try {
            if (fe.second) {
                fe.second->stop();
            }
        } catch (...) {
            logCaughtExceptionNoexcept("App::stopLocalPipelineNoexcept frontend stop");
        }
    }
    for (auto& dh : data_handlers_) {
        try {
            if (dh.second) {
                dh.second->after_stop();
            }
        } catch (...) {
            logCaughtExceptionNoexcept("App::stopLocalPipelineNoexcept data_handler after_stop");
        }
    }
}

void App::stopNetworkLegacyCommandBestEffort(const char* cmd, const uint8_t* param, std::size_t param_size) {
    try {
        std::vector<uint8_t> msg = buildLegacyCmd(cmd, param, param_size);
        for (auto& fe : frontends_) {
            try {
                if (fe.second) {
                    fe.second->sendTCP(msg.data(), msg.size());
                }
            } catch (...) {
                logCaughtExceptionNoexcept("App::stopNetworkLegacyCommandBestEffort per-DU send");
            }
        }
    } catch (...) {
        logCaughtExceptionNoexcept("App::stopNetworkLegacyCommandBestEffort build/send");
    }
}

void App::rollbackStartedLocalNoexcept() {
    stopNetworkLegacyCommandBestEffort("STOP", nullptr, 0);
    waitDotriggerQuiescent();
    stopLocalPipelineNoexcept();
    resetT3TriggerNoexcept();
    stopBuilderNoexcept(builder_.get());
    drainBuilderCompletedToStore();
    quiesceCdWritesBeforeCloseStoreNoexcept();
    drainBuilderDiagnosticsToLog();
    try {
        if (data_store_) {
            data_store_->close();
        }
    } catch (...) {
        logCaughtExceptionNoexcept("App::rollbackStartedLocalNoexcept data_store close");
    }
    for (auto& fe : frontends_) {
        try {
            if (fe.second) {
                fe.second->unconfigure();
            }
        } catch (...) {
            logCaughtExceptionNoexcept("App::rollbackStartedLocalNoexcept unconfigure");
        }
    }
    status_ = "initialized";
}

void App::sendLegacyCommandAllThrowing(const char* cmd, const uint8_t* param, std::size_t param_size) {
    std::vector<uint8_t> msg = buildLegacyCmd(cmd, param, param_size);
    for (auto& fe : frontends_) {
        if (!fe.second) {
            throw SoftwareException("Frontend missing for legacy command");
        }
        fe.second->sendTCP(msg.data(), msg.size());
    }
}

void App::terminate() {
    const std::string st = status_;
    if (st == "idle") {
        return;
    }

    if (st == "initializing") {
        resetT3TriggerNoexcept();
        frontends_.clear();
        data_handlers_.clear();
        builder_.reset();
        data_store_.reset();
        ParamManager::getInstance().clearParams();
        status_ = "idle";
        return;
    }

    if (st == "running" || st == "starting" || st == "stopping") {
        stopNetworkLegacyCommandBestEffort("STOP", nullptr, 0);
        waitDotriggerQuiescent();
        stopLocalPipelineNoexcept();
        resetT3TriggerNoexcept();
        stopBuilderNoexcept(builder_.get());
        drainBuilderCompletedToStore();
        quiesceCdWritesBeforeCloseStoreNoexcept();
        drainBuilderDiagnosticsToLog();
        logStopObservabilitySummary();
        try {
            if (data_store_) {
                data_store_->close();
            }
        } catch (...) {
            logCaughtExceptionNoexcept("App::terminate data_store close");
        }
    }

    status_ = "terminating";

    for (const auto& fe : frontends_) {
        try {
            if (fe.second && fe.second->isTransportReady()) {
                auto msg = buildLegacyCmd("TERM", nullptr, 0);
                fe.second->sendTCP(msg.data(), msg.size());
            }
        } catch (...) {
            logCaughtExceptionNoexcept("App::terminate TERM send");
        }
    }

    frontends_.clear();
    data_handlers_.clear();
    builder_.reset();
    data_store_.reset();
    resetT3TriggerNoexcept();
    ParamManager::getInstance().clearParams();
    status_ = "idle";
}

// shadowlist → DataStore open → Builder/DataHandler/Frontend 启停序列 → ZMQ 广播 STOP/TERM/INIT/CONF/STAR
void App::start() {
    if (status_ != "initialized") {
        throw SoftwareException("App is not initialized, cannot start");
    }

    std::string addr_map_yaml;
    std::string readable_conf_yaml;
    if (!resolveLegacyElecYamlPaths(config_file_, addr_map_yaml, readable_conf_yaml)) {
        throw SoftwareException(
            "Legacy shadowlist: missing DU-address-map.yaml or DU-readable-conf.yaml (check GRAND_DAQ_CONFIG or cfgs/ next to sysconfig)");
    }
    grand::ElecConfig::instance()->load(addr_map_yaml, readable_conf_yaml);

    std::map<uint32_t, std::vector<uint8_t>> shadows;
    for (const auto& fe : frontends_) {
        std::vector<uint8_t> buf(static_cast<size_t>(Reg_End), 0);
        const size_t used =
            grand::ElecConfig::instance()->toShadowlist(buf.data(), std::to_string(fe.first));
        if (used == 0 || used > buf.size()) {
            throw SoftwareException("Legacy shadowlist: empty or invalid compile for DU " +
                                    std::to_string(fe.first));
        }
        buf.resize(used);
        shadows[fe.first] = std::move(buf);
    }

    status_ = "starting";

    try {
        if (data_store_ && !data_store_->open()) {
            throw SoftwareException("Failed to open DataStore output files");
        }
        const uint32_t cd_run_number = data_store_ ? data_store_->currentCDRunNumber() : 0U;
        createAndBindT3TriggerForRun(cd_run_number);
        builder_->start();

        for (const auto& data_handler : data_handlers_) {
            data_handler.second->before_start();
        }

        for (const auto& frontend : frontends_) {
            frontend.second->configure();
        }
        for (const auto& frontend : frontends_) {
            frontend.second->start();
        }

        {
            const uint32_t inv_stop =
                control_plane_test_invalidate_before_stop_du_.exchange(0, std::memory_order_acq_rel);
            if (inv_stop != 0) {
                auto it = frontends_.find(inv_stop);
                if (it != frontends_.end() && it->second) {
                    try {
                        it->second->unconfigure();
                    } catch (...) {
                        logCaughtExceptionNoexcept("App::start test invalidate transport before STOP");
                    }
                }
            }
        }

        auto stop_summary = sendStartupBroadcastCmd(frontends_, "STOP", nullptr, 0);
        logControlPlaneSummary(stop_summary);
        if (stop_summary.should_abort) {
            throw SoftwareException("Startup abort: STOP pre-clean fatal failure");
        }

        {
            const int pre_clean_stop_term_ms = static_cast<int>(readEnvSizeT(
                "GRAND_PRE_CLEAN_STOP_TERM_MS", static_cast<size_t>(kStopToTermGapMsDefault)));
            sleepMs(pre_clean_stop_term_ms);
        }

        auto term_summary = sendStartupBroadcastCmd(frontends_, "TERM", nullptr, 0);
        logControlPlaneSummary(term_summary);
        if (term_summary.should_abort) {
            throw SoftwareException("Startup abort: TERM pre-clean fatal failure");
        }

        sleepMs(kControlPlanePhaseGapMs);

        {
            const uint32_t inv_du =
                control_plane_test_invalidate_before_init_du_.exchange(0, std::memory_order_acq_rel);
            if (inv_du != 0) {
                auto it = frontends_.find(inv_du);
                if (it != frontends_.end() && it->second) {
                    try {
                        it->second->unconfigure();
                    } catch (...) {
                        logCaughtExceptionNoexcept("App::start test invalidate transport before INIT");
                    }
                }
            }
        }

        auto init_summary = sendStartupBroadcastCmd(frontends_, "INIT", nullptr, 0);
        logControlPlaneSummary(init_summary);
        if (init_summary.should_abort ||
            (init_summary.success_count == 0 && init_summary.failure_count > 0)) {
            throw SoftwareException("Startup abort: INIT failed");
        }

        sleepMs(kControlPlanePhaseGapMs);

        auto conf_summary = sendStartupConfPerDu(frontends_, shadows);
        logControlPlaneSummary(conf_summary);
        if (conf_summary.should_abort ||
            (conf_summary.success_count == 0 && conf_summary.failure_count > 0)) {
            throw SoftwareException("Startup abort: CONF failed");
        }

        sleepMs(kControlPlanePhaseGapMs);

        {
            const uint32_t inv_star =
                control_plane_test_invalidate_before_star_du_.exchange(0, std::memory_order_acq_rel);
            if (inv_star != 0) {
                auto it = frontends_.find(inv_star);
                if (it != frontends_.end() && it->second) {
                    try {
                        it->second->unconfigure();
                    } catch (...) {
                        logCaughtExceptionNoexcept("App::start test invalidate transport before STAR");
                    }
                }
            }
        }

        auto star_summary = sendStartupBroadcastCmd(frontends_, "STAR", nullptr, 0);
        logControlPlaneSummary(star_summary);
        if (star_summary.should_abort ||
            (star_summary.success_count == 0 && star_summary.failure_count > 0)) {
            throw SoftwareException("Startup abort: STAR failed");
        }

        sleepMs(kControlPlanePhaseGapMs);

        status_ = "running";
    } catch (...) {
        rollbackStartedLocalNoexcept();
        throw;
    }
}

// STOP、等 do_trigger 排空、停 Frontend/本地管线、刷 Builder、关 DataStore
void App::stop() {
    if (status_ != "running") {
        throw SoftwareException("App is not running, cannot stop");
    }

    status_ = "stopping";

    stopNetworkLegacyCommandBestEffort("STOP", nullptr, 0);
    waitDotriggerQuiescent();
    stopLocalPipelineNoexcept();
    resetT3TriggerNoexcept();
    stopBuilderNoexcept(builder_.get());
    drainBuilderCompletedToStore();
    quiesceCdWritesBeforeCloseStoreNoexcept();
    drainBuilderDiagnosticsToLog();
    logStopObservabilitySummary();
    if (data_store_) {
        data_store_->close();
    }

    status_ = "initialized";
}

void App::reloadYamlConfig() {
    if (status_ != "initialized") {
        throw SoftwareException("reloadYamlConfig requires status \"initialized\" (stopped-state reload only); "
                                "current status is \"" +
                                status_ + "\"");
    }
    if (!last_applied_legacy_yaml_.has_value()) {
        throw SoftwareException("reloadYamlConfig: internal error, missing last_applied_legacy_yaml_ snapshot");
    }

    const grand::LegacyYamlConfig new_legacy = grand::CsYamlConfigLoader::loadSysConfig(config_file_);

    try {
        ConfigManager::instance().applyLegacyYamlConfig(new_legacy);
        rebuildInitializedMembersFromLegacyYaml(new_legacy);
        last_applied_legacy_yaml_ = new_legacy;
    } catch (...) {
        ConfigManager::instance().applyLegacyYamlConfig(*last_applied_legacy_yaml_);
        throw;
    }
}

std::string App::status() {
    return status_;
}

void App::waitDotriggerQueueInflightDrainUnderLock(std::unique_lock<std::mutex>& lk) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        if (dotrigger_queue_.empty() && dotrigger_inflight_ == 0) {
            return;
        }
        dotrigger_cv_.wait_until(lk, deadline, [this] {
            return dotrigger_queue_.empty() && dotrigger_inflight_ == 0;
        });
        if (dotrigger_queue_.empty() && dotrigger_inflight_ == 0) {
            return;
        }
    }
    LOG_ERROR << "App: DOTRIGGER queue/inflight drain timed out after 30s (queue_size="
              << dotrigger_queue_.size() << " inflight=" << dotrigger_inflight_ << ")";
}

void App::waitDotriggerQuiescent() {
    std::unique_lock<std::mutex> lk(dotrigger_queue_mutex_);
    dotrigger_accept_.store(false, std::memory_order_release);
    dotrigger_cv_.notify_all();
    waitDotriggerQueueInflightDrainUnderLock(lk);
}

uint64_t App::dotriggerLatencyKey(EventTag tag, uint32_t du_id) {
    return (static_cast<uint64_t>(tag) << 32U) | static_cast<uint64_t>(du_id);
}

void App::recordDotriggerSend(EventTag tag,
                              uint32_t du_id,
                              std::chrono::steady_clock::time_point sent_at) {
    std::lock_guard<std::mutex> lk(dotrigger_latency_mutex_);
    dotrigger_send_times_[dotriggerLatencyKey(tag, du_id)] = sent_at;
    ++dotrigger_du_latency_stats_[du_id].sent_count;
}

void App::recordDotriggerResponse(const SubFragmentHeader& header) {
    if (header.waveform_envelope != WaveformEnvelopeKind::GP300DaqEvent || header.event_tag == 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(dotrigger_latency_mutex_);
    const auto key = dotriggerLatencyKey(header.event_tag, header.du_id);
    auto it = dotrigger_send_times_.find(key);
    auto& du_stats = dotrigger_du_latency_stats_[header.du_id];
    if (it == dotrigger_send_times_.end()) {
        ++dotrigger_response_missing_send_count_;
        ++du_stats.response_missing_send_count;
        return;
    }

    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(now - it->second).count();
    dotrigger_send_times_.erase(it);
    ++dotrigger_response_count_;
    ++du_stats.response_count;
    dotrigger_response_last_ms_ = elapsed_ms;
    du_stats.response_last_ms = elapsed_ms;
    dotrigger_response_sum_ms_ += elapsed_ms;
    du_stats.response_sum_ms += elapsed_ms;
    if (elapsed_ms > dotrigger_response_max_ms_) {
        dotrigger_response_max_ms_ = elapsed_ms;
    }
    if (elapsed_ms > du_stats.response_max_ms) {
        du_stats.response_max_ms = elapsed_ms;
    }
    if (dotrigger_response_slow_threshold_ms_ > 0.0 &&
        elapsed_ms >= dotrigger_response_slow_threshold_ms_) {
        ++dotrigger_response_slow_count_;
        ++du_stats.response_slow_count;
    }
}

void App::resetDotriggerLatencyTracking(double slow_threshold_ms) {
    std::lock_guard<std::mutex> lk(dotrigger_latency_mutex_);
    dotrigger_send_times_.clear();
    dotrigger_du_latency_stats_.clear();
    dotrigger_response_count_ = 0;
    dotrigger_response_missing_send_count_ = 0;
    dotrigger_response_slow_count_ = 0;
    dotrigger_response_last_ms_ = 0.0;
    dotrigger_response_sum_ms_ = 0.0;
    dotrigger_response_max_ms_ = 0.0;
    dotrigger_response_slow_threshold_ms_ = slow_threshold_ms;
}

App::DotriggerLatencySnapshot App::dotriggerLatencySnapshot() const {
    std::lock_guard<std::mutex> lk(dotrigger_latency_mutex_);
    for (auto& [_, stats] : dotrigger_du_latency_stats_) {
        stats.pending_count = 0;
    }
    for (const auto& [key, _] : dotrigger_send_times_) {
        const uint32_t du_id = static_cast<uint32_t>(key & 0xffffffffULL);
        ++dotrigger_du_latency_stats_[du_id].pending_count;
    }
    DotriggerLatencySnapshot snap{};
    snap.pending = dotrigger_send_times_.size();
    snap.response_count = dotrigger_response_count_;
    snap.response_missing_send_count = dotrigger_response_missing_send_count_;
    snap.response_slow_count = dotrigger_response_slow_count_;
    snap.response_last_ms = dotrigger_response_last_ms_;
    snap.response_avg_ms = dotrigger_response_count_ == 0
        ? 0.0
        : dotrigger_response_sum_ms_ / static_cast<double>(dotrigger_response_count_);
    snap.response_max_ms = dotrigger_response_max_ms_;
    snap.response_slow_threshold_ms = dotrigger_response_slow_threshold_ms_;
    return snap;
}

void App::processDotriggerDispatch(const grand::TriggerResult& result, uint32_t tag) {
    const int pause_ms = dotrigger_test_pause_before_send_ms_.load(std::memory_order_relaxed);
    if (pause_ms > 0) {
        sleepMs(pause_ms);
    }
    std::lock_guard<std::mutex> send_lk(dotrigger_send_mutex_);
    // 触发阶段已由 T3Filter 做过因果/去重筛选，这里直接按 du_ids/trigger_timestamps 分派：
    // - trigger_type==0/1：T3Filter 退化为全通过，du_ids/trigger_timestamps 等价于全窗口
    // - trigger_type==2：一般只发给因果子集；高重数旁路会保留该候选窗口的全部 selected ts
    const uint32_t trace_count = static_cast<uint32_t>(result.trigger_timestamps.size());
    const std::size_t payload_size = result.trigger_timestamps.size() * sizeof(uint64_t) +
                                     sizeof(tag) + sizeof(trace_count);
    std::vector<uint8_t> payload(payload_size);
    std::size_t offset = 0;
    if (!result.trigger_timestamps.empty()) {
        const std::size_t ts_bytes = result.trigger_timestamps.size() * sizeof(uint64_t);
        std::memcpy(payload.data(), result.trigger_timestamps.data(), ts_bytes);
        offset += ts_bytes;
    }
    std::memcpy(payload.data() + offset, &tag, sizeof(tag));
    offset += sizeof(tag);
    std::memcpy(payload.data() + offset, &trace_count, sizeof(trace_count));

    std::vector<uint8_t> msg = buildLegacyCmd("DOTRIGGER", payload.data(), payload.size());
    dotrigger_dispatch_count_.fetch_add(1, std::memory_order_relaxed);
    std::vector<uint32_t> sent_ok;
    sent_ok.reserve(result.du_ids.size());
    for (uint32_t du_id : result.du_ids) {
        auto it = frontends_.find(du_id);
        if (it == frontends_.end() || !it->second) {
            LOG_ERROR << "DOTRIGGER not sent: missing frontend for du_id=" << du_id;
            dotrigger_send_fail_count_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        try {
            it->second->sendTCP(msg.data(), msg.size());
            recordDotriggerSend(tag, du_id, std::chrono::steady_clock::now());
            sent_ok.push_back(du_id);
            dotrigger_send_success_count_.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception& e) {
            LOG_ERROR << "DOTRIGGER send failed for du_id=" << du_id << ": " << e.what();
            dotrigger_send_fail_count_.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            LOG_ERROR << "DOTRIGGER send failed for du_id=" << du_id << " (non-std exception)";
            dotrigger_send_fail_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (sent_ok.empty()) {
        return;
    }

    if (builder_) {
        builder_->beginEvent(tag, sent_ok, result.trigger_timestamps, result.timestamp_ns);
    }
}

void App::joinDotriggerWorkerDrain() {
    {
        std::lock_guard<std::mutex> lk(dotrigger_queue_mutex_);
        dotrigger_accept_.store(false, std::memory_order_release);
        dotrigger_shutdown_.store(true, std::memory_order_release);
    }
    dotrigger_cv_.notify_all();
    if (dotrigger_worker_.joinable()) {
        dotrigger_worker_.join();
    }
    {
        std::lock_guard<std::mutex> lk(dotrigger_queue_mutex_);
        dotrigger_queue_.clear();
        dotrigger_inflight_ = 0;
    }
    dotrigger_shutdown_.store(false, std::memory_order_release);
}

void App::startDotriggerWorkerIfNeeded() {
    if (dotrigger_worker_.joinable()) {
        return;
    }
    dotrigger_accept_.store(true, std::memory_order_release);
    dotrigger_shutdown_.store(false, std::memory_order_release);
    dotrigger_worker_ = std::thread([this] { dotriggerWorkerLoop(); });
}

void App::dotriggerWorkerLoop() {
    for (;;) {
        std::unique_lock<std::mutex> lk(dotrigger_queue_mutex_);
        dotrigger_cv_.wait(lk, [this] {
            return !dotrigger_queue_.empty() ||
                   dotrigger_shutdown_.load(std::memory_order_acquire);
        });
        while (!dotrigger_queue_.empty()) {
            DotriggerWorkItem item = std::move(dotrigger_queue_.front());
            dotrigger_queue_.pop_front();
            ++dotrigger_inflight_;
            dotrigger_cv_.notify_all();
            lk.unlock();
            processDotriggerDispatch(item.result, item.tag);
            lk.lock();
            --dotrigger_inflight_;
            dotrigger_cv_.notify_all();
        }
        if (dotrigger_shutdown_.load(std::memory_order_acquire)) {
            break;
        }
    }
}

// 为本次触发分配 tag，入队 do_trigger 工作线程；队列满则同步降级 processDotriggerDispatch
void App::handleT3TriggerResult(const grand::TriggerResult& result) {
    if (result.du_ids.empty() || result.trigger_timestamps.empty()) {
        return;
    }
    const uint32_t tag = t3_trigger_tag_.fetch_add(1, std::memory_order_relaxed) + 1;

    if (!dotrigger_accept_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lk(dotrigger_queue_mutex_);
        while (dotrigger_queue_.size() >= kDotriggerQueueCapacity) {
            dotrigger_cv_.wait(lk, [this] {
                return dotrigger_queue_.size() < kDotriggerQueueCapacity;
            });
        }
        dotrigger_queue_.push_back(DotriggerWorkItem{result, tag});
        dotrigger_shutdown_enqueue_count_.fetch_add(1, std::memory_order_relaxed);
        dotrigger_cv_.notify_one();
        return;
    }

    bool overflow = false;
    {
        std::lock_guard<std::mutex> lk(dotrigger_queue_mutex_);
        if (dotrigger_queue_.size() >= kDotriggerQueueCapacity) {
            overflow = true;
        } else {
            dotrigger_queue_.push_back(DotriggerWorkItem{result, tag});
        }
    }
    if (overflow) {
        dotrigger_overflow_count_.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR << "DOTRIGGER dispatch queue overflow; falling back to inline send for this item";
        processDotriggerDispatch(result, tag);
        return;
    }
    dotrigger_cv_.notify_one();
}

// DataHandler 解析波形后回调：进 Builder 并尝试落盘已完成 CD 片段
void App::onSubFragmentFromDataHandler(SubFragmentHeader& header, uint8_t* data) {
    if (!builder_) {
        return;
    }
    recordDotriggerResponse(header);
    builder_->addFragment(header, data);
    drainBuilderCompletedToStore();
}

// 仅 GP300DaqEvent 片段写 CD，event_key 用 Builder 的 tag；实际写盘在 CD worker。
void App::drainBuilderCompletedToStore() {
    if (!builder_) {
        return;
    }
    auto completed = builder_->drainCompleted();
    if (!data_store_) {
        drainBuilderDiagnosticsToLog();
        return;
    }
    std::vector<CdWriteWorkItem> items_to_enqueue;
    items_to_enqueue.reserve(completed.size());
    for (auto& ev : completed) {
        if (ev.is_partial && kLogPartialEventDetails) {
            std::ostringstream missing_oss;
            bool first = true;
            for (uint32_t du : ev.expected_du_ids) {
                if (ev.actual_du_ids.find(du) == ev.actual_du_ids.end()) {
                    if (!first) missing_oss << ",";
                    missing_oss << du;
                    first = false;
                }
            }
            LOG_WARN << "PARTIAL_EVENT: tag=" << ev.tag
                     << " expected=" << ev.expected_du_ids.size()
                     << " actual=" << ev.actual_du_ids.size()
                     << " missing=[" << missing_oss.str() << "]";
        }
        CdWriteWorkItem item;
        item.event_key = ev.tag;
        for (auto& fragment : ev.fragments) {
            if (fragment.waveform_envelope != WaveformEnvelopeKind::GP300DaqEvent) {
                continue;
            }
            if (fragment.payload.empty()) {
                continue;
            }
            CdEventFragment out_fragment;
            out_fragment.du_id = fragment.du_id;
            out_fragment.payload = std::move(fragment.payload);
            item.fragments.push_back(std::move(out_fragment));
        }
        if (item.fragments.empty()) {
            continue;
        }
        items_to_enqueue.push_back(std::move(item));
    }
    {
        std::unique_lock<std::mutex> lk(cd_write_queue_mutex_);
        for (auto& item : items_to_enqueue) {
            while (cd_write_queue_.size() >= kCdWriteQueueCapacity) {
                LOG_WARN << "App: CD write queue at capacity; blocking until space (capacity="
                         << kCdWriteQueueCapacity << ")";
                cd_write_cv_.wait(lk, [this] {
                    return cd_write_queue_.size() < kCdWriteQueueCapacity;
                });
            }
            cd_write_queue_.push_back(std::move(item));
            cd_write_cv_.notify_one();
        }
    }
    drainBuilderDiagnosticsToLog();
}

void App::waitCdWriteQueueInflightDrainUnderLock(std::unique_lock<std::mutex>& lk) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cd_write_queue_.empty() && cd_write_inflight_ == 0) {
            return;
        }
        cd_write_cv_.wait_until(lk, deadline, [this] {
            return cd_write_queue_.empty() && cd_write_inflight_ == 0;
        });
        if (cd_write_queue_.empty() && cd_write_inflight_ == 0) {
            return;
        }
    }
    LOG_ERROR << "App: CD write queue/inflight drain timed out after 30s (queue_size="
              << cd_write_queue_.size() << " inflight=" << cd_write_inflight_ << ")";
}

void App::joinCdWriteWorkerDrainNoexcept() noexcept {
    try {
        {
            std::lock_guard<std::mutex> lk(cd_write_queue_mutex_);
            cd_write_shutdown_.store(true, std::memory_order_release);
        }
        cd_write_cv_.notify_all();
        if (cd_write_worker_.joinable()) {
            cd_write_worker_.join();
        }
        std::lock_guard<std::mutex> lk(cd_write_queue_mutex_);
        cd_write_queue_.clear();
        cd_write_inflight_ = 0;
        cd_write_shutdown_.store(false, std::memory_order_release);
        cd_write_accept_.store(true, std::memory_order_release);
    } catch (...) {
    }
}

void App::quiesceCdWritesBeforeCloseStoreNoexcept() noexcept {
    try {
        {
            std::unique_lock<std::mutex> lk(cd_write_queue_mutex_);
            cd_write_accept_.store(false, std::memory_order_release);
            cd_write_cv_.notify_all();
            waitCdWriteQueueInflightDrainUnderLock(lk);
        }
        joinCdWriteWorkerDrainNoexcept();
    } catch (...) {
    }
}

void App::startCdWriteWorkerIfNeeded() {
    if (cd_write_worker_.joinable()) {
        return;
    }
    cd_write_accept_.store(true, std::memory_order_release);
    cd_write_shutdown_.store(false, std::memory_order_release);
    cd_write_worker_ = std::thread([this] { cdWriteWorkerLoop(); });
}

void App::cdWriteWorkerLoop() {
    for (;;) {
        std::unique_lock<std::mutex> lk(cd_write_queue_mutex_);
        cd_write_cv_.wait(lk, [this] {
            return !cd_write_queue_.empty() ||
                   cd_write_shutdown_.load(std::memory_order_acquire);
        });
        while (!cd_write_queue_.empty()) {
            CdWriteWorkItem item = std::move(cd_write_queue_.front());
            cd_write_queue_.pop_front();
            ++cd_write_inflight_;
            cd_write_cv_.notify_all();
            lk.unlock();
            try {
                if (data_store_) {
                    std::vector<DataStore::CdFragmentRecord> records;
                    records.reserve(item.fragments.size());
                    for (const auto& fragment : item.fragments) {
                        records.push_back(DataStore::CdFragmentRecord{
                            fragment.du_id,
                            fragment.payload.data(),
                            fragment.payload.size(),
                        });
                    }
                    if (!data_store_->writeCdEvent(item.event_key, records)) {
                        LOG_ERROR << "DataStore writeCdEvent failed (async CD worker), event_key="
                                  << item.event_key << ", fragments=" << item.fragments.size()
                                  << ", errno=" << data_store_->lastIoErrno()
                                  << ", detail=" << data_store_->lastIoError();
                    }
                }
            } catch (...) {
                logCaughtExceptionNoexcept("App::cdWriteWorkerLoop writeCdEvent");
            }
            lk.lock();
            --cd_write_inflight_;
            cd_write_cv_.notify_all();
        }
        if (cd_write_shutdown_.load(std::memory_order_acquire)) {
            break;
        }
    }
}

void App::drainBuilderDiagnosticsToLog() {
    if (!builder_) {
        return;
    }
    for (const auto& diag : builder_->drainDiagnostics()) {
        LOG_WARN << "Builder diag type=" << builderDiagnosticTypeName(diag.type)
                 << " tag=" << diag.tag
                 << " du_id=" << diag.du_id
                 << " trigger_number=" << diag.trigger_number
                 << " fragment_trigger_time=" << diag.fragment_trigger_time
                 << " event_trigger_timestamp_ns=" << diag.event_trigger_timestamp_ns;
    }
}

void App::logStopObservabilitySummary() {
    size_t queue_depth = 0;
    size_t inflight = 0;
    {
        std::lock_guard<std::mutex> lk(dotrigger_queue_mutex_);
        queue_depth = dotrigger_queue_.size();
        inflight = dotrigger_inflight_;
    }
    LOG_INFO << "DOTRIGGER stop summary: queue_depth=" << queue_depth
             << " inflight=" << inflight
             << " overflow_count=" << dotrigger_overflow_count_.load(std::memory_order_relaxed)
             << " shutdown_enqueue_count="
             << dotrigger_shutdown_enqueue_count_.load(std::memory_order_relaxed);

    if (builder_) {
        LOG_INFO << "Builder stop summary: pending=" << builder_->pendingCount()
                 << " completed=" << builder_->completedCount()
                 << " timeout=" << builder_->timeoutCount()
                 << " unmatched=" << builder_->unmatchedCount();
    }

    if (data_store_) {
        LOG_INFO << "DataStore stop summary: rotates=" << data_store_->rotateCount()
                 << " io_failures=" << data_store_->ioFailureCount()
                 << " last_errno=" << data_store_->lastIoErrno()
                 << " last_error=" << data_store_->lastIoError();
    }

    for (const auto& kv : frontends_) {
        if (!kv.second) {
            continue;
        }
        LOG_INFO << "Frontend stop summary: du_id=" << kv.first
                 << " ring_drops=" << kv.second->ringDropCount()
                 << " ring_buffered=" << kv.second->ringBufferedBytes()
                 << " ring_capacity=" << kv.second->ringCapacityBytes();
    }
}

nlohmann::json App::collectHealthSnapshot() const {
    nlohmann::json j;

    uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    int active_count = 0;
    nlohmann::json du_details = nlohmann::json::array();
    uint64_t zmq_send_call_count = 0;
    uint64_t zmq_send_eagain_count = 0;
    uint64_t zmq_send_retry_count = 0;
    uint64_t zmq_send_retry_success_count = 0;
    uint64_t zmq_send_deadline_fail_count = 0;
    uint64_t zmq_send_non_retry_fail_count = 0;
    for (const auto& [du_id, fe] : frontends_) {
        if (!fe) continue;
        uint64_t last = fe->lastActiveNs();
        bool active = (last > 0 && now_ns > last && (now_ns - last) < 5000000000ULL);
        if (active) active_count++;
        const auto send_stats = fe->zmqSendStats();
        zmq_send_call_count += send_stats.send_call_count;
        zmq_send_eagain_count += send_stats.eagain_count;
        zmq_send_retry_count += send_stats.retry_count;
        zmq_send_retry_success_count += send_stats.retry_success_count;
        zmq_send_deadline_fail_count += send_stats.deadline_fail_count;
        zmq_send_non_retry_fail_count += send_stats.non_retry_fail_count;
        du_details.push_back({
            {"du_id", du_id},
            {"active", active},
            {"ring_drops", fe->ringDropCount()},
            {"ring_buffered_bytes", fe->ringBufferedBytes()},
            {"zmq_send_call_count", send_stats.send_call_count},
            {"zmq_send_eagain_count", send_stats.eagain_count},
            {"zmq_send_retry_count", send_stats.retry_count},
            {"zmq_send_retry_success_count", send_stats.retry_success_count},
            {"zmq_send_deadline_fail_count", send_stats.deadline_fail_count},
            {"zmq_send_non_retry_fail_count", send_stats.non_retry_fail_count}
        });
    }
    j["active_du_count"] = active_count;
    j["du_details"] = du_details;

    if (builder_) {
        nlohmann::json missing_per_du = nlohmann::json::object();
        for (const auto& [du_id, count] : builder_->statMissingOnTimeoutPerDu()) {
            missing_per_du[std::to_string(du_id)] = count;
        }
        j["builder"] = {
            {"registered_events", builder_->statRegisteredEvents()},
            {"expected_fragments", builder_->statExpectedFragments()},
            {"received_fragments", builder_->statReceivedFragments()},
            {"complete_events", builder_->statCompleteEvents()},
            {"timeout_events", builder_->statTimeoutEvents()},
            {"missing_on_timeout", builder_->statMissingOnTimeout()},
            {"missing_on_timeout_per_du", missing_per_du},
            {"partial_events", builder_->statPartialEvents()},
            {"partial_missing_fragments", builder_->statPartialMissingFragments()},
            {"pending", builder_->pendingCount()},
            {"max_pending_events", builder_->maxPendingEvents()},
            {"completed_total", builder_->completedCount()},
            {"timeout_total", builder_->timeoutCount()},
            {"unmatched_total", builder_->unmatchedCount()},
            {"diag_no_fifo", builder_->diagNoFifoCount()},
            {"diag_timestamp_mismatch", builder_->diagTimestampMismatchCount()},
            {"diag_duplicate", builder_->diagDuplicateCount()},
            {"diag_admission_rejected", builder_->diagAdmissionRejectedCount()}
        };
        uint64_t exp = builder_->statExpectedFragments();
        uint64_t rcv = builder_->statReceivedFragments();
        if (exp > 0) {
            j["builder"]["fragment_receive_rate"] = static_cast<double>(rcv) / exp;
        } else {
            j["builder"]["fragment_receive_rate"] = nullptr;
        }

        const auto loss = builder_->lossStatsSnapshot(10, 200);
        nlohmann::json loss_buckets = nlohmann::json::array();
        for (const auto& bucket : loss.buckets) {
            std::vector<std::pair<uint32_t, Builder::LossDuStats>> top_du(bucket.per_du.begin(),
                                                                          bucket.per_du.end());
            std::sort(top_du.begin(), top_du.end(),
                      [](const auto& a, const auto& b) {
                          if (a.second.missing != b.second.missing) {
                              return a.second.missing > b.second.missing;
                          }
                          return a.first < b.first;
                      });
            nlohmann::json top_missing_du = nlohmann::json::array();
            for (const auto& [du_id, du_stats] : top_du) {
                if (du_stats.missing == 0) {
                    continue;
                }
                top_missing_du.push_back({
                    {"du_id", du_id},
                    {"missing", du_stats.missing},
                    {"expected", du_stats.expected == 0 ? nlohmann::json(nullptr) : nlohmann::json(du_stats.expected)},
                    {"loss_pct", du_stats.expected == 0 ? nlohmann::json(nullptr) : nlohmann::json(du_stats.lossPct())}
                });
            }
            loss_buckets.push_back({
                {"start_unix_ms", bucket.start_unix_ms},
                {"expected_fragments", bucket.expected_fragments},
                {"missing_fragments", bucket.missing_fragments},
                {"timeout_events", bucket.timeout_events},
                {"loss_pct", bucket.lossPct()},
                {"top_missing_du", top_missing_du}
            });
        }
        nlohmann::json recent_events = nlohmann::json::array();
        for (const auto& event : loss.recent_events) {
            recent_events.push_back({
                {"tag", event.tag},
                {"event_ts_ns", event.event_ts_ns},
                {"observed_unix_ms", event.observed_unix_ms},
                {"expected", event.expected},
                {"attached", event.attached},
                {"missing_count", event.missing_du_ids.size()},
                {"missing_du", event.missing_du_ids}
            });
        }
        j["loss_stats"] = {
            {"bucket_sec", loss.bucket_sec},
            {"retention_sec", loss.retention_sec},
            {"telemetry_dropped", loss.telemetry_dropped},
            {"totals", {
                {"expected_fragments", loss.totals.expected_fragments},
                {"missing_fragments", loss.totals.missing_fragments},
                {"timeout_events", loss.totals.timeout_events},
                {"loss_pct", loss.totals.lossPct()}
            }},
            {"buckets", loss_buckets},
            {"recent_events", recent_events}
        };
    }

    uint64_t wc = 0;
    uint64_t bucket_wait_sum_ns = 0;
    uint64_t bucket_wait_max_ns = 0;
    grand::GateLogic::Stats gs{};
    grand::TriggerPipeline::T3FilterStats fs{};
    bool has_t3_trigger = false;
    {
        std::lock_guard<std::mutex> lk(t3_dispatch_mutex_);
        if (t3_trigger_) {
            has_t3_trigger = true;
            wc = t3_trigger_->bucketWaitCount();
            bucket_wait_sum_ns = t3_trigger_->bucketWaitSumNs();
            bucket_wait_max_ns = t3_trigger_->bucketWaitMaxNs();
            gs = t3_trigger_->getGateStats();
            fs = t3_trigger_->getT3FilterStats();
        }
    }
    if (has_t3_trigger) {
        j["gate"] = {
            {"bucket_wait_count", wc},
            {"bucket_wait_max_ms", bucket_wait_max_ns / 1000000.0},
            {"pending_bucket_count", gs.pending_bucket_count},
            {"bucket_timeout_count", gs.bucket_timeout_count},
            {"late_arrival_after_close_count", gs.late_arrival_after_close_count},
            {"late_arrival_guard_count", gs.late_arrival_guard_count},
            {"late_arrival_ingress_close_count", gs.late_arrival_ingress_close_count},
            {"late_arrival_age_max_ms", gs.late_arrival_age_max_ns / 1000000.0},
            {"force_ready_count", gs.force_ready_count}
        };
        j["t3_filter"] = {
            {"candidate_window_count", fs.candidate_window_count},
            {"causal_eval_count", fs.causal_eval_count},
            {"causal_input_detector_sum", fs.causal_input_detector_sum},
            {"causal_input_detector_max", fs.causal_input_detector_max},
            {"causal_selected_detector_sum", fs.causal_selected_detector_sum},
            {"causal_selected_detector_max", fs.causal_selected_detector_max},
            {"causal_reject_count", fs.causal_reject_count},
            {"duplicate_reject_count", fs.duplicate_reject_count},
            {"high_multiplicity_bypass_count", fs.high_multiplicity_bypass_count},
            {"high_multiplicity_bypass_input_detector_sum", fs.high_multiplicity_bypass_input_detector_sum},
            {"high_multiplicity_bypass_input_detector_max", fs.high_multiplicity_bypass_input_detector_max},
            {"high_multiplicity_bypass_unique_du_sum", fs.high_multiplicity_bypass_unique_du_sum},
            {"high_multiplicity_bypass_unique_du_max", fs.high_multiplicity_bypass_unique_du_max},
            {"maximum_clique_time_count", fs.maximum_clique_time_count},
            {"maximum_clique_time_sum_ms", fs.maximum_clique_time_sum_ns / 1000000.0},
            {"maximum_clique_time_max_ms", fs.maximum_clique_time_max_ns / 1000000.0},
            {"template_match_enabled",
             last_applied_legacy_yaml_
                 ? last_applied_legacy_yaml_->app.t3TemplateMatchFilterEnable
                 : false},
            {"template_match_coefficient",
             last_applied_legacy_yaml_
                 ? last_applied_legacy_yaml_->app.t3TemplateMatchCoefficient
                 : 0.0},
            {"template_match_eval_count", fs.template_match_eval_count},
            {"template_match_reject_count", fs.template_match_reject_count}
        };
        if (fs.template_match_eval_count > 0) {
            j["t3_filter"]["template_match_avg_related_value"] =
                static_cast<double>(fs.template_match_avg_sum_scaled) /
                static_cast<double>(fs.template_match_eval_count) /
                10000.0;
        } else {
            j["t3_filter"]["template_match_avg_related_value"] = nullptr;
        }
        if (fs.template_match_avg_max_scaled > 0) {
            j["t3_filter"]["template_match_max_related_value"] =
                static_cast<double>(fs.template_match_avg_max_scaled) / 10000.0;
        } else {
            j["t3_filter"]["template_match_max_related_value"] = nullptr;
        }
        if (fs.maximum_clique_time_count > 0) {
            j["t3_filter"]["maximum_clique_time_avg_ms"] =
                static_cast<double>(fs.maximum_clique_time_sum_ns) /
                static_cast<double>(fs.maximum_clique_time_count) / 1000000.0;
        } else {
            j["t3_filter"]["maximum_clique_time_avg_ms"] = nullptr;
        }
        if (wc > 0) {
            j["gate"]["bucket_wait_avg_ms"] = bucket_wait_sum_ns / wc / 1000000.0;
        } else {
            j["gate"]["bucket_wait_avg_ms"] = nullptr;
        }
        if (gs.late_arrival_after_close_count > 0) {
            j["gate"]["late_arrival_age_avg_ms"] =
                gs.late_arrival_age_sum_ns / gs.late_arrival_after_close_count / 1000000.0;
        } else {
            j["gate"]["late_arrival_age_avg_ms"] = nullptr;
        }
    }

    nlohmann::json t2_ordering = nlohmann::json::object();
    uint64_t affected_du_count = 0;
    uint64_t total_monotonic_violation_events = 0;
    uint64_t max_backward_ns = 0;
    for (const auto& [du_id, handler] : data_handlers_) {
        if (!handler) continue;
        const uint64_t violations = handler->monotonicViolationCount();
        const uint64_t du_max_backward_ns = handler->maxMonotonicBackwardNs();
        total_monotonic_violation_events += violations;
        if (du_max_backward_ns > max_backward_ns) {
            max_backward_ns = du_max_backward_ns;
        }
        if (violations > 0) {
            ++affected_du_count;
            t2_ordering[std::to_string(du_id)] = {
                {"monotonic_violation_count", violations},
                {"max_backward_ns", du_max_backward_ns},
                {"last_observed_timestamp_ns", handler->lastObservedTimestampNs()},
                {"last_violation_prev_ts", handler->lastViolationPrevTimestampNs()},
                {"last_violation_current_ts", handler->lastViolationCurrentTimestampNs()},
                {"last_violation_prev_tm_id", handler->lastViolationPrevTmId()},
                {"last_violation_current_tm_id", handler->lastViolationCurrentTmId()}
            };
        }
    }
    j["t2_ordering"] = {
        {"affected_du_count", affected_du_count},
        {"total_monotonic_violation_events", total_monotonic_violation_events},
        {"max_backward_ns", max_backward_ns},
        {"per_du", t2_ordering}
    };

    if (data_store_) {
        j["data_store"] = {
            {"io_failures", data_store_->ioFailureCount()},
            {"rotate_count", data_store_->rotateCount()},
            {"last_errno", data_store_->lastIoErrno()}
        };
    }

    {
        std::size_t q_depth = 0;
        std::size_t inflight = 0;
        {
            std::lock_guard<std::mutex> lk(dotrigger_queue_mutex_);
            q_depth = dotrigger_queue_.size();
            inflight = dotrigger_inflight_;
        }
        const auto latency = dotriggerLatencySnapshot();
        nlohmann::json dotrigger_per_du = nlohmann::json::object();
        {
            std::lock_guard<std::mutex> latency_lk(dotrigger_latency_mutex_);
            for (const auto& [du_id, stats] : dotrigger_du_latency_stats_) {
                dotrigger_per_du[std::to_string(du_id)] = {
                    {"sent_count", stats.sent_count},
                    {"response_count", stats.response_count},
                    {"response_pending", stats.pending_count},
                    {"response_missing_send_count", stats.response_missing_send_count},
                    {"response_slow_count", stats.response_slow_count},
                    {"response_last_ms", stats.response_count == 0 ? nlohmann::json(nullptr) : nlohmann::json(stats.response_last_ms)},
                    {"response_avg_ms", stats.response_count == 0 ? nlohmann::json(nullptr) : nlohmann::json(stats.response_sum_ms / static_cast<double>(stats.response_count))},
                    {"response_max_ms", stats.response_count == 0 ? nlohmann::json(nullptr) : nlohmann::json(stats.response_max_ms)}
                };
            }
        }
        j["dotrigger"] = {
            {"overflow_count", dotrigger_overflow_count_.load(std::memory_order_relaxed)},
            {"dispatch_count_trigger_level", dotrigger_dispatch_count_.load(std::memory_order_relaxed)},
            {"send_success_count_per_du", dotrigger_send_success_count_.load(std::memory_order_relaxed)},
            {"send_fail_count_per_du", dotrigger_send_fail_count_.load(std::memory_order_relaxed)},
            {"zmq_send_call_count", zmq_send_call_count},
            {"zmq_send_eagain_count", zmq_send_eagain_count},
            {"zmq_send_retry_count", zmq_send_retry_count},
            {"zmq_send_retry_success_count", zmq_send_retry_success_count},
            {"zmq_send_deadline_fail_count", zmq_send_deadline_fail_count},
            {"zmq_send_non_retry_fail_count", zmq_send_non_retry_fail_count},
            {"queue_depth", q_depth},
            {"inflight", inflight},
            {"response_pending", latency.pending},
            {"response_count", latency.response_count},
            {"response_missing_send_count", latency.response_missing_send_count},
            {"response_slow_count", latency.response_slow_count},
            {"response_last_ms", latency.response_count == 0 ? nlohmann::json(nullptr) : nlohmann::json(latency.response_last_ms)},
            {"response_avg_ms", latency.response_count == 0 ? nlohmann::json(nullptr) : nlohmann::json(latency.response_avg_ms)},
            {"response_max_ms", latency.response_count == 0 ? nlohmann::json(nullptr) : nlohmann::json(latency.response_max_ms)},
            {"response_slow_threshold_ms", latency.response_slow_threshold_ms},
            {"per_du", dotrigger_per_du}
        };
    }

    j["status"] = status_;
    return j;
}

void App::resetT3TriggerNoexcept() {
    {
        std::lock_guard<std::mutex> lk(dotrigger_queue_mutex_);
        dotrigger_accept_.store(false, std::memory_order_release);
        dotrigger_cv_.notify_all();
    }

    grand::T3Trigger* trigger = nullptr;
    {
        std::lock_guard<std::mutex> lk(t3_dispatch_mutex_);
        if (t3_trigger_) {
            trigger = t3_trigger_.get();
        }
    }

    try {
        if (trigger) {
            trigger->stop();
        }
    } catch (...) {
        logCaughtExceptionNoexcept("App::resetT3TriggerNoexcept trigger->stop");
    }

    {
        std::unique_lock<std::mutex> lk(dotrigger_queue_mutex_);
        waitDotriggerQueueInflightDrainUnderLock(lk);
    }

    {
        std::lock_guard<std::mutex> lk(t3_dispatch_mutex_);
        if (t3_trigger_) {
            t3_trigger_->setTriggerResultCallback({});
        }
        for (auto& kv : data_handlers_) {
            if (kv.second) {
                kv.second->set_t3_trigger(nullptr);
            }
        }
        t3_trigger_.reset();
    }

    joinDotriggerWorkerDrain();
}

// 新建 T3Trigger 实例并注入各 DataHandler::set_t3_trigger
void App::createAndBindT3TriggerForRun(uint32_t cd_run_number) {
    resetT3TriggerNoexcept();
    joinCdWriteWorkerDrainNoexcept();
    t3_trigger_tag_.store(0, std::memory_order_relaxed);
    dotrigger_overflow_count_.store(0, std::memory_order_relaxed);
    dotrigger_shutdown_enqueue_count_.store(0, std::memory_order_relaxed);
    dotrigger_dispatch_count_.store(0, std::memory_order_relaxed);
    dotrigger_send_success_count_.store(0, std::memory_order_relaxed);
    dotrigger_send_fail_count_.store(0, std::memory_order_relaxed);
    const double dotrigger_slow_threshold_ms = last_applied_legacy_yaml_.has_value()
        ? static_cast<double>(last_applied_legacy_yaml_->app.builderEventTimeoutMs)
        : 0.0;
    resetDotriggerLatencyTracking(dotrigger_slow_threshold_ms);
    startDotriggerWorkerIfNeeded();
    startCdWriteWorkerIfNeeded();
    grand::T3Config run_t3_config = t3_config_;
    if (run_t3_config.duplicateRejectLogEnable) {
        run_t3_config.duplicateRejectLogPath =
            grand::DuplicateRejectLogger::makeRunLogPath(
                run_t3_config.duplicateRejectLogPath,
                cd_run_number,
                std::time(nullptr)).string();
    }
    auto new_t3_trigger = std::make_unique<grand::T3Trigger>(std::move(run_t3_config));
    new_t3_trigger->setTriggerResultCallback(
        [this](const grand::TriggerResult& result) { handleT3TriggerResult(result); });
    {
        std::lock_guard<std::mutex> lk(t3_dispatch_mutex_);
        t3_trigger_ = std::move(new_t3_trigger);
        for (auto& kv : data_handlers_) {
            if (kv.second) {
                kv.second->set_t3_trigger(t3_trigger_.get());
            }
        }
    }
}
