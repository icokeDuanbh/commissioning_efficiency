#include <params.h>
#include <ctime>
#include <iomanip>
#include <sstream>

// ==================== Param基类实现 ====================
Param::Param(const std::string& name, Type type) : name_(name), type_(type) {}

// ==================== BasicParam实现 ====================
BasicParam::BasicParam(const std::string& name, const std::string& value, std::string unit) 
    : Param(name, Type::BASIC), value_(value), unit_(unit) {}

BasicParam::BasicParam(const std::string& name, int value, std::string unit) 
    : Param(name, Type::BASIC), value_(value), unit_(unit) {}

BasicParam::BasicParam(const std::string& name, double value, std::string unit) 
    : Param(name, Type::BASIC), value_(value), unit_(unit) {}

BasicParam::BasicParam(const std::string& name, bool value, std::string unit) 
    : Param(name, Type::BASIC), value_(value), unit_(unit) {}

nlohmann::json BasicParam::toJson() const {
    nlohmann::json j;
    j["name"] = name_;
    j["type"] = "basic";
    j["value"] = value_;
    j["unit"] = unit_;
    return j;
}

// ==================== HistogramParam实现 ====================
HistogramParam::HistogramParam(const std::string& name, 
                               const std::vector<double>& bins,
                               const std::vector<int>& counts,
                               double min_val,
                               double max_val)
    : Param(name, Type::HISTOGRAM), bins_(bins), counts_(counts), 
      min_val_(min_val), max_val_(max_val) {}

nlohmann::json HistogramParam::toJson() const {
    nlohmann::json j;
    j["name"] = name_;
    j["type"] = "histogram";
    j["bins"] = bins_;
    j["counts"] = counts_;
    j["min_val"] = min_val_;
    j["max_val"] = max_val_;
    return j;
}

// ==================== HistoryParam实现 ====================
HistoryParam::HistoryParam(const std::string& name,
                           const std::vector<std::chrono::system_clock::time_point>& timestamps,
                           const std::vector<double>& values,
                           const std::string& unit)
    : Param(name, Type::HISTORY), timestamps_(timestamps), values_(values), unit_(unit) {}

nlohmann::json HistoryParam::toJson() const {
    nlohmann::json j;
    j["name"] = name_;
    j["type"] = "history";
    j["unit"] = unit_;
    
    // 转换时间戳为字符串
    std::vector<std::string> time_strings;
    for (const auto& ts : timestamps_) {
        auto time_t = std::chrono::system_clock::to_time_t(ts);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
        time_strings.push_back(ss.str());
    }
    j["timestamps"] = time_strings;
    j["values"] = values_;
    
    return j;
}

// ==================== WaveParam实现 ====================
WaveParam::WaveParam(const std::string& name,
                     const std::vector<double>& amplitudes,
                     double sampling_rate,
                     const std::string& unit)
    : Param(name, Type::WAVE), amplitudes_(amplitudes), 
      sampling_rate_(sampling_rate), unit_(unit) {}

nlohmann::json WaveParam::toJson() const {
    nlohmann::json j;
    j["name"] = name_;
    j["type"] = "wave";
    j["amplitudes"] = amplitudes_;
    j["sampling_rate"] = sampling_rate_;
    j["unit"] = unit_;
    return j;
}

// ==================== ParamManager实现 ====================
ParamManager& ParamManager::getInstance() {
    static ParamManager instance;
    return instance;
}

void ParamManager::addParam(std::shared_ptr<Param> param) {
    if (param) {
        params_[param->getName()] = param;
    }
}

std::shared_ptr<Param> ParamManager::getParam(const std::string& name) {
    auto it = params_.find(name);
    return (it != params_.end()) ? it->second : nullptr;
}

std::map<std::string, std::shared_ptr<Param>> ParamManager::getAllParams() {
    return params_;
}

void ParamManager::removeParam(const std::string& name) {
    auto it = params_.find(name);
    if (it != params_.end()) {
        params_.erase(it);
    }
}

void ParamManager::clearParams() {
    params_.clear();
}
