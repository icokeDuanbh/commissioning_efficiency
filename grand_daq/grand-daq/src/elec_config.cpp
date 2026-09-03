#include "elec_config.h"
#include <string>
#include <fstream>
#include <math.h>
#include <vector>
#include <algorithm>
#include <functional>
#include <cstring>
#include <cstdio>
#include <limits>

#define Reg_End 0x2FC

namespace {

inline uint32_t truncToU32(double x) {
    if (x <= 0) return 0;
    const double m = static_cast<double>(std::numeric_limits<uint32_t>::max());
    if (x >= m) return std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(x);
}

} // namespace

using namespace std;
using namespace grand;

ElecConfig* ElecConfig::instance()
{
    static ElecConfig self;
    return &self;
}

ElecConfig::ElecConfig() {
}

ElecConfigAddress::addr_t ElecConfigAddress::global(string group, string name)
{
    return ElecConfigAddress::addr_t();
}

ElecConfigAddress::addr_t ElecConfigAddress::channel(string channelName, string name)
{
    return ElecConfigAddress::addr_t();
}

uint32_t ElecConfig::fundefault(uint32_t value){return value;}
void ElecConfig::fundefaultArray(double* value, size_t size, uint16_t*, size_t&) {
    std::cout << 0 << std::endl;
}

uint32_t ElecConfig::funInternalTriggerRate(uint32_t value)
{
    if(value == 0){
	return 15;
    }
    return (value<<8)|((1<<7)+15);
}

uint32_t ElecConfig::funTriggerOverlap(uint32_t value) {return value/2;}
uint32_t ElecConfig::funBattery(uint32_t value) {
    int volt = int(value)*(2.5*(18+91))/(18*4096);
    volt = int(10*(volt+0.05))/10;
    return volt;
}
uint32_t ElecConfig::funTriggerBlock(uint32_t value) {
    if (value>16384 || value<0){
        return 10880;
    }else{
        return value;
    }
}
uint32_t ElecConfig::funInput_Off(uint32_t value) {return !value;}
uint32_t ElecConfig::funInput_ADC(uint32_t value) {
    assert(value >= 1 && value <= 8 && "value should be between 1 and 8");
    return value-1;
}
uint32_t ElecConfig::funPreorPostTri(uint32_t value){return value/2;}
uint32_t ElecConfig::funQuiettime(uint32_t value){return value/2;}
uint32_t ElecConfig::funtimeAfter(uint32_t value){return value/10;}
uint32_t ElecConfig::funMaxTime(uint32_t value){return value/2;}
double ElecConfig::funAdditionaGain(double value) {
    assert(value >= -14.0 && value <= 23.5 && "AdditionaGain must be in [-14, 23.5]");
    uint32_t GainDB2=uint32_t(int((4096*(value+14)/(37.5*2.5))+0.5));
    return GainDB2;
}

int ElecConfig::float2fixed(float x, int len_int, int len_frac){
    int f;
    if (len_int + len_frac > 32) {
        printf("ERROR: too many bits!  Must fit in 32b int!\n");
        return 0;
    }
    f = (int)(x*(1 << len_frac) + (x < 0 ? -1.0 : 1.0)*0.5);
    f &= 0xffffffff >> (32-len_int-len_frac);

    return f;
}

void ElecConfig::funIIR(double *value, size_t sz, uint16_t* values, size_t& length)
{
    int a[10],b[10];
    double a_dbl[10], b_dbl[10];
    int SAMP_FREQ = 500;
    double nu_s;
    int W_INT = 20;
    int W_FRAC = 12;
    double freq, width;
    freq = value[0];
    width = value[1];
    nu_s = freq/SAMP_FREQ;
    a_dbl[1] = 2 * width * width * width * width * cos(4*2*3.1416*nu_s);
    a_dbl[2] = -width*width*width*width*width*width*width*width;
    b_dbl[0] = 1;
    b_dbl[1] = -2 * cos(2*3.1416*nu_s);
    b_dbl[2] = 1;
    b_dbl[3] = 2 * width * cos(2*3.1416*nu_s);
    b_dbl[4] = width*width;
    b_dbl[5] = 2 * width * width * cos(2*2*3.1416*nu_s);
    b_dbl[6] = width*width*width*width;

    a[1] = float2fixed(a_dbl[1], W_INT, W_FRAC);
    a[2] = float2fixed(a_dbl[2], W_INT, W_FRAC);

    for (int i = 0; i < 7; i++)
        b[i] = float2fixed(b_dbl[i], W_INT, W_FRAC);

    int m = 1;
    int n = 1;
    int j = 0;
    for(int i=0; i<8; i++){
        if(i < 2){
            values[j] = a[m]&0xffff;
            m++;
        }
        else{
            values[j] = b[n]&0xffff;
            n++;
        }
        j++;
    }
}

// 载入地址表与可读配置，注册大量「路径字符串 → 量化 lambda」供 toShadowlist 遍历
void ElecConfig::load(std::string addressFile, std::string dataFile)
{
    m_config = YAML::LoadFile(addressFile);
    n_config = YAML::LoadFile(dataFile);
    m_transformFunction["default"] = [this](double x) {
        return static_cast<double>(fundefault(truncToU32(x)));
    };
    m_transformFunction["Global|Internal Trigger Rate|(1000000,124)Hz"] = [this](double x) {
        return static_cast<double>(funInternalTriggerRate(truncToU32(x)));
    };
    m_transformFunction["Global|TriggerOverlap|Time(ns)"] = [this](double x) {
        return static_cast<double>(funTriggerOverlap(truncToU32(x)));
    };
    m_transformFunction["Global|Battery Voltages(off,on)V|BatLow"] = [this](double x) {
        return static_cast<double>(funBattery(truncToU32(x)));
    };
    m_transformFunction["Global|Battery Voltages(off,on)V|BatHigh"] = [this](double x) {
        return static_cast<double>(funBattery(truncToU32(x)));
    };
    m_transformFunction["Global|TriggerBlock|FIFO count"] = [this](double x) {
        return static_cast<double>(funTriggerBlock(truncToU32(x)));
    };
    m_transformFunction["Input|Ch1|Off"] = [this](double x) {
        return static_cast<double>(funInput_Off(truncToU32(x)));
    };
    m_transformFunction["Input|Ch1|ADC"] = [this](double x) {
        return static_cast<double>(funInput_ADC(truncToU32(x)));
    };
    m_transformFunction["Input|Ch2|Off"] = [this](double x) {
        return static_cast<double>(funInput_Off(truncToU32(x)));
    };
    m_transformFunction["Input|Ch2|ADC"] = [this](double x) {
        return static_cast<double>(funInput_ADC(truncToU32(x)));
    };
    m_transformFunction["Input|Ch3|Off"] = [this](double x) {
        return static_cast<double>(funInput_Off(truncToU32(x)));
    };
    m_transformFunction["Input|Ch3|ADC"] = [this](double x) {
        return static_cast<double>(funInput_ADC(truncToU32(x)));
    };
    m_transformFunction["Input|Ch4|Off"] = [this](double x) {
        return static_cast<double>(funInput_Off(truncToU32(x)));
    };
    m_transformFunction["Input|Ch4|ADC"] = [this](double x) {
        return static_cast<double>(funInput_ADC(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 1|Pre Trigger"] = [this](double x) {
        return static_cast<double>(funPreorPostTri(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 1|Post Trigger"] = [this](double x) {
        return static_cast<double>(funPreorPostTri(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 1|Quiet Time before Sig treshold(ns)"] = [this](double x) {
        return static_cast<double>(funQuiettime(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 1|Time after Sig threshold(ns)"] = [this](double x) {
        return static_cast<double>(funtimeAfter(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 1|Max Time between threshold crossings(ns)"] = [this](double x) {
        return static_cast<double>(funMaxTime(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 1|Additional Gain[-14,23.5](dB)"] = [this](double x) {
        return funAdditionaGain(x);
    };
    m_transformFunction["Channels|Channel 2|Pre Trigger"] = [this](double x) {
        return static_cast<double>(funPreorPostTri(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 2|Post Trigger"] = [this](double x) {
        return static_cast<double>(funPreorPostTri(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 2|Quiet Time before Sig treshold(ns)"] = [this](double x) {
        return static_cast<double>(funQuiettime(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 2|Time after Sig threshold(ns)"] = [this](double x) {
        return static_cast<double>(funtimeAfter(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 2|Max Time between threshold crossings(ns)"] = [this](double x) {
        return static_cast<double>(funMaxTime(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 2|Additional Gain[-14,23.5](dB)"] = [this](double x) {
        return funAdditionaGain(x);
    };
    m_transformFunction["Channels|Channel 3|Pre Trigger"] = [this](double x) {
        return static_cast<double>(funPreorPostTri(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 3|Post Trigger"] = [this](double x) {
        return static_cast<double>(funPreorPostTri(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 3|Quiet Time before Sig treshold(ns)"] = [this](double x) {
        return static_cast<double>(funQuiettime(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 3|Time after Sig threshold(ns)"] = [this](double x) {
        return static_cast<double>(funtimeAfter(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 3|Max Time between threshold crossings(ns)"] = [this](double x) {
        return static_cast<double>(funMaxTime(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 3|Additional Gain[-14,23.5](dB)"] = [this](double x) {
        return funAdditionaGain(x);
    };
    m_transformFunction["Channels|Channel 4|Pre Trigger"] = [this](double x) {
        return static_cast<double>(funPreorPostTri(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 4|Post Trigger"] = [this](double x) {
        return static_cast<double>(funPreorPostTri(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 4|Quiet Time before Sig treshold(ns)"] = [this](double x) {
        return static_cast<double>(funQuiettime(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 4|Time after Sig threshold(ns)"] = [this](double x) {
        return static_cast<double>(funtimeAfter(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 4|Max Time between threshold crossings(ns)"] = [this](double x) {
        return static_cast<double>(funMaxTime(truncToU32(x)));
    };
    m_transformFunction["Channels|Channel 4|Additional Gain[-14,23.5](dB)"] = [this](double x) {
        return funAdditionaGain(x);
    };

    m_transformFunctionArray["default"] = std::bind(&ElecConfig::fundefaultArray, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 1|Filter1"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 1|Filter2"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 1|Filter3"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 1|Filter4"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 1|Filter5"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 1|Filter6"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 1|Filter7"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 1|Filter8"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 2|Filter1"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 2|Filter2"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 2|Filter3"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 2|Filter4"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 2|Filter5"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 2|Filter6"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 2|Filter7"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 2|Filter8"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 3|Filter1"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 3|Filter2"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 3|Filter3"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 3|Filter4"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 3|Filter5"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 3|Filter6"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 3|Filter7"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 3|Filter8"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 4|Filter1"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 4|Filter2"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 4|Filter3"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 4|Filter4"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 4|Filter5"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 4|Filter6"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 4|Filter7"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    m_transformFunctionArray["Channels|Channel 4|Filter8"] = std::bind(&ElecConfig::funIIR, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);

    m_transformFunction_2["default"] = std::bind(&ElecConfig::fundefault, this, std::placeholders::_1);
}

void ElecConfig::setBit(uint8_t *sl, uint32_t baseAddr, uint32_t startBit, uint32_t value)
{
    if(value == 0) {
        sl[baseAddr] &= (~(0x01<<startBit));
    }
    else {
        sl[baseAddr] |= (0x01<<startBit);
    }
}

void ElecConfig::setBits(uint8_t *sl, uint32_t baseAddr, uint32_t startBit, uint32_t nBits, uint32_t value)
{
    uint32_t value1= value;
    for(int i=0; i<nBits; i++) {
        int iByte = (i+startBit)/8;
        int iBit = (i+startBit)%8;
        int newBaseAddr = baseAddr + iByte;
        int newValue = (value1>>i)&0x01;
        setBit(sl, newBaseAddr, iBit, newValue);
    }
}

void ElecConfig::setBits(uint8_t *sl, uint32_t baseAddr, uint32_t startBit, uint32_t nBits, uint16_t *value)
{
    int nloop = nBits/16;
    uint32_t value1;
    for(int i=0; i<nloop; i++){
        value1 = value[i];
        startBit = i*16;
        setBits(sl, baseAddr, startBit, 16, value1);
    }
}

void ElecConfig::setBits(uint8_t *sl, uint32_t baseAddr, uint32_t startBit, uint32_t nBits, double *value)
{
    int nloop = nBits/16;
    uint32_t value1;
    for(int i=0; i<nloop; i++){
        value1 = value[i];
        startBit = i*16;
        setBits(sl, baseAddr, startBit, 16, value1);
    }
}

std::function<double(double)> ElecConfig::transformFunction(string first, string second, string third)
{
    string key = first + "|" + second + "|" + third;
    if(m_transformFunction.count(key) > 0) {
        return m_transformFunction[first + "|" + second + "|" + third];
    }
    else {
        return m_transformFunction["default"];
    }
};

std::function<double(double)> ElecConfig::transformFunction2(string first, string second, string third)
{
    string key = first + "|" + second + "|" + third;
    if(m_transformFunction.count(key) > 0) {
        return m_transformFunction[first + "|" + second + "|" + third];
    }
    else {
        return m_transformFunction["default"];
    }
};

std::function<void(double*, size_t, uint16_t*, size_t&)> ElecConfig::transformFunctionArray(string first, string second, string third)
{
    string key = first + "|" + second + "|" + third;
    if(m_transformFunctionArray.count(key) > 0) {
        return m_transformFunctionArray[first + "|" + second + "|" + third];
    }
    else {
        return m_transformFunctionArray["default"];
    }
};

std::function<uint32_t(uint32_t)> ElecConfig::transformFunction_2(string first)
{
    string key = first;
    if(m_transformFunction_2.count(key) > 0) {
        return m_transformFunction_2[first];
    }
    else {
        return m_transformFunction_2["default"];
    }
};

size_t ElecConfig::toShadowlist(uint8_t *sl)
{
    return toShadowlist(sl, std::string());
}

// DUid 非空时只展开 SpecialConfig 下该 DU 分支；否则仅 CommonConfig。返回 Reg_End 作写入上界参考
size_t ElecConfig::toShadowlist(uint8_t *sl, std::string DUid)
{
    ElecConfigAddress::addr_t addr;
    int SHADOW_SIZE = 256;
    memset(sl, 0, SHADOW_SIZE);
    YAML::Node::const_iterator node1;
    for(node1 = n_config.begin(); node1 != n_config.end(); node1++){
        std::string configMode = node1->first.as<string>();
            if((configMode == "CommonConfig")) {
            YAML::Node::const_iterator node2;
            for(node2 = n_config[configMode].begin(); node2 != n_config[configMode].end(); node2++) {
                string group = node2->first.as<string>();
                if((group == "ifopen")){
                    if(n_config[configMode][group].as<int>() == 1) {
                        COMMON = true;
                        continue;
                    }
                    else{
                        COMMON = false;
                    }

                }

                if(COMMON == true) {
                    if(n_config[configMode][group].Type() == YAML::NodeType::Scalar){
                        uint32_t value = n_config[configMode][group].as<int>();
                        uint32_t value1 = transformFunction_2(group)(value);
                        addr.baseAddr = m_config[group]["baseAddr"].as<int>();
                        addr.startBit = m_config[group]["startBit"].as<int>();
                        addr.nBits = m_config[group]["nBits"].as<int>();
                        setBits(sl, addr.baseAddr, addr.startBit, addr.nBits, value1);
                    }
                    YAML::Node::const_iterator node3;
                    for(node3 = n_config[configMode][group].begin(); node3 != n_config[configMode][group].end(); node3++){
                        string name = node3->first.as<string>();
                        YAML::Node::const_iterator node4;
                        for(node4 = n_config[configMode][group][name].begin(); node4 != n_config[configMode][group][name].end(); node4++) {
                            string name2 = node4->first.as<string>();
                            addr.baseAddr = m_config[group][name][name2]["baseAddr"].as<int>();
                            addr.startBit = m_config[group][name][name2]["startBit"].as<int>();
                            addr.nBits = m_config[group][name][name2]["nBits"].as<int>();
                            if(n_config[configMode][group][name][name2].Type() == YAML::NodeType::Sequence) {
                                YAML::Node::const_iterator node_array;
                                int i=0;
                                int seqSize = n_config[configMode][group][name][name2].size();
                                double array[seqSize];
                                for(node_array = n_config[configMode][group][name][name2].begin(); node_array != n_config[configMode][group][name][name2].end(); node_array++){
                                    array[i] = n_config[configMode][group][name][name2][i].as<double>();
                                    i++;
                                }
                                int size = i+1;
                                uint16_t filter[8];
                                size_t szRet = size;
                                transformFunctionArray(group, name, name2)( array, size, filter, szRet);
                                setBits(sl, addr.baseAddr, addr.startBit, addr.nBits, filter);
                                continue;
                            }
                            if(name2 == "(1000000,124)Hz") {
                                uint32_t value = node4->second.as<int>();
                                uint32_t value1 = truncToU32(transformFunction(group, name, name2)(static_cast<double>(value)));
                                setBits(sl, addr.baseAddr, addr.startBit, addr.nBits, value1);
                            }
                            else {
                                double value = node4->second.as<double>();
                                double value2 = transformFunction2(group, name, name2)(value);
                                setBits(sl, addr.baseAddr, addr.startBit, addr.nBits, value2);

                            }
                        }
                    }
                }
            }
        }

        if((configMode == "SpecialConfig")) {
            YAML::Node::const_iterator node_1;
            for(node_1 = n_config[configMode].begin(); node_1 != n_config[configMode].end(); node_1++) {
                string ifopen = node_1->first.as<string>();
                if((ifopen == "ifopen")){
                    if(n_config[configMode][ifopen].as<int>() == 1) {
                        SPECIAL = true;
                        continue;
                    }
                    else{
                        SPECIAL = false;
                    }
                }
                if(SPECIAL == true){
                    std::string duID = node_1->first.as<string>();
                    if(duID == DUid) {
                        YAML::Node::const_iterator node_2;
                        for(node_2 = n_config[configMode][duID].begin(); node_2 != n_config[configMode][duID].end(); node_2++) {
                            string group = node_2->first.as<string>();
                            if(n_config[configMode][duID][group].Type() == YAML::NodeType::Scalar){
                                uint32_t value = n_config[configMode][duID][group].as<int>();
                                uint32_t value1 = transformFunction_2(group)(value);
                                addr.baseAddr = m_config[group]["baseAddr"].as<int>();
                                addr.startBit = m_config[group]["startBit"].as<int>();
                                addr.nBits = m_config[group]["nBits"].as<int>();
                                setBits(sl, addr.baseAddr, addr.startBit, addr.nBits, value1);
                            }
                            YAML::Node::const_iterator node_3;
                            for(node_3 = n_config[configMode][duID][group].begin(); node_3 != n_config[configMode][duID][group].end(); node_3++){
                                string name = node_3->first.as<string>();
                                YAML::Node::const_iterator node_4;
                                for(node_4 = n_config[configMode][duID][group][name].begin(); node_4 != n_config[configMode][duID][group][name].end(); node_4++) {
                                    string name2 = node_4->first.as<string>();
                                    addr.baseAddr = m_config[group][name][name2]["baseAddr"].as<int>();
                                    addr.startBit = m_config[group][name][name2]["startBit"].as<int>();
                                    addr.nBits = m_config[group][name][name2]["nBits"].as<int>();
                                    if(n_config[configMode][duID][group][name][name2].Type() == YAML::NodeType::Sequence) {
                                        YAML::Node::const_iterator node_array;
                                        int i=0;
                                        int seqSize = n_config[configMode][duID][group][name][name2].size();
                                        double array[seqSize];
                                        for(node_array = n_config[configMode][duID][group][name][name2].begin(); node_array != n_config[configMode][duID][group][name][name2].end(); node_array++){
                                            array[i] = n_config[configMode][duID][group][name][name2][i].as<double>();
                                            i++;
                                        }
                                        int size = i+1;
                                        uint16_t filter[8];
                                        size_t szRet = size;
                                        transformFunctionArray(group, name, name2)( array, size, filter, szRet);
                                        setBits(sl, addr.baseAddr, addr.startBit, addr.nBits, filter);
                                        continue;
                                    }
                                    if(name2 == "(1000000,124)Hz") {
                                        uint32_t value = node_4->second.as<int>();
                                        uint32_t value1 = truncToU32(transformFunction(group, name, name2)(static_cast<double>(value)));
                                        setBits(sl, addr.baseAddr, addr.startBit, addr.nBits, value1);
                                    }
                                    else {
                                        double value = node_4->second.as<double>();
                                        double value2 = transformFunction2(group, name, name2)(value);
                                        setBits(sl, addr.baseAddr, addr.startBit, addr.nBits, value2);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return Reg_End;
}
