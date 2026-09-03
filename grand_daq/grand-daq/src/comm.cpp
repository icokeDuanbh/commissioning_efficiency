#include "comm.h"
#include "message.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cerrno>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

#include <zmq.h>
#include <utils.h>
#include <common.h>

namespace {

ssize_t legacyZmqRecvEnvelopeImpl(void* socket, char* buffer, size_t max_size) {
    int more = 0;
    size_t more_size = sizeof(more);

    char delimiter[16];
    int nbytes = zmq_recv(socket, delimiter, sizeof(delimiter), 0);
    if (nbytes < 0) {
        return -1;
    }
    if (nbytes != 1) {
        return -1;
    }

    int rc = zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &more_size);
    if (rc != 0 || !more) {
        return -1;
    }

    zmq_msg_t frame2;
    zmq_msg_init(&frame2);
    nbytes = zmq_msg_recv(&frame2, socket, 0);
    if (nbytes < 0) {
        zmq_msg_close(&frame2);
        return -1;
    }

    rc = zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &more_size);
    if (rc != 0) {
        zmq_msg_close(&frame2);
        return -1;
    }

    if (!more) {
        const size_t payload_size = zmq_msg_size(&frame2);
        if (payload_size > max_size) {
            zmq_msg_close(&frame2);
            return -1;
        }
        std::memcpy(buffer, zmq_msg_data(&frame2), payload_size);
        zmq_msg_close(&frame2);
        return static_cast<ssize_t>(payload_size);
    }

    zmq_msg_close(&frame2);

    zmq_msg_t payload;
    zmq_msg_init(&payload);
    nbytes = zmq_msg_recv(&payload, socket, 0);
    if (nbytes < 0) {
        zmq_msg_close(&payload);
        return -1;
    }
    const size_t total_received = zmq_msg_size(&payload);
    if (total_received > max_size) {
        zmq_msg_close(&payload);
        return -1;
    }
    std::memcpy(buffer, zmq_msg_data(&payload), total_received);
    zmq_msg_close(&payload);

    rc = zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &more_size);
    if (rc == 0 && more) {
        char discard[4096];
        while (more) {
            zmq_recv(socket, discard, sizeof(discard), 0);
            zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &more_size);
        }
    }

    return static_cast<ssize_t>(total_received);
}

} // namespace

constexpr long kLegacyZmqSendRetryPollMs = 1;

std::vector<char> legacyZmqFormatDaqModeFrame(std::int32_t daq_mode)
{
    std::vector<char> out(kLegacyZmqDaqModeFrameBytes, '\0');
    std::snprintf(out.data(), kLegacyZmqDaqModeFrameBytes, "%d", static_cast<int>(daq_mode));
    return out;
}

std::vector<std::vector<char>> buildLegacyZmqOutboundFrames(
    std::int32_t daq_mode,
    std::vector<char> payload)
{
    std::vector<std::vector<char>> frames;
    frames.emplace_back(kLegacyZmqDelimiterBytes, '\0');
    frames.push_back(legacyZmqFormatDaqModeFrame(daq_mode));
    frames.push_back(std::move(payload));
    return frames;
}

bool legacyZmqParseInboundFrames(
    const std::vector<std::vector<char>>& frames,
    std::vector<char>& out_payload)
{
    out_payload.clear();
    if (frames.size() != 2 && frames.size() != 3) {
        return false;
    }
    if (frames[0].size() != kLegacyZmqDelimiterBytes) {
        return false;
    }
    if (frames.size() == 2) {
        out_payload = frames[1];
        return true;
    }
    if (frames[1].size() != kLegacyZmqDaqModeFrameBytes) {
        return false;
    }
    out_payload = frames[2];
    return true;
}

ZmqComm::ZmqComm() = default;

void ZmqComm::closeAllSocketsAndContextLocked()
{
    for (auto& e : endpoints_) {
        if (e.socket) {
            zmq_close(e.socket);
            e.socket = nullptr;
        }
    }
    if (ctx_) {
        zmq_ctx_destroy(ctx_);
        ctx_ = nullptr;
    }
}

void ZmqComm::shutdownTransportNoexcept()
{
    stopRecvLoop();
    std::lock_guard<std::mutex> lk(zmq_mtx_);
    closeAllSocketsAndContextLocked();
}

ZmqComm::~ZmqComm()
{
    shutdownTransportNoexcept();
}

void ZmqComm::configure(const SoftwareConfig& sw)
{
    sw_ = sw;
    recv_capacity_ = std::max(sw_.network_input_buffer_size, sw_.zmq_send_buffer_size);
    if (recv_capacity_ == 0) {
        recv_capacity_ = 2048000;
    }
    recv_storage_ = std::make_unique<char[]>(recv_capacity_);
}

void ZmqComm::addEndpoint(uint32_t du_id, const std::string& ip, uint16_t port)
{
    std::lock_guard<std::mutex> lk(zmq_mtx_);
    endpoints_.push_back({du_id, ip, port, nullptr});
}

void ZmqComm::setReceiveCallback(ReceiveCallback cb)
{
    std::lock_guard<std::mutex> lk(cb_mtx_);
    recv_cb_ = std::move(cb);
}

bool ZmqComm::setupSocketLocked(void* socket)
{
    int linger = 0;
    if (zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(linger)) != 0) {
        return false;
    }
    const std::string& id = sw_.zmq_dealer_identity;
    if (zmq_setsockopt(socket, ZMQ_IDENTITY, id.c_str(), id.size()) != 0) {
        return false;
    }
    int buf = static_cast<int>(sw_.zmq_send_buffer_size);
    if (buf <= 0) {
        buf = 2048000;
    }
    if (zmq_setsockopt(socket, ZMQ_SNDBUF, &buf, sizeof(buf)) != 0) {
        return false;
    }
    if (zmq_setsockopt(socket, ZMQ_RCVBUF, &buf, sizeof(buf)) != 0) {
        return false;
    }
    int hwm = sw_.zmq_snd_hwm;
    if (zmq_setsockopt(socket, ZMQ_SNDHWM, &hwm, sizeof(hwm)) != 0) {
        return false;
    }
    return true;
}

void ZmqComm::sendHandshakeLocked()
{
    grand::MessageHeader header{};
    header.size = grand::kMessageHeaderSize;
    header.type = grand::MT_HSHAKE;
    char buf[grand::kMessageHeaderSize];
    std::memcpy(buf, &header, sizeof(header));
    char daq_mode[kLegacyZmqDaqModeFrameBytes] = {0};
    std::snprintf(daq_mode, sizeof(daq_mode), "%d", static_cast<int>(sw_.zmq_daq_mode));

    std::vector<uint32_t> du_ids;
    {
        std::lock_guard<std::mutex> lk(zmq_mtx_);
        for (const auto& e : endpoints_) {
            if (e.socket) {
                du_ids.push_back(e.du_id);
            }
        }
    }

    for (uint32_t du_id : du_ids) {
        bool sent = false;
        for (int attempt = 0; attempt < 200; ++attempt) {
            {
                std::lock_guard<std::mutex> lk(zmq_mtx_);
                void* socket = nullptr;
                for (auto& e : endpoints_) {
                    if (e.du_id == du_id && e.socket) {
                        socket = e.socket;
                        break;
                    }
                }
                if (!socket) {
                    break;
                }
                const int mf = ZMQ_SNDMORE | ZMQ_DONTWAIT;
                const int lf = ZMQ_DONTWAIT;
                int rc = zmq_send(socket, "", 1, mf);
                if (rc == 1) {
                    rc = zmq_send(socket, daq_mode, sizeof(daq_mode), mf);
                    if (rc == static_cast<int>(sizeof(daq_mode))) {
                        rc = zmq_send(socket, buf, sizeof(buf), lf);
                        if (rc == static_cast<int>(sizeof(buf))) {
                            sent = true;
                        }
                    }
                }
            }
            if (sent) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!sent) {
            std::cerr << "ZmqComm: failed to send HSHAKE after retries\n";
        }
    }
}

// 建 context、各 DU 的 DEALER connect、握手并起 recv_thread_
bool ZmqComm::start()
{
    {
        std::lock_guard<std::mutex> lk(zmq_mtx_);
        if (!ctx_) {
            ctx_ = zmq_ctx_new();
            if (!ctx_) {
                return false;
            }
        }

        for (auto& e : endpoints_) {
            if (e.socket) {
                continue;
            }
            e.socket = zmq_socket(ctx_, ZMQ_DEALER);
            if (!e.socket) {
                closeAllSocketsAndContextLocked();
                return false;
            }
            if (!setupSocketLocked(e.socket)) {
                closeAllSocketsAndContextLocked();
                return false;
            }
            std::string url = "tcp://" + e.ip + ":" + std::to_string(static_cast<int>(e.port));
            if (zmq_connect(e.socket, url.c_str()) != 0) {
                closeAllSocketsAndContextLocked();
                return false;
            }
        }
    }

    if (recv_loop_running_.load()) {
        return true;
    }

    sendHandshakeLocked();

    recv_loop_running_.store(true);
    try {
        recv_thread_ = std::thread(&ZmqComm::recvLoop, this);
    } catch (...) {
        recv_loop_running_.store(false);
        shutdownTransportNoexcept();
        return false;
    }
    return true;
}

void ZmqComm::stopRecvLoop()
{
    recv_loop_running_.store(false);
    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }
}

void ZmqComm::recvLoop()
{
    while (recv_loop_running_.load()) {
        ReceiveCallback cb;
        {
            std::lock_guard<std::mutex> lk(cb_mtx_);
            cb = recv_cb_;
        }

        int rc = 0;
        std::vector<std::pair<uint32_t, std::vector<char>>> received;
        {
            std::lock_guard<std::mutex> lk(zmq_mtx_);
            std::vector<zmq_pollitem_t> poll_items;
            std::vector<uint32_t> du_ids;
            for (auto& e : endpoints_) {
                if (!e.socket) {
                    continue;
                }
                zmq_pollitem_t item{};
                item.socket = e.socket;
                item.events = ZMQ_POLLIN;
                item.revents = 0;
                poll_items.push_back(item);
                du_ids.push_back(e.du_id);
            }

            if (!poll_items.empty()) {
                rc = zmq_poll(poll_items.data(), poll_items.size(), 0);
            }

            if (rc > 0) {
                for (size_t i = 0; i < poll_items.size(); ++i) {
                    if (poll_items[i].revents & ZMQ_POLLIN) {
                        const ssize_t n = recvEnvelope(
                            poll_items[i].socket,
                            recv_storage_.get(),
                            recv_capacity_);
                        if (n > 0) {
                            received.emplace_back(
                                du_ids[i],
                                std::vector<char>(
                                    recv_storage_.get(),
                                    recv_storage_.get() + static_cast<size_t>(n)));
                        }
                    }
                }
            }
        }

        for (auto& pr : received) {
            if (cb) {
                cb(pr.first, std::move(pr.second));
            }
        }

        if (rc <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

ssize_t ZmqComm::recvEnvelope(void* socket, char* buffer, size_t max_size)
{
    return legacyZmqRecvEnvelopeImpl(socket, buffer, max_size);
}

bool isRetryableLegacyZmqSendErrno(int err)
{
    return err == EAGAIN;
}

bool ZmqComm::send(uint32_t du_id,
                   const void* data,
                   size_t len,
                   std::chrono::milliseconds eagain_retry_deadline)
{
    std::lock_guard<std::mutex> lk(zmq_mtx_);
    send_call_count_.fetch_add(1, std::memory_order_relaxed);
    void* socket = nullptr;
    for (auto& e : endpoints_) {
        if (e.du_id == du_id) {
            socket = e.socket;
            break;
        }
    }
    if (!socket || (!data && len > 0)) {
        return false;
    }

    char daq_mode[kLegacyZmqDaqModeFrameBytes] = {0};
    std::snprintf(daq_mode, sizeof(daq_mode), "%d", static_cast<int>(sw_.zmq_daq_mode));
    const int mf = ZMQ_SNDMORE | ZMQ_DONTWAIT;
    const int lf = ZMQ_DONTWAIT;
    const auto deadline = std::chrono::steady_clock::now() + eagain_retry_deadline;
    auto send_frame = [&](const char* frame_name, const void* frame_data, size_t frame_len, int flags) -> bool {
        bool retried = false;
        for (;;) {
            const int rc = zmq_send(socket, frame_data, frame_len, flags);
            if (rc == static_cast<int>(frame_len)) {
                if (retried) {
                    send_retry_success_count_.fetch_add(1, std::memory_order_relaxed);
                }
                return true;
            }

            const int err = zmq_errno();
            if (!isRetryableLegacyZmqSendErrno(err)) {
                send_non_retry_fail_count_.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN << "ZmqComm::send du_id=" << du_id << " " << frame_name
                         << " frame failed: len=" << frame_len << " errno=" << err;
                return false;
            }

            send_eagain_count_.fetch_add(1, std::memory_order_relaxed);
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                send_deadline_fail_count_.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN << "ZmqComm::send du_id=" << du_id << " " << frame_name
                         << " frame failed after EAGAIN retry deadline: len=" << frame_len
                         << " errno=" << err;
                return false;
            }

            retried = true;
            send_retry_count_.fetch_add(1, std::memory_order_relaxed);
            zmq_pollitem_t item{};
            item.socket = socket;
            item.events = ZMQ_POLLOUT;
            const int poll_rc = zmq_poll(&item, 1, kLegacyZmqSendRetryPollMs);
            if (poll_rc < 0 && !isRetryableLegacyZmqSendErrno(zmq_errno()) && zmq_errno() != EINTR) {
                send_non_retry_fail_count_.fetch_add(1, std::memory_order_relaxed);
                LOG_WARN << "ZmqComm::send du_id=" << du_id << " " << frame_name
                         << " poll failed: errno=" << zmq_errno();
                return false;
            }
        }
    };

    if (!send_frame("delimiter", "", 1, mf)) return false;
    if (!send_frame("daq_mode", daq_mode, sizeof(daq_mode), mf)) return false;
    if (!send_frame("payload", data, len, lf)) return false;
    return true;
}

ZmqComm::SendStats ZmqComm::sendStats() const noexcept
{
    SendStats stats{};
    stats.send_call_count = send_call_count_.load(std::memory_order_relaxed);
    stats.eagain_count = send_eagain_count_.load(std::memory_order_relaxed);
    stats.retry_count = send_retry_count_.load(std::memory_order_relaxed);
    stats.retry_success_count = send_retry_success_count_.load(std::memory_order_relaxed);
    stats.deadline_fail_count = send_deadline_fail_count_.load(std::memory_order_relaxed);
    stats.non_retry_fail_count = send_non_retry_fail_count_.load(std::memory_order_relaxed);
    return stats;
}

bool legacyZmqRuntimeAvailable()
{
    int major = 0;
    int minor = 0;
    int patch = 0;
    zmq_version(&major, &minor, &patch);
    return major > 0;
}
