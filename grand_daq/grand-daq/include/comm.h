#pragma once

/**
 * DU 通信：现役运行路径是 legacy ZMQ（`ZmqComm` + 本文件前半段帧辅助函数）。
 */

#include "config.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>

/** 与 `old-cs-daq-cw` `ZmqAdapter::sendWithStatus` 一致：delimiter 帧为 1 字节。 */
constexpr std::size_t kLegacyZmqDelimiterBytes = 1;
/** 第二帧固定 20 字节（`snprintf(..., "%d", daq_mode)`）。 */
constexpr std::size_t kLegacyZmqDaqModeFrameBytes = 20;

/**
 * 生成固定 20 字节的 daq_mode 帧（末尾由 '\0' 填充，与 snprintf 写入一致）。
 */
std::vector<char> legacyZmqFormatDaqModeFrame(std::int32_t daq_mode);

/**
 * DEALER 侧待发多帧（不含 ZMQ_IDENTITY；identity 由 `zmq_setsockopt(ZMQ_IDENTITY, ...)` 设置）。
 * 帧序：1 字节 delimiter + 20 字节 daq_mode + payload。
 */
std::vector<std::vector<char>> buildLegacyZmqOutboundFrames(
    std::int32_t daq_mode,
    std::vector<char> payload);

/**
 * 解析对端多帧为应用层 payload。
 * 支持：delimiter+payload（2 帧）、delimiter+daq_mode+payload（3 帧，daq_mode 须为 20 字节）。
 * delimiter 须为 1 字节。
 */
bool legacyZmqParseInboundFrames(
    const std::vector<std::vector<char>>& frames,
    std::vector<char>& out_payload);

/** 确认已与 libzmq 链接且可调用其运行时 API（`zmq_version`）。 */
bool legacyZmqRuntimeAvailable();

/** Whether a zmq_send errno should be retried within the bounded send deadline. */
bool isRetryableLegacyZmqSendErrno(int err);

/**
 * Legacy ZMQ DEALER：per-DU `tcp://ip:port`，握手与多帧收发对齐 `old-cs-daq-cw` `comm_zmq_adapter.cpp`。
 */
class ZmqComm {
public:
    using ReceiveCallback = std::function<void(uint32_t du_id, std::vector<char> payload)>;

    struct SendStats {
        uint64_t send_call_count = 0;
        uint64_t eagain_count = 0;
        uint64_t retry_count = 0;
        uint64_t retry_success_count = 0;
        uint64_t deadline_fail_count = 0;
        uint64_t non_retry_fail_count = 0;
    };

    ZmqComm();
    ~ZmqComm();

    ZmqComm(const ZmqComm&) = delete;
    ZmqComm& operator=(const ZmqComm&) = delete;

    void configure(const SoftwareConfig& sw);
    void addEndpoint(uint32_t du_id, const std::string& ip, uint16_t port);
    void setReceiveCallback(ReceiveCallback cb);

    /**
     * 建连、`ZMQ_IDENTITY`、握手、启动接收线程。接收线程已在跑时再次调用不再发握手并返回 true。
     * 失败（含接收线程构造失败）时回滚 `recv_loop_running_` 并 `shutdownTransportNoexcept()`，可安全重试。
     */
    bool start();
    /** 结束接收线程；不关闭 socket/context（正常 `App::stop()` 热停路径仍靠此 + 析构收尾）。 */
    void stopRecvLoop();
    /**
     * join 接收线程并关闭所有 endpoint socket、销毁 context；不抛异常，可重复调用。
     * `Frontend::unconfigure()` / 失败回滚路径用它，使下一轮 `start()` 从零建连。
     */
    void shutdownTransportNoexcept();

    /** 对指定 DU 发三帧业务消息；identity 不在消息里。 */
    bool send(uint32_t du_id,
              const void* data,
              size_t len,
              std::chrono::milliseconds eagain_retry_deadline = std::chrono::milliseconds(50));

    bool recvLoopRunning() const { return recv_loop_running_.load(); }
    SendStats sendStats() const noexcept;

private:
    void closeAllSocketsAndContextLocked();
    struct Endpoint {
        uint32_t du_id = 0;
        std::string ip;
        uint16_t port = 0;
        void* socket = nullptr; // ZMQ DEALER，须在 zmq_mtx_ 下创建/关闭
    };

    bool setupSocketLocked(void* socket);
    void sendHandshakeLocked();
    void recvLoop(); // 轮询各 endpoint，解析帧后调 recv_cb_
    static ssize_t recvEnvelope(void* socket, char* buffer, size_t max_size);

    SoftwareConfig sw_;
    std::vector<Endpoint> endpoints_;
    void* ctx_ = nullptr;

    /** 与 `old-cs-daq-cw` `zmq_mutex_` 一致：所有 `zmq_*`、socket 生命周期与 `endpoints_` 读写均须在此锁内。 */
    std::mutex zmq_mtx_;
    std::mutex cb_mtx_;
    ReceiveCallback recv_cb_;

    std::atomic<bool> recv_loop_running_{false};
    std::thread recv_thread_;

    std::unique_ptr<char[]> recv_storage_;
    std::size_t recv_capacity_ = 0;

    std::atomic<uint64_t> send_call_count_{0};
    std::atomic<uint64_t> send_eagain_count_{0};
    std::atomic<uint64_t> send_retry_count_{0};
    std::atomic<uint64_t> send_retry_success_count_{0};
    std::atomic<uint64_t> send_deadline_fail_count_{0};
    std::atomic<uint64_t> send_non_retry_fail_count_{0};
};
