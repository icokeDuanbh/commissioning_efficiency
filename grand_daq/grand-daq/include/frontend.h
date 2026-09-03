#pragma once

#include <common.h>
#include <utils.h>
#include <comm.h>
#include <config.h>
#include <data_handler.h>
#include <atomic>
#include <mutex>

class Frontend {
public:
    Frontend(const ElectronicsConfig &elec_config, DataHandleCallback data_handle_callback);
    ~Frontend();

    uint32_t duId() const noexcept { return du_id_; }

    /**
     * @brief 初始化
     *
     * 此处构造 `ZmqComm` 并完成 `configure/addEndpoint` 与收包回调登记。
     * 环形缓冲在 `configure()` 于 ZMQ 收包启动前分配。
     */
    void initialize();
    
    /**
     * @brief 建立 legacy ZMQ（建连、握手、接收线程）。控制命令统一由 `App` 经 `sendTCP()` 下发。
     * @note 必须先于 `zmq_comm_->start()` 建好 `ring_buffer_`，避免收包回调面对空环静默丢包。
     */
    void configure();

    /**
     * @brief 结束 ZMQ 接收线程并关闭 socket。
     */
    void unconfigure();

    /**
     * @brief 启动数据读出线程（消费环形缓冲）；。
     */
    void start();

    /**
     * @brief 停止数据读出，不在这里承担控制时序。
     * @note 仅停止消费线程；ZMQ 仍可收包，但会丢弃入环并在环上 `clear()`，避免跨 `App::start()` 串包。
     */
    void stop();

    /** 经 legacy 三帧封装发往 DU（`ZMQ_IDENTITY` 在 socket 上，不在消息帧内）。 */
    void sendTCP(uint8_t* data, size_t len);

    /** 结构化检查：ZMQ 已初始化且 recv 循环在跑（与 `sendTCP` 前置条件一致）。 */
    bool isTransportReady() const noexcept;

    uint64_t ringDropCount() const noexcept { return ring_drop_count_.load(std::memory_order_relaxed); }
    uint64_t lastActiveNs() const noexcept { return last_active_ns_.load(std::memory_order_relaxed); }
    size_t ringBufferedBytes() const noexcept;
    size_t ringCapacityBytes() const noexcept;
    ZmqComm::SendStats zmqSendStats() const noexcept;

private:
    void daqThread();       // 从 RingBuffer 取块调 data_handle_callback_
    void daqThreadDebug();  // 调试变体（若编译启用）

    void onZmqPayload(std::vector<char> chunk); // ZmqComm 收包回调，写入环

    const ElectronicsConfig &elec_config_;
    uint32_t du_id_;

    std::thread daq_thread_;
    std::atomic<bool> running_;

    std::unique_ptr<ZmqComm> zmq_comm_;

    DataHandleCallback data_handle_callback_;

    size_t ring_buffer_size_;
    std::unique_ptr<RingBuffer> ring_buffer_;

    /** 仅当为 true 时 ZMQ 收包可写入环（`configure()` 至 `stop()` 收口之间为 true）。 */
    std::atomic<bool> ring_accepts_inbound_{false};
    std::atomic<uint64_t> ring_drop_count_{0};
    std::atomic<uint64_t> last_active_ns_{0};

    mutable std::mutex ring_mutex_; // 保护 ring_buffer_ 与 running 协作
};
