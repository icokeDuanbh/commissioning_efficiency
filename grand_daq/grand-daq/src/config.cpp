#include <common.h>
#include <utils.h>
#include <config.h>
#include <cs_yaml_config.h>
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <cstdint>

namespace {

/** RBCP 0x0A 等寄存器历史上一字节板号：从逻辑 du_id 局部推导，非配置字段。 */
inline uint8_t legacy_rbcp_board_byte(uint32_t du_id) {
    return static_cast<uint8_t>(du_id & 0xFFU);
}

} // namespace

// ElectronicsConfigItem 实现
ElectronicsConfigItem::ElectronicsConfigItem(ElectronicsConfigItem::ItemType type, 
        uint32_t addr, std::vector<uint8_t> data, int delay_ms, int timeout_ms, int retry)
    : type(type)
    , addr(addr)
    , data(data)
    , timeout_ms(timeout_ms)
    , delay_ms(delay_ms)
    , retry(retry)
{}

// ElectronicsConfig 实现
ElectronicsConfig::ElectronicsConfig() 
    : du_id(0)
    , port(0)
    , trigger_mode(TriggerMode::SINGLE_BOARD)
    , trigger_threshold_nhit(1)
    , readout_window_before_trigger_ns(100)
    , readout_window_after_trigger_ns(200)
    , waveform_points(200)
    , waveform_points_pre_threshold(100)
{
    channel_thresholds.resize(NUM_CHANNELS, 50); // 默认阈值50mV
    channel_enable.resize(NUM_CHANNELS, false);
}

// 优先 CSDAQ_SYSCONFIG；否则沿可执行文件相对 ../cfgs/sysconfig.yaml 等候选查找；失败则退回 defaultConfigPath() 字面量
std::string resolvedDefaultSysconfigPath() {
    if (const char* p = std::getenv("CSDAQ_SYSCONFIG"); p && *p) {
        return std::string(p);
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return std::string(defaultConfigPath());
    }
    fs::path binDir = exe.parent_path();
    const fs::path candidates[] = {
        (binDir / ".." / "cfgs" / "sysconfig.yaml").lexically_normal(),
        (binDir / ".." / ".." / "cfgs" / "sysconfig.yaml").lexically_normal(),
        (binDir / "cfgs" / "sysconfig.yaml").lexically_normal(),
    };
    for (const auto& cand : candidates) {
        if (fs::is_regular_file(cand, ec)) {
            fs::path abs = fs::weakly_canonical(cand, ec);
            if (!ec) {
                return abs.string();
            }
            return cand.string();
        }
    }
    return std::string(defaultConfigPath());
}

// 解析 DU-address-map.yaml 与 DU-readable-conf.yaml：GRAND_DAQ_CONFIG → sysconfig 同目录
bool resolveLegacyElecYamlPaths(const std::string& sysconfig_yaml_path,
                                std::string& out_du_address_map,
                                std::string& out_du_readable_conf)
{
    namespace fs = std::filesystem;
    auto file_ok = [](const fs::path& p) {
        std::error_code ec;
        return fs::is_regular_file(p, ec);
    };
    auto join_dir = [](const std::string& dir, const char* leaf) -> std::string {
        if (dir.empty() || dir == ".") {
            return std::string(leaf);
        }
        if (!dir.empty() && dir.back() == '/') {
            return dir + leaf;
        }
        return dir + "/" + leaf;
    };
    auto parent_dir = [](const std::string& file_path) -> std::string {
        const size_t pos = file_path.find_last_of('/');
        if (pos == std::string::npos) {
            return ".";
        }
        if (pos == 0) {
            return "/";
        }
        return file_path.substr(0, pos);
    };

    if (const char* env = std::getenv("GRAND_DAQ_CONFIG"); env && *env) {
        const fs::path base(env);
        const fs::path a = base / "DU-address-map.yaml";
        const fs::path b = base / "DU-readable-conf.yaml";
        if (file_ok(a) && file_ok(b)) {
            out_du_address_map = a.string();
            out_du_readable_conf = b.string();
            LOG_INFO << "resolveLegacyElecYamlPaths: using GRAND_DAQ_CONFIG=" << env
                     << " addr_map=" << out_du_address_map
                     << " readable_conf=" << out_du_readable_conf;
            return true;
        }
        LOG_ERROR << "resolveLegacyElecYamlPaths: GRAND_DAQ_CONFIG=" << env
                  << " set but files not found: " << a.string() << " / " << b.string();
        return false;
    }

    {
        const std::string d = parent_dir(sysconfig_yaml_path);
        const std::string a = join_dir(d, "DU-address-map.yaml");
        const std::string b = join_dir(d, "DU-readable-conf.yaml");
        if (file_ok(fs::path(a)) && file_ok(fs::path(b))) {
            out_du_address_map = a;
            out_du_readable_conf = b;
            LOG_INFO << "resolveLegacyElecYamlPaths: using sysconfig dir=" << d
                     << " addr_map=" << out_du_address_map
                     << " readable_conf=" << out_du_readable_conf;
            return true;
        }
    }

    LOG_ERROR << "resolveLegacyElecYamlPaths: could not find DU-address-map.yaml and DU-readable-conf.yaml"
              << " in GRAND_DAQ_CONFIG (unset) or sysconfig dir";
    return false;
}

// Legacy RBCP 寄存器展开：保留给 `ElecConfig::toShadowlist`/对照与潜在脚本；**不是**当前 ZMQ `App::start()` 控制链。
std::vector<ElectronicsConfigItem> ElectronicsConfig::configItems() const {
    std::vector<ElectronicsConfigItem> items;
    
    // RBCP协议格式: [地址(4字节)] [数据(1字节)]
    auto addItem = [&items](ElectronicsConfigItem::ItemType type, uint32_t addr, std::vector<uint8_t> data, int delay_ms = 0, int timeout_ms = 100, int retry = 1) {
        ElectronicsConfigItem item(type, addr, data, delay_ms, timeout_ms, retry);
        items.push_back(item);
    };

    // 全局reset
    addItem(ElectronicsConfigItem::WR, 0x00000006, {0x01U}, 1000, 100, 1);

    // 全局reset
    addItem(ElectronicsConfigItem::WR, 0x00000006, {0x00U}, 1000, 100, 1);
    
    // 通道使能
    uint8_t channel_enable_byte = 0;
    for (size_t i = 0; i < channel_enable.size(); i++) {
        if (channel_enable[i]) {
            std::cout << "enable channel: " << i << std::endl;
            channel_enable_byte |= (1 << i);
        }
    }
    addItem(ElectronicsConfigItem::RDWR, 0x00000009, {channel_enable_byte}, 200, 100, 1);
    
    // 板号（寄存器一字节；从 du_id 局部推导，非配置字段 fee_id）
    addItem(ElectronicsConfigItem::RDWR, 0x0000000A, {legacy_rbcp_board_byte(du_id)}, 200, 100, 1);
    
    // 触发阈值nHIT
    addItem(ElectronicsConfigItem::RDWR, 0x0000000B, {trigger_threshold_nhit}, 200, 100, 1);
    
     // 波形采样点点数
    if (waveform_points % 8 != 0) {
        LOG_ERROR << "waveform_points must be a multiple of 8"
                << ", current value = " << waveform_points;
        throw ConfigException("waveform_points must be a multiple of 8");
    }
    uint16_t waveform_points_elec = waveform_points / 4;
    addItem(ElectronicsConfigItem::RDWR, 0x0000000C, {
                static_cast<uint8_t>(waveform_points_elec & 0xFF),
            }, 200, 100, 1);
    addItem(ElectronicsConfigItem::RDWR, 0x0000000D, {
                static_cast<uint8_t>((waveform_points_elec >> 8) & 0xFF)
            }, 200, 100, 1);

    // 过阈前采样点点数
    if (waveform_points_pre_threshold % 4 != 0) {
        LOG_ERROR << "waveform_points_pre_threshold must be a multiple of 4"
                << ", current value = " << waveform_points_pre_threshold;
        throw ConfigException("waveform_points_pre_threshold must be a multiple of 4");
    }
    if (waveform_points_pre_threshold > waveform_points) {
        LOG_ERROR << "waveform_points_pre_threshold must be less than or equal to waveform_points"
                << ", current value = " << waveform_points_pre_threshold << ", waveform_points = " << waveform_points;
        throw ConfigException("waveform_points_pre_threshold must be less than or equal to waveform_points");
    }
    uint16_t waveform_points_pre_threshold_elec = waveform_points_pre_threshold / 4;
    addItem(ElectronicsConfigItem::RDWR, 0x0000000E, {
                static_cast<uint8_t>(waveform_points_pre_threshold_elec & 0xFF),
            }, 200, 100, 1);
    addItem(ElectronicsConfigItem::RDWR, 0x0000000F, {
                static_cast<uint8_t>((waveform_points_pre_threshold_elec >> 8) & 0xFF)
            }, 200, 100, 1);

    // 读出窗口大小，触发前，ns
    if (readout_window_before_trigger_ns % 8 != 0) {
        LOG_ERROR << "readout_window_before_trigger_ns must be a multiple of 8"
                << ", unit = ns, current value = " << readout_window_before_trigger_ns;
        throw ConfigException("readout_window_before_trigger_ns must be a multiple of 8");
    }
    uint16_t readout_window_before_trigger_ns_elec = readout_window_before_trigger_ns / 8;
    addItem(ElectronicsConfigItem::RDWR, 0x00000010, {
                static_cast<uint8_t>(readout_window_before_trigger_ns_elec & 0xFF),
            }, 200, 100, 1);
    addItem(ElectronicsConfigItem::RDWR, 0x00000011, {
                static_cast<uint8_t>((readout_window_before_trigger_ns_elec >> 8) & 0xFF),
            }, 200, 100, 1);
    
    // 读出窗口大小，触发后，ns
    if (readout_window_after_trigger_ns % 8 != 0) {
        LOG_ERROR << "readout_window_after_trigger_ns must be a multiple of 8"
                << ", unit = ns, current value = " << readout_window_after_trigger_ns;
        throw ConfigException("readout_window_after_trigger_ns must be a multiple of 8");
    }
    uint16_t readout_window_after_trigger_ns_elec = readout_window_after_trigger_ns / 8;
    addItem(ElectronicsConfigItem::RDWR, 0x00000012, {
                static_cast<uint8_t>(readout_window_after_trigger_ns_elec & 0xFF),
            }, 200, 100, 1);
    addItem(ElectronicsConfigItem::RDWR, 0x00000013, {
                static_cast<uint8_t>((readout_window_after_trigger_ns_elec >> 8) & 0xFF),
            }, 200, 100, 1);

    // 触发窗口大小
    if (trigger_window_ns % 8 != 0) {
        LOG_ERROR << "trigger_window_ns must be a multiple of 8, unit = ns, current value = " << trigger_window_ns;
        throw ConfigException("trigger_window_ns must be a multiple of 8");
    }
    uint16_t trigger_window_elec = trigger_window_ns / 8;
    addItem(ElectronicsConfigItem::RDWR, 0x00000014, {
                static_cast<uint8_t>(trigger_window_elec & 0xFF),
            }, 200, 100, 1);
    addItem(ElectronicsConfigItem::RDWR, 0x00000015, {
                static_cast<uint8_t>((trigger_window_elec >> 8) & 0xFF),
            }, 200, 100, 1);
    
    // 各通道阈值
    for (size_t ch = 0; ch < channel_thresholds.size() && ch < NUM_CHANNELS; ch++) {
        uint32_t base_addr = 0x80001000 + ch * 0x100;
        addItem(ElectronicsConfigItem::RDWR, base_addr, {
                static_cast<uint8_t>(channel_thresholds[ch] & 0xFF),
            }, 200, 100, 1);
        addItem(ElectronicsConfigItem::RDWR, base_addr + 1, {
                static_cast<uint8_t>((channel_thresholds[ch] >> 8) & 0xFF),
            }, 200, 100, 1);
    }
    
    return items;
}

std::vector<ElectronicsConfigItem> ElectronicsConfig::startTriggerItems() const {
    std::vector<ElectronicsConfigItem> items;
    // Set trigger mode 
    items.push_back(ElectronicsConfigItem(ElectronicsConfigItem::WR, 0x00000008, 
        {static_cast<uint8_t>(trigger_mode == TriggerMode::SINGLE_BOARD ? 0x00U : 0x01U)}, 200, 100, 1));

    items.push_back(ElectronicsConfigItem(ElectronicsConfigItem::WR, 0x00000007, {0x01U}, 200, 100, 1));
    
    return items;
}

std::vector<ElectronicsConfigItem> ElectronicsConfig::stopTriggerItems() const {
    std::vector<ElectronicsConfigItem> items;
    items.push_back(ElectronicsConfigItem(ElectronicsConfigItem::WR, 0x00000007, {0x00U}, 200, 100, 1));
    return items;
}

// SoftwareConfig 实现
SoftwareConfig::SoftwareConfig()
    : backend_ip("127.0.0.1")
    , backend_port(8080)
    , electronics_tcp_port(8000)
    , electronics_udp_port(4660)
    , buffer_size(1024 * 1024) // 1MB
    , network_input_buffer_size(2048000)
    , zmq_send_buffer_size(2048000)
    , log_level(1)
    , http_port(8080)
    , debug_raw(false)
{
}

// ConfigManager 实现
ConfigManager::ConfigManager() {
}

ConfigManager::~ConfigManager() {
}

// 用 legacy 快照覆盖 software_config_ 与 electronics_configs_；环境变量可改 l1/l3 存储路径与 http_port
void ConfigManager::applyLegacyYamlConfig(const grand::LegacyYamlConfig& legacy) {
    SoftwareConfig new_sw = SoftwareConfig();
    // 优先 YAML → 环境变量覆盖 → 默认值
    auto resolveStorePath = [](const std::string& from_yaml,
                               const char* env_key,
                               const char* default_path) -> std::string {
        std::string v = from_yaml.empty() ? std::string(default_path) : from_yaml;
        if (const char* p = std::getenv(env_key); p && *p) {
            v = p;
        }
        return v;
    };
    new_sw.l1_store_path = resolveStorePath(legacy.app.l1StorePath,
                                            "CSDAQ_L1_STORE_PATH",
                                            "/tmp/csdaq/l1");
    new_sw.l3_store_path = resolveStorePath(legacy.app.l3StorePath,
                                            "CSDAQ_L3_STORE_PATH",
                                            "/tmp/csdaq/l3");
    for (const auto& store_dir : { new_sw.l1_store_path, new_sw.l3_store_path }) {
        std::error_code ec;
        std::filesystem::create_directories(store_dir, ec);
        if (ec) {
            throw ConfigException("Cannot create store path: " + store_dir + ": " + ec.message());
        }
    }
    if (const char* p = std::getenv("CSDAQ_HTTP_PORT"); p && *p) {
        new_sw.http_port = std::atoi(p);
    }

    new_sw.network_input_buffer_size =
        legacy.app.networkInputBufferSize ? static_cast<size_t>(legacy.app.networkInputBufferSize)
                                            : 2048000U;
    new_sw.zmq_send_buffer_size =
        legacy.app.zmqSndBufferSize ? static_cast<size_t>(legacy.app.zmqSndBufferSize) : 2048000U;
    new_sw.zmq_daq_mode = static_cast<std::int32_t>(legacy.app.daqMode);

    std::vector<ElectronicsConfig> new_elec;
    new_elec.reserve(legacy.dus.size());
    for (const auto& du : legacy.dus) {
        ElectronicsConfig ec;
        unsigned long raw = 0;
        try {
            raw = std::stoul(du.id);
        } catch (...) {
            throw ConfigException("Invalid dataUnits[].ID for runtime mapping: " + du.id);
        }
        if (raw > static_cast<unsigned long>(std::numeric_limits<uint32_t>::max())) {
            throw ConfigException("dataUnits[].ID out of uint32 range");
        }
        ec.du_id = static_cast<uint32_t>(raw);
        ec.type = du.type;
        ec.ip = du.ip;
        if (du.port == 0 || du.port > 65535U) {
            throw ConfigException("dataUnits[].port must be between 1 and 65535");
        }
        ec.port = static_cast<uint16_t>(du.port);
        new_elec.push_back(std::move(ec));
    }

    if (new_elec.empty()) {
        throw ConfigException("No dataUnits after legacy load");
    }

    software_config_ = std::move(new_sw);
    electronics_configs_ = std::move(new_elec);
}

// 成功则提交新配置；异常时恢复进入函数前的快照并打印错误
bool ConfigManager::loadLegacySysConfigYaml(const std::string& sysconfig_yaml_path) {
    const SoftwareConfig snapshot_sw = software_config_;
    const std::vector<ElectronicsConfig> snapshot_elec = electronics_configs_;
    try {
        grand::LegacyYamlConfig legacy = grand::CsYamlConfigLoader::loadSysConfig(sysconfig_yaml_path);
        applyLegacyYamlConfig(legacy);
        return true;
    } catch (const std::exception& e) {
        software_config_ = snapshot_sw;
        electronics_configs_ = snapshot_elec;
        std::cerr << "Error loading legacy sysconfig yaml: " << e.what() << std::endl;
        return false;
    }
}
