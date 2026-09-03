#include <http_api.h>
#include <params.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <random>
#include <cmath>

// 生成示例直方图数据
std::vector<double> generateHistogramBins(double min_val, double max_val, int num_bins) {
    std::vector<double> bins;
    double step = (max_val - min_val) / num_bins;
    for (int i = 0; i <= num_bins; ++i) {
        bins.push_back(min_val + i * step);
    }
    return bins;
}

std::vector<int> generateHistogramCounts(int num_bins, double mean, double stddev) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::normal_distribution<> dist(mean, stddev);
    
    std::vector<int> counts(num_bins, 0);
    for (int i = 0; i < 1000; ++i) {
        double value = dist(gen);
        int bin = static_cast<int>((value + 3 * stddev) / (6 * stddev / num_bins));
        if (bin >= 0 && bin < num_bins) {
            counts[bin]++;
        }
    }
    return counts;
}

// 生成示例历史数据
std::vector<std::chrono::system_clock::time_point> generateTimestamps(int count) {
    std::vector<std::chrono::system_clock::time_point> timestamps;
    auto now = std::chrono::system_clock::now();
    
    for (int i = count - 1; i >= 0; --i) {
        timestamps.push_back(now - std::chrono::minutes(i * 5));
    }
    return timestamps;
}

std::vector<double> generateHistoryValues(int count, double base_value, double amplitude) {
    std::vector<double> values;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::normal_distribution<> noise(0.0, 0.1);
    
    for (int i = 0; i < count; ++i) {
        double trend = base_value + amplitude * std::sin(i * 0.1);
        values.push_back(trend + noise(gen));
    }
    return values;
}

std::vector<double> generateWaveform(double duration, double sampling_rate, double frequency) {
    std::vector<double> amplitudes;
    int num_points = static_cast<int>(duration * sampling_rate);
    
    for (int i = 0; i < num_points; ++i) {
        double t = i / sampling_rate;
        double amplitude = std::sin(2 * M_PI * frequency * t) * 
                          std::exp(-t / 2.0) + 
                          0.1 * std::sin(2 * M_PI * frequency * 3 * t);
        amplitudes.push_back(amplitude);
    }
    return amplitudes;
}

int main() {
    std::cout << "启动HTTP API示例..." << std::endl;
    
    // 创建HTTP API实例
    HttpApi api;
    
    // 初始化并启动服务器
    if (!api.initialize(8080)) {
        std::cerr << "HTTP API初始化失败" << std::endl;
        return 1;
    }
    
    if (!api.start()) {
        std::cerr << "HTTP API启动失败" << std::endl;
        return 1;
    }
    
    std::cout << "HTTP API服务器已启动在端口 8080" << std::endl;
    std::cout << "访问 http://localhost:8080/api/params 查看所有参数" << std::endl;
    std::cout << "访问 http://localhost:8080/api/health 检查健康状态" << std::endl;
    
    // 获取参数管理器实例
    auto& param_manager = ParamManager::getInstance();
    
    // 添加基本类型参数
    param_manager.addParam(std::make_shared<BasicParam>("system_status", "运行中"));
    param_manager.addParam(std::make_shared<BasicParam>("temperature", 25.6));
    param_manager.addParam(std::make_shared<BasicParam>("pressure", 1013.25));
    param_manager.addParam(std::make_shared<BasicParam>("is_connected", true));
    param_manager.addParam(std::make_shared<BasicParam>("error_count", 0));
    
    // 添加直方图参数
    auto histogram_bins = generateHistogramBins(0.0, 10.0, 20);
    auto histogram_counts = generateHistogramCounts(20, 5.0, 1.5);
    param_manager.addParam(std::make_shared<HistogramParam>("energy_spectrum", 
                                                           histogram_bins, 
                                                           histogram_counts, 
                                                           0.0, 10.0));
    
    // 添加历史曲线参数
    auto timestamps = generateTimestamps(50);
    auto history_values = generateHistoryValues(50, 100.0, 10.0);
    param_manager.addParam(std::make_shared<HistoryParam>("cpu_usage", 
                                                         timestamps, 
                                                         history_values, 
                                                         "%"));
    
    // 添加波形参数
    auto waveform = generateWaveform(0.1, 10000.0, 1000.0); // 1kHz信号
    param_manager.addParam(std::make_shared<WaveParam>("analog_signal", 
                                                      waveform, 
                                                      10000.0, 
                                                      "V"));
    
    std::cout << "已添加示例参数，服务器正在运行..." << std::endl;
    std::cout << "按 Ctrl+C 停止服务器" << std::endl;
    
    // 模拟实时数据更新
    int update_counter = 0;
    while (api.isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        // 更新温度参数
        double new_temp = 25.0 + 5.0 * std::sin(update_counter * 0.1);
        param_manager.addParam(std::make_shared<BasicParam>("temperature", new_temp));
        
        // 更新CPU使用率历史
        auto new_timestamps = generateTimestamps(50);
        auto new_cpu_values = generateHistoryValues(50, 80.0 + 10.0 * std::sin(update_counter * 0.2), 5.0);
        param_manager.addParam(std::make_shared<HistoryParam>("cpu_usage", 
                                                             new_timestamps, 
                                                             new_cpu_values, 
                                                             "%"));
        
        update_counter++;
        std::cout << "已更新参数 (第 " << update_counter << " 次)" << std::endl;
    }
    
    return 0;
} 