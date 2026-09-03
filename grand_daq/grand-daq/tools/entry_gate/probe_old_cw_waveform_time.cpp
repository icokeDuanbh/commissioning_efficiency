#include <csdaq_format.h>
#include <gp300_eformat.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("failed to open payload: " + path);
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

std::string requireArg(char** begin, char** end, const std::string& name) {
    for (char** it = begin; it != end - 1; ++it) {
        if (std::string(*it) == name) {
            return *(it + 1);
        }
    }
    throw std::runtime_error("missing required argument: " + name);
}

uint32_t requireU32Arg(char** begin, char** end, const std::string& name) {
    return static_cast<uint32_t>(std::stoul(requireArg(begin, end, name)));
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string payload_path = requireArg(argv + 1, argv + argc, "--payload");
        const std::string out_path = requireArg(argv + 1, argv + argc, "--out");
        const uint32_t source_du = requireU32Arg(argv + 1, argv + argc, "--source-du");

        auto payload = readBytes(payload_path);
        grand::DAQHeader hdr{};
        hdr.size = static_cast<uint32_t>(sizeof(grand::DAQHeader) + payload.size());
        hdr.type = grand::DAQPCK_TYPE_DUEVENT;
        hdr.source = source_du;

        std::vector<uint8_t> packet(sizeof(grand::DAQHeader) + payload.size(), 0);
        std::memcpy(packet.data(), &hdr, sizeof(hdr));
        std::memcpy(packet.data() + sizeof(hdr), payload.data(), payload.size());

        csdaq::EventParser parser;
        const bool ok = parser.parse(packet.data(), packet.size());

        json root;
        root["tool"] = "entry_gate_probe_old_cw_waveform_time";
        root["payload_path"] = std::filesystem::absolute(payload_path).string();
        root["payload_size"] = payload.size();
        root["source_du"] = source_du;
        root["wrapped_packet_size"] = packet.size();
        root["parse_ok"] = ok;
        if (ok) {
            root["du_id"] = parser.getDuId();
            root["timestamp_ns"] = parser.getTimestampNs();
            root["event_id"] = parser.getEventId();
            root["event_length"] = parser.getEventLength();
            root["event_data_size"] = parser.getEventDataSize();
        }

        std::ofstream ofs(out_path);
        if (!ofs.good()) {
            throw std::runtime_error("failed to open output: " + out_path);
        }
        ofs << root.dump(2) << '\n';
        return ok ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << "entry_gate_probe_old_cw_waveform_time: " << e.what() << std::endl;
        return 1;
    }
}
