#pragma once
#include "chatdb/config.hpp"
#include "chatdb/protocol.hpp"
#include <string>
#include <memory>
#include <functional>

namespace chatdb {

class SQLiteStorage;
class RedisClient;
class OllamaClient;
class MessageProcessor;
class QueryEngine;
class HttpServer;
class OneBotV11Client;
class EmbeddingProviderManager;
class MemorySummarizer;
class ProcessGuard;
struct ActiveChatRequest {
    int64_t group_id;
    std::string topic;
    float urgency;
};

struct RawMessage {
    int64_t group_id;
    int64_t qq_id;
    std::string nickname;
    std::string content;
    int msg_type;
    int64_t timestamp;
};

struct SearchRequest;
struct SearchResult;
struct ProcessedMessage {
    int64_t id;
    int64_t group_id;
    int64_t qq_id;
    std::string content;
    bool is_duplicate;
    int64_t timestamp;
};

class ChatDatabase {
public:
    QueryEngine* query() const { return query_.get(); }
    ChatDatabase();
    ~ChatDatabase();
    ChatDatabase(const ChatDatabase&) = delete;
    ChatDatabase& operator=(const ChatDatabase&) = delete;

    bool initialize(const DatabaseConfig& cfg = DatabaseConfig{});
    void shutdown();
    bool is_ready() const;

    // 消息接收
    void receive_message(int64_t group_id, int64_t qq_id,
                         const std::string& nickname,
                         const std::string& content,
                         int msg_type = 1,
                         int64_t timestamp = 0);
    void receive_messages(std::vector<RawMessage> msgs);

    // 搜索
    std::vector<SearchResult> search(const SearchRequest& req);
    std::vector<SearchResult> search_fulltext(const std::string& keyword, int64_t group_id = 0, int limit = 20);
    std::vector<SearchResult> search_semantic(const std::string& sentence, int64_t group_id = 0, int limit = 20);
    std::vector<SearchResult> search_hybrid(const std::string& query, int64_t group_id = 0, int limit = 20);
    std::vector<SearchResult> get_recent(int64_t group_id, int limit = 50);

    // 记忆
    std::vector<protocol::MemoryEntry> get_memories(int64_t group_id, const std::string& level = "", int limit = 50);
    int64_t summarize_now(int64_t group_id, const std::string& level);
    void handle_recall(int64_t group_id, int64_t msg_id, const std::string& content, bool important);

    // Provider
    bool switch_provider(const std::string& name);
    std::vector<std::string> list_providers() const;

    // 统计与维护
    int64_t count_messages(int64_t group_id = 0);
    protocol::json get_stats();
    int cleanup_old_data(int retain_days);
    void vacuum();
    bool backup_index();
    bool restore_index(const std::string& name = "");

    // 回调
    void on_message_processed(std::function<void(const ProcessedMessage&)> cb);
    void on_error(std::function<void(const std::string&)> cb);
    void on_active_chat(std::function<void(const ActiveChatRequest&)> cb);

    // 组件访问
    SQLiteStorage* sqlite() const { return sqlite_.get(); }
    RedisClient* redis() const { return redis_.get(); }
    HttpServer* tcp_server() const { return tcp_.get(); }
    OneBotV11Client* ws_client() const { return ws_.get(); }
    EmbeddingProviderManager* provider_manager() const { return provider_mgr_.get(); }
    MemorySummarizer* summarizer() const { return summarizer_.get(); }
    ProcessGuard* process_guard() const { return guard_.get(); }

private:
    bool detect_and_connect_ports(DatabaseConfig& cfg);

    std::unique_ptr<SQLiteStorage> sqlite_;
    std::unique_ptr<RedisClient> redis_;
    std::unique_ptr<OllamaClient> ollama_;
    std::unique_ptr<MessageProcessor> processor_;
    std::unique_ptr<QueryEngine> query_;
    std::unique_ptr<HttpServer> tcp_;
    std::unique_ptr<OneBotV11Client> ws_;
    std::unique_ptr<EmbeddingProviderManager> provider_mgr_;
    std::unique_ptr<MemorySummarizer> summarizer_;
    std::unique_ptr<ProcessGuard> guard_;

    DatabaseConfig cfg_;
    bool ready_ = false;
};

} // namespace chatdb
