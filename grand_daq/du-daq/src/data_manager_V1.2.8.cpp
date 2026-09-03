/****************/
// Created by duanbh,
// 20220910.
/****************/
#include <data_manager.h>
#include <cassert>
#include <message_impl.h>
#include <chrono>
#include <cmath>
#include <mutex>

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
        delete m_eventBuffer;
        delete m_server;
        delete m_msgDispatcher;
        delete m_ringBuffer;
        delete m_tag;
        delete m_duNumbers;
        delete m_bakRingBuffer;
        m_bakRingBuffer = nullptr;
        m_duNumbers = nullptr;
        m_tag = nullptr;
        m_ringBuffer = nullptr;
        m_msgDispatcher = nullptr;
        m_server = nullptr;
        m_eventBuffer = nullptr;
    }
    if(m_duTimeStampSave) {
        delete m_duTimeStampSave;
        delete m_timeBuffer;
        m_timeBuffer= nullptr;
        m_duTimeStampSave = nullptr;
    }

    delete m_rawEvent;
    m_rawEvent = nullptr;
}

void DataManager::addRawEvent(char *data, size_t sz)
{   
    uint64_t nanoTime = 0;
    uint16_t* evtbuf;
    evtbuf = new uint16_t[sz/sizeof(uint16_t)];
    memset(evtbuf, 0, sz);
    memcpy(evtbuf, data, sz);
    ElecEvent ev(evtbuf, sz/sizeof(uint16_t));
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

    delete evtbuf;
    evtbuf = nullptr;
}

void DataManager::addEvent(char *data, size_t sz, int daqMode) 
{   
    std::cout << "add event func" << std::endl;
    if(daqMode == 3) {
        // 36 means 34+2, order of trigger pattern
        uint16_t triggerpattern = *(uint16_t*)(data + POS_TRIGGER_PATTERN*sizeof(uint16_t));
        // std::cout << "trigger pattern: " << triggerpattern << std::endl;
        if(triggerpattern==TEN_SEC_DATA){
            RawEvent rawMsg(m_rawEvent, M_RAWEVENTBUF_SZ, true);
            rawMsg.copyFrom(data, sz);
            if(m_rawEventOutputFun) {
                m_rawEventOutputFun(rawMsg.base(), rawMsg.size());
            }
            return;
        }
    }

    std::lock_guard<std::mutex> mylockguard(m_mutex_data); // address threads confilicts

    m_rawDataSize = sz;
    uint64_t t0, t_acceptT2Msg;
    char timeTmp[EACH_DATA_SZ] = {0};
    char timeT0[EACH_DATA_SZ] = {0};
    char timeAcceptT2Msg[EACH_DATA_SZ] = {0};
    char duIdTmp[4] = {0};
    uint64_t m_time;


    // get nanosecond
    uint16_t* evtbuf;
    evtbuf = new uint16_t[sz/sizeof(uint16_t)];
    memset(evtbuf, 0, sz);
    memcpy(evtbuf, data, sz);
    ElecEvent ev(evtbuf, sz/sizeof(uint16_t));
    m_time = ev.getTimeFullDataSz().totalSec; // use getTimeFullDataSz because we include index 0 data length here.
    delete evtbuf;
    evtbuf = nullptr;

    sprintf(duIdTmp, "%d", duID);
    if (szofRingBuffer+sz < M_RINGBUFSZ) {
        memcpy(m_ringBuffer + szofRingBuffer, data, sz); // save raw DAQ data.
    } else {
        // ******** MODIFIED BY DUANBOHAO ******** //
        memset(m_bakRingBuffer, 0, M_BAKRINGBUFSZ);
        szofBakRingBuffer = szofRingBuffer - (szSended/EACH_DATA_SZ)*sz;
        memcpy(m_bakRingBuffer, m_ringBuffer + (szSended/EACH_DATA_SZ)*sz, szofBakRingBuffer);
        memset(m_ringBuffer, 0, M_RINGBUFSZ);
        memcpy(m_ringBuffer, m_bakRingBuffer, szofBakRingBuffer);
        szofRingBuffer = szofBakRingBuffer;
        memcpy(m_ringBuffer + szofRingBuffer, data, sz);
        szofBakRingBuffer = 0;

        szof_m_bakDuTimeStampSave = szof_m_duTimeStampSave - szSended;
        printf("szof_m_duTimeStampSave - szSended: %d\n", szof_m_duTimeStampSave - szSended);
        memset(m_bakDuTimeStampSave, 0, M_DUTIMESTAMPSZ);
        // memcpy(m_bakDuTimeStampSave, duIdTmp, sizeof(int));
        memcpy(m_bakDuTimeStampSave, m_duTimeStampSave + sizeof(int) + szSended, szof_m_bakDuTimeStampSave);
        memset(m_duTimeStampSave, 0, M_DUTIMESTAMPSZ);
        memcpy(m_duTimeStampSave, duIdTmp, sizeof(int)); 
        memcpy(m_duTimeStampSave + sizeof(int), m_bakDuTimeStampSave, szof_m_bakDuTimeStampSave);
        szof_m_duTimeStampSave = szof_m_bakDuTimeStampSave;
        printf("new szof_m_duTimeStampSave is %d\n", szof_m_duTimeStampSave);
        szSended = 0;
        szof_m_bakDuTimeStampSave = 0;
    }

    if(szof_m_duTimeStampSave == 0)
        memcpy(m_duTimeStampSave, duIdTmp, sizeof(int)); // ****** need to be improved
    
    int ret = snprintf(timeTmp, EACH_DATA_SZ, "%lld", m_time);
    memcpy(m_duTimeStampSave + sizeof(int) + szof_m_duTimeStampSave, timeTmp, EACH_DATA_SZ);
    assert(("sizeof(int) + szSended should less than M_DUTIMESTAMPSZ",sizeof(int) + szSended < M_DUTIMESTAMPSZ));
    memcpy(timeT0, m_duTimeStampSave+sizeof(int) + szSended, EACH_DATA_SZ); // get first time in m_duTimeStampSave
    assert(("sizeof(int) + szof_m_duTimeStampSave should less than M_DUTIMESTAMPSZ", sizeof(int) + szof_m_duTimeStampSave < M_DUTIMESTAMPSZ));
    memcpy(timeAcceptT2Msg, m_duTimeStampSave + sizeof(int) + szof_m_duTimeStampSave, EACH_DATA_SZ);
    t_acceptT2Msg = atoll(timeAcceptT2Msg);
    m_timeStart = (atoll(timeT0)/timeCut)*timeCut;
    printf("m_time is %lld\n", m_time);
    if ( (t_acceptT2Msg - m_timeStart > timeCut))
    {    
        // printf("m_timeStart is %lld\n", m_timeStart);
        // printf("t_acceptT2Msg-m_timeStart is %lld\n", t_acceptT2Msg-m_timeStart);
        if(t_acceptT2Msg - m_timeStart < 0) {
            m_timeStart += timeCut;
        }
        else {
            if(szof_m_duTimeStampSave >= szSended) {
                memset(m_timeBuffer, 0, M_TIMEBUFFERSZ);
                T2Message t2msg(m_timeBuffer, M_TIMEBUFFERSZ, true);
                assert(szSended < M_DUTIMESTAMPSZ);
                assert( szof_m_duTimeStampSave + sizeof(int) - szSended < M_DUTIMESTAMPSZ);
                t2msg.addTime(m_duTimeStampSave + szSended, szof_m_duTimeStampSave + sizeof(int)-szSended); // keep tmID same, because t_acceptT2Msg-m_timeStart>timeCut 
                szSended = szof_m_duTimeStampSave;
                if(m_t2EventOutputFun){
                    m_t2EventOutputFun(t2msg.base(), t2msg.size());
                    // printf("t2msg header type is %d\n", t2msg.type());
                    // printf("modified t2msg size is %d\n", t2msg.size());
                }
                m_timeStart += timeCut;
            }
            else {
                szSended = 0; // In this case, m_duTimeStampSave has been initialized;
            }
        }
    }
    szof_m_duTimeStampSave += EACH_DATA_SZ;
    szofRingBuffer += sz;
    std::cout << "add event end" << std::endl;
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

void DataManager::acceptT3Trigger(char* data, size_t sz) {
    std::cout << "acceptT3Trigger func" << std::endl;
    std::lock_guard<std::mutex> mylockguard(m_mutex_data);
    szT3Trigger = sz;
    int q = sz/EACH_DATA_SZ;
    int sign=0;
    int szUsed = 0;
    uint32_t TAG = 0;
    char tmp[EACH_DATA_SZ] = {0};
    char tmpTag[4] = {0};
    char tmpBuf[EACH_DATA_SZ] = {0};
    
    uint64_t t1 = 0;
    uint64_t t2[q];
    
    for(int i=0; i<q; i++) {
        memcpy(tmp, data+i*EACH_DATA_SZ, EACH_DATA_SZ);
        t2[i] = atoll(tmp);
    }
    // m_tag and m_duNumbers is added after all raw data, the position may change if we add another new data when we get trigger timestamp.
    memset(m_tag, 0, 20);
    memcpy(m_tag, data+szT3Trigger-2*sizeof(uint32_t), sizeof(uint32_t));
    memset(m_duNumbers, 0, 20);
    memcpy(m_duNumbers, data+szT3Trigger-sizeof(uint32_t), sizeof(uint32_t));
    printf("m_tag is %d\n", *(uint32_t*)(m_tag));

    // printf("trigger dus number is %d\n", atoi(m_duNumbers));
    for(int i=0; i<szof_m_duTimeStampSave/EACH_DATA_SZ; i++) {
        tmpBuf[EACH_DATA_SZ] = {0};
        std::cout << 111 << std::endl;
        if (m_duTimeStampSave == nullptr) return;
        memcpy(tmpBuf, m_duTimeStampSave+sizeof(int)+i*EACH_DATA_SZ, EACH_DATA_SZ); // bug here, fixed on 20250730
        std::cout << 112 << std::endl;
        for(int j=0; j<q; j++) {
            t1 = t2[j];
            assert((j<q, "j should less than q"));
            if(t1 == atoll(tmpBuf)) {
                m_mutex.lock();
                evtID1 = 0;
                evtID1 = i; 
                std::cout << 113 << std::endl;
                DAQEvent t3msg(m_eventBuffer, SZ_EVTBUFFER, true);
                assert((evtID1*m_rawDataSize < szofRingBuffer,"evtID1*m_rawDataSize shoule less than szofRingBuffer"));
                t3msg.copyFrom(m_tag, sizeof(uint32_t));
                std::cout << 1131 << std::endl;
                t3msg.copyFrom(m_duNumbers, sizeof(uint32_t));
                std::cout << 1132 << std::endl;
                t3msg.copyFrom(m_ringBuffer+evtID1*m_rawDataSize, m_rawDataSize);
                std::cout << 114 << std::endl;
                // char test1[4]; // ok here
                // memcpy(test1, m_eventBuffer + sizeof(MessageHeader), 4);
                // printf("m_tag is %d\n", atol(test1));
                
                if(m_eventOutputFun) {
                    m_eventOutputFun(t3msg.base(), t3msg.size()); // call write function, which is packed by m_dataManager.
                }
                printf("t3msg sz is %d\n", t3msg.size());
                memset(m_eventBuffer,0,SZ_EVTBUFFER);
                m_mutex.unlock();
                break;
            }
            else 
                continue;
        }
    }

    printf("AcceptT3Trigger func end\n");
}

