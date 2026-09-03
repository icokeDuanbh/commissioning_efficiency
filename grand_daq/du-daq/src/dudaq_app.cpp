
/************************/
// Created by gumh, duanbh
// 2022.09.02
/************************/

#include <dudaq_app.h>
#include <utils.h>
#include <functional>
#include <du_sys_config.h>
#include <du_fsm.h>
#include <scope_dummy.h>
#include <scope_a.h>
#include <message_impl.h>
#include <mutex>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <cstring>
#include <string>
#include <chrono>


using namespace std::placeholders;
using namespace grand;

int duID;

static uint64_t nowMs(){
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

DUDAQApp::DUDAQApp() {
    m_manualDuID = 0;
}

void DUDAQApp::sysInit(int port, int manualDuID) {
    m_manualDuID = manualDuID;
    // CHANGE-BEGIN(du-io-thread)
    // Store the config instance in the member (the original code accidentally
    // shadowed the member with a local variable).
    m_sysConfig = DUSysConfig::instance();
    // CHANGE-END(du-io-thread)
    if (port > 0) {
        m_sysConfig->backendBindUrl = "tcp://*:" + std::to_string(port);
        std::cout << "Overriding backendBindUrl to " << m_sysConfig->backendBindUrl << std::endl;
    }

#ifdef REAL_DU
    m_frontend = new ScopeA;
#else
    m_frontend = new ScopeDummy;
#endif
    m_server = new ZMQServer;
    // std::cout << "Now will connect scoket " << m_sysConfig->backendBindUrl << std::endl;
    m_server->setup(m_sysConfig->messageInputBufferSize, m_sysConfig->backendBindUrl, m_sysConfig->maxClientAddressSize);
    m_msgDispatcher = new MessageDispatcher;
    m_msgDispatcher->addProcessor((MessageType)MT_CMD, std::bind(&DUDAQApp::processCommand, this, _1, _2));
    m_dataManager = new DataManager;

    // CHANGE-BEGIN(du-io-thread)
    // All sending is redirected into a bounded queue.
    // This ensures that:
    // - FPGA readout thread never blocks on zmq_send()
    // - DataManager never calls into ZMQ directly
    //
    // Backpressure policy for DAQ data messages when outbound queue is full:
    // - drop-newest for MT_T2 / MT_RAWEVENT / MT_DAQEVENT
    // - for other message types (if any), we block until space is available
    //   (these are expected to be rare and more important than data).
    // - 
    auto enqueueFromDataManager = [this](char *data, size_t sz) -> void {
        if (!data || sz < sizeof(MessageHeader)) {
            return;
        }
        MessageHeader *hdr = reinterpret_cast<MessageHeader*>(data);
        OutboundItem item;
        item.type = hdr->type;
        item.bytes.assign(data, data + sz);

        const bool isDataMsg = (item.type == MT_T2) || (item.type == MT_RAWEVENT) || (item.type == MT_DAQEVENT);
        (void)this->enqueueOutbound(std::move(item), /*dropIfFull=*/isDataMsg);
    };

    m_dataManager->setEventOutput(enqueueFromDataManager);
    m_dataManager->setT2EventOutput(enqueueFromDataManager);
    m_dataManager->setRawEventOutput(enqueueFromDataManager);
    // CHANGE-END(du-io-thread)
    
    m_dataManager->initialize();

    DUFSM::start();

    // CHANGE-BEGIN(du-io-thread)
    // Start worker threads after initialization.
    // - ZMQ socket is created without Server::inputThread; zmqIoThreadMain()
    //   is the only thread allowed to call ZMQ APIs.
    m_stopThreads.store(false);
    m_server->initializeNoThread();
    m_zmqIoThread = std::thread(&DUDAQApp::zmqIoThreadMain, this);
    m_t3WorkerThread = std::thread(&DUDAQApp::t3WorkerThreadMain, this);
    // CHANGE-END(du-io-thread)
}

void DUDAQApp::sysTerm() {
    // CHANGE-BEGIN(du-io-thread)
    // Stop threads first so no one touches objects that are about to be freed.
    m_stopThreads.store(true);
    m_outCv.notify_all();
    m_t3Cv.notify_all();
    if (m_zmqIoThread.joinable()) {
        m_zmqIoThread.join();
    }
    if (m_t3WorkerThread.joinable()) {
        m_t3WorkerThread.join();
    }
    // CHANGE-END(du-io-thread)

    m_frontend->terminate();
    // CHANGE-BEGIN(du-io-thread)
    m_server->terminateNoThread();
    // CHANGE-END(du-io-thread)
    m_dataManager->terminate();

    delete m_msgDispatcher;
    delete m_dataManager;
    delete m_server;
    delete m_frontend;
}

void DUDAQApp::udp_broadcast(const std::string &message,
                             const std::string &broadcast_ip,
                             int port)
{
    int sockfd = -1;
    struct sockaddr_in addr;
    int broadcastEnable = 1;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket failed");
        return;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    struct in_addr inaddr;
    if (inet_aton(broadcast_ip.c_str(), &inaddr) == 0) {
        fprintf(stderr, "udp_broadcast: invalid IP %s\n", broadcast_ip.c_str());
        close(sockfd);
        return;
    }
    addr.sin_addr = inaddr;

    if (broadcast_ip == "192.168.61.255" || broadcast_ip == BROADCASTIP) {
        if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST,
                       &broadcastEnable, sizeof(broadcastEnable)) < 0) {
            perror("setsockopt SO_BROADCAST failed");
            close(sockfd);
            return;
        }
    }

    ssize_t sent = sendto(sockfd, message.c_str(), message.size(), 0,
                          reinterpret_cast<struct sockaddr*>(&addr),
                          sizeof(addr));

    if (sent < 0) {
        perror("sendto failed");
    }

    close(sockfd);
}

const char* DUDAQApp::messageTypeName(int type)
{
    switch (type) {
        case MT_T2:
            return "MT_T2";
        case MT_RAWEVENT:
            return "MT_RAWEVENT";
        case MT_DAQEVENT:
            return "MT_DAQEVENT";
        case MT_CMD:
            return "MT_CMD";
        default:
            return "MT_UNKNOWN";
    }
}

bool DUDAQApp::enqueueOutbound(OutboundItem&& item, bool dropIfFull) {
    std::unique_lock<std::mutex> lock(m_outMutex);

    if (m_outQ.size() >= m_outQCapacity) {

        m_outQDropCount.fetch_add(1, std::memory_order_relaxed);

        return false;
    }

    m_outQ.emplace_back(std::move(item));
    lock.unlock();
    m_outCv.notify_one();

    return true;
}

// bool DUDAQApp::enqueueOutbound(OutboundItem&& item, bool dropIfFull) {
//     std::unique_lock<std::mutex> lock(m_outMutex);
//     if (m_outQ.size() >= m_outQCapacity) {

//         static std::atomic<uint64_t> lastOutBroadcastMs{0};

//         uint64_t now = nowMs();
//         uint64_t last = lastOutBroadcastMs.load();

//         // if (now - last >= 1000 && lastOutBroadcastMs.compare_exchange_strong(last, now)) {

// //         //     std::string msg =
// //         //         "DUDAQ_QUEUE_FAIL du_id=" + std::to_string(duID) +
// //         //         " queue=outQ" +
// //         //         " depth=" + std::to_string(m_outQ.size()) +
// //         //         " capacity=" + std::to_string(m_outQCapacity) +
// //         //         " type=" + std::string(messageTypeName(item.type));

// //         //     udp_broadcast(msg, BROADCASTIP, BROADCASTPORT);
// //         // }

// //         return false;
// //     } else {
// //         // For non-data messages, apply backpressure by waiting for space.
// //         // This path should be rare.
// //         m_outCv.wait(lock, [this]() {
// //             return m_stopThreads.load() || (m_outQ.size() < m_outQCapacity);
// //         });
// //         if (m_stopThreads.load()) {
// //             return false;
// //         }
// //     }

// //     m_outQ.emplace_back(std::move(item));
// //     lock.unlock();
// //     m_outCv.notify_one();
// //     return true;
// // }

// bool DUDAQApp::enqueueT3Trigger(T3TriggerItem&& item) {
//     std::unique_lock<std::mutex> lock(m_t3Mutex);
//     if (m_t3Q.size() >= m_t3QCapacity) {
//         // Triggers can be dropped under overload; we prefer keeping readout and
//         return false;
//     }
//     m_t3Q.emplace_back(std::move(item));
//     lock.unlock();
//     m_t3Cv.notify_one();
//     return true;
// }

bool DUDAQApp::enqueueT3Trigger(T3TriggerItem&& item) {
    if (m_t3Q.size() >= m_t3QCapacity) {

        // static std::atomic<uint64_t> lastT3BroadcastMs{0};

        // uint64_t now = nowMs();
        // uint64_t last = lastT3BroadcastMs.load();

        // if (now - last >= 1000 &&
        //     lastT3BroadcastMs.compare_exchange_strong(last, now)) {

        //     std::string msg =
        //         "DUDAQ_QUEUE_FAIL du_id=" + std::to_string(duID) +
        //         " queue=t3Q" +
        //         " depth=" + std::to_string(m_t3Q.size()) +
        //         " capacity=" + std::to_string(m_t3QCapacity);

        //     udp_broadcast(msg, BROADCASTIP, BROADCASTPORT);
        // }

        m_t3QDropCount.fetch_add(1, std::memory_order_relaxed);

        return false;
    }

    m_t3Q.emplace_back(std::move(item));
    m_t3Cv.notify_one();
    
    return true;
}

void DUDAQApp::zmqIoThreadMain() {
    std::vector<char> inBuf;
    inBuf.resize(m_sysConfig ? m_sysConfig->messageInputBufferSize : 102400);
    uint64_t lastStatMs = 0;

    while (!m_stopThreads.load()) {
        // 1) Receive side (commands from CS-DAQ). Keep timeout small so we can
        //    also drain the outbound queue promptly.
        const size_t rsz = m_server->read(inBuf.data(), inBuf.size(), /*pollTimeoutMs=*/10);
        if (rsz > 0) {
            m_msgDispatcher->dispatch(inBuf.data(), rsz);
        }

        // 2) heartbeat every second

        uint64_t now = nowMs();
        if (now - lastStatMs >= 1000) {
            lastStatMs = now;
            uint64_t outDrop = m_outQDropCount.exchange(0, std::memory_order_relaxed);
            uint64_t t3Drop = m_t3QDropCount.exchange(0, std::memory_order_relaxed);
            size_t outDepth = 0;
            size_t t3Depth = 0;

            {
                std::lock_guard<std::mutex> lock(m_outMutex);
                outDepth = m_outQ.size();
            }

            {
                std::lock_guard<std::mutex> lock(m_t3Mutex);
                t3Depth = m_t3Q.size();
            }

            std::string msg =
                "DUDAQ_DROP_STAT du_id=" + std::to_string(duID) +
                " outQ_drop=" + std::to_string(outDrop) +
                " t3Q_drop=" + std::to_string(t3Drop) +
                " outQ_depth=" + std::to_string(outDepth) +
                " outQ_capacity=" + std::to_string(m_outQCapacity) +
                " t3Q_depth=" + std::to_string(t3Depth) +
                " t3Q_capacity=" + std::to_string(m_t3QCapacity);

            udp_broadcast(msg, BROADCASTUDPIP, BROADCASTUDPPORT);
        }

        // 3) Send side (data from readout / T3 worker). Drain as much as possible.
        for (;;) {
            OutboundItem item;
            {
                std::unique_lock<std::mutex> lock(m_outMutex);
                if (m_outQ.empty()) {
                    break;
                }
                item = std::move(m_outQ.front());
                m_outQ.pop_front();
                lock.unlock();
                m_outCv.notify_one();
            }

            // If we don't yet have a known client address, drop outbound.
            // (The address is learned from the last received message.)
            if (!m_server->hasClient()) {
                continue;
            }
            m_server->write(item.bytes.data(), item.bytes.size());
        }
    }
}

void DUDAQApp::t3WorkerThreadMain() {
    while (!m_stopThreads.load()) {
        T3TriggerItem item;
        {
            std::unique_lock<std::mutex> lock(m_t3Mutex);
            m_t3Cv.wait(lock, [this]() {
                return m_stopThreads.load() || !m_t3Q.empty();
            });
            if (m_stopThreads.load()) {
                return;
            }
            item = std::move(m_t3Q.front());
            m_t3Q.pop_front();
        }

        // Heavy trigger matching + DAQEvent building happens here, not in the
        // ZMQ I/O thread.
        if (m_dataManager) {
            m_dataManager->accept(item.bytes.data(), item.bytes.size());
        }
    }
}
// CHANGE-END(du-io-thread)

void DUDAQApp::processCommand(char *data, size_t sz) {
    CommandMessage msg(data, sz);
    std::string cmd = msg.cmd();

    int daqMode = m_server->m_duDAQMode;
    m_daqMode = daqMode;

    if(cmd == "INIT") {
        EInitialize e;
        e.fun = std::bind(&DUDAQApp::initialize, this);
        DUFSM::sendEvent(e);
    }
    else if(cmd == "CONF") {
        EConfigure e;
        e.fun = std::bind(&DUDAQApp::configure, this, msg.param());
        if (m_manualDuID > 0) {
            duID = m_manualDuID;
            std::cout << "Using manual DU ID: " << duID << std::endl;
        } else {
            duID = getLocalIPLastOctet();
        }
        DUFSM::sendEvent(e);
    }
    else if(cmd == "CONFONE") {
        duID = atoi((char*)msg.param());
    }
    else if(cmd == "STAR") {
        EStart e;
        e.fun = std::bind(&DUDAQApp::start, this);
        DUFSM::sendEvent(e);
    }
    else if(cmd == "DOTRIGGER") {
        // CHANGE-BEGIN(du-io-thread)
        // Enqueue trigger payload to be processed by dedicated T3 worker.
        // This keeps the ZMQ I/O thread responsive and prevents trigger
        // processing from delaying subsequent recv/send.
        T3TriggerItem item;
        item.bytes.assign(reinterpret_cast<char*>(msg.param()), reinterpret_cast<char*>(msg.param()) + msg.paramSize());
        (void)enqueueT3Trigger(std::move(item));
        // CHANGE-END(du-io-thread)
    }
    else if(cmd == "STOP") {
        EStop e;
        e.fun = std::bind(&DUDAQApp::stop, this);
        DUFSM::sendEvent(e);
    }
    else if(cmd == "TERM") {
        ETerminate e;
        e.fun = std::bind(&DUDAQApp::terminate, this);
        DUFSM::sendEvent(e);
    }
    
    if(m_daqMode == 1) {
        m_frontend->setCallback([this](char *data, size_t sz)->void {
            this->m_dataManager->addRawEvent(data, sz); 
        });
    }
    if(m_daqMode == 2) {
        m_frontend->setCallback([this](char *data, size_t sz)->void {
            this->m_dataManager->addEvent(data, sz, m_daqMode);
        });
    }
    if(m_daqMode == 3) {
        m_frontend->setCallback([this](char *data, size_t sz)->void {
            this->m_dataManager->addEvent(data, sz, m_daqMode);
        });

    }
}

bool DUDAQApp::initialize() {
    m_frontend->initialize();
    return true;
}

bool DUDAQApp::configure(void *param) {
    m_frontend->configure(param);
    return true;
}

bool DUDAQApp::start() {
    m_frontend->start();
    return true;
}

bool DUDAQApp::stop() {
    m_frontend->stop();
    if(m_daqMode == 2)
        m_dataManager->stop();
    return true;
}

bool DUDAQApp::terminate() {
    m_frontend->terminate();
    return true;
}

bool DUDAQApp::toError() {
    return true;
}

int DUDAQApp::getLocalIPLastOctet() {
    struct ifaddrs *ifaddr, *ifa;
    char host[INET_ADDRSTRLEN];

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return -1;
    }

    int lastOctet = -1;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;

        if (ifa->ifa_addr->sa_family == AF_INET) {
            void *addr = &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
            inet_ntop(AF_INET, addr, host, sizeof(host));

            std::string ip(host);

            if (ip.rfind("192.168.61.", 0) == 0) {
                size_t pos = ip.rfind('.');
                if (pos != std::string::npos) {
                    lastOctet = std::stoi(ip.substr(pos + 1));
                    break;
                }
            }
        }
    }

    freeifaddrs(ifaddr);

    if (lastOctet < 0) {
        lastOctet = 1;   // fallback for local test
    }

    return lastOctet;
}

// // 获取本机 IP 的最后一段（限定 192.168.61.xx）(get the last IP address to define the duID)
// int DUDAQApp::getLocalIPLastOctet() {
//     struct ifaddrs *ifaddr, *ifa;
//     int family;
//     char host[INET_ADDRSTRLEN];  // 替代 NI_MAXHOST, IPv4地址够用了

//     if (getifaddrs(&ifaddr) == -1) {
//         perror("getifaddrs");
//         return -1;
//     }

//     int lastOctet = -1;

//     for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
//         if (ifa->ifa_addr == NULL)
//             continue;

//         family = ifa->ifa_addr->sa_family;

//         if (family == AF_INET) { // IPv4
//             void *addr = &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
//             inet_ntop(AF_INET, addr, host, sizeof(host));

//             std::string ip(host);

//             // 只要 192.168.61 开头的
//             if (ip.rfind("192.168.61.", 0) == 0) {
//                 size_t pos = ip.rfind('.');
//                 if (pos != std::string::npos) {
//                     lastOctet = std::stoi(ip.substr(pos + 1));
//                     break;
//                 }
//             }
//             if (ip == "127.0.0.1") {
//                 lastOctet = 101;
//                 break;
//             }
//         }
//     }

//     freeifaddrs(ifaddr);
//     return lastOctet;
// }
