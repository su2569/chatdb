#pragma once
#include <string>
#include <cstdint>
#include <vector>

namespace chatdb {

struct DatabaseConfig {
    // SQLite
    std::string sqlite_path = "chatdb.sqlite";
    int sqlite_wal_size_limit_mb = 100;

    // Redis
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    std::string redis_password;
    int redis_db = 0;
    int redis_pool_size = 4;

    // Ollama
    std::string ollama_host = "127.0.0.1";
    int ollama_port = 11434;
    std::string ollama_model = "nomic-embed-text";
    int ollama_timeout_ms = 300000;  // 300秒
    int ollama_embedding_dim = 768;

    // OpenAI (可选)
    std::string openai_api_base = "https://api.openai.com/v1";
    std::string openai_api_key;
    std::string openai_model = "text-embedding-3-small";

    // 阿里云 (可选)
    std::string aliyun_api_base = "https://dashscope.aliyuncs.com/api/v1";
    std::string aliyun_api_key;
    std::string aliyun_model = "text-embedding-v2";

    // 向量策略
    int vector_retention_days = 7;
    bool use_int8_quantization = false;
    int vector_batch_size = 8;

    // 性能
    int sqlite_batch_size = 100;
    int sqlite_flush_interval_ms = 500;
    int worker_threads = 2;

    // 去重
    int dedup_window_seconds = 300;

    // TCP Server (前端 / AstrBot)
    int tcp_port = 17320;
    int tcp_max_clients = 32;

    // WS Client (QQ)
    std::string ws_host = "127.0.0.1";
    int ws_port = 3001;
    std::string ws_path = "/";
    std::string ws_access_token;
    int ws_reconnect_interval_ms = 5000;

    // 进程守护
    std::vector<std::string> guard_processes = {"python", "node"};
    int guard_check_interval_ms = 10000;

    // 端口检测
    bool auto_detect_ports = true;
    bool interactive_fallback = true;
};

class ConfigManager {
public:
    static ConfigManager& instance();
    bool load_from_file(const std::string& path);
    bool load_from_env();
    void load_defaults();
    DatabaseConfig& config() { return cfg_; }
    const DatabaseConfig& config() const { return cfg_; }
private:
    ConfigManager() = default;
    DatabaseConfig cfg_;
};

} // namespace chatdb
