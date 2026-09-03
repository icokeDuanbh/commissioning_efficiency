#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace grand {

struct DuplicateRejectLogHit {
    uint32_t du_id = 0;
    uint64_t timestamp_ns = 0;
};

class DuplicateRejectLogger {
public:
    static constexpr std::size_t kDefaultMaxEventsPerFile = 1000000;

    DuplicateRejectLogger(std::string path,
                          std::size_t queue_capacity,
                          std::size_t max_events_per_file = kDefaultMaxEventsPerFile);
    ~DuplicateRejectLogger();

    DuplicateRejectLogger(const DuplicateRejectLogger&) = delete;
    DuplicateRejectLogger& operator=(const DuplicateRejectLogger&) = delete;

    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    uint64_t droppedCount() const noexcept { return dropped_count_.load(std::memory_order_relaxed); }
    bool likelyAccepting() const noexcept {
        return enabled() && pending_count_.load(std::memory_order_relaxed) < queue_capacity_;
    }

    bool tryLog(std::vector<DuplicateRejectLogHit>&& hits) noexcept;
    void noteDropped() noexcept { dropped_count_.fetch_add(1, std::memory_order_relaxed); }
    void stop() noexcept;
    static std::filesystem::path makeRunLogPath(const std::string& directory,
                                                uint32_t run_number,
                                                std::time_t utc_time);

private:
    std::string path_;
    std::size_t queue_capacity_ = 1;
    std::size_t max_events_per_file_ = kDefaultMaxEventsPerFile;
    std::filesystem::path current_path_;
    std::string rotated_file_prefix_;
    uint64_t events_in_current_file_ = 0;
    uint64_t rotation_index_ = 0;
    std::ofstream out_;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<DuplicateRejectLogHit>> queue_;
    bool stopping_ = false;
    std::atomic<bool> enabled_{false};
    std::atomic<uint64_t> dropped_count_{0};
    std::atomic<std::size_t> pending_count_{0};

    static std::string formatLine(const std::vector<DuplicateRejectLogHit>& hits);
    static std::string stripTimestampSuffix(const std::string& stem);
    std::filesystem::path makeRotatedLogPath(std::time_t utc_time) const;
    bool openLogFile(const std::filesystem::path& path) noexcept;
    bool rotateIfNeeded() noexcept;
    void workerLoop() noexcept;
};

} // namespace grand
