#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <chrono>
#include <nlohmann/json.hpp>

// 参数基类：统一序列化成 JSON，供 HTTP/监控侧拉取
class Param {
public:
    enum class Type {
        BASIC,      // 标量（字符串/数/布尔）
        HISTOGRAM,  // 直方图 bins + counts
        HISTORY,    // 时间序列
        WAVE        // 波形样点序列
    };

    Param(const std::string& name, Type type);
    virtual ~Param() = default;

    const std::string& getName() const { return name_; }
    Type getType() const { return type_; }
    // 子类把自身字段填入统一 JSON 形状（含 name/type 子字段）
    virtual nlohmann::json toJson() const = 0;

protected:
    std::string name_; // 参数名，作 map 键
    Type type_;        // 分类，与 toJson 里 type 字符串对应
};

// 基本类型参数：value 存为 json 变体，unit 可选
class BasicParam : public Param {
public:
    BasicParam(const std::string& name, const std::string& value, std::string unit = "");
    BasicParam(const std::string& name, int value, std::string unit = "");
    BasicParam(const std::string& name, double value, std::string unit = "");
    BasicParam(const std::string& name, bool value, std::string unit = "");

    nlohmann::json toJson() const override;

private:
    nlohmann::json value_; // 实际取值
    std::string unit_;       // 展示用单位，可空
};

// 直方图：bins 与 counts 等长；min/max 为展示轴范围
class HistogramParam : public Param {
public:
    HistogramParam(const std::string& name, 
                   const std::vector<double>& bins,
                   const std::vector<int>& counts,
                   double min_val = 0.0,
                   double max_val = 100.0);

    nlohmann::json toJson() const override;

private:
    std::vector<double> bins_;   // 分箱边界或中心，与实现约定一致
    std::vector<int> counts_;    // 每箱计数
    double min_val_;
    double max_val_;
};

// 历史曲线：timestamps 与 values 等长
class HistoryParam : public Param {
public:
    HistoryParam(const std::string& name,
                 const std::vector<std::chrono::system_clock::time_point>& timestamps,
                 const std::vector<double>& values,
                 const std::string& unit = "");

    nlohmann::json toJson() const override;

private:
    std::vector<std::chrono::system_clock::time_point> timestamps_;
    std::vector<double> values_;
    std::string unit_; // Y 轴物理单位
};

// 波形：amplitudes 为样点序列；sampling_rate 单位 Hz（与 toJson 一致）
class WaveParam : public Param {
public:
    WaveParam(const std::string& name,
              const std::vector<double>& amplitudes,
              double sampling_rate = 1000.0,
              const std::string& unit = "V");

    nlohmann::json toJson() const override;

private:
    std::vector<double> amplitudes_;
    double sampling_rate_;
    std::string unit_;
};

// 全局单例：按 name 索引可查询参数，供监控/HTTP 聚合
class ParamManager {
public:
    static ParamManager& getInstance();

    void addParam(std::shared_ptr<Param> param); // 同名覆盖

    // 未找到返回 nullptr
    std::shared_ptr<Param> getParam(const std::string& name);

    std::map<std::string, std::shared_ptr<Param>> getAllParams();

    void removeParam(const std::string& name);

    void clearParams();

private:
    ParamManager() = default;
    ~ParamManager() = default;
    ParamManager(const ParamManager&) = delete;
    ParamManager& operator=(const ParamManager&) = delete;

    std::map<std::string, std::shared_ptr<Param>> params_;
};
