#include <cs_yaml_config.h>

#include <yaml-cpp/yaml.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace grand {
namespace {

// 缺键或类型失败时返回 def，不抛异常（非关键字段）
template <typename T>
T getScalarOr(const YAML::Node& n, const char* key, const T& def) {
    if (!n || !n[key]) return def;
    try {
        return n[key].as<T>();
    } catch (...) {
        return def;
    }
}

// 启动关键字段：键存在但解析失败则抛 runtime_error
template <typename T>
T getScalarStartup(const YAML::Node& n, const char* key, const T& def, const char* section_label) {
    if (!n || !n[key]) {
        return def;
    }
    try {
        return n[key].as<T>();
    } catch (const YAML::Exception& e) {
        throw std::runtime_error(std::string("Invalid startup-critical field '") + key + "' in " +
                                 section_label + ": " + e.what());
    }
}

template <typename T>
std::optional<T> getOptionalScalarStrict(const YAML::Node& n, const char* key) {
    if (!n || !n[key]) {
        return std::nullopt;
    }
    try {
        return n[key].as<T>();
    } catch (const YAML::Exception& e) {
        throw std::runtime_error(std::string("Invalid value for '") + key + "': " + e.what());
    }
}

// 校验 dataUnits[] 单条：必填 ID/ip/port，写回 out
void parseDataUnitStrict(const YAML::Node& du, LegacyDUConfig* out) {
    if (!du || !du.IsMap()) {
        throw std::runtime_error("dataUnits[] entry must be a map");
    }
    LegacyDUConfig cfg;
    cfg.type = getScalarOr<std::string>(du, "type", "");

    if (!du["ID"]) {
        throw std::runtime_error("dataUnits[] missing required field 'ID'");
    }
    try {
        const uint32_t id_num = du["ID"].as<uint32_t>();
        cfg.id = std::to_string(id_num);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error(std::string("Invalid dataUnits[].ID: ") + e.what());
    }

    cfg.ip = getScalarStartup<std::string>(du, "ip", "", "dataUnits[]");
    if (cfg.ip.empty()) {
        throw std::runtime_error("dataUnits[].ip must be non-empty");
    }

    const uint32_t port_raw = getScalarStartup<uint32_t>(du, "port", 0, "dataUnits[]");
    if (port_raw == 0 || port_raw > 65535U) {
        throw std::runtime_error("dataUnits[].port must be between 1 and 65535");
    }
    cfg.port = port_raw;

    *out = std::move(cfg);
}

} // namespace

// 读 global + dataUnits，可选 gate 字段用 optional 区分「未写」与「写了无效」
LegacyYamlConfig CsYamlConfigLoader::loadSysConfig(const std::string& sysconfig_yaml_path) {
    LegacyYamlConfig out;

    YAML::Node root = YAML::LoadFile(sysconfig_yaml_path);
    if (!root) {
        throw std::runtime_error("Failed to load YAML: " + sysconfig_yaml_path);
    }

    const YAML::Node global = root["global"];
    out.app.networkInputBufferSize =
        getScalarStartup<uint32_t>(global, "networkInputBufferSize", 0, "global");
    out.app.zmqSndBufferSize = getScalarStartup<uint32_t>(global, "zmqSndBufferSize", 0, "global");
    out.app.t2BufferPageSize = getScalarOr<uint32_t>(global, "t2BufferPageSize", 0);
    out.app.t2BufferNumberOfPages = getScalarOr<uint32_t>(global, "t2BufferNumberOfPages", 0);
    out.app.eventBufferPageSize = getScalarOr<uint32_t>(global, "eventBufferPageSize", 0);
    out.app.eventBufferNumberOfPages = getScalarOr<uint32_t>(global, "eventBufferNumberOfPages", 0);
    out.app.t3TriggerTimeCut = getScalarStartup<uint32_t>(global, "t3TriggerTimeCut", 0, "global");
    out.app.t3TriggerTimeOut = getScalarStartup<uint32_t>(global, "t3TriggerTimeOut", 0, "global");
    out.app.t3TriggerTimeWindow = getScalarStartup<uint32_t>(global, "t3TriggerTimeWindow", 0, "global");
    out.app.t3TriggerDuNumber = getScalarStartup<uint32_t>(global, "t3TriggerDuNumber", 0, "global");
    out.app.l1FileName = getScalarOr<std::string>(global, "l1FileName", "");
    out.app.l3FileName = getScalarOr<std::string>(global, "l3FileName", "");
    out.app.builderEventTimeoutMs = getScalarOr<uint32_t>(global, "builderEventTimeoutMs", 15000);
    out.app.daqMode = getScalarStartup<uint32_t>(global, "daqMode", 0, "global");
    out.app.eventNumberSaved = getScalarOr<uint32_t>(global, "eventNumberSaved", 0);
    out.app.t3TriggerType = getScalarStartup<uint32_t>(global, "t3TriggerType", 0, "global");
    out.app.t3CausalWindowEnable =
        getScalarStartup<bool>(global, "t3CausalWindowEnable", false, "global");
    out.app.t3CausalTimeToleranceNs =
        getScalarStartup<double>(global, "t3CausalTimeToleranceNs", 100.0, "global");
    out.app.t3DuplicateFilterEnable =
        getScalarStartup<bool>(global, "t3DuplicateFilterEnable", true, "global");
    out.app.t3DuplicateTimeDiffThresholdNs =
        getScalarStartup<int32_t>(global, "t3DuplicateTimeDiffThresholdNs", 100, "global");
    out.app.t3DuplicateHistorySize =
        getScalarStartup<int32_t>(global, "t3DuplicateHistorySize", 10, "global");
    out.app.t3DuplicateMinPair =
        getScalarStartup<int32_t>(global, "t3DuplicateMinPair", 2, "global");
    out.app.t3DuplicateRejectLogEnable =
        getScalarOr<bool>(global, "t3DuplicateRejectLogEnable", false);
    out.app.t3DuplicateRejectLogPath =
        getScalarOr<std::string>(global,
                                 "t3DuplicateRejectLogPath",
                                 "/data/home/grand/workarea/t3_data");
    out.app.t3DuplicateRejectLogQueueCapacity =
        getScalarOr<uint32_t>(global, "t3DuplicateRejectLogQueueCapacity", 4096U);
    out.app.t3TemplateMatchFilterEnable =
        getScalarOr<bool>(global, "t3TemplateMatchFilterEnable", false);
    out.app.t3TemplateMatchCoefficient =
        getScalarOr<double>(global, "t3TemplateMatchCoefficient", 0.0);
    out.app.dudaqFileInjection = getScalarOr<bool>(global, "dudaqFileInjection", false);
    out.app.antennaDistancesFile =
        getScalarStartup<std::string>(global, "antennaDistancesFile", "", "global");
    out.app.t3TriggerTimeOutCap = getOptionalScalarStrict<int64_t>(global, "t3TriggerTimeOutCap");
    out.app.t3GateIdleGap = getOptionalScalarStrict<int64_t>(global, "t3GateIdleGap");
    out.app.t3ClosedBucketRetention = getOptionalScalarStrict<int64_t>(global, "t3ClosedBucketRetention");
    out.app.t3GateDeadlineMode = getOptionalScalarStrict<std::string>(global, "t3GateDeadlineMode");
    out.app.t3GateExtendOnProgressOnly =
        getOptionalScalarStrict<bool>(global, "t3GateExtendOnProgressOnly");
    out.app.l1StorePath = getScalarOr<std::string>(global, "l1StorePath", "");
    out.app.l3StorePath = getScalarOr<std::string>(global, "l3StorePath", "");
    out.app.maxFileSizeMB = getScalarOr<uint32_t>(global, "maxFileSizeMB", 100);

    const YAML::Node dus = root["dataUnits"];
    if (!dus || !dus.IsSequence()) {
        throw std::runtime_error("sysconfig must contain a dataUnits sequence: " + sysconfig_yaml_path);
    }
    for (const auto& du : dus) {
        LegacyDUConfig cfg;
        parseDataUnitStrict(du, &cfg);
        out.dus.emplace_back(std::move(cfg));
    }

    if (out.dus.empty()) {
        throw std::runtime_error("dataUnits must contain at least one entry: " + sysconfig_yaml_path);
    }

    return out;
}

} // namespace grand

