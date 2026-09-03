#include "data_store.h"
#include "utils.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace {

constexpr uint32_t kDaqPckTypeDuEvent = 0x01;
constexpr uint32_t kFileHeadSize = 256;
constexpr uint32_t kLegacyDataVersion = 0x00010000;

inline void writeU32Le(char* p, uint32_t value) {
    std::memcpy(p, &value, sizeof(value));
}

} // namespace

DataStore::DataStore()
    : DataStore(Config{}) {}

DataStore::DataStore(Config cfg)
    : cfg_(std::move(cfg)) {}

DataStore::~DataStore() {
    close();
}

// 关旧文件、清状态，按两个输出目录分别扫描 RUN 号并打开 L1/MD/CD 三套 FileState
bool DataStore::open() {
    std::lock_guard<std::mutex> lock(mu_);
    (void)closeFileLocked(cd_);
    (void)closeFileLocked(md_);
    (void)closeFileLocked(l1_);
    is_open_ = false;
    io_failure_count_ = 0;
    rotate_count_ = 0;
    last_io_errno_ = 0;
    last_io_error_.clear();

    resetStateForOpenLocked(l1_);
    resetStateForOpenLocked(md_);
    resetStateForOpenLocked(cd_);

    for (const std::string& dir : { cfg_.l1_output_dir, cfg_.l3_output_dir }) {
        if (dir.empty()) continue;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            markIoFailureLocked("create_directories(" + dir + ")", ec.value());
            return false;
        }
    }

    // 自动分配 RUN 号：扫描目录里 max(RUN<N>)+1；空目录时 L1 从 1 起、CD 从 10001 起
    l1_run_number_ = scanNextRunNumberLocked(cfg_.l1_output_dir, "MD_", 1U);
    cd_run_number_ = scanNextRunNumberLocked(cfg_.l3_output_dir, "CD_", 10001U);

    is_open_ = openFilesLocked();
    return is_open_;
}

void DataStore::close() {
    std::lock_guard<std::mutex> lock(mu_);
    (void)closeFileLocked(cd_);
    (void)closeFileLocked(md_);
    (void)closeFileLocked(l1_);
    is_open_ = false;
}

bool DataStore::writeRawRecord(uint32_t du_id, const uint8_t* payload, std::size_t payload_size) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!is_open_ || !cfg_.enable_writing || payload == nullptr || payload_size == 0) {
        return false;
    }

    // 与 old-cs-daq 行为对齐：daqMode=4 时原始流写入 MD。
    if (cfg_.daq_mode == 4) {
        if (!shouldWriteMDLocked()) {
            return true;
        }
        return writeRecordLocked(md_, du_id, payload, payload_size);
    }

    if (!shouldWriteL1Locked()) {
        return true;
    }
    return writeRecordLocked(l1_, du_id, payload, payload_size);
}

bool DataStore::writeMdRecord(uint32_t du_id, const uint8_t* payload, std::size_t payload_size) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!is_open_ || !cfg_.enable_writing || payload == nullptr || payload_size == 0) {
        return false;
    }
    if (!shouldWriteMDLocked()) {
        return true;
    }
    return writeRecordLocked(md_, du_id, payload, payload_size);
}

bool DataStore::writeCdRecord(uint32_t du_id, uint32_t event_key, const uint8_t* payload, std::size_t payload_size) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!is_open_ || !cfg_.enable_writing || payload == nullptr || payload_size == 0) {
        return false;
    }
    if (!shouldWriteCDLocked()) {
        return true;
    }

    constexpr std::size_t kRecordHeaderBytes = sizeof(LegacyDatRecordHeader);
    if (payload_size > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()) - kRecordHeaderBytes) {
        markIoFailureLocked("writeCdRecord(size-overflow:" + cd_.path + ")", EOVERFLOW);
        return false;
    }
    const std::size_t record_bytes = kRecordHeaderBytes + payload_size;

    if (cfg_.max_event_number_saved != 0) {
        const uint64_t ek = static_cast<uint64_t>(event_key);
        if (cd_.event_save_keys.find(ek) == cd_.event_save_keys.end()) {
            if (cd_.event_save_keys.size() >= cfg_.max_event_number_saved) {
                if (!closeFileLocked(cd_)) {
                    return false;
                }
                if (!openCDLocked()) {
                    return false;
                }
                cd_.event_save_keys.clear();
            }
        }
    }

    if (!preflightCdSizeLocked(record_bytes, "writeCdRecord")) {
        return false;
    }

    const bool ok = writeRecordLocked(cd_, du_id, payload, payload_size);
    if (!ok) {
        return false;
    }

    if (cfg_.max_event_number_saved != 0) {
        cd_.event_save_keys.insert(static_cast<uint64_t>(event_key));
    }
    return true;
}

bool DataStore::preflightCdSizeLocked(std::size_t bytes_to_write, const char* op_name) {
    if (cfg_.max_file_size_bytes == 0 || cd_.current_written <= kFileHeadSize) {
        return true;
    }
    if (bytes_to_write > std::numeric_limits<std::size_t>::max() - cd_.current_written) {
        markIoFailureLocked(std::string(op_name) + "(size-overflow:" + cd_.path + ")", EOVERFLOW);
        return false;
    }
    if (cd_.current_written + bytes_to_write <= cfg_.max_file_size_bytes) {
        return true;
    }
    if (!closeFileLocked(cd_)) {
        return false;
    }
    if (!openCDLocked()) {
        return false;
    }
    cd_.event_save_keys.clear();
    ++rotate_count_;
    return true;
}

bool DataStore::writeCdEvent(uint32_t event_key, const std::vector<CdFragmentRecord>& fragments) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!is_open_ || !cfg_.enable_writing) {
        return false;
    }
    if (!shouldWriteCDLocked()) {
        return true;
    }

    std::size_t event_bytes = 0;
    std::size_t valid_fragment_count = 0;
    constexpr std::size_t kRecordHeaderBytes = sizeof(LegacyDatRecordHeader);
    const std::size_t max_size = std::numeric_limits<std::size_t>::max();
    for (const CdFragmentRecord& fragment : fragments) {
        if (fragment.payload == nullptr || fragment.payload_size == 0) {
            continue;
        }
        if (fragment.payload_size >
            static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()) - kRecordHeaderBytes) {
            markIoFailureLocked("writeCdEvent(size-overflow:" + cd_.path + ")", EOVERFLOW);
            return false;
        }
        if (fragment.payload_size > max_size - kRecordHeaderBytes) {
            markIoFailureLocked("writeCdEvent(size-overflow:" + cd_.path + ")", EOVERFLOW);
            return false;
        }
        const std::size_t record_bytes = kRecordHeaderBytes + fragment.payload_size;
        if (event_bytes > max_size - record_bytes) {
            markIoFailureLocked("writeCdEvent(size-overflow:" + cd_.path + ")", EOVERFLOW);
            return false;
        }
        event_bytes += record_bytes;
        ++valid_fragment_count;
    }
    if (valid_fragment_count == 0) {
        return true;
    }

    if (cfg_.max_event_number_saved != 0) {
        const uint64_t ek = static_cast<uint64_t>(event_key);
        if (cd_.event_save_keys.find(ek) == cd_.event_save_keys.end()) {
            if (cd_.event_save_keys.size() >= cfg_.max_event_number_saved) {
                if (!closeFileLocked(cd_)) {
                    return false;
                }
                if (!openCDLocked()) {
                    return false;
                }
                cd_.event_save_keys.clear();
            }
        }
    }

    if (cfg_.max_file_size_bytes != 0) {
        if (cfg_.max_file_size_bytes <= kFileHeadSize ||
            event_bytes > cfg_.max_file_size_bytes - kFileHeadSize) {
            markIoFailureLocked("writeCdEvent(event-too-large:" + cd_.path + ")", EFBIG);
            return false;
        }
        if (!preflightCdSizeLocked(event_bytes, "writeCdEvent")) {
            return false;
        }
    }

    for (const CdFragmentRecord& fragment : fragments) {
        if (fragment.payload == nullptr || fragment.payload_size == 0) {
            continue;
        }
        if (!writeRecordNoRotateLocked(cd_, fragment.du_id, fragment.payload, fragment.payload_size)) {
            return false;
        }
    }

    if (cfg_.max_event_number_saved != 0) {
        cd_.event_save_keys.insert(static_cast<uint64_t>(event_key));
    }
    return true;
}

std::string DataStore::currentL1FilePath() const {
    std::lock_guard<std::mutex> lock(mu_);
    return l1_.path;
}

std::string DataStore::currentMDFilePath() const {
    std::lock_guard<std::mutex> lock(mu_);
    return md_.path;
}

std::string DataStore::currentCDFilePath() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cd_.path;
}

uint32_t DataStore::currentCDRunNumber() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cd_run_number_;
}

std::uint64_t DataStore::ioFailureCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return io_failure_count_;
}

std::uint64_t DataStore::rotateCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return rotate_count_;
}

int DataStore::lastIoErrno() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_io_errno_;
}

std::string DataStore::lastIoError() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_io_error_;
}

bool DataStore::shouldWriteL1Locked() const {
    return cfg_.daq_mode == 1 || cfg_.daq_mode == 3 || cfg_.daq_mode == 4;
}

bool DataStore::shouldWriteMDLocked() const {
    return cfg_.daq_mode == 4;
}

bool DataStore::shouldWriteCDLocked() const {
    return cfg_.daq_mode == 2 || cfg_.daq_mode == 3;
}

bool DataStore::openFilesLocked() {
    if (shouldWriteL1Locked() && !openL1Locked()) {
        closeFileLocked(l1_);
        return false;
    }
    if (shouldWriteMDLocked() && !openMDLocked()) {
        closeFileLocked(md_);
        closeFileLocked(l1_);
        return false;
    }
    if (shouldWriteCDLocked() && !openCDLocked()) {
        closeFileLocked(cd_);
        closeFileLocked(md_);
        closeFileLocked(l1_);
        return false;
    }
    return true;
}

bool DataStore::openL1Locked() {
    return openFileLocked(l1_, makeL1FilenameLocked(), cfg_.l1_output_dir);
}

bool DataStore::openMDLocked() {
    return openFileLocked(md_, makeMDFilenameLocked(), cfg_.l1_output_dir);
}

bool DataStore::openCDLocked() {
    return openFileLocked(cd_, makeCDFilenameLocked(), cfg_.l3_output_dir);
}

bool DataStore::openFileLocked(FileState& state, const std::string& filename, const std::string& dir) {
    const std::string resolved_dir = dir.empty() ? std::string(".") : dir;
    const std::string full_path = resolved_dir + "/" + filename;

    state.file = std::fopen(full_path.c_str(), "wb");
    if (state.file == nullptr) {
        markIoFailureLocked("fopen(" + full_path + ")", errno);
        return false;
    }

    state.path = full_path;
    state.filename = filename;
    state.current_written = 0;
    if (!writeFileHeaderLocked(state, filename)) {
        (void)closeFileLocked(state);
        return false;
    }
    return true;
}

bool DataStore::closeFileLocked(FileState& state) {
    if (state.file == nullptr) {
        return true;
    }
    bool ok = patchFileCloseTimeLocked(state);
    if (std::fclose(state.file) != 0) {
        markIoFailureLocked("fclose(" + state.path + ")", errno);
        ok = false;
    }
    state.file = nullptr;
    state.current_written = 0;
    state.has_file_header = false;
    return ok;
}

bool DataStore::rotateIfNeededLocked(FileState& state) {
    if (state.file == nullptr) {
        return false;
    }
    if (cfg_.max_file_size_bytes == 0 || state.current_written <= cfg_.max_file_size_bytes) {
        return true;
    }

    const bool is_l1 = (&state == &l1_);
    const bool is_md = (&state == &md_);
    const bool is_cd = (&state == &cd_);
    const char* kind = is_l1 ? "L1" : (is_md ? "MD" : (is_cd ? "CD" : "UNKNOWN"));
    LOG_INFO << "DataStore rotate: kind=" << kind
             << " path=" << state.path
             << " written=" << state.current_written
             << " limit=" << cfg_.max_file_size_bytes;
    if (!closeFileLocked(state)) {
        return false;
    }
    if (is_l1) {
        const bool ok = openL1Locked();
        if (ok) {
            ++rotate_count_;
        }
        return ok;
    }
    if (is_md) {
        const bool ok = openMDLocked();
        if (ok) {
            ++rotate_count_;
        }
        return ok;
    }
    if (is_cd) {
        const bool ok = openCDLocked();
        if (ok) {
            // max_event_number_saved is a per-file cap; a successful size rotate
            // starts a new shard and must not inherit the previous file's keys.
            cd_.event_save_keys.clear();
            ++rotate_count_;
        }
        return ok;
    }
    return false;
}

bool DataStore::writeRecordLocked(FileState& state,
                                  uint32_t du_id,
                                  const uint8_t* payload,
                                  std::size_t payload_size) {
    if (state.file == nullptr) {
        return false;
    }
    if (!rotateIfNeededLocked(state)) {
        return false;
    }
    return writeRecordNoRotateLocked(state, du_id, payload, payload_size);
}

bool DataStore::writeRecordNoRotateLocked(FileState& state,
                                          uint32_t du_id,
                                          const uint8_t* payload,
                                          std::size_t payload_size) {
    if (state.file == nullptr) {
        return false;
    }
    constexpr std::size_t kRecordHeaderBytes = sizeof(LegacyDatRecordHeader);
    if (payload_size > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()) - kRecordHeaderBytes) {
        markIoFailureLocked("writeRecord(size-overflow:" + state.path + ")", EOVERFLOW);
        return false;
    }
    const std::size_t record_size = kRecordHeaderBytes + payload_size;

    LegacyDatRecordHeader rec{};
    rec.size = static_cast<uint32_t>(record_size);
    rec.type = kDaqPckTypeDuEvent;
    rec.source = du_id;

    if (std::fwrite(&rec, sizeof(rec), 1, state.file) != 1) {
        markIoFailureLocked("fwrite(record:" + state.path + ")", errno);
        return false;
    }
    if (std::fwrite(payload, payload_size, 1, state.file) != 1) {
        markIoFailureLocked("fwrite(payload:" + state.path + ")", errno);
        return false;
    }

    state.current_written += record_size;
    state.total_written += record_size;
    return true;
}

void DataStore::resetStateForOpenLocked(FileState& state) {
    state.file_id = 0;
    state.current_written = 0;
    state.total_written = 0;
    state.event_save_keys.clear();
}

std::string DataStore::makeL1FilenameLocked() {
    // 合作组要求的严格命名：{module}_{ts}_RUN<N>_{tag}-{file_id}.dat
    std::ostringstream oss;
    ++l1_.file_id;
    oss << cfg_.module << "_" << formatUtcTimestampLocked()
        << "_RUN" << l1_run_number_ << "_"
        << cfg_.l1_tag << "-"
        << std::setfill('0') << std::setw(4) << l1_.file_id << ".dat";
    return oss.str();
}

std::string DataStore::makeMDFilenameLocked() {
    // MD 与 L1 共用目录与 RUN 号（MD 仅在 daqMode=4 时产生）
    std::ostringstream oss;
    ++md_.file_id;
    oss << cfg_.module << "_" << formatUtcTimestampLocked()
        << "_RUN" << l1_run_number_ << "_"
        << cfg_.l1_tag << "-MD-"
        << std::setfill('0') << std::setw(4) << md_.file_id << ".dat";
    return oss.str();
}

std::string DataStore::makeCDFilenameLocked() {
    std::ostringstream oss;
    ++cd_.file_id;
    oss << cfg_.module << "_" << formatUtcTimestampLocked()
        << "_RUN" << cd_run_number_ << "_"
        << cfg_.l3_tag << "-CD-"
        << std::setfill('0') << std::setw(4) << cfg_.max_event_number_saved << "-"
        << cd_.file_id << ".dat";
    return oss.str();
}

std::time_t DataStore::nowUtcLocked() const {
    if (cfg_.fixed_utc_time >= 0) {
        return static_cast<std::time_t>(cfg_.fixed_utc_time);
    }
    return std::time(nullptr);
}

std::string DataStore::formatUtcTimestampLocked() const {
    const std::time_t now = nowUtcLocked();
    std::tm tm_utc{};
    gmtime_r(&now, &tm_utc);

    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y%m%d_%H%M%S");
    return oss.str();
}

bool DataStore::writeFileHeaderLocked(FileState& state, const std::string& filename) {
    state.file_header.fill(0);
    writeU32Le(state.file_header.data() + 0, kFileHeadSize);
    writeU32Le(state.file_header.data() + 4, kLegacyDataVersion);
    // CD 文件头写 CD 自己的 RUN，L1/MD 文件头写 L1 RUN（与 rotateIfNeededLocked 的 state 身份判断一致）
    const uint32_t rn = (&state == &cd_) ? cd_run_number_ : l1_run_number_;
    writeU32Le(state.file_header.data() + 8, rn);
    writeU32Le(state.file_header.data() + 12, static_cast<uint32_t>(nowUtcLocked()));
    writeU32Le(state.file_header.data() + 16, 0);
    writeU32Le(state.file_header.data() + 20, static_cast<uint32_t>(filename.size()));

    const std::size_t copy_len = std::min<std::size_t>(128, filename.size());
    std::memcpy(state.file_header.data() + 24, filename.data(), copy_len);

    if (std::fwrite(state.file_header.data(), state.file_header.size(), 1, state.file) != 1) {
        markIoFailureLocked("fwrite(file-header:" + state.path + ")", errno);
        return false;
    }
    if (std::fflush(state.file) != 0) {
        markIoFailureLocked("fflush(file-header:" + state.path + ")", errno);
        return false;
    }
    state.current_written += state.file_header.size();
    state.total_written += state.file_header.size();
    state.has_file_header = true;
    return true;
}

bool DataStore::patchFileCloseTimeLocked(FileState& state) {
    if (!state.has_file_header || state.file == nullptr) {
        return true;
    }
    writeU32Le(state.file_header.data() + 16, static_cast<uint32_t>(nowUtcLocked()));
    if (std::fseek(state.file, 0, SEEK_SET) != 0) {
        markIoFailureLocked("fseek(file-header:" + state.path + ")", errno);
        return false;
    }
    if (std::fwrite(state.file_header.data(), state.file_header.size(), 1, state.file) != 1) {
        markIoFailureLocked("fwrite(close-header:" + state.path + ")", errno);
        return false;
    }
    if (std::fflush(state.file) != 0) {
        markIoFailureLocked("fflush(close-header:" + state.path + ")", errno);
        return false;
    }
    return true;
}

void DataStore::markIoFailureLocked(const std::string& op, int err) {
    ++io_failure_count_;
    last_io_errno_ = err;
    if (err != 0) {
        last_io_error_ = op + ": " + std::strerror(err);
    } else {
        last_io_error_ = op;
    }
}

// 扫描 dir 里所有包含 "RUN<N>_" token 且匹配 substring_filter 的 .dat 文件，返回 max(N)+1。
//
// 支持两种命名格式共存：
//   新版 (new-cs-daq-chengwei): RUN<N>_GP80_<ts>_<tag>-<fileid>.dat      —— RUN 在开头
//   老版 (newdataformat等):    GP80_<ts>_RUN<N>_<tag>-<fileid>.dat      —— RUN 在中间
//
// 扫描规则：
//   - 文件名必须以 ".dat" 结尾
//   - substring_filter（"MD_"/"CD_"）可以出现在文件名任意位置
//   - RUN<N>_ token 中的 "RUN" 必须位于文件名开头，或紧跟在 '_' 之后（保证 "RUN" 是
//     独立 token 而不是 "FOORUN123_" 这种误匹配）
//   - RUN 后至少一个十进制数字，数字后必须紧跟 '_'
//
// 这样无论新老命名，新启动的 DAQ 分配到的 RUN 号始终大于目录内历史最大 RUN 号。
uint32_t DataStore::scanNextRunNumberLocked(const std::string& dir,
                                            const char* substring_filter,
                                            uint32_t fallback_start) {
    namespace fs = std::filesystem;
    if (dir.empty()) {
        return fallback_start;
    }
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) {
        return fallback_start;
    }

    const std::string filter_str = substring_filter ? std::string(substring_filter) : std::string();
    uint32_t max_found = 0;
    bool any = false;
    fs::directory_iterator it(dir, ec);
    if (ec) {
        return fallback_start;
    }
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string name = it->path().filename().string();

        // 必须以 ".dat" 结尾
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".dat") != 0) continue;

        // 过滤子串（"MD_"/"CD_"）可以出现在文件名任意位置
        if (!filter_str.empty() && name.find(filter_str) == std::string::npos) continue;

        // 在文件名里扫描所有 "RUN<N>_" token，取最大值。RUN 必须是独立 token
        // —— 位于文件名开头或紧跟在 '_' 之后 —— 避免把 "FOORUN123_" 这种前缀
        // 误识为 RUN 123。允许一个文件名出现多个 RUN token（实际不会），取 max。
        std::size_t search_pos = 0;
        uint32_t name_max = 0;
        bool name_any = false;
        while (search_pos < name.size()) {
            const std::size_t run_pos = name.find("RUN", search_pos);
            if (run_pos == std::string::npos) break;

            // 边界检查：'RUN' 必须在文件名开头或紧跟 '_'
            const bool boundary_ok = (run_pos == 0) || (name[run_pos - 1] == '_');
            if (!boundary_ok) {
                search_pos = run_pos + 1;
                continue;
            }

            // 解析 RUN 后的数字
            const std::size_t digit_start = run_pos + 3;
            std::size_t digit_end = digit_start;
            uint64_t n64 = 0;
            bool overflow = false;
            while (digit_end < name.size() &&
                   std::isdigit(static_cast<unsigned char>(name[digit_end]))) {
                n64 = n64 * 10 + static_cast<uint64_t>(name[digit_end] - '0');
                if (n64 > std::numeric_limits<uint32_t>::max()) {
                    overflow = true;
                    break;
                }
                ++digit_end;
            }
            if (overflow || digit_end == digit_start) {
                // 没有数字或溢出，跳过 "RUN" 这 3 个字符后继续搜
                search_pos = run_pos + 3;
                continue;
            }

            // 数字后必须紧跟 '_'
            if (digit_end >= name.size() || name[digit_end] != '_') {
                search_pos = digit_end;
                continue;
            }

            const uint32_t n = static_cast<uint32_t>(n64);
            if (n > name_max) name_max = n;
            name_any = true;
            search_pos = digit_end + 1;
        }

        if (!name_any) continue;
        if (name_max > max_found) max_found = name_max;
        any = true;
    }
    if (!any) return fallback_start;
    if (max_found == std::numeric_limits<uint32_t>::max()) return max_found; // 防溢出
    return max_found + 1U;
}
