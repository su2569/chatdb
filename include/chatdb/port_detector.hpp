#pragma once
#include <string>
#include <optional>
#include <vector>

namespace chatdb {

struct PortInfo {
    std::string service_name;
    int port;
    std::string process_name;
    int pid;
};

class PortDetector {
public:
    // 尝试连接默认端口，返回是否成功
    static bool test_port(const std::string& host, int port, int timeout_ms = 1000);

    // 扫描进程列表查找服务端口
    static std::optional<PortInfo> find_redis_port();
    static std::optional<PortInfo> find_ollama_port();

    // 通用进程扫描（平台适配）
    static std::vector<PortInfo> scan_processes();

    // 交互式询问
    static int ask_user_port(const std::string& service_name, int default_port);

    // 完整检测流程：默认→进程扫描→交互式
    static int detect_port(const std::string& service_name, 
                           int default_port,
                           const std::string& process_keyword);
};

} // namespace chatdb
