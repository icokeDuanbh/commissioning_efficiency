#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

class DataStore {
public:
    struct Config {
        std::string module = "GP80";   // 文件名前缀模块名
        std::string l1_tag = "L1";     // L1 文件名模板（不含 RUN 前缀，RUN 由扫描分配）
        std::string l3_tag = "L3";     // CD 文件名模板（不含 RUN 前缀）
        std::string l1_output_dir = "./"; // L1/MD 输出目录，open 时创建并扫描
        std::string l3_output_dir = "./"; // CD 输出目录，open 时创建并扫描
        std::size_t max_file_size_bytes = 100U * 1024U * 1024U; // 单文件轮转阈值
        uint32_t daq_mode = 0;          // 4 时 RAW 走 MD，否则走 L1（与 writeRawRecord 一致）
        uint32_t max_event_number_saved = 0; // 0 表示不限制；>0 时 CD 侧可截断
        bool enable_writing = true;
        // 测试钩子：固定 UTC 秒，用于可复现文件名；<0 代表使用系统时间。
        int64_t fixed_utc_time = -1;
    };

    DataStore();
    explicit DataStore(Config cfg);
    ~DataStore();

    // 扫描 l1_output_dir / l3_output_dir 自动分配下一 RUN 号并打开所有 FileState。
    // L1/MD 从目录内匹配 "RUN<N>_...MD_*.dat" 的最大 N+1（空目录从 1 起）；
    // CD 从匹配 "RUN<N>_...CD_*.dat" 的最大 N+1（空目录从 10001 起）。
    bool open();
    void close();

    bool writeRawRecord(uint32_t du_id, const uint8_t* payload, std::size_t payload_size);
    bool writeMdRecord(uint32_t du_id, const uint8_t* payload, std::size_t payload_size);
    /** CD 轮转按显式 logical event key（与 Builder tag 对齐），不从 payload 字节推断。 */
    bool writeCdRecord(uint32_t du_id, uint32_t event_key, const uint8_t* payload, std::size_t payload_size);
    struct CdFragmentRecord {
        uint32_t du_id = 0;
        /** Payload must remain valid only for the duration of the synchronous writeCdEvent() call. */
        const uint8_t* payload = nullptr;
        std::size_t payload_size = 0;
    };

    /** CD event-level write: preflights all fragment records so one logical event does not cross files. */
    bool writeCdEvent(uint32_t event_key, const std::vector<CdFragmentRecord>& fragments);

    std::string currentL1FilePath() const;
    std::string currentMDFilePath() const;
    std::string currentCDFilePath() const;
    uint32_t currentCDRunNumber() const;
    std::uint64_t ioFailureCount() const;
    std::uint64_t rotateCount() const;
    int lastIoErrno() const;
    std::string lastIoError() const;

private:
    struct __attribute__((__packed__)) LegacyDatRecordHeader {
        uint32_t size;
        uint32_t type;
        uint32_t source;
    };

    struct FileState {
        FILE* file = nullptr;
        std::string path;
        std::string filename;
        uint32_t file_id = 0;              // 同前缀下第几个分卷
        std::size_t current_written = 0;   // 当前文件已写字节（含头）
        std::size_t total_written = 0;     // 跨分卷累计
        std::array<char, 256> file_header{};
        bool has_file_header = false;
        std::unordered_set<uint64_t> event_save_keys; // CD：已写过的事件键，防重复
    };

    bool shouldWriteL1Locked() const;
    bool shouldWriteMDLocked() const;
    bool shouldWriteCDLocked() const;

    bool openFilesLocked();
    bool openL1Locked();
    bool openMDLocked();
    bool openCDLocked();
    bool openFileLocked(FileState& state, const std::string& filename, const std::string& dir);
    bool closeFileLocked(FileState& state);
    bool writeRecordLocked(FileState& state, uint32_t du_id, const uint8_t* payload, std::size_t payload_size);
    bool writeRecordNoRotateLocked(FileState& state,
                                   uint32_t du_id,
                                   const uint8_t* payload,
                                   std::size_t payload_size);
    bool preflightCdSizeLocked(std::size_t bytes_to_write, const char* op_name);
    bool rotateIfNeededLocked(FileState& state);
    void resetStateForOpenLocked(FileState& state);

    std::string makeL1FilenameLocked();
    std::string makeMDFilenameLocked();
    std::string makeCDFilenameLocked();
    std::time_t nowUtcLocked() const;
    std::string formatUtcTimestampLocked() const;

    bool writeFileHeaderLocked(FileState& state, const std::string& filename);
    bool patchFileCloseTimeLocked(FileState& state);
    void markIoFailureLocked(const std::string& op, int err);

    // 扫描 dir 里匹配 "RUN<N>_...<substring_filter>...<.dat>" 的文件，
    // 返回 max(N)+1；目录不存在或无命中返回 fallback_start。
    // substring_filter 用 "MD_" 或 "CD_" 可同时命中老版和新版的文件命名。
    static uint32_t scanNextRunNumberLocked(const std::string& dir,
                                            const char* substring_filter,
                                            uint32_t fallback_start);

private:
    Config cfg_;
    mutable std::mutex mu_;
    uint32_t l1_run_number_ = 0;   // L1 + MD 共用（MD 在 daqMode=4 下也落 l1_output_dir）
    uint32_t cd_run_number_ = 0;
    bool is_open_ = false;
    std::uint64_t io_failure_count_ = 0;
    std::uint64_t rotate_count_ = 0;
    int last_io_errno_ = 0;
    std::string last_io_error_;
    FileState l1_;
    FileState md_;
    FileState cd_;
};
