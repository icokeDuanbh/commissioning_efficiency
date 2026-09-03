#pragma once

#include <std_types.h>
#include <frontend.h>
#include <stdint.h>
#include <utils.h>

namespace grand {

class ScopeDummy : public IFrontend {
public:
    ScopeDummy();
    ~ScopeDummy();
protected:
    virtual void elecInit() { CLOG(INFO, "elec") << "Dummy electronic init"; }
    virtual void elecConfig(void *param) { CLOG(INFO, "elec") << "Dummy electronic config"; }
    virtual void elecStartRun() {  CLOG(INFO, "elec") << "Dummy electronic start run"; }
    virtual void elecStopRun() {  CLOG(INFO, "elec") << "Dummy electronic stop run"; }
    virtual void elecTerminate() {  CLOG(INFO, "elec") << "Dummy electronic terminate "; }

    virtual int elecReadData(char *data, size_t maxSize, uint32_t* hitId) {    
        for(int i=0; i<128; i++) {
            data[i] = (unsigned char)(unsigned int)i;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CLOG(INFO, "elec") << "Dummy electronic get data, size = " << 128;
        return 128;
    }

    virtual void terminateElectronics() {}
};

}
