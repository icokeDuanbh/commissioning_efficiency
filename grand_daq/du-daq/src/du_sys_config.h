#pragma once

#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>
#include "yaml-cpp/node/parse.h"

namespace grand {

class DUSysConfig {
public:
    static DUSysConfig *instance() {
        static DUSysConfig self;
        return &self;
    }

    uint32_t readoutBufferSize;
    uint32_t messageInputBufferSize;
    std::string backendBindUrl;
    uint32_t maxClientAddressSize;
    
private:

    DUSysConfig() {
        readoutBufferSize = 102400;
        messageInputBufferSize = 102400;
        backendBindUrl = "tcp://*:19555";
        maxClientAddressSize = 64;
    }
};

}
