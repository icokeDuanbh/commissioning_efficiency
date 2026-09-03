#pragma once 

#include <du_sys_config.h>
#include <zmq_server.h>
#include <message_dispatcher.h>
#include <frontend.h>
#include <data_manager.h>
#include <message_impl.h>
#include <map>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

#define BROADCASTUDPIP "192.168.61.255"
#define BROADCASTUDPPORT 9910

namespace grand {

class DUDAQApp {
public:
    DUDAQApp();
    void sysInit(int port = 0, int manualDuID = 0);
    void sysTerm();

private:
    DUSysConfig *m_sysConfig;
    MessageDispatcher *m_msgDispatcher;
    ZMQServer *m_server;
    IFrontend *m_frontend;
    DataManager *m_dataManager;
    DataManager *m_t2DataManager;
    AcceptMessage *m_acceptMessage; 

    int m_daqMode;
    int m_manualDuID; // New member for manual ID override

    // CHANGE-BEGIN(du-io-thread)
    // Single ZMQ I/O thread (owns ROUTER socket):
    // - receives commands and dispatches them
    // - sends all outbound messages produced by other threads
    //
    // Motivation:
    // - ZMQ sockets are not thread-safe
    // - we want FPGA readout thread to never block on networking
    struct OutboundItem {
        grand::MessageType type;
        std::vector<char> bytes;
    };

    struct T3TriggerItem {
        std::vector<char> bytes;
    };

    std::atomic<bool> m_stopThreads{false};

    std::thread m_zmqIoThread;
    std::thread m_t3WorkerThread;

    std::mutex m_outMutex;
    std::condition_variable m_outCv;
    std::deque<OutboundItem> m_outQ;
    size_t m_outQCapacity{2048};

    std::mutex m_t3Mutex;
    std::condition_variable m_t3Cv;
    std::deque<T3TriggerItem> m_t3Q;
    size_t m_t3QCapacity{2048};

    void zmqIoThreadMain();
    void t3WorkerThreadMain();
    bool enqueueOutbound(OutboundItem&& item, bool dropIfFull);
    bool enqueueT3Trigger(T3TriggerItem&& item);
    // CHANGE-END(du-io-thread)

private:
    bool initialize();
    bool configure(void *param);
    bool configureOne(void *param);
    bool start();
    bool stop();
    bool terminate();
    bool toError();
    void processCommand(char *data, size_t sz);
    void getT3Trigger(char *data, size_t sz);
    int getLocalIPLastOctet();

    std::atomic<uint64_t> m_outQDropCount{0};
    std::atomic<uint64_t> m_t3QDropCount{0};
    void udp_broadcast(const std::string &message,
                       const std::string &broadcast_ip,
                       int port);
    const char* messageTypeName(int type);
};

}
