/****************/
// Created by duanbh,
// 20220910.
/****************/
#include <arpa/inet.h>
#include <sys/socket.h>
#include <data_manager.h>
#include <cassert>
#include <message_impl.h>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_set>

using namespace grand;

extern int duID;

DataManager::DataManager() : m_eventBuffer(nullptr), m_duTimeStampSave(nullptr) {
    m_server = new ZMQServer;
    m_frequenceCountBefore = 0;
    m_frequenceCountAfter = 0;
}
DataManager::~DataManager() {
    terminate();
}

void DataManager::setEventOutput(EventOutput fun) {
    m_eventOutputFun = fun;
}

void DataManager::setT2EventOutput(EventOutput fun) { // decide if we output TR info.
    m_t2EventOutputFun = fun;
}

// ========== UDP 广播函数（DataManager 私有方法）, UDP broadcast function to get monitor information ==========
void DataManager::udp_broadcast(const std::string &message,
                                const std::string &broadcast_ip,
                                int port)
{
    int sockfd = -1;
    struct sockaddr_in addr;
    int broadcastEnable = 1;

    // 创建 UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket failed");
        return;
    }

    // 配置目标地址
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

    // 如果是广播地址，就打开 SO_BROADCAST
    if (broadcast_ip == "255.255.255.255" || broadcast_ip == BROADCASTIP) {
        if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST,
                       &broadcastEnable, sizeof(broadcastEnable)) < 0) {
            perror("setsockopt SO_BROADCAST failed");
            close(sockfd);
            return;
        }
    }

    // 发送
    ssize_t sent = sendto(sockfd, message.c_str(), message.size(), 0,
                          reinterpret_cast<struct sockaddr*>(&addr),
                          sizeof(addr));
    if (sent < 0) {
        perror("sendto failed");
    } else if ((size_t)sent != message.size()) {
        fprintf(stderr, "udp_broadcast: partial send (%zd of %zu)\n",
                sent, message.size());
    }

    close(sockfd);
}

void DataManager::setRawEventOutput(EventOutput fun) { // decide if we output TR info.
    m_rawEventOutputFun = fun;
}

void DataManager::initialize()
{
    // TODO: create event buffer
    if(m_eventBuffer == nullptr) {
        m_eventBuffer = new char[SZ_EVTBUFFER]();
        m_ringBuffer = new char[M_RINGBUFSZ]();

        m_bakRingBuffer = new char[M_RINGBUFSZ]();
        m_tag = new char[20]();
        m_duNumbers = new char[20];

        // ****** Circular ringbuffer, bohao ****** //
        m_ringOffset = 0;
        m_timestampRing.resize(M_TIMESTAMPRINGSZ, 0);
        m_timestampIndex.reserve(M_TIMESTAMPRINGSZ);
    }

    if(m_duTimeStampSave == nullptr) {
        m_duTimeStampSave = new char[M_DUTIMESTAMPSZ]();
        m_bakDuTimeStampSave = new char[M_DUTIMESTAMPSZ]();
        m_timeBuffer = new char[M_TIMEBUFFERSZ](); // This array should keep same as m_duTimeStampSave.
    }

    m_rawEvent = new char[M_RAWEVENTBUF_SZ]();
}

void DataManager::stop(){
    lastAddEvent();
}

void DataManager::terminate()
{   
    // Focus: release all buffer.
    if(m_eventBuffer) {
        delete[] m_eventBuffer;
        delete m_server;
        delete m_msgDispatcher;
        delete[] m_ringBuffer;
        delete[] m_tag;
        delete[] m_duNumbers;
        delete[] m_bakRingBuffer;
        delete[] m_bakDuTimeStampSave;
        m_bakRingBuffer = nullptr;
        m_duNumbers = nullptr;
        m_tag = nullptr;
        m_ringBuffer = nullptr;
        m_msgDispatcher = nullptr;
        m_server = nullptr;
        m_eventBuffer = nullptr;
    }
    if(m_duTimeStampSave) {
        delete[] m_duTimeStampSave;
        delete[] m_timeBuffer;
        m_timeBuffer= nullptr;
        m_duTimeStampSave = nullptr;
    }

    delete[] m_rawEvent;
    m_rawEvent = nullptr;
}

void DataManager::addRawEvent(char *data, size_t sz)
{  
    uint64_t nanoTime = 0;
    ElecEvent ev((uint16_t*)data, sz/sizeof(uint16_t));
    nanoTime = ev.getTimeFullDataSz().totalSec; // use getTimeFullDataSz because we include index 0 data length here.
    // printf("nanotime is %lld\n", nanoTime);
	
    RawEvent msg(m_eventBuffer, SZ_EVTBUFFER, true);
    msg.copyFrom(data, sz);
    if(m_rawEventOutputFun) {
        // std::cout << "msg.size() in addRawEvent is " << msg.size() << std::endl;
        m_rawEventOutputFun(msg.base(), msg.size());
    }

    if(m_frequenceCountBefore == 0) {
        m_t0 = XClock::nowNanoSeconds();
    }

    m_frequenceCountBefore++;
    m_t1 = XClock::nowNanoSeconds();
    if(m_t1 - m_t0 > timeCount) {
        printf("L1 trigger frequence is %d/s\n", m_frequenceCountBefore - m_frequenceCountAfter);
        m_frequenceCountAfter = m_frequenceCountBefore;
        m_t0 = m_t1;
    }
}

void DataManager::appendRingBuffer(const char* data, size_t sz, uint64_t timestamp)
{
    if(sz > M_RINGBUFSZ)
    {
        printf("Event too large for ring buffer\n");
        return;
    }

    // ****** When the buffer end is reached, data are overwritten from the beginning. 如果写到尾部，直接从头覆盖 ****** //
    // Bingo! Another easter egg, so you find me again!
    // Today's question: where we will go after we leave the world?
    // Bohao, Paris de France. 20260309
    if(m_ringOffset + sz > M_RINGBUFSZ)
    {
        m_ringOffset = 0;
    }

    // 计算 event index
    size_t eventIndex = m_ringOffset / m_rawDataSize;
    size_t maxEvents = M_RINGBUFSZ / m_rawDataSize;
    if(eventIndex >= maxEvents)
    {
        printf("Ring buffer index error\n");
        return;
    }

    uint64_t oldTimestamp = m_timestampRing[eventIndex];
    if(oldTimestamp != 0)
    {
        m_timestampIndex.erase(oldTimestamp);
    }

    // save raw data
    memcpy(m_ringBuffer + m_ringOffset, data, sz);

    // save timestamp
    m_timestampRing[eventIndex] = timestamp;

    // save hash index
    m_timestampIndex[timestamp] = eventIndex;

    // update offset
    m_ringOffset += sz;
    // **************************************************************************************************************** //
}

void DataManager::addEvent(char *data, size_t sz, int daqMode)
{
    if(m_frequenceCountBefore == 0)
        m_t0 = XClock::nowNanoSeconds();

    m_frequenceCountBefore++;
    m_t1 = XClock::nowNanoSeconds();

    if(daqMode == 3)
    {
        uint16_t triggerpattern =
        *(uint16_t*)(data + POS_TRIGGER_PATTERN*sizeof(uint16_t));

        if(triggerpattern == TEN_SEC_DATA)
        {
            RawEvent rawMsg(m_rawEvent, M_RAWEVENTBUF_SZ, true);
            rawMsg.copyFrom(data, sz);

            if(m_rawEventOutputFun)
                m_rawEventOutputFun(rawMsg.base(), rawMsg.size());

            return;
        }
    }

    m_xCorre = *(uint16_t*)(data + POS_X_CORRE * sizeof(uint16_t));
    m_yCorre = *(uint16_t*)(data + POS_Y_CORRE * sizeof(uint16_t));
    std::cout << "m_xCorre value: " << m_xCorre << std::endl;
    std::cout << "m_yCorre value: " << m_yCorre << std::endl;
    std::lock_guard<std::mutex> mylockguard(m_mutex_data);

    m_rawDataSize = sz;

    // -------- get timestamp --------

    ElecEvent ev(reinterpret_cast<uint16_t*>(data), sz/sizeof(uint16_t));

    uint64_t timestamp = ev.getTimeFullDataSz().totalSec;

    // ****** add time info into a unordered map ****** //
    m_timeMap[timestamp] = 1;
    currentSecond = timestamp/timeCut;
    // printf("time: %lld\n", timestamp);
    if (m_lastClearSecond == 0) {
        m_lastClearSecond = currentSecond;
    }

    // if(m_t1 - m_t0 > timeCount)
    // {
    //     int trigger_rate = m_frequenceCountBefore - m_frequenceCountAfter;
    //     m_frequenceCountAfter = m_frequenceCountBefore;
    //     m_t0 = m_t1;
    //     printf("L1 trigger rate: %d Hz\n", trigger_rate);

    //     // ****** UDP monitor module, bohao ****** //
    //     int duSnd = duID;
    //     m_t2 = m_t1 / timeCut;
    //     std::string heartbeatMsg =
    //     "numTmStamps| GPS time: " +
    //     std::to_string(currentSecond) + "| feb: " +
    //     std::to_string(duSnd) + "| trigger rate: " +
    //     std::to_string(trigger_rate);
    //     udp_broadcast(heartbeatMsg, BROADCASTIP, BROADCASTPORT);
    //     trigger_rate = 0;
    //     // *************************************** //
    // }

    uint64_t expireBefore = (currentSecond - TIME_TO_RENITIAL_QT_MAP)*timeCut;
    for (auto it = m_timeMap.begin(); it != m_timeMap.end(); ) {
        if (it->first < expireBefore) {
            it = m_timeMap.erase(it);   // 注意： erase 返回下一个 iterator Focus: after erase operation, iter will return the next one
        } else {
            ++it;
        }
    }
    // ************************************************ //

    // -------- circular ring buffer, bohao 20260307 -------- //
    appendRingBuffer(data, sz, timestamp);
    // ------------------------------------------------------ //

    // -------- save timestamp for T2, bohao -------- //
    size_t newTimestampPos = sizeof(int) + szof_m_duTimeStampSave;

    // 如果 buffer 快满，需要压缩未发送的数据
    if(newTimestampPos + szT2TraceInfo > M_DUTIMESTAMPSZ)
    {
        // 计算未发送的数据长度
        size_t unsent = szof_m_duTimeStampSave - szSended;

        if(unsent > 0)
        {
            // 把未发送的数据移动到 buffer 开头（sizeof(int) 后面）
            memmove(
                m_duTimeStampSave + sizeof(int),
                m_duTimeStampSave + sizeof(int) + szSended,
                unsent
            );
        }

        // 更新指针
        szof_m_duTimeStampSave = unsent;
        szSended = 0;

        // 更新写入位置
        newTimestampPos = sizeof(int) + szof_m_duTimeStampSave;
    }

    T2TraceInfo hit;
    hit.timestamp = timestamp;
    hit.xCorre = m_xCorre;
    hit.yCorre = m_yCorre;
    
    printf("time: %lld, xCorre: %d, yCorre: %d\n", timestamp, m_xCorre, m_yCorre);

    // -------- save timestamp for T2, bohao -------- //
    // *(uint64_t*)(m_duTimeStampSave + sizeof(int) + szof_m_duTimeStampSave) = timestamp;
    memcpy(m_duTimeStampSave + sizeof(int) + szof_m_duTimeStampSave, &hit, szT2TraceInfo);
    uint64_t t_acceptT2Msg = timestamp;
    m_timeStart = *(uint64_t*)(m_duTimeStampSave + sizeof(int) + szSended);
    m_timeStart = (m_timeStart / timeCut) * timeCut;
    if(t_acceptT2Msg - m_timeStart > timeCut)
    {
        if(szof_m_duTimeStampSave >= szSended)
        {
            memset(m_timeBuffer, 0, M_TIMEBUFFERSZ);
            T2Message t2msg(m_timeBuffer, M_TIMEBUFFERSZ, true);
            t2msg.addTime( m_duTimeStampSave + szSended, szof_m_duTimeStampSave + sizeof(int) - szSended);
            // m_numTmstamps = (szof_m_duTimeStampSave - szSended) / sizeof(uint64_t);
            szSended = szof_m_duTimeStampSave;

            if(m_t2EventOutputFun)
                m_t2EventOutputFun(t2msg.base(), t2msg.size());

            // ****** UDP monitor module, bohao ****** //
            int duSnd = duID;
            int trigger_rate = m_frequenceCountBefore - m_frequenceCountAfter;
            m_frequenceCountAfter = m_frequenceCountBefore;
            std::string heartbeatMsg =
            "numTmStamps| GPS time: " +
            std::to_string(t_acceptT2Msg) + "| feb: " +
            std::to_string(duSnd) + "| trigger rate: " +
            std::to_string(trigger_rate);
            udp_broadcast(heartbeatMsg, BROADCASTIP, BROADCASTPORT);
            // *************************************** //

            m_timeStart += timeCut;
        }
    }

    // szof_m_duTimeStampSave += sizeof(uint64_t);
    szof_m_duTimeStampSave += szT2TraceInfo;
}

void DataManager::lastAddEvent() {
    if(szof_m_duTimeStampSave >= szSended) {
        memset(m_timeBuffer, 0, M_TIMEBUFFERSZ);
        T2Message t2msg(m_timeBuffer, M_TIMEBUFFERSZ, true);
        assert(szSended < M_DUTIMESTAMPSZ);
        assert( szof_m_duTimeStampSave + sizeof(int) - szSended < M_DUTIMESTAMPSZ);
        t2msg.addTime(m_duTimeStampSave + szSended, szof_m_duTimeStampSave + sizeof(int)-szSended); // keep tmID same, because t_acceptT2Msg-m_timeStart>timeCut 
        szSended = szof_m_duTimeStampSave;
        if(m_t2EventOutputFun){
            m_t2EventOutputFun(t2msg.base(), t2msg.size());
        }
    }
    else {
        szSended = 0; // In this case, m_duTimeStampSave has been initialized;
    }
}

void DataManager::accept(char *data, size_t sz) // send a buffer which include eventIDs' information
{   
    // *********** Random Trigger *************//
    // acceptRandomTrigger(data, sz);

    // *********** T3 Trigger ************ //
    acceptT3Trigger(data, sz);
}

void DataManager::acceptT3Trigger(char* data, size_t sz)
{
    std::lock_guard<std::mutex> mylockguard(m_mutex_data);

    szT3Trigger = sz-2*sizeof(uint32_t);

    int q = szT3Trigger / sizeof(uint64_t);

    std::vector<uint64_t> t2(q);

    // ---------- 读取 T3 timestamps ---------- //
    for(int i = 0; i < q; i++)
    {
        t2[i] = *(uint64_t*)(data + i*sizeof(uint64_t));
    }

    // ---------- 读取 event tag ---------- //
    memset(m_tag, 0, 20);
    memcpy(m_tag, data + sz - 2*sizeof(uint32_t), sizeof(uint32_t));
    memset(m_duNumbers, 0, 20);
    memcpy(m_duNumbers, data + sz - sizeof(uint32_t), sizeof(uint32_t));

    printf("Evt Id: %d\n", *(uint32_t*)(m_tag));

    // ---------- hash search ----------
    std::unordered_set<uint64_t> seenTs;
    for(int j = 0; j < q; j++)
    {
        uint64_t ts = t2[j];
        if (seenTs.find(ts) != seenTs.end()) {
            continue;
        }
        seenTs.insert(ts);
        auto it = m_timestampIndex.find(ts);
        if(it != m_timestampIndex.end())
        {
            bool valid = false;
            size_t index = it->second;

            if(m_timestampRing[index] == ts) {

                printf("[HIT] ts=%llu index=%zu ring_ts=%llu\n",
                ts,
                index,
                (unsigned long long)m_timestampRing[index]);
                // ****** bohao, data has not been refreshed yet ****** //
                size_t offset = index * m_rawDataSize;
                if(offset + m_rawDataSize <= M_RINGBUFSZ)
                {
                    valid = true;

                    // ****** write data into send buffer ****** //
                    m_evtTraceCounter++;
                
                    DAQEvent t3msg(m_eventBuffer, SZ_EVTBUFFER, true);
                    t3msg.copyFrom(m_tag, sizeof(uint32_t));
                    t3msg.copyFrom(m_duNumbers, sizeof(uint32_t));
                    t3msg.copyFrom((char*)(&ts), sizeof(uint64_t));
                    t3msg.copyFrom(m_ringBuffer + offset, m_rawDataSize);

                    // printf("DEBUG: m_rawDataSize = %zu\n", m_rawDataSize);
                    // printf("DEBUG: base = %p\n", m_ringBuffer);
                    // printf("DEBUG: calc addr = %p\n", m_ringBuffer + evtID1*m_rawDataSize);
                    // printf("BUILD ts=%llu addr=%p\n", ts, t3msg.base());
                    
                    if(m_eventOutputFun)
                    {
                        m_eventOutputFun(t3msg.base(), t3msg.size());
                    }
                }
            }
            else{
                // ****** bohao, data has been refreshed before ****** //
                // printf("LOST, time: %lld\n", t2[j]);
                
                DAQEvent t3msg(m_eventBuffer, SZ_EVTBUFFER, true);
                // assert((evtID1*m_rawDataSize < szofRingBuffer, "evtID1*m_rawDataSize shoule less than szofRingBuffer"));
                t3msg.copyFrom(m_tag, sizeof(uint32_t));
                t3msg.copyFrom(m_duNumbers, sizeof(uint32_t));
                t3msg.copyFrom((char*)(&ts), sizeof(uint64_t));
                
                if(m_eventOutputFun) {
                    m_eventOutputFun(t3msg.base(), t3msg.size()); // call write function, which is packed by m_dataManager.
                }
                printf("LOST: t3msg sz: %d\n", t3msg.size());
                memset(m_eventBuffer, 0, SZ_EVTBUFFER);
            }
        }
    }
    
    m_evtCount++;

    // // ****** UDP monitor module ****** //
    // uint64_t t3SndBack = XClock::nowNanoSeconds() / TIMECUTMILLISECOND;

    // std::string T3EvtInfo =
    //     "duEvtInfo|" +
    //     std::to_string(t3SndBack) + "|" +
    //     std::to_string(duID) + "|" +
    //     std::to_string(*(uint32_t*)m_tag);

    // udp_broadcast(T3EvtInfo, BROADCASTIP, BROADCASTPORT);
    // // ******************************* //
}