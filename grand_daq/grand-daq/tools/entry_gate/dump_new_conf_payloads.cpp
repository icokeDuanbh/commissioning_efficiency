#include <elec_config.h>

#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct DuEntry {
    uint32_t du_id = 0;
    std::string type;
    std::string ip;
    uint16_t port = 0;
};

std::string hexEncode(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t b : bytes) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

std::vector<DuEntry> loadDus(const std::string& sysconfig_path) {
    YAML::Node root = YAML::LoadFile(sysconfig_path);
    YAML::Node dus = root["dataUnits"];
    if (!dus || !dus.IsSequence()) {
        throw std::runtime_error("sysconfig missing dataUnits sequence");
    }
    std::vector<DuEntry> out;
    out.reserve(dus.size());
    for (const auto& du : dus) {
        if (!du["ID"] || !du["ip"] || !du["port"]) {
            continue;
        }
        DuEntry entry;
        entry.du_id = du["ID"].as<uint32_t>();
        entry.type = du["type"] ? du["type"].as<std::string>() : std::string{};
        entry.ip = du["ip"].as<std::string>();
        const uint32_t port = du["port"].as<uint32_t>();
        if (port > 65535U) {
            throw std::runtime_error("port out of range for du_id=" + std::to_string(entry.du_id));
        }
        entry.port = static_cast<uint16_t>(port);
        out.push_back(entry);
    }
    if (out.empty()) {
        throw std::runtime_error("no active DU entries found in sysconfig");
    }
    return out;
}

bool fileOk(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool resolveSidecars(const std::string& sysconfig_path,
                     std::string* out_addr_map,
                     std::string* out_readable_conf) {
    if (const char* env = std::getenv("GRAND_DAQ_CONFIG"); env && *env) {
        const fs::path base(env);
        const fs::path addr = base / "DU-address-map.yaml";
        const fs::path readable = base / "DU-readable-conf.yaml";
        if (fileOk(addr) && fileOk(readable)) {
            *out_addr_map = addr.string();
            *out_readable_conf = readable.string();
            return true;
        }
    }

    const fs::path sys_dir = fs::absolute(fs::path(sysconfig_path)).parent_path();
    {
        const fs::path addr = sys_dir / "DU-address-map.yaml";
        const fs::path readable = sys_dir / "DU-readable-conf.yaml";
        if (fileOk(addr) && fileOk(readable)) {
            *out_addr_map = addr.string();
            *out_readable_conf = readable.string();
            return true;
        }
    }

    for (const fs::path& base : {fs::path("cfgs"), fs::path("gp300/cfgs")}) {
        const fs::path addr = base / "DU-address-map.yaml";
        const fs::path readable = base / "DU-readable-conf.yaml";
        if (fileOk(addr) && fileOk(readable)) {
            *out_addr_map = addr.string();
            *out_readable_conf = readable.string();
            return true;
        }
    }

    return false;
}

std::string requireArg(const std::vector<std::string>& args, const std::string& name) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            return args[i + 1];
        }
    }
    throw std::runtime_error("missing required argument: " + name);
}

std::string optionalArg(const std::vector<std::string>& args, const std::string& name) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            return args[i + 1];
        }
    }
    return {};
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::vector<std::string> args(argv + 1, argv + argc);
        const std::string sysconfig = requireArg(args, "--sysconfig");
        const std::string out_path = requireArg(args, "--out");
        std::string addr_map = optionalArg(args, "--addr-map");
        std::string readable_conf = optionalArg(args, "--readable-conf");

        if (addr_map.empty() || readable_conf.empty()) {
            if (!resolveSidecars(sysconfig, &addr_map, &readable_conf)) {
                throw std::runtime_error("failed to resolve DU-address-map.yaml / DU-readable-conf.yaml");
            }
        }

        const auto dus = loadDus(sysconfig);
        grand::ElecConfig::instance()->load(addr_map, readable_conf);

        json root;
        root["tool"] = "entry_gate_dump_new_conf";
        root["sysconfig"] = fs::absolute(sysconfig).string();
        root["addr_map"] = fs::absolute(addr_map).string();
        root["readable_conf"] = fs::absolute(readable_conf).string();
        root["du_count"] = dus.size();
        root["dus"] = json::array();

        for (const auto& du : dus) {
            std::vector<uint8_t> shadow(static_cast<size_t>(Reg_End), 0);
            const size_t used =
                grand::ElecConfig::instance()->toShadowlist(shadow.data(), std::to_string(du.du_id));
            if (used == 0 || used > shadow.size()) {
                throw std::runtime_error("invalid shadowlist size for du_id=" + std::to_string(du.du_id));
            }
            shadow.resize(used);

            json item;
            item["du_id"] = du.du_id;
            item["type"] = du.type;
            item["ip"] = du.ip;
            item["port"] = du.port;
            item["conf_payload_size"] = shadow.size();
            item["conf_payload_hex"] = hexEncode(shadow);
            root["dus"].push_back(std::move(item));
        }

        std::ofstream ofs(out_path);
        if (!ofs.good()) {
            throw std::runtime_error("failed to open output file: " + out_path);
        }
        ofs << root.dump(2) << '\n';
        if (!ofs.good()) {
            throw std::runtime_error("failed to write output file: " + out_path);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "entry_gate_dump_new_conf: " << e.what() << std::endl;
        return 1;
    }
}
