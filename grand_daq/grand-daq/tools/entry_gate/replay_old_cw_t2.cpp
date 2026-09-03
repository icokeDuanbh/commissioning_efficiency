#include <config_adapter.h>
#include <data_parser.h>
#include <dotrigger_async_sender.h>
#include <trigger_pipeline.h>
#include <nlohmann/json.hpp>

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

std::string hexEncode(const std::vector<char>& bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char b : bytes) {
        out.push_back(kHex[(b >> 4) & 0xF]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

grand::GateLogic::AdvanceCompleteMode readAdvanceMode() {
    const char* raw = std::getenv("GRAND_GATE_ADVANCE_COMPLETE_MODE");
    std::string mode = (raw && *raw) ? raw : "shadow";
    if (mode == "0" || mode == "disable" || mode == "disabled") {
        return grand::GateLogic::AdvanceCompleteMode::Disabled;
    }
    if (mode == "gated" || mode == "phase1") {
        return grand::GateLogic::AdvanceCompleteMode::GatedEnable;
    }
    return grand::GateLogic::AdvanceCompleteMode::ShadowOnly;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string sysconfig = requireArg(argv + 1, argv + argc, "--sysconfig");
        const std::string batches_path = requireArg(argv + 1, argv + argc, "--batches");
        const std::string out_path = requireArg(argv + 1, argv + argc, "--out");

        LegacyConfig legacy = LegacyConfigAdapter::loadFromYaml(sysconfig);
        LegacyConfigAdapter::validate(legacy);
        auto cfg = LegacyConfigAdapter::buildTriggerPipelineConfig(legacy);
        LegacyConfigAdapter::resolveAndValidateRuntimeTriggerConfig(cfg, sysconfig);

        grand::TriggerPipeline pipeline(cfg);
        std::vector<uint16_t> expected_du_ids;
        expected_du_ids.reserve(legacy.data_units.size());
        for (const auto& du : legacy.data_units) {
            expected_du_ids.push_back(static_cast<uint16_t>(du.id));
        }
        pipeline.configureGateAdvanceCompletion(
            std::move(expected_du_ids),
            readAdvanceMode(),
            32,
            4096);

        std::vector<grand::TriggerResult> results;
        pipeline.setTriggerCallback([&results](const grand::TriggerResult& result, const grand::TmIdBucket&) {
            results.push_back(result);
        });
        pipeline.start();

        json batches = readJson(batches_path);
        for (const auto& batch : batches["batches"]) {
            const uint16_t du_id = static_cast<uint16_t>(batch["du_id"].get<uint32_t>());
            auto timestamps = batch["timestamps"].get<std::vector<uint64_t>>();
            if (!timestamps.empty()) {
                pipeline.addT2HitsBatch(du_id, timestampsToHits(timestamps));
            }
        }
        pipeline.stop();

        json root;
        root["tool"] = "entry_gate_replay_old_cw_t2";
        root["sysconfig"] = sysconfig;
        root["batches"] = batches_path;
        root["results"] = json::array();
        uint32_t tag = 1;
        for (const auto& result : results) {
            std::vector<char> payload;
            payload.resize(result.trigger_timestamps.size() * sizeof(uint64_t) + sizeof(uint32_t) * 2);
            size_t off = 0;
            if (!result.trigger_timestamps.empty()) {
                const size_t ts_bytes = result.trigger_timestamps.size() * sizeof(uint64_t);
                std::memcpy(payload.data(), result.trigger_timestamps.data(), ts_bytes);
                off += ts_bytes;
            }
            const uint32_t du_count = static_cast<uint32_t>(result.du_ids.size());
            std::memcpy(payload.data() + off, &tag, sizeof(tag));
            off += sizeof(tag);
            std::memcpy(payload.data() + off, &du_count, sizeof(du_count));

            json item;
            item["tag"] = tag;
            item["tm_id"] = result.tm_id;
            item["triggered"] = result.triggered;
            item["du_count"] = result.du_count;
            item["timestamp_ns"] = result.timestamp_ns;
            item["du_ids"] = result.du_ids;
            item["trigger_timestamps"] = result.trigger_timestamps;
            item["dotrigger_payload_hex"] = hexEncode(payload);
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
        std::cerr << "entry_gate_replay_old_cw_t2: " << e.what() << std::endl;
        return 1;
    }
}
