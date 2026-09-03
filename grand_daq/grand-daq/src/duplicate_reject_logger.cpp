#include "duplicate_reject_logger.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

namespace grand {
namespace {

constexpr uint64_t kNsPerSecond = 1000000000ULL;

std::string formatUtcTimestamp(std::time_t utc_time) {
    std::tm tm_utc{};
    gmtime_r(&utc_time, &tm_utc);

    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y%m%d_%H%M%S");
    return oss.str();
}

} // namespace

DuplicateRejectLogger::DuplicateRejectLogger(std::string path,
                                             std::size_t queue_capacity,
                                             std::size_t max_events_per_file)
    : path_(std::move(path))
    , queue_capacity_(queue_capacity == 0 ? 1 : queue_capacity)
    , max_events_per_file_(max_events_per_file == 0 ? kDefaultMaxEventsPerFile
                                                    : max_events_per_file) {
    if (path_.empty()) {
        return;
    }

    current_path_ = std::filesystem::path(path_);
    rotated_file_prefix_ = stripTimestampSuffix(current_path_.stem().string());
    if (!openLogFile(current_path_)) {
        return;
    }

    try {
        worker_ = std::thread(&DuplicateRejectLogger::workerLoop, this);
        enabled_.store(true, std::memory_order_relaxed);
    } catch (...) {
        out_.close();
    }
}

DuplicateRejectLogger::~DuplicateRejectLogger() {
    stop();
}

bool DuplicateRejectLogger::tryLog(std::vector<DuplicateRejectLogHit>&& hits) noexcept {
    try {
        if (!enabled_.load(std::memory_order_relaxed) || hits.empty()) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || queue_.size() >= queue_capacity_) {
                dropped_count_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            queue_.push_back(std::move(hits));
            pending_count_.fetch_add(1, std::memory_order_relaxed);
        }
        cv_.notify_one();
        return true;
    } catch (...) {
        enabled_.store(false, std::memory_order_relaxed);
        dropped_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

void DuplicateRejectLogger::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
    enabled_.store(false, std::memory_order_relaxed);
}

std::filesystem::path DuplicateRejectLogger::makeRunLogPath(const std::string& directory,
                                                            uint32_t run_number,
                                                            std::time_t utc_time) {
    std::ostringstream name;
    name << "DuplicateRejectLog_" << run_number << "_"
         << formatUtcTimestamp(utc_time)
         << ".log";

    const std::filesystem::path dir = directory.empty()
        ? std::filesystem::path(".")
        : std::filesystem::path(directory);
    return dir / name.str();
}

std::string DuplicateRejectLogger::stripTimestampSuffix(const std::string& stem) {
    constexpr std::size_t kTimestampWithSeparatorLen = 16; // _YYYYMMDD_HHMMSS
    if (stem.size() < kTimestampWithSeparatorLen) {
        return stem;
    }

    const std::size_t pos = stem.size() - kTimestampWithSeparatorLen;
    const std::string suffix = stem.substr(pos);
    if (suffix.size() != kTimestampWithSeparatorLen || suffix[0] != '_' || suffix[9] != '_') {
        return stem;
    }
    for (std::size_t i = 1; i < suffix.size(); ++i) {
        if (i == 9) {
            continue;
        }
        if (suffix[i] < '0' || suffix[i] > '9') {
            return stem;
        }
    }
    return stem.substr(0, pos);
}

std::filesystem::path DuplicateRejectLogger::makeRotatedLogPath(std::time_t utc_time) const {
    std::ostringstream filename;
    filename << rotated_file_prefix_ << "_"
             << formatUtcTimestamp(utc_time)
             << "_part" << std::setfill('0') << std::setw(6)
             << rotation_index_
             << current_path_.extension().string();
    return current_path_.parent_path() / filename.str();
}

bool DuplicateRejectLogger::openLogFile(const std::filesystem::path& path) noexcept {
    try {
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                return false;
            }
        }
        out_.open(path, std::ios::out | std::ios::app);
        return out_.is_open();
    } catch (...) {
        return false;
    }
}

bool DuplicateRejectLogger::rotateIfNeeded() noexcept {
    if (events_in_current_file_ < max_events_per_file_) {
        return true;
    }

    try {
        if (out_.is_open()) {
            out_.flush();
            out_.close();
        }
        events_in_current_file_ = 0;
        ++rotation_index_;
        current_path_ = makeRotatedLogPath(std::time(nullptr));
        return openLogFile(current_path_);
    } catch (...) {
        return false;
    }
}

std::string DuplicateRejectLogger::formatLine(const std::vector<DuplicateRejectLogHit>& hits) {
    std::vector<DuplicateRejectLogHit> sorted = hits;
    std::sort(sorted.begin(), sorted.end(),
              [](const DuplicateRejectLogHit& lhs, const DuplicateRejectLogHit& rhs) {
                  if (lhs.du_id != rhs.du_id) {
                      return lhs.du_id < rhs.du_id;
                  }
                  return lhs.timestamp_ns < rhs.timestamp_ns;
              });

    const auto earliest = std::min_element(
        sorted.begin(),
        sorted.end(),
        [](const DuplicateRejectLogHit& lhs, const DuplicateRejectLogHit& rhs) {
            return lhs.timestamp_ns < rhs.timestamp_ns;
        });

    std::ostringstream oss;
    oss << (earliest->timestamp_ns / kNsPerSecond)
        << "."
        << std::setfill('0') << std::setw(9)
        << (earliest->timestamp_ns % kNsPerSecond)
        << ": [";

    for (std::size_t i = 0; i < sorted.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << "(" << sorted[i].du_id << ", "
            << (sorted[i].timestamp_ns % kNsPerSecond) << ")";
    }
    oss << "]\n";
    return oss.str();
}

void DuplicateRejectLogger::workerLoop() noexcept {
    for (;;) {
        std::vector<DuplicateRejectLogHit> hits;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_ || !queue_.empty();
            });
            if (queue_.empty()) {
                if (stopping_) {
                    break;
                }
                continue;
            }
            hits = std::move(queue_.front());
            queue_.pop_front();
            pending_count_.fetch_sub(1, std::memory_order_relaxed);
        }

        try {
            if (!rotateIfNeeded()) {
                enabled_.store(false, std::memory_order_relaxed);
                dropped_count_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            out_ << formatLine(hits);
            if (!out_) {
                enabled_.store(false, std::memory_order_relaxed);
            } else {
                ++events_in_current_file_;
            }
        } catch (...) {
            enabled_.store(false, std::memory_order_relaxed);
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

} // namespace grand
