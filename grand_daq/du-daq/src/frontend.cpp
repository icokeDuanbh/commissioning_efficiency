/**
 ** Created by duanbh,
 ** 2022/4/25
 **/
#include <iostream>
#include "frontend.h"
#include "du_sys_config.h"
#include <chrono>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <cstring>

using namespace grand;

class XClock {
    public:
        inline static uint64_t nowMilliSeconds() {
            std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds> tp = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
            return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
        }

        inline static uint64_t nowNanoSeconds() {
            std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds> tp = std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
            return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
        }
};


IFrontend::IFrontend() : m_callback(nullptr) {
}

IFrontend::~IFrontend() {
}

void IFrontend::inputThread() {
    // std::cout << "HERE IS INPUTTHREAD IN IFRONTEND FUNC " << std::endl;
    size_t bufSize = DUSysConfig::instance()->readoutBufferSize;
    char *buf = new char[bufSize];
    hitId = 0;
    int n = 0;
    m_time0 = 0;
    m_time1 = 0;
    m_count = 0;
    std::cout << "inputThread func" << std::endl;
    while(true) {
        
        usleep(100); // walk around, should be fixed
        if(m_stop) {
            break;
        }
        int retSz = elecReadData(buf, bufSize, &hitId);
	    if(retSz > 0) { // Here we think there is data from electronics.
            m_time1 = XClock::nowNanoSeconds();
            // if(m_count == 0) {
            // 	printf("hitId is %d, ", hitId);
            // }
            // else if(m_count>0) {
            // 	printf("hitId is %d, timediff is %lld\n, ", hitId, m_time1 - m_time0);
            // }
            m_time0 = m_time1;
            m_count++;

            if(m_callback) {
                m_callback(buf, retSz);
            }
        }
    }
    delete buf;
    buf = nullptr;
    std::cout << "inputThread func end" << std::endl;
}

void IFrontend::setCallback(ReadoutCallback cb) {
    m_callback = cb;
}

void IFrontend::initialize() {
    elecInit();
}

void IFrontend::configure(void *parameters) {
    elecConfig(parameters);
}

void IFrontend::configureOne(void *parameters) {
    // std::cout << "here is IFRONTEND::configureOne" << std::endl;
    // elecConfig(parameters);
}

void IFrontend::start(){
    elecStopRun();
    elecStartRun();

    m_stop = false;
    std::cout << "THIS IFRONTEND START" << std::endl;
    m_thread = std::thread(&IFrontend::inputThread,this);
}

void IFrontend::stop(){
    m_stop = true;
    m_thread.join();

    elecStopRun();
}

void IFrontend::terminate() {
    elecTerminate();
}

