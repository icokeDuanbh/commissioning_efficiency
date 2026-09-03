#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace grand {

// sysconfig 里一条 dataUnits 项的运行时映射字段
struct LegacyDUConfig {
    std::string id;   // 字符串形式的 DU ID（YAML 里多为数字，加载时转 string）
    std::string type; // 硬件类型标签，透传
    std::string ip;   // DU 控制/数据面 IP
    uint32_t port = 0; // TCP 端口，1..65535
};

// global 段映射到 T3/缓冲/DAQ 模式等，供 App 构造 TriggerPipeline 与 ZMQ
struct LegacyAppConfig {
    uint32_t networkInputBufferSize = 0;   // 网络读缓冲建议值
    uint32_t zmqSndBufferSize = 0;           // ZMQ 发送缓冲
    uint32_t t2BufferPageSize = 0;           // T2 环形缓冲页大小（legacy 字段名）
    uint32_t t2BufferNumberOfPages = 0;
    uint32_t eventBufferPageSize = 0;
    uint32_t eventBufferNumberOfPages = 0;
    uint32_t t3TriggerTimeCut = 0;           // T3 时间分桶粒度 (ns 量级，与 YAML 一致)
    uint32_t t3TriggerTimeOut = 0;           // 桶超时
    uint32_t t3TriggerTimeWindow = 0;        // nhit 窗口宽度
    uint32_t t3TriggerDuNumber = 0;          // 参与 T3 的 DU 数阈值相关（见 App 映射）
    std::string l1FileName;                // 外部 L1 文件名，可选
    std::string l3FileName;
    uint32_t builderEventTimeoutMs = 15000;  // Builder 单事件最大等待 (ms)
    uint32_t daqMode = 0;                    // 写入 ZMQ 第二帧的 daq_mode
    uint32_t eventNumberSaved = 0;           // 持久化事件数一类配置
    uint32_t t3TriggerType = 0;              // 触发类型标记进 TriggerResult
    bool t3CausalWindowEnable = false;       // 因果窗
    double t3CausalTimeToleranceNs = 100.0;  // 因果容差 (ns)
    bool t3DuplicateFilterEnable = true;     // 跨桶重复过滤
    int32_t t3DuplicateTimeDiffThresholdNs = 100;
    int32_t t3DuplicateHistorySize = 10;
    int32_t t3DuplicateMinPair = 2;
    bool t3DuplicateRejectLogEnable = false; // 记录 duplicate 过滤剔除的候选事例
    std::string t3DuplicateRejectLogPath;
    uint32_t t3DuplicateRejectLogQueueCapacity = 4096;
    bool t3TemplateMatchFilterEnable = false;
    double t3TemplateMatchCoefficient = 0.0;
    bool dudaqFileInjection = false;         // 调试：文件注入路径开关
    std::string antennaDistancesFile;        // 因果模式天线间距表
    // Keep advanced gate fields optional here so App can distinguish
    // omission (use old-cw defaults) from explicit invalid values.
    std::optional<int64_t> t3TriggerTimeOutCap;       // 超时上限；未设则 App 用默认
    std::optional<int64_t> t3GateIdleGap;             // 空闲间隙阈值
    std::optional<int64_t> t3ClosedBucketRetention; // 关桶后保留时间
    std::optional<std::string> t3GateDeadlineMode;  // 截止时间模式字符串，映射到枚举
    std::optional<bool> t3GateExtendOnProgressOnly;  // 是否仅在有新 hit 时延长截止
    std::string l1StorePath;                         // L1/MD .dat 输出目录；空则用默认
    std::string l3StorePath;                         // CD .dat 输出目录；空则用默认
    uint32_t maxFileSizeMB = 100;                    // 单文件轮转大小 (MB)
};

// 整份 sysconfig 的内存镜像
struct LegacyYamlConfig {
    LegacyAppConfig app;
    std::vector<LegacyDUConfig> dus;
};

class CsYamlConfigLoader {
public:
    // sysconfig_yaml_path - 绝对或相对路径；抛异常表示 YAML 非法或缺必填项
    static LegacyYamlConfig loadSysConfig(const std::string& sysconfig_yaml_path);
};

} // namespace grand

