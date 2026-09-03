#include <utils.h>
#include <config.h>
#include <app.h>
#include <http_api.h>
#include <iostream>
#include <string>

// 加载 sysconfig → App + HttpApi；阻塞在 http_api.wait() 直至信号停止
int main(int argc, char* argv[]) {
    std::cout << "SEASTAR DAQ FRONTEND STANDALONE v1.0" << std::endl;
    std::cout << "=========================" << std::endl;
    
    // 检查命令行参数（未传入时使用与可执行文件位置相关的默认 sysconfig，避免依赖 cwd）
    std::string config_file = resolvedDefaultSysconfigPath();
    if (argc > 1) {
        config_file = argv[1];
    }

    std::cout << "Using configuration file: " << config_file << std::endl;

    if (!ConfigManager::instance().loadLegacySysConfigYaml(config_file)) {
        std::cerr << "Failed to load config: " << config_file << std::endl;
        return 1;
    }

    // 创建应用程序
    App app(config_file);

    // 创建http api server
    HttpApi http_api;
    http_api.initialize(ConfigManager::instance().getSoftwareConfig().http_port, &app);

    if (!http_api.start()) {
        return 1;
    }
    http_api.wait();
    
    std::cout << "SEASTAR DAQ FRONTEND STANDALONE terminated" << std::endl;
    return 0;
} 
