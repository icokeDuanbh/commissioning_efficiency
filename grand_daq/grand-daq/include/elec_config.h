#pragma once

#include <string.h>
#include <functional>
#include <iostream>
#include "yaml-cpp/yaml.h"
#include "yaml-cpp/node/parse.h"
#include <fstream>
#include <map>
#include <assert.h>

 // #define Reg_End 0x1FC
#define Reg_End 0x2FC

using namespace std;

namespace grand {

// 从地址映射 YAML 查「逻辑名 → 位域」，当前 global/channel 桩实现返回空结构（待接表）
class ElecConfigAddress
{
public:
     typedef struct{
        uint32_t baseAddr ; // 影子内存中的字对齐基址
        uint32_t startBit ; // 位域起点
        uint32_t nBits ;    // 位宽
    }addr_t;

    addr_t global(string group, string name);
    addr_t channel(string channelName, string name);
};

// 旧版 elec 配置：读 DU-address-map + DU-readable-conf，把人类可读参数压成 shadowlist 字节流（供对照/脚本）
class ElecConfig
{
public:
    static ElecConfig* instance();
    // addressFile - 寄存器地址表；dataFile - 可读参数表；内部建变换函数字典
    void load(string addressFile, std::string dataFile);
    // sl - 影子内存缓冲区，长度至少覆盖 Reg_End；返回写入字节数或有效范围（见实现）
    size_t toShadowlist(uint8_t *sl);
    size_t toShadowlist(uint8_t *sl, std::string DUid);

private:
    /** 旧版 elec_config：数值分支混用 uint32 与 double ，统一用 double 承载绑定侧签名。 */
    map<string, std::function<double(double)>> m_transformFunction;
    map<string, std::function<void(double*, size_t, uint16_t*, size_t&)>> m_transformFunctionArray;
    map<string, std::function<uint32_t(uint32_t)>> m_transformFunction_2;
    YAML::Node m_config; // addressFile 解析结果
    YAML::Node n_config; // dataFile 解析结果
    ElecConfigAddress m_configAddress;

    bool COMMON, SPECIAL; // 配置分支：通用/特殊板型（见 load 内用法）

    ElecConfig();
    // YAML 路径三元组 → 单值量化 lambda（键格式如 "Global|..."）
    std::function<double(double)> transformFunction(string first, string second, string third);
    std::function<double(double)> transformFunction2(string first, string second, string third);
    std::function<void(double*, size_t, uint16_t*, size_t&)> transformFunctionArray(string first, string second, string third);
    std::function<uint32_t(uint32_t)> transformFunction_2(string first);
    // shadowlist 按 baseAddr 字节偏移写位
    void setBit(uint8_t *sl, uint32_t baseAddr, uint32_t startBit, uint32_t value);
    void setBits(uint8_t *sl, uint32_t baseAddr, uint32_t startBit, uint32_t nBits, uint32_t value);
    void setBits(uint8_t *sl, uint32_t baseAddr, uint32_t startBit, uint32_t nBits, uint16_t *value);
    void setBits(uint8_t *sl, uint32_t baseAddr, uint32_t startBit, uint32_t nBits, double *value);
    int float2fixed(float x, int len_int, int len_frac); // 定点数打包
    uint32_t fundefault(uint32_t value);
    void fundefaultArray(double *value, size_t sz, uint16_t*, size_t&);
    uint32_t funInternalTriggerRate(uint32_t value);
    uint32_t funTriggerOverlap(uint32_t value);
    uint32_t funTriggerBlock(uint32_t value);
    uint32_t funBattery(uint32_t value);
    uint32_t funInput_Off(uint32_t value);
    uint32_t funInput_ADC(uint32_t value);
    uint32_t funPreorPostTri(uint32_t value);
    uint32_t funQuiettime(uint32_t value);
    uint32_t funtimeAfter(uint32_t value);
    uint32_t funMaxTime(uint32_t value);
    double funAdditionaGain(double value);
    void funIIR(double *value, size_t sz, uint16_t* values, size_t& length); // IIR 系数展开到 16 位字序列
};

}
