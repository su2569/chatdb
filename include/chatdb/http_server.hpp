#pragma once
#include "chatdb/protocol.hpp"
#include <string>
#include <queue>
#include <condition_variable>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>

// Forward declaration for cpp-httplib
namespace httplib {
    class Server;
    struct Request;
    struct Response;
}

namespace chatdb {

class ChatDatabase;

// HTTP REST API Server（连接前端和 AstrBot）
struct SseClient {
    std::string id;
    std::queue<std::string> events;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> active{true};
};

class HttpServer {
public:
    struct Config {
        std::string bind_host = "0.0.0.0";
        int port = 17320;
        int read_timeout_ms = 30000;
        int write_timeout_ms = 30000;
        std::string api_prefix = "/api/v1";
        bool enable_cors = true;
        bool enable_sse = true;

        // 鉴权
        std::string access_key;       // SM3 哈希后的 key（十六进制）
        bool require_auth = true;     // 是否强制鉴权
    };

    HttpServer(const Config& cfg, ChatDatabase* db);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }
    std::string listen_address() const;

    // 广播 SSE 事件
    void broadcast_event(const std::string& event_name, const protocol::json& data);

private:
    void setup_routes();
    bool check_auth(const httplib::Request& req, httplib::Response& res);

    // RESTful API handlers
    void handle_post_messages(const httplib::Request& req, httplib::Response& res);
    void handle_post_messages_batch(const httplib::Request& req, httplib::Response& res);
    void handle_post_messages_recall(const httplib::Request& req, httplib::Response& res);
    void handle_get_search(const httplib::Request& req, httplib::Response& res);
    void handle_get_search_semantic(const httplib::Request& req, httplib::Response& res);
    void handle_get_search_hybrid(const httplib::Request& req, httplib::Response& res);
    void handle_get_recent(const httplib::Request& req, httplib::Response& res);
    void handle_get_context(const httplib::Request& req, httplib::Response& res);
    void handle_get_memories(const httplib::Request& req, httplib::Response& res);
    void handle_post_memories_summarize(const httplib::Request& req, httplib::Response& res);
    void handle_post_memories_importance(const httplib::Request& req, httplib::Response& res);
    void handle_post_memories_merge(const httplib::Request& req, httplib::Response& res);
    void handle_delete_memory(const httplib::Request& req, httplib::Response& res);
    void handle_get_providers(const httplib::Request& req, httplib::Response& res);
    void handle_post_providers_switch(const httplib::Request& req, httplib::Response& res);
    void handle_get_providers_status(const httplib::Request& req, httplib::Response& res);
    void handle_get_stats(const httplib::Request& req, httplib::Response& res);
    void handle_get_health(const httplib::Request& req, httplib::Response& res);
    void handle_post_backup(const httplib::Request& req, httplib::Response& res);
    void handle_post_restore(const httplib::Request& req, httplib::Response& res);
    void handle_post_vacuum(const httplib::Request& req, httplib::Response& res);
    void handle_post_cleanup(const httplib::Request& req, httplib::Response& res);
    void handle_get_config(const httplib::Request& req, httplib::Response& res);
    void handle_put_config(const httplib::Request& req, httplib::Response& res);
    void handle_sse_events(const httplib::Request& req, httplib::Response& res);

    void set_json_response(httplib::Response& res, const protocol::json& j, int status = 200);
    void set_error_response(httplib::Response& res, int status, const std::string& message);
    protocol::json search_results_to_json(const std::vector<SearchResult>& results);

    Config cfg_;
    ChatDatabase* db_;
    std::unique_ptr<httplib::Server> server_;
    std::atomic<bool> running_{false};
    std::thread server_thread_;

    mutable std::mutex sse_mutex_;
    std::vector<std::weak_ptr<SseClient>> sse_clients_;
};

} // namespace chatdb
