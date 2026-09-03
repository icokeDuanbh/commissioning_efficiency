#include <common.h>
#include <config.h>
#include <frontend.h>
#include <t2_stage_trace.h>
#include <utils.h>
#include <thread>
#include <cstring>
#include <fstream>
#include <chrono>

namespace {
inline uint64_t steadyNowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

Frontend::Frontend(const ElectronicsConfig &elec_config, DataHandleCallback data_handle_callback)
    : elec_config_(elec_config)
    , du_id_(elec_config.du_id)
    , running_(false)
    , data_handle_callback_(data_handle_callback)
{
    const auto& sw = ConfigManager::instance().getSoftwareConfig();
    ring_buffer_size_ = sw.network_input_buffer_size ? sw.network_input_buffer_size : static_cast<size_t>(sw.buffer_size);
}

Frontend::~Frontend() {
    stop();
    unconfigure();
}

size_t Frontend::ringBufferedBytes() const noexcept {
    std::lock_guard<std::mutex> lk(ring_mutex_);
    return ring_buffer_ ? ring_buffer_->size() : 0U;
}

size_t Frontend::ringCapacityBytes() const noexcept {
    std::lock_guard<std::mutex> lk(ring_mutex_);
    return ring_buffer_ ? ring_buffer_->capacity() : 0U;
}

ZmqComm::SendStats Frontend::zmqSendStats() const noexcept {
    return zmq_comm_ ? zmq_comm_->sendStats() : ZmqComm::SendStats{};
}

void Frontend::initialize() {
    zmq_comm_ = std::make_unique<ZmqComm>();
    const auto& sw = ConfigManager::instance().getSoftwareConfig();
    zmq_comm_->configure(sw);
    zmq_comm_->addEndpoint(du_id_, elec_config_.ip, elec_config_.port);
    zmq_comm_->setReceiveCallback([this](uint32_t, std::vector<char> chunk) {
        onZmqPayload(std::move(chunk));
    });

    std::cout << "Frontend initialized successfully" << std::endl;
}

// 分配 RingBuffer、允许入环，再 ZmqComm::start（失败则关 inbound）
void Frontend::configure() {
    if (!zmq_comm_) {
        throw FrontendException("ZMQ frontend not initialized, du_id = " + std::to_string(du_id_));
    }
    ring_drop_count_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(ring_mutex_);
        if (!ring_buffer_) {
            ring_buffer_ = std::make_unique<RingBuffer>(ring_buffer_size_);
        }
        ring_buffer_->clear();
    }
    ring_accepts_inbound_.store(true, std::memory_order_release);
    if (!zmq_comm_->start()) {
        ring_accepts_inbound_.store(false, std::memory_order_release);
        throw FrontendException("ZmqComm::start failed, du_id = " + std::to_string(du_id_));
    }
}

void Frontend::unconfigure() {
    ring_accepts_inbound_.store(false, std::memory_order_release);
    if (zmq_comm_) {
        zmq_comm_->shutdownTransportNoexcept();
    }
    std::lock_guard<std::mutex> lk(ring_mutex_);
    ring_buffer_.reset();
}

// ZMQ 线程：在 ring_accepts_inbound_ 且环有空间时写入一块 payload
void Frontend::onZmqPayload(std::vector<char> chunk)
{
    if (chunk.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lk(ring_mutex_);
    if (!ring_buffer_) {
        LOG_ERROR << "FATAL: ZMQ payload while ring_buffer_ is null, du_id=" << du_id_;
        std::abort();
    }
    const uint64_t now_steady_ns = steadyNowNs();
    last_active_ns_.store(now_steady_ns, std::memory_order_relaxed);

    const bool ring_accepts = ring_accepts_inbound_.load(std::memory_order_acquire);
    const std::size_t ring_size_before = ring_buffer_->size();
    const std::size_t ring_free_before = ring_buffer_->freeSize();
    const auto trace_sig = t2_stage_trace::inspectT2Message(
        reinterpret_cast<const std::uint8_t*>(chunk.data()), chunk.size());

    if (!ring_accepts) {
        if (trace_sig.has_value()) {
            t2_stage_trace::writeFrontendRx(
                t2_stage_trace::FrontendTraceRecord{
                    du_id_,
                    now_steady_ns,
                    t2_stage_trace::wallNowNs(),
                    false,
                    false,
                    ring_size_before,
                    ring_free_before,
                },
                *trace_sig);
        }
        return;
    }
    if (chunk.size() > ring_free_before) {
        if (trace_sig.has_value()) {
            t2_stage_trace::writeFrontendRx(
                t2_stage_trace::FrontendTraceRecord{
                    du_id_,
                    now_steady_ns,
                    t2_stage_trace::wallNowNs(),
                    false,
                    true,
                    ring_size_before,
                    ring_free_before,
                },
                *trace_sig);
        }
        ring_drop_count_.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR << "Ring buffer full, dropping ZMQ chunk, du_id=" << du_id_
                  << " len=" << chunk.size();
        return;
    }
    if (trace_sig.has_value()) {
        t2_stage_trace::writeFrontendRx(
            t2_stage_trace::FrontendTraceRecord{
                du_id_,
                now_steady_ns,
                t2_stage_trace::wallNowNs(),
                true,
                true,
                ring_size_before,
                ring_free_before,
            },
            *trace_sig);
    }
    std::memcpy(ring_buffer_->writeHead(), chunk.data(), chunk.size());
    ring_buffer_->commit(chunk.size());
}

void Frontend::start() {
    if (running_.load()) {
        std::cout << "Frontend is already running" << std::endl;
        return;
    }

    {
        std::lock_guard<std::mutex> lk(ring_mutex_);
        if (!ring_buffer_) {
            ring_buffer_ = std::make_unique<RingBuffer>(ring_buffer_size_);
        }
    }

    running_.store(true);
    if (ConfigManager::instance().getSoftwareConfig().debug_raw) {
        daq_thread_ = std::thread(&Frontend::daqThreadDebug, this);
    }
    else {
        daq_thread_ = std::thread(&Frontend::daqThread, this);
    }

    std::cout << "Frontend started successfully" << std::endl;
}

void Frontend::stop() {
    if (running_.load()) {
        running_.store(false);
        if (daq_thread_.joinable()) {
            daq_thread_.join();
        }

        std::cout << "Frontend stopped" << std::endl;
    }

    ring_accepts_inbound_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lk(ring_mutex_);
    if (ring_buffer_) {
        ring_buffer_->clear();
    }
}

bool Frontend::isTransportReady() const noexcept {
    return zmq_comm_ != nullptr && zmq_comm_->recvLoopRunning();
}

void Frontend::daqThreadDebug() {
    std::cout << "DAQ thread debug started" << std::endl;

    std::ofstream ofs("data-debug.bin", std::ios::binary);
    while (running_.load()) {
        try {
            std::unique_lock<std::mutex> lk(ring_mutex_);
            if (!ring_buffer_) {
                lk.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (ring_buffer_->size() == 0) {
                lk.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            while (true) {
                if (!ring_buffer_ || ring_buffer_->size() == 0) {
                    break;
                }
                uint8_t* rh = ring_buffer_->readHead();
                const size_t sz = ring_buffer_->size();
                lk.unlock();
                ssize_t ret = data_handle_callback_(rh, sz);
                lk.lock();
                if (!ring_buffer_) {
                    break;
                }
                if (ret != 0) {
                    const size_t n = static_cast<size_t>(std::abs(ret));
                    ofs.write(reinterpret_cast<const char*>(ring_buffer_->readHead()),
                        static_cast<std::streamsize>(n));
                    ring_buffer_->consume(n);
                } else {
                    break;
                }
            }
        } catch (const DAQException& e) {
            LOG_ERROR << "Error in DAQ thread: " << e.message << std::endl;
            break;
        }
    }
    ofs.close();

    std::cout << "DAQ thread stopped" << std::endl;
}

void Frontend::daqThread() {
    std::cout << "DAQ thread started" << std::endl;

    while (running_.load()) {
        try {
            std::unique_lock<std::mutex> lk(ring_mutex_);
            if (!ring_buffer_) {
                lk.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (ring_buffer_->size() == 0) {
                lk.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            while (true) {
                if (!ring_buffer_ || ring_buffer_->size() == 0) {
                    break;
                }
                uint8_t* rh = ring_buffer_->readHead();
                const size_t sz = ring_buffer_->size();
                lk.unlock();
                ssize_t ret = data_handle_callback_(rh, sz);
                lk.lock();
                if (!ring_buffer_) {
                    break;
                }
                if (ret != 0) {
                    ring_buffer_->consume(static_cast<size_t>(std::abs(ret)));
                } else {
                    break;
                }
            }
        } catch (const DAQException& e) {
            LOG_ERROR << "Error in DAQ thread: " << e.message << std::endl;
            break;
        }
    }

    std::cout << "DAQ thread stopped" << std::endl;
}

void Frontend::sendTCP(uint8_t* data, size_t len) {
    if (!zmq_comm_) {
        throw FrontendException("ZMQ not initialized, du_id = " + std::to_string(du_id_));
    }
    if (!zmq_comm_->recvLoopRunning()) {
        throw FrontendException("ZMQ recv not running (call configure/start chain), du_id = "
            + std::to_string(du_id_));
    }
    if (!zmq_comm_->send(du_id_, data, len)) {
        throw FrontendException("ZMQ send failed, du_id = " + std::to_string(du_id_));
    }
}
