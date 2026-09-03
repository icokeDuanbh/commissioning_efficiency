#pragma once

#include <app.h>

// 嵌入式 HTTP：参数查询、配置热重载等；实现细节藏在 Impl（pImpl）
class HttpApi {
public:
    HttpApi();
    ~HttpApi();

    // app - 可选，供 reload 等路由回调；port 绑定监听
    bool initialize(int port = 8080, App* app = nullptr);

    bool start();

    void wait(); // 阻塞至 stop 或进程信号

    void stop();

    /**
     * 委托 `App::reloadYamlConfig()`：仅当 App 处于 `initialized`（已 `initialize` 且未在 `running`）时有效；
     * 非该状态由 App 抛 `SoftwareException`。非 HTTP 路由入口，不保证静默成功。
     */
    void reloadYamlConfig();

    void registerParamsRoute();

    std::string getParamsJson() const;

    bool isRunning() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;

private:
    void signalHandler();
};
