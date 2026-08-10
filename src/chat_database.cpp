#include "chatdb/chat_database.hpp"
#include "chatdb/sqlite_storage.hpp"
#include "chatdb/redis_client.hpp"
#include "chatdb/ollama_client.hpp"
#include "chatdb/message_processor.hpp"
#include "chatdb/query_engine.hpp"
#include "chatdb/http_server.hpp"
#include "chatdb/onebot_v11_client.hpp"
#include "chatdb/embedding_provider.hpp"
#include "chatdb/memory_summarizer.hpp"
#include "chatdb/process_guard.hpp"
#include "chatdb/port_detector.hpp"
#include "chatdb/config.hpp"
#include <fstream>
#include <spdlog/spdlog.h>
#include <fmt/format.h>

namespace chatdb {

ChatDatabase::ChatDatabase() = default;
ChatDatabase::~ChatDatabase() { shutdown(); }

bool ChatDatabase::initialize(const DatabaseConfig& cfg) {
    cfg_ = cfg;
    spdlog::info("ChatDatabase v2.0 initializing...");

    if (cfg_.auto_detect_ports) {
        if (!detect_and_connect_ports(cfg_)) {
            spdlog::error("Port detection failed");
            return false;
        }
    }

    // 1. SQLite
    sqlite_ = std::make_unique<SQLiteStorage>(cfg_.sqlite_path);
    if (!sqlite_->initialize()) {
        spdlog::error("SQLite init failed");
        return false;
    }

    // 2. Provider Manager（多 Embedding 源）
    provider_mgr_ = std::make_unique<EmbeddingProviderManager>();

    // 注册 Ollama
    OllamaProvider::Config ollama_cfg;
    ollama_cfg.host = cfg_.ollama_host;
    ollama_cfg.port = cfg_.ollama_port;
    ollama_cfg.model = cfg_.ollama_model;
    ollama_cfg.timeout_ms = cfg_.ollama_timeout_ms;
    ollama_cfg.embedding_dim = cfg_.ollama_embedding_dim;
    auto ollama_provider = std::make_shared<OllamaProvider>(ollama_cfg);
    provider_mgr_->register_provider(ollama_provider);

    // 注册 OpenAI（如果有配置）
    if (!cfg_.openai_api_key.empty()) {
        OpenAIProvider::Config openai_cfg;
        openai_cfg.api_base = cfg_.openai_api_base;
        openai_cfg.api_key = cfg_.openai_api_key;
        openai_cfg.model = cfg_.openai_model;
        openai_cfg.timeout_ms = 60000;
        openai_cfg.name = "openai";
        openai_cfg.display_name = "OpenAI";
        auto openai_provider = std::make_shared<OpenAIProvider>(openai_cfg);
        provider_mgr_->register_provider(openai_provider);
    }

    // 注册阿里云（如果有配置）
    if (!cfg_.aliyun_api_key.empty()) {
        OpenAIProvider::Config aliyun_cfg;
        aliyun_cfg.api_base = cfg_.aliyun_api_base;
        aliyun_cfg.api_key = cfg_.aliyun_api_key;
        aliyun_cfg.model = cfg_.aliyun_model;
        aliyun_cfg.timeout_ms = 60000;
        aliyun_cfg.name = "aliyun";
        aliyun_cfg.display_name = "阿里云";
        auto aliyun_provider = std::make_shared<OpenAIProvider>(aliyun_cfg);
        provider_mgr_->register_provider(aliyun_provider);
    }

    // 3. Redis
    RedisClient::Config redis_cfg{cfg_.redis_host, cfg_.redis_port, cfg_.redis_password, cfg_.redis_db};
    redis_ = std::make_unique<RedisClient>(redis_cfg);
    if (!redis_->connect()) {
        spdlog::warn("Redis unavailable, continuing without vector search/dedup");
    } else {
        auto current = provider_mgr_->current();
        if (current) redis_->create_vector_index(current->dimension());
    }

    // 4. OllamaClient（兼容旧版，实际应逐步迁移到 ProviderManager）
    OllamaClient::Config old_ollama_cfg;
    old_ollama_cfg.host = cfg_.ollama_host;
    old_ollama_cfg.port = cfg_.ollama_port;
    old_ollama_cfg.model = cfg_.ollama_model;
    old_ollama_cfg.timeout_ms = cfg_.ollama_timeout_ms;
    ollama_ = std::make_unique<OllamaClient>(old_ollama_cfg);
    if (ollama_->test_connection()) ollama_->start();

    // 5. Processor & Query
    processor_ = std::make_unique<MessageProcessor>(sqlite_.get(), redis_.get(), ollama_.get());
    processor_->start();
    query_ = std::make_unique<QueryEngine>(sqlite_.get(), redis_.get(), provider_mgr_.get());

    // 6. Memory Summarizer
    summarizer_ = std::make_unique<MemorySummarizer>(sqlite_.get(), redis_.get(), provider_mgr_.get());
    summarizer_->start();

    // 7. TCP Server（前端 / AstrBot）
    HttpServer::Config tcp_cfg;
    tcp_cfg.port = cfg_.tcp_port;
    tcp_ = std::make_unique<HttpServer>(tcp_cfg, this);
    if (!tcp_->start()) {
        spdlog::warn("TCP server failed to start on port {}", cfg_.tcp_port);
    }

    // 8. WS Client（QQ）
    OneBotV11Client::Config ws_cfg;
    ws_cfg.host = cfg_.ws_host;
    ws_cfg.port = cfg_.ws_port;
    ws_cfg.access_token = cfg_.ws_access_token;
    ws_ = std::make_unique<OneBotV11Client>(ws_cfg, this);
    ws_->connect();

    // 9. Process Guard
    ProcessGuard::Config guard_cfg;
    guard_cfg.watch_list = cfg_.guard_processes;
    guard_cfg.on_process_down = [](const ProcessInfo& p) {
        spdlog::warn("Process {} (PID {}) is DOWN!", p.name, p.pid);
    };
    guard_cfg.on_process_up = [](const ProcessInfo& p) {
        spdlog::info("Process {} (PID {}) is UP", p.name, p.pid);
    };
    guard_ = std::make_unique<ProcessGuard>(guard_cfg);
    guard_->start();

    // Provider 切换回调
    provider_mgr_->on_switch([this](const std::string& old_name, const std::string& new_name) {
        spdlog::info("Provider switched from {} to {}, rebuilding index...", old_name, new_name);
        // 触发索引重建广播
        if (tcp_) {
            tcp_->broadcast_event(protocol::events::PROVIDER_CHANGED, {{"old", old_name}, {"new", new_name}});
        }
    });

    ready_ = true;
    spdlog::info("ChatDatabase v2.0 initialized");
    return true;
}

void ChatDatabase::shutdown() {
    if (tcp_) tcp_->stop();
    if (ws_) ws_->disconnect();
    if (guard_) guard_->stop();
    if (summarizer_) summarizer_->stop();
    if (processor_) processor_->stop();
    if (ollama_) ollama_->stop();
    if (sqlite_) sqlite_->shutdown();
    if (redis_) redis_->disconnect();
    ready_ = false;
    spdlog::info("ChatDatabase shutdown complete");
}

bool ChatDatabase::is_ready() const { return ready_; }

bool ChatDatabase::detect_and_connect_ports(DatabaseConfig& cfg) {
    if (!PortDetector::test_port(cfg.redis_host, cfg.redis_port, 1000)) {
        cfg.redis_port = PortDetector::detect_port("Redis", cfg.redis_port, "redis");
    }
    if (!PortDetector::test_port(cfg.ollama_host, cfg.ollama_port, 1000)) {
        cfg.ollama_port = PortDetector::detect_port("Ollama", cfg.ollama_port, "ollama");
    }
    return true;
}

void ChatDatabase::receive_message(int64_t group_id, int64_t qq_id,
                                    const std::string& nickname,
                                    const std::string& content,
                                    int msg_type, int64_t timestamp) {
    if (!ready_) return;
    RawMessage msg{group_id, qq_id, nickname, content, msg_type, timestamp};
    processor_->receive_message(std::move(msg));
}

std::vector<SearchResult> ChatDatabase::search(const SearchRequest& req) {
    return ready_ ? query_->search(req) : std::vector<SearchResult>{};
}

std::vector<SearchResult> ChatDatabase::search_fulltext(const std::string& keyword, int64_t group_id, int limit) {
    return ready_ ? query_->search_fulltext(keyword, group_id, limit) : std::vector<SearchResult>{};
}

std::vector<SearchResult> ChatDatabase::search_semantic(const std::string& sentence, int64_t group_id, int limit) {
    return ready_ ? query_->search_semantic(sentence, group_id, limit) : std::vector<SearchResult>{};
}

std::vector<SearchResult> ChatDatabase::search_hybrid(const std::string& query, int64_t group_id, int limit) {
    return ready_ ? query_->search_hybrid(query, group_id, limit) : std::vector<SearchResult>{};
}

std::vector<SearchResult> ChatDatabase::get_recent(int64_t group_id, int limit) {
    return ready_ ? query_->get_recent(group_id, limit) : std::vector<SearchResult>{};
}

std::vector<protocol::MemoryEntry> ChatDatabase::get_memories(int64_t group_id, const std::string& level, int limit) {
    return ready_ ? summarizer_->get_memories(group_id, level, "", limit) : std::vector<protocol::MemoryEntry>{};
}

int64_t ChatDatabase::summarize_now(int64_t group_id, const std::string& level) {
    return ready_ ? summarizer_->summarize_now(group_id, level, true) : -1;
}

void ChatDatabase::handle_recall(int64_t group_id, int64_t msg_id, const std::string& content, bool important) {
    if (ready_ && summarizer_) summarizer_->handle_recall(group_id, msg_id, content, important);
}

bool ChatDatabase::switch_provider(const std::string& name) {
    return ready_ && provider_mgr_ ? provider_mgr_->switch_to(name) : false;
}

std::vector<std::string> ChatDatabase::list_providers() const {
    return provider_mgr_ ? provider_mgr_->list_names() : std::vector<std::string>{};
}

int64_t ChatDatabase::count_messages(int64_t group_id) {
    return ready_ ? sqlite_->count_messages(group_id) : 0;
}

protocol::json ChatDatabase::get_stats() {
    if (!ready_) return {{"error", "Not initialized"}};
    auto total = sqlite_->count_messages();
    auto processed = processor_->processed_count();
    auto dup = processor_->duplicate_count();
    auto err = processor_->error_count();
    auto queue = processor_->queue_size();
    int64_t redis_mem = redis_ && redis_->is_connected() ? redis_->get_memory_usage() : 0;

    return protocol::json{
        {"version", "2.1.0"},
        {"messages", {{"total", total}, {"processed", processed}, {"duplicate", dup}, {"error", err}}},
        {"queue_pending", queue},
        {"redis_memory_bytes", redis_mem}
    };
}

int ChatDatabase::cleanup_old_data(int retain_days) {
    if (!ready_) return 0;
    int deleted = sqlite_->cleanup_old_messages(retain_days);
    if (redis_ && redis_->is_connected()) redis_->cleanup_expired_vectors(retain_days);
    return deleted;
}

void ChatDatabase::vacuum() {
    if (sqlite_) sqlite_->vacuum();
}

bool ChatDatabase::backup_index() {
    if (!redis_ || !redis_->is_connected()) {
        spdlog::warn("Redis not connected, cannot backup index");
        return false;
    }

    try {
        // 1. 获取所有向量 key
        auto keys = redis_->command<std::vector<std::string>>({"KEYS", "vec:*"});
        if (keys.empty()) {
            spdlog::info("No vectors to backup");
            return true;
        }

        // 2. 序列化所有向量数据到 JSON
        protocol::json backup_data = protocol::json::array();
        for (const auto& key : keys) {
            // redis command removed - using sqlite only
            protocol::json item;
            for (size_t i = 0; i + 1 < fields.size(); i += 2) {
                item[fields[i]] = fields[i + 1];
            }
            backup_data.push_back(item);
        }

        // 3. 压缩并保存到 SQLite (index_backups 表)
        std::string json_str = backup_data.dump();

        // 计算 checksum (简化: 用长度+前100字符的hash)
        size_t checksum = std::hash<std::string>{}(json_str.substr(0, std::min(json_str.size(), size_t(1000))));

        // 插入备份记录
        // 这里需要 SQLiteStorage 暴露执行原始 SQL 的接口
        // 简化：直接通过 sqlite3_exec
        sqlite3* db = nullptr;
        // 获取底层 db 指针（需要暴露接口或友元）
        // 由于封装限制，这里改为写入文件备份

        std::string filename = fmt::format("index_backup_{}.json", time(nullptr));
        std::ofstream ofs(filename, std::ios::binary);
        if (!ofs) {
            spdlog::error("Failed to open backup file: {}", filename);
            return false;
        }
        ofs.write(json_str.data(), json_str.size());
        ofs.close();

        spdlog::info("Index backup saved: {} ({} vectors, {} bytes)", 
                     filename, keys.size(), json_str.size());
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Backup failed: {}", e.what());
        return false;
    }
}

bool ChatDatabase::restore_index(const std::string& name) {
    try {
        // 查找最新的备份文件
        // 简化实现：需要用户指定文件名或通过文件系统扫描
        spdlog::info("Index restore: please specify backup file via API");
        return false;
    } catch (const std::exception& e) {
        spdlog::error("Restore failed: {}", e.what());
        return false;
    }
}

void ChatDatabase::on_message_processed(std::function<void(const ProcessedMessage&)> cb) {
    if (processor_) processor_->on_processed(std::move(cb));
}

void ChatDatabase::on_error(std::function<void(const std::string&)> cb) {
    if (processor_) processor_->on_error(std::move(cb));
}

void ChatDatabase::on_active_chat(std::function<void(const ActiveChatRequest&)> cb) {
    if (summarizer_) summarizer_->set_active_chat_callback(std::move(cb));
}

} // namespace chatdb
