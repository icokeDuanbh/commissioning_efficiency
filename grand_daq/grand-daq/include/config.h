#pragma once

#include <cs_yaml_config.h>

#include <string>
#include <vector>
#include <cstdint>

#define NUM_CHANNELS 8

/**
 * 逻辑默认相对路径（文档/对比用）；实际默认加载请用 resolvedDefaultSysconfigPath()。
 */
inline const char* defaultConfigPath() {
    return "cfgs/sysconfig.yaml";
}

/**
 * 解析后的默认 sysconfig 绝对路径（Linux：基于 /proc/self/exe 与 ../cfgs 或 ./cfgs）。
 * 可被环境变量 CSDAQ_SYSCONFIG 覆盖；若无法解析则回退为 defaultConfigPath() 字面值（仍可能依赖 cwd）。
 */
std::string resolvedDefaultSysconfigPath();

/**
 * 解析 `DU-address-map.yaml` 与 `DU-readable-conf.yaml` 路径（与 `old-cs-daq-cw` `resolveElecConfigPaths` 优先级一致：
 * GRAND_DAQ_CONFIG → sysconfig 同目录 → cwd/cfgs → cwd/gp300/cfgs）。
 */
bool resolveLegacyElecYamlPaths(const std::string& sysconfig_yaml_path,
                                std::string& out_du_address_map,
                                std::string& out_du_readable_conf);

/**
 * @brief 触发模式枚举
 */
enum class TriggerMode {
    SINGLE_BOARD = 0,  // 单串自触发
    EXTERNAL = 1       // 外部触发
};

/**
 * @brief 单条 RBCP 风格寄存器访问描述（legacy 辅助模型）
 *
 * 由 `ElectronicsConfig::configItems()` 等生成。**不是**当前 `App::start()` 下 DU 控制的主路径；
 * 现役板级控制序列在 `App` 内走 ZMQ `MT_CMD`（`STOP`/`TERM`/`CONF`/`STAR` 等），载荷侧走 `ZmqComm`。
 */
struct ElectronicsConfigItem {
    enum ItemType {
        WR = 0,
        RD = 1,
        RDWR = 2
    };

    ItemType type;                 // 访问类型
    uint32_t addr;               // RBCP 寄存器地址
    std::vector<uint8_t> data;   // 写入或读回缓冲（长度依协议）
    int timeout_ms;              // 单次访问超时
    int delay_ms;                // 与上一条指令间隔（脚本节拍）
    int retry;                   // 失败重试次数

    // delay_ms/timeout_ms/retry 默认与历史脚本一致；addr/data 为一条 RBCP 事务内容
    ElectronicsConfigItem(ItemType type, uint32_t addr, std::vector<uint8_t> data, int delay_ms = 0, int timeout_ms = 100, int retry = 1);
};

/**
 * @brief 电子学配置参数
 */
struct ElectronicsConfig {
    uint32_t du_id = 0;              // legacy：`dataUnits[].ID`（主路由键）
    std::string type;                // legacy：`dataUnits[].type`
    std::string ip;                  // legacy：`dataUnits[].ip`
    uint16_t port = 0;               // legacy：`dataUnits[].port`
    TriggerMode trigger_mode;        // 触发模式
    uint8_t trigger_threshold_nhit;  // 触发阈值nhit (1-8)
    uint16_t trigger_window_ns;      // 触发窗口大小 (ns)
    uint16_t readout_window_before_trigger_ns;      // 触发前读出时间窗 (ns)
    uint16_t readout_window_after_trigger_ns;       // 触发后读出时间窗 (ns)
    uint16_t waveform_points;       // 波形点数量
    uint16_t waveform_points_pre_threshold;      // 过阈前波形点数量
    std::vector<uint16_t> channel_thresholds; // 每个通道的阈值 (mV)
    std::vector<bool> channel_enable;     // 通道使能状态
    
    ElectronicsConfig();

    /** Legacy：由结构体字段展开为 RBCP 写寄存器序列；供 shadowlist/旧工具链等，非 ZMQ 运行时主控。 */
    std::vector<ElectronicsConfigItem> configItems() const;
    /** Legacy：触发启动寄存器序列；非当前 `Frontend::configure/start` 主路径。 */
    std::vector<ElectronicsConfigItem> startTriggerItems() const;
    /** Legacy：触发停止寄存器序列；非当前 `Frontend::stop` 主路径（现役 stop 为 `App` 编排 + ZMQ）。 */
    std::vector<ElectronicsConfigItem> stopTriggerItems() const;
};

/**
 * @brief 软件运行配置
 */
struct SoftwareConfig {
    std::string backend_ip;          // 后端计算机IP地址
    int backend_port;                // 后端计算机端口
    int electronics_tcp_port;        // 电子学板TCP端口
    int electronics_udp_port;        // 电子学板UDP端口
    int buffer_size;                 // 环形缓冲区大小
    std::size_t network_input_buffer_size = 2048000; // legacy：与 sysconfig networkInputBufferSize 对应
    std::size_t zmq_send_buffer_size = 2048000;       // legacy：与 sysconfig zmqSndBufferSize 对应
    /** legacy ZMQ：第二帧 `daq_mode`（`snprintf(..., "%d", daqMode)`），见 `old-cs-daq-cw` `ZmqAdapter::Config::daq_mode`。 */
    std::int32_t zmq_daq_mode = 0;
    /** `ZMQ_IDENTITY` 字节（非消息帧）。默认与旧系统一致。 */
    std::string zmq_dealer_identity = "CSDAQ";
    int zmq_snd_hwm = 10000;
    int log_level;                   // 日志级别
    std::string l1_store_path;       // L1/MD .dat 输出目录
    std::string l3_store_path;       // CD .dat 输出目录
    int http_port;                   // HTTP端口
    bool debug_raw;                  // 是否开启原始数据调试
    
    SoftwareConfig();
};

/**
 * @brief 配置管理类
 */
class ConfigManager {
public:
    static ConfigManager& instance() {
        static ConfigManager instance;
        return instance;
    }

    ~ConfigManager();

    // 从路径读 YAML，成功则替换单例内 software + DU 列表；失败保持旧快照
    bool loadLegacySysConfigYaml(const std::string& sysconfig_yaml_path);

    /** Apply an already-parsed legacy sysconfig snapshot into this singleton (rollback on failure). */
    // legacy - 已由 CsYamlConfigLoader 解析；异常时由调用方或 loadLegacySysConfigYaml 回滚
    void applyLegacyYamlConfig(const grand::LegacyYamlConfig& legacy);
    const SoftwareConfig& getSoftwareConfig() const { return software_config_; }
    const std::vector<ElectronicsConfig>& getElectronicsConfigs() const { return electronics_configs_; }
    int getElectronicsCount() const { return electronics_configs_.size(); }

private:
    ConfigManager();
    SoftwareConfig software_config_;                    // 进程级网络/HTTP/ZMQ 等
    std::vector<ElectronicsConfig> electronics_configs_; // 与 dataUnits 一一对应
}; 
