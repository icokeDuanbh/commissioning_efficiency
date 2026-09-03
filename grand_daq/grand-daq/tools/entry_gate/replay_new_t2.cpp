#include <cs_yaml_config.h>
#include <data_parser.h>
#include <duplicate_reject_logger.h>
#include <t3_trigger.h>
#include <nlohmann/json.hpp>

#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

std::vector<Gp300T2Hit> timestampsToHits(const std::vector<uint64_t>& timestamps) {
    std::vector<Gp300T2Hit> hits;
    hits.reserve(timestamps.size());
    for (uint64_t ts : timestamps) {
        hits.push_back(Gp300T2Hit{ts, 0, 0});
    }
    return hits;
}

std::string requireArg(char** begin, char** end, const std::string& name) {
    for (char** it = begin; it != end - 1; ++it) {
        if (std::string(*it) == name) {
            return *(it + 1);
        }
    }
    throw std::runtime_error("missing required argument: " + name);
}

json readJson(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        throw std::runtime_error("failed to open json: " + path);
    }
    json j;
    ifs >> j;
    return j;
}

std::string hexEncode(const std::vector<uint8_t>& bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(kHex[(b >> 4) & 0xF]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

std::vector<uint8_t> makeDotriggerPayload(const grand::TriggerResult& result, uint32_t tag) {
    const uint32_t du_count = static_cast<uint32_t>(result.du_ids.size());
    const size_t payload_size = result.trigger_timestamps.size() * sizeof(uint64_t) + sizeof(tag) + sizeof(du_count);
    std::vector<uint8_t> payload(payload_size, 0);
    size_t off = 0;
    if (!result.trigger_timestamps.empty()) {
        const size_t ts_bytes = result.trigger_timestamps.size() * sizeof(uint64_t);
        std::memcpy(payload.data(), result.trigger_timestamps.data(), ts_bytes);
        off += ts_bytes;
    }
    std::memcpy(payload.data() + off, &tag, sizeof(tag));
    off += sizeof(tag);
    std::memcpy(payload.data() + off, &du_count, sizeof(du_count));
    return payload;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string sysconfig = requireArg(argv + 1, argv + argc, "--sysconfig");
        const std::string batches_path = requireArg(argv + 1, argv + argc, "--batches");
        const std::string out_path = requireArg(argv + 1, argv + argc, "--out");

        const auto legacy = grand::CsYamlConfigLoader::loadSysConfig(sysconfig);
        std::vector<uint32_t> active_du_ids;
        active_du_ids.reserve(legacy.dus.size());
        for (const auto& du : legacy.dus) {
            active_du_ids.push_back(static_cast<uint32_t>(std::stoul(du.id)));
        }
        grand::T3Config cfg = grand::buildT3RuntimeConfig(legacy.app, active_du_ids);
        if (cfg.duplicateRejectLogEnable) {
            cfg.duplicateRejectLogPath =
                grand::DuplicateRejectLogger::makeRunLogPath(
                    cfg.duplicateRejectLogPath,
                    0U,
                    std::time(nullptr)).string();
        }

        json batches = readJson(batches_path);
        std::vector<grand::TriggerResult> results;
        {
            grand::T3Trigger trigger(cfg);
            trigger.setTriggerResultCallback([&results](const grand::TriggerResult& result) {
                results.push_back(result);
            });
            for (const auto& batch : batches["batches"]) {
                const uint32_t du_id = batch["du_id"].get<uint32_t>();
                const std::vector<uint64_t> timestamps = batch["timestamps"].get<std::vector<uint64_t>>();
                if (!timestamps.empty()) {
                    trigger.onT2(du_id, timestampsToHits(timestamps));
                }
            }
            trigger.stop();
        }

        json root;
        root["tool"] = "entry_gate_replay_new_t2";
        root["sysconfig"] = sysconfig;
        root["batches"] = batches_path;
        root["results"] = json::array();
        uint32_t tag = 1;
        for (const auto& result : results) {
            json item;
            item["tag"] = tag;
            item["tm_id"] = result.tm_id;
            item["triggered"] = result.triggered;
            item["du_count"] = result.du_count;
            item["timestamp_ns"] = result.timestamp_ns;
            item["du_ids"] = result.du_ids;
            item["trigger_timestamps"] = result.trigger_timestamps;
            item["dotrigger_payload_hex"] = hexEncode(makeDotriggerPayload(result, tag));
            root["results"].push_back(std::move(item));
            ++tag;
        }
        root["result_count"] = root["results"].size();

        std::ofstream ofs(out_path);
        if (!ofs.good()) {
            throw std::runtime_error("failed to open output: " + out_path);
        }
        ofs << root.dump(2) << '\n';
        if (!ofs.good()) {
            throw std::runtime_error("failed to write output: " + out_path);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "entry_gate_replay_new_t2: " << e.what() << std::endl;
        return 1;
    }
}
