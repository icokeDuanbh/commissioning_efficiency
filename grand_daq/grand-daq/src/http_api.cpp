#include <utils.h>
#include <http_api.h>
#include <params.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <system_error>
#include <thread>
#include <httplib.h>
#include <signal.h>

// ==================== HttpApi实现 ====================
class HttpApi::Impl {
public:
    static HttpApi::Impl* current_instance_;
    
    Impl() : server_(nullptr), is_running_(false) {
        current_instance_ = this;
    }
    
    ~Impl() {
        stop();
    }
    
    bool initialize(int port, App* app) {
        try {
            app_ = app;
            server_ = std::make_unique<httplib::Server>();
            port_ = port;
            
            // 设置CORS头
            server_->set_default_headers({
                {"Access-Control-Allow-Origin", "*"},
                {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
                {"Access-Control-Allow-Headers", "Content-Type"}
            });
            
            // 注册路由
            registerRoutes();
            
            return true;
        } catch (const std::exception& e) {
            std::cerr << "HTTP服务器初始化失败: " << e.what() << std::endl;
            return false;
        }
    }
    
    bool start() {
        if (!server_) {
            LOG_ERROR << "HTTP server not initialized";
            return false;
        }

        LOG_INFO << "Starting HTTP server, port=" << port_;
        try {
            server_thread_ = std::thread([this]() {
                if (!server_->listen("0.0.0.0", port_)) {
                    LOG_ERROR << "HTTP server listen failed";
                }
            });
        } catch (const std::system_error& e) {
            LOG_ERROR << "HTTP server thread creation failed: " << e.what();
            return false;
        }

        server_->wait_until_ready();
        if (!server_->is_running()) {
            LOG_ERROR << "HTTP server failed to start (bind error?)";
            if (server_thread_.joinable()) {
                server_thread_.join();
            }
            return false;
        }

        is_running_ = true;
        LOG_INFO << "HTTP server started on port " << port_;
        return true;
    }
    
    void stop() {
        if (server_) {
            server_->stop();
        }
        
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        
        is_running_ = false;
    }

    // 纯委托；契约见 `App::reloadYamlConfig()`（须 stopped / initialized，非 running 热重载）。
    void reloadYamlConfig() {
        if (app_) {
            app_->reloadYamlConfig();
        }
    }
    
    bool isRunning() const {
        return is_running_;
    }
    
    std::string getParamsJson() const {
        auto& param_manager = ParamManager::getInstance();
        auto params = param_manager.getAllParams();
        
        nlohmann::json result;
        result["status"] = "success";
        result["timestamp"] = std::chrono::system_clock::now().time_since_epoch().count();
        result["data"] = nlohmann::json::object();
        
        for (const auto& [name, param] : params) {
            result["data"][name] = param->toJson();
        }
        
        return result.dump(2);
    }

private:
    void registerRoutes() {
        // GET /api/status - 获取状态
        server_->Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
            nlohmann::json result;
            result["status"] = "success";
            result["data"] = app_->status();
            res.set_content(result.dump(2), "application/json");
        });

        // GET /api/health - 健康快照
        server_->Get("/api/health", [this](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            if (app_) {
                res.set_content(app_->collectHealthSnapshot().dump(2), "application/json");
            } else {
                res.set_content("{\"error\": \"app not initialized\"}", "application/json");
            }
        });

        // GET /api/params - 获取所有参数
        server_->Get("/api/params", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(getParamsJson(), "application/json");
        });
        
        // GET /api/params/{name} - 获取特定参数
        server_->Get("/api/params/([^/]+)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string param_name = req.matches[1].str();
            auto& param_manager = ParamManager::getInstance();
            auto param = param_manager.getParam(param_name);
            
            if (param) {
                nlohmann::json result;
                result["status"] = "success";
                result["data"] = param->toJson();
                res.set_content(result.dump(2), "application/json");
            } else {
                res.status = 404;
                nlohmann::json error;
                error["status"] = "error";
                error["message"] = "参数未找到: " + param_name;
                res.set_content(error.dump(2), "application/json");
            }
        });

        LOG_INFO << "registering cmd route";
        // POST /api/cmd - 控制命令
        server_->Post("/api/cmd/([^/]+)", [this](const httplib::Request& req, httplib::Response& res) {
            std::string cmd = req.matches[1].str();
            if (cmd == "initialize") {
                LOG_INFO << "initializing ..";
                app_->initialize();
                LOG_INFO << "done";
            } else if (cmd == "terminate") {
                LOG_INFO << "terminating ..";
                app_->terminate();
                LOG_INFO << "done";
            } else if (cmd == "start") {
                LOG_INFO << "starting ..";
                app_->start();
                LOG_INFO << "done";
            } else if (cmd == "stop") {
                LOG_INFO << "stopping ..";
                app_->stop();
                LOG_INFO << "done";
            }
            nlohmann::json result;
            result["status"] = "success";
            result["message"] = "Command sent: " + cmd;
            res.set_content(result.dump(2), "application/json");
        });
        
        // OPTIONS 处理CORS预检请求
        server_->Options(".*", [](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
        });
    }
    
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_;
    bool is_running_;
    App* app_;
};

// 静态成员变量定义
HttpApi::Impl* HttpApi::Impl::current_instance_ = nullptr;

// HttpApi公共接口实现
HttpApi::HttpApi() : pImpl_(std::make_unique<Impl>()) {}

HttpApi::~HttpApi() = default;

bool HttpApi::initialize(int port, App* app) {
    return pImpl_->initialize(port, app);
}

bool HttpApi::start() {
    signalHandler();
    return pImpl_->start();
}

void HttpApi::stop() {
    pImpl_->stop();
}

void HttpApi::reloadYamlConfig() {
    pImpl_->reloadYamlConfig();
}

void HttpApi::wait() {
    while (isRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void HttpApi::registerParamsRoute() {
    // 路由已在Impl中注册
}

std::string HttpApi::getParamsJson() const {
    return pImpl_->getParamsJson();
}

bool HttpApi::isRunning() const {
    return pImpl_->isRunning();
}

void HttpApi::signalHandler() {
    auto handler = [](int sig) {
        std::cout << "Received signal " << sig << ", stopping HTTP server" << std::endl;
        if (Impl::current_instance_) {
            Impl::current_instance_->stop();
        }
    };
    ::signal(SIGINT, handler);
    ::signal(SIGTERM, handler);
}
