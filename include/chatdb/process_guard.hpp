#pragma once
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>

namespace chatdb {

// 进程守护：探查 Python / Node 进程，保持连接
struct ProcessInfo {
    std::string name;       // "python" | "node"
    int pid = 0;
    std::string cmdline;
    bool is_alive = false;
    int64_t last_seen = 0;
};

class ProcessGuard {
public:
    struct Config {
        int check_interval_ms = 10000;    // 检查间隔 10s
        int max_restart_attempts = 3;     // 最大重启次数
        std::vector<std::string> watch_list = {"python", "node"};  // 监视列表
        bool auto_restart = false;        // 是否自动重启（预留）
        std::function<void(const ProcessInfo&)> on_process_down;
        std::function<void(const ProcessInfo&)> on_process_up;
    };

    explicit ProcessGuard(const Config& cfg);
    ~ProcessGuard();

    void start();
    void stop();

    // 手动添加监视
    void watch(const std::string& process_name);
    void unwatch(const std::string& process_name);

    // 获取状态
    std::vector<ProcessInfo> get_status() const;
    bool is_process_alive(const std::string& name) const;

    // 获取 Python/Node 的通信端口（如果它们暴露了 HTTP/WS）
    std::vector<int> find_ports(const std::string& process_name);

private:
    void guard_loop();
    std::vector<ProcessInfo> scan_processes();
    bool check_process_alive(int pid);

    Config cfg_;
    std::atomic<bool> running_{false};
    std::thread guard_thread_;
    mutable std::mutex status_mutex_;
    std::vector<ProcessInfo> current_status_;
};

} // namespace chatdb
