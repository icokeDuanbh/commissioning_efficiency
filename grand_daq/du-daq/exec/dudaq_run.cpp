#include <dudaq_app.h>
#include <utils.h>

#include <dirent.h>
#include <fstream>
#include <string>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>

// 检查进程是否存在，排除自身 PID
bool processExists(const std::string& processName) {
    pid_t selfPid = getpid();  // 当前进程 PID

    DIR* procDir = opendir("/proc");
    if (!procDir) return false;

    struct dirent* entry;
    while ((entry = readdir(procDir)) != nullptr) {
        // 只处理数字目录
        if (entry->d_type == DT_DIR &&
            std::all_of(entry->d_name, entry->d_name + std::strlen(entry->d_name), ::isdigit)) {

            pid_t pid = std::stoi(entry->d_name);
            if (pid == selfPid) continue;  // 排除自身

            std::string commPath = std::string("/proc/") + entry->d_name + "/comm";
            std::ifstream commFile(commPath);
            if (commFile.is_open()) {
                std::string name;
                std::getline(commFile, name);
                commFile.close();
                if (name == processName) {
                    closedir(procDir);
                    return true;
                }
            }
        }
    }

    closedir(procDir);
    return false;
}

int main(int argc, char **argv)
{
    if (processExists("dudaq_run")) {
        LOG(ERROR) << " Porcess dudaq_run is already running. Exiting...";
        return 1;
    }

    LOG(INFO) << "DU DAQ is runing..";

    grand::DUDAQApp app;
    app.sysInit();
    while(true) {
        sleep(1);
    }
    return 0;
}
