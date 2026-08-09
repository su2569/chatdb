#include "chatdb/port_detector.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <cstring>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <poll.h>
#endif

namespace chatdb {

bool PortDetector::test_port(const std::string& host, int port, int timeout_ms) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;
#endif

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    // 设置非阻塞
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    int ret = connect(sock, (sockaddr*)&addr, sizeof(addr));

#ifdef _WIN32
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(sock);
        WSACleanup();
        return false;
    }
#else
    if (ret < 0 && errno != EINPROGRESS) {
        close(sock);
        return false;
    }
#endif

    // 使用poll/select等待连接
#ifdef _WIN32
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(sock, &fdset);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ret = select(0, nullptr, &fdset, nullptr, &tv);
    bool success = (ret > 0 && FD_ISSET(sock, &fdset));
    closesocket(sock);
    WSACleanup();
#else
    pollfd pfd{sock, POLLOUT, 0};
    ret = poll(&pfd, 1, timeout_ms);
    bool success = (ret > 0 && (pfd.revents & POLLOUT));
    close(sock);
#endif

    return success;
}

std::vector<PortInfo> PortDetector::scan_processes() {
    std::vector<PortInfo> results;

#ifdef _WIN32
    // Windows: 使用 netstat -ano 或 powershell
    FILE* pipe = _popen("netstat -ano | findstr LISTENING", "r");
    if (pipe) {
        char buffer[512];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            std::string line(buffer);
            // 解析格式: TCP    0.0.0.0:6379    0.0.0.0:0    LISTENING    1234
            size_t port_pos = line.find(':');
            if (port_pos != std::string::npos) {
                size_t space_pos = line.find(' ', port_pos);
                if (space_pos != std::string::npos) {
                    std::string port_str = line.substr(port_pos + 1, space_pos - port_pos - 1);
                    int port = std::stoi(port_str);

                    size_t pid_pos = line.rfind(' ');
                    if (pid_pos != std::string::npos) {
                        int pid = std::stoi(line.substr(pid_pos + 1));
                        results.push_back({"unknown", port, "unknown", pid});
                    }
                }
            }
        }
        _pclose(pipe);
    }

    // 通过PID获取进程名
    for (auto& info : results) {
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, info.pid);
        if (hProcess) {
            char procName[MAX_PATH];
            if (GetModuleBaseNameA(hProcess, nullptr, procName, MAX_PATH)) {
                info.process_name = procName;
                if (strstr(procName, "redis")) info.service_name = "redis";
                else if (strstr(procName, "ollama")) info.service_name = "ollama";
            }
            CloseHandle(hProcess);
        }
    }
#else
    // Linux: 使用 ss -tlnp 或 netstat -tlnp
    FILE* pipe = popen("ss -tlnp 2>/dev/null || netstat -tlnp 2>/dev/null", "r");
    if (pipe) {
        char buffer[512];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            std::string line(buffer);
            // 解析格式: LISTEN  0  128  0.0.0.0:6379  0.0.0.0:*  users:(("redis-server",pid=1234,fd=6))
            size_t colon_pos = line.find(':');
            if (colon_pos != std::string::npos) {
                size_t space_pos = line.find(' ', colon_pos);
                if (space_pos != std::string::npos) {
                    std::string port_str = line.substr(colon_pos + 1, space_pos - colon_pos - 1);
                    int port = std::stoi(port_str);

                    std::string proc_name = "unknown";
                    size_t proc_pos = line.find("\"");
                    if (proc_pos != std::string::npos) {
                        size_t proc_end = line.find("\"", proc_pos + 1);
                        if (proc_end != std::string::npos) {
                            proc_name = line.substr(proc_pos + 1, proc_end - proc_pos - 1);
                        }
                    }

                    std::string svc = "unknown";
                    if (proc_name.find("redis") != std::string::npos) svc = "redis";
                    else if (proc_name.find("ollama") != std::string::npos) svc = "ollama";

                    results.push_back({svc, port, proc_name, 0});
                }
            }
        }
        pclose(pipe);
    }
#endif

    return results;
}

std::optional<PortInfo> PortDetector::find_redis_port() {
    auto processes = scan_processes();
    for (const auto& p : processes) {
        if (p.service_name == "redis" || p.process_name.find("redis") != std::string::npos) {
            return p;
        }
    }
    return std::nullopt;
}

std::optional<PortInfo> PortDetector::find_ollama_port() {
    auto processes = scan_processes();
    for (const auto& p : processes) {
        if (p.service_name == "ollama" || p.process_name.find("ollama") != std::string::npos) {
            return p;
        }
    }
    return std::nullopt;
}

int PortDetector::ask_user_port(const std::string& service_name, int default_port) {
    std::cout << "[ChatDB] " << service_name << " 端口检测失败。" << std::endl;
    std::cout << "[ChatDB] 请输入 " << service_name << " 端口 (默认 " << default_port << "): ";
    std::string input;
    std::getline(std::cin, input);
    if (input.empty()) return default_port;
    try {
        return std::stoi(input);
    } catch (...) {
        return default_port;
    }
}

int PortDetector::detect_port(const std::string& service_name, 
                               int default_port,
                               const std::string& process_keyword) {
    // 1. 尝试默认端口
    if (test_port("127.0.0.1", default_port, 1000)) {
        std::cout << "[ChatDB] " << service_name << " 默认端口 " << default_port << " 可用。" << std::endl;
        return default_port;
    }

    std::cout << "[ChatDB] " << service_name << " 默认端口 " << default_port << " 未响应，尝试检测进程..." << std::endl;

    // 2. 扫描进程
    auto processes = scan_processes();
    for (const auto& p : processes) {
        if (p.process_name.find(process_keyword) != std::string::npos) {
            if (test_port("127.0.0.1", p.port, 1000)) {
                std::cout << "[ChatDB] 发现 " << service_name << " 在端口 " << p.port 
                          << " (进程: " << p.process_name << ")" << std::endl;
                return p.port;
            }
        }
    }

    // 3. 交互式询问
    std::cout << "[ChatDB] 未检测到 " << service_name << " 进程。" << std::endl;
    return ask_user_port(service_name, default_port);
}

} // namespace chatdb
