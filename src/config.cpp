#include "chatdb/config.hpp"
#include <spdlog/spdlog.h>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace chatdb {

ConfigManager& ConfigManager::instance() {
    static ConfigManager inst;
    return inst;
}

void ConfigManager::load_defaults() {
    // 默认值已在结构体初始化
}

bool ConfigManager::load_from_env() {
    auto get_env = [](const char* name, auto& val) {
        if (const char* p = std::getenv(name)) {
            if constexpr (std::is_same_v<std::decay_t<decltype(val)>, std::string>) {
                val = p;
            } else if constexpr (std::is_same_v<std::decay_t<decltype(val)>, int>) {
                val = std::atoi(p);
            } else if constexpr (std::is_same_v<std::decay_t<decltype(val)>, bool>) {
                val = (std::string(p) == "true" || std::string(p) == "1");
            }
        }
    };

    get_env("CHATDB_SQLITE_PATH", cfg_.sqlite_path);
    get_env("CHATDB_REDIS_HOST", cfg_.redis_host);
    get_env("CHATDB_REDIS_PORT", cfg_.redis_port);
    get_env("CHATDB_REDIS_PASSWORD", cfg_.redis_password);
    get_env("CHATDB_OLLAMA_HOST", cfg_.ollama_host);
    get_env("CHATDB_OLLAMA_PORT", cfg_.ollama_port);
    get_env("CHATDB_OLLAMA_MODEL", cfg_.ollama_model);
    get_env("CHATDB_OLLAMA_TIMEOUT", cfg_.ollama_timeout_ms);
    get_env("CHATDB_OPENAI_KEY", cfg_.openai_api_key);
    get_env("CHATDB_ALIYUN_KEY", cfg_.aliyun_api_key);
    get_env("CHATDB_VECTOR_DAYS", cfg_.vector_retention_days);
    get_env("CHATDB_WORKER_THREADS", cfg_.worker_threads);
    get_env("CHATDB_TCP_PORT", cfg_.tcp_port);
    get_env("CHATDB_WS_HOST", cfg_.ws_host);
    get_env("CHATDB_WS_PORT", cfg_.ws_port);
    get_env("CHATDB_WS_TOKEN", cfg_.ws_access_token);
    return true;
}

bool ConfigManager::load_from_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);

        if (key == "sqlite_path") cfg_.sqlite_path = val;
        else if (key == "redis_host") cfg_.redis_host = val;
        else if (key == "redis_port") { try { cfg_.redis_port = std::stoi(val); } catch (...) { spdlog::warn("Invalid config redis_port: {}", val); } }
        else if (key == "redis_password") cfg_.redis_password = val;
        else if (key == "ollama_host") cfg_.ollama_host = val;
        else if (key == "ollama_port") { try { cfg_.ollama_port = std::stoi(val); } catch (...) { spdlog::warn("Invalid config ollama_port: {}", val); } }
        else if (key == "ollama_model") cfg_.ollama_model = val;
        else if (key == "ollama_timeout_ms") { try { cfg_.ollama_timeout_ms = std::stoi(val); } catch (...) { spdlog::warn("Invalid config ollama_timeout_ms: {}", val); } }
        else if (key == "openai_api_key") cfg_.openai_api_key = val;
        else if (key == "openai_model") cfg_.openai_model = val;
        else if (key == "aliyun_api_key") cfg_.aliyun_api_key = val;
        else if (key == "aliyun_model") cfg_.aliyun_model = val;
        else if (key == "vector_retention_days") { try { cfg_.vector_retention_days = std::stoi(val); } catch (...) { spdlog::warn("Invalid config vector_retention_days: {}", val); } }
        else if (key == "worker_threads") { try { cfg_.worker_threads = std::stoi(val); } catch (...) { spdlog::warn("Invalid config worker_threads: {}", val); } }
        else if (key == "tcp_port") { try { cfg_.tcp_port = std::stoi(val); } catch (...) { spdlog::warn("Invalid config tcp_port: {}", val); } }
        else if (key == "ws_host") cfg_.ws_host = val;
        else if (key == "ws_port") { try { cfg_.ws_port = std::stoi(val); } catch (...) { spdlog::warn("Invalid config ws_port: {}", val); } }
        else if (key == "ws_access_token") cfg_.ws_access_token = val;
    }
    return true;
}

} // namespace chatdb
