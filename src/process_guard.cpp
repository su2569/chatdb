#include "chatdb/process_guard.hpp"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <chrono>
#include <thread>

#ifdef _WIN32
    #include <windows.h>
    #include <tlhelp32.h>
#else
    #include <dirent.h>
    #include <unistd.h>
    #include <signal.h>
#endif

namespace chatdb {

ProcessGuard::ProcessGuard(const Config& cfg) : cfg_(cfg) {}

ProcessGuard::~ProcessGuard() { stop(); }

void ProcessGuard::start() {
    if (running_) return;
    running_ = true;
    guard_thread_ = std::thread(&ProcessGuard::guard_loop, this);
    spdlog::info("ProcessGuard started, watching: {}", fmt::join(cfg_.watch_list, ", "));
}

void ProcessGuard::stop() {
    running_ = false;
    if (guard_thread_.joinable()) guard_thread_.join();
}

void ProcessGuard::watch(const std::string& process_name) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    auto it = std::find_if(current_status_.begin(), current_status_.end(),
                           [&](const auto& p) { return p.name == process_name; });
    if (it == current_status_.end()) {
        ProcessInfo info;
        info.name = process_name;
        current_status_.push_back(info);
    }
}

void ProcessGuard::unwatch(const std::string& process_name) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    current_status_.erase(
        std::remove_if(current_status_.begin(), current_status_.end(),
                       [&](const auto& p) { return p.name == process_name; }),
        current_status_.end()
    );
}

std::vector<ProcessInfo> ProcessGuard::get_status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return current_status_;
}

bool ProcessGuard::is_process_alive(const std::string& name) const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    auto it = std::find_if(current_status_.begin(), current_status_.end(),
                           [&](const auto& p) { return p.name == name && p.is_alive; });
    return it != current_status_.end();
}

void ProcessGuard::guard_loop() {
    while (running_) {
        auto new_status = scan_processes();

        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            for (auto& new_p : new_status) {
                auto it = std::find_if(current_status_.begin(), current_status_.end(),
                                       [&](const auto& p) { return p.name == new_p.name; });
                if (it != current_status_.end()) {
                    bool was_alive = it->is_alive;
                    it->is_alive = new_p.is_alive;
                    it->pid = new_p.pid;
                    it->cmdline = new_p.cmdline;
                    it->last_seen = new_p.last_seen;

                    if (!was_alive && new_p.is_alive && cfg_.on_process_up) {
                        cfg_.on_process_up(new_p);
                    } else if (was_alive && !new_p.is_alive && cfg_.on_process_down) {
                        cfg_.on_process_down(*it);
                    }
                } else {
                    current_status_.push_back(new_p);
                    if (new_p.is_alive && cfg_.on_process_up) {
                        cfg_.on_process_up(new_p);
                    }
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.check_interval_ms));
    }
}

std::vector<ProcessInfo> ProcessGuard::scan_processes() {
    std::vector<ProcessInfo> results;
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

#ifdef _WIN32
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(pe);
        if (Process32First(hSnapshot, &pe)) {
            do {
                std::string name = pe.szExeFile;
                std::string lower_name = name;
                std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

                for (const auto& watch : cfg_.watch_list) {
                    std::string lower_watch = watch;
                    std::transform(lower_watch.begin(), lower_watch.end(), lower_watch.begin(), ::tolower);

                    if (lower_name.find(lower_watch) != std::string::npos) {
                        ProcessInfo info;
                        info.name = watch;
                        info.pid = pe.th32ProcessID;
                        info.cmdline = name;
                        info.is_alive = true;
                        info.last_seen = now;
                        results.push_back(info);
                    }
                }
            } while (Process32Next(hSnapshot, &pe));
        }
        CloseHandle(hSnapshot);
    }
#else
    // Linux: 扫描 /proc
    DIR* dir = opendir("/proc");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type != DT_DIR) continue;
            int pid = atoi(entry->d_name);
            if (pid <= 0) continue;

            std::string cmdline_path = fmt::format("/proc/{}/cmdline", pid);
            FILE* f = fopen(cmdline_path.c_str(), "r");
            if (!f) continue;

            char cmdline[4096];
            size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, f);
            fclose(f);
            if (n == 0) continue;
            cmdline[n] = '\0';

            std::string lower_cmdline = cmdline;
            std::transform(lower_cmdline.begin(), lower_cmdline.end(), lower_cmdline.begin(), ::tolower);

            for (const auto& watch : cfg_.watch_list) {
                std::string lower_watch = watch;
                std::transform(lower_watch.begin(), lower_watch.end(), lower_watch.begin(), ::tolower);

                if (lower_cmdline.find(lower_watch) != std::string::npos) {
                    ProcessInfo info;
                    info.name = watch;
                    info.pid = pid;
                    info.cmdline = cmdline;
                    info.is_alive = true;
                    info.last_seen = now;
                    results.push_back(info);
                }
            }
        }
        closedir(dir);
    }
#endif

    return results;
}

std::vector<int> ProcessGuard::find_ports(const std::string& process_name) {
    std::vector<int> ports;
    // 简化：通过 netstat/ss 查找进程的端口
    // 实际实现与 port_detector.cpp 类似
    return ports;
}

} // namespace chatdb
