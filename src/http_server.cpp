#include "chatdb/http_server.hpp"
#include "chatdb/chat_database.hpp"
#include "chatdb/protocol.hpp"
#include "chatdb/query_engine.hpp"
#include "chatdb/memory_summarizer.hpp"
#include "chatdb/embedding_provider.hpp"
#include <fmt/format.h>
#include <httplib.h>
#include <openssl/evp.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <cstring>

namespace chatdb {

// ========== SHA1 工具 ==========
static std::string sha1_hex(const std::string& input) {
    unsigned char hash[SHA_DIGEST_LENGTH];
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, hash, &len);
    EVP_MD_CTX_free(ctx);

    std::string hex;
    hex.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        hex += fmt::format("{:02x}", hash[i]);
    }
    return hex;
}

// ========== 安全解析辅助 ==========
static std::optional<int64_t> safe_stoll(const std::string& s) {
    try { return std::stoll(s); } catch (...) { return std::nullopt; }
}
static std::optional<int> safe_stoi(const std::string& s) {
    try { return std::stoi(s); } catch (...) { return std::nullopt; }
}

// ========== 参数获取辅助（httplib get_param_value 第二个参数是 index 而非 default）==========
static std::string get_param_or_default(const httplib::Request& req, const std::string& key, const std::string& default_val = "") {
    auto val = req.get_param_value(key);
    return val.empty() ? default_val : val;
}

HttpServer::HttpServer(const Config& cfg, ChatDatabase* db) : cfg_(cfg), db_(db) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start() {
    server_ = std::make_unique<httplib::Server>();

    if (cfg_.enable_cors) {
        server_->set_base_dir(".");
        server_->set_file_extension_and_mimetype_mapping("html", "text/html");
    }

    setup_routes();

    running_ = true;
    server_thread_ = std::thread([this]() {
        spdlog::info("HTTP Server starting on {}:{}", cfg_.bind_host, cfg_.port);
        if (!server_->listen(cfg_.bind_host.c_str(), cfg_.port)) {
            spdlog::error("HTTP Server failed to bind {}:{}", cfg_.bind_host, cfg_.port);
            running_ = false;
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return running_.load();
}

void HttpServer::stop() {
    running_ = false;

    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        for (auto& wp : sse_clients_) {
            if (auto sp = wp.lock()) {
                sp->active = false;
                sp->cv.notify_all();
            }
        }
        sse_clients_.clear();
    }

    if (server_) server_->stop();
    if (server_thread_.joinable()) server_thread_.join();
    spdlog::info("HTTP Server stopped");
}

std::string HttpServer::listen_address() const {
    return fmt::format("http://{}:{}", cfg_.bind_host, cfg_.port);
}

bool HttpServer::check_auth(const httplib::Request& req, httplib::Response& res) {
    if (!cfg_.require_auth || cfg_.access_key.empty()) return true;

    auto auth = req.get_header_value("Authorization");
    if (auth.empty()) {
        set_error_response(res, 401, "Missing Authorization header");
        return false;
    }

    std::string key;
    if (auth.rfind("Bearer ", 0) == 0) {
        key = auth.substr(7);
    } else {
        key = auth;
    }

    std::string hashed = sha1_hex(key);
    if (hashed != cfg_.access_key) {
        set_error_response(res, 403, "Invalid access key");
        return false;
    }
    return true;
}

void HttpServer::setup_routes() {
    auto prefix = cfg_.api_prefix;

    if (cfg_.enable_cors) {
        server_->Options(prefix + ".*", [](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
            res.status = 204;
        });
    }

    server_->Post(prefix + "/messages", [this](const auto& req, auto& res) { handle_post_messages(req, res); });
    server_->Post(prefix + "/messages/batch", [this](const auto& req, auto& res) { handle_post_messages_batch(req, res); });
    server_->Post(prefix + "/messages/recall", [this](const auto& req, auto& res) { handle_post_messages_recall(req, res); });

    server_->Get(prefix + "/search", [this](const auto& req, auto& res) { handle_get_search(req, res); });
    server_->Get(prefix + "/search/semantic", [this](const auto& req, auto& res) { handle_get_search_semantic(req, res); });
    server_->Get(prefix + "/search/hybrid", [this](const auto& req, auto& res) { handle_get_search_hybrid(req, res); });
    server_->Get(prefix + "/recent/(\\d+)", [this](const auto& req, auto& res) { handle_get_recent(req, res); });
    server_->Get(prefix + "/context/(\\d+)", [this](const auto& req, auto& res) { handle_get_context(req, res); });

    server_->Get(prefix + "/memories", [this](const auto& req, auto& res) { handle_get_memories(req, res); });
    server_->Post(prefix + "/memories/summarize", [this](const auto& req, auto& res) { handle_post_memories_summarize(req, res); });
    server_->Post(prefix + "/memories/importance", [this](const auto& req, auto& res) { handle_post_memories_importance(req, res); });
    server_->Post(prefix + "/memories/merge", [this](const auto& req, auto& res) { handle_post_memories_merge(req, res); });
    server_->Delete(prefix + "/memories/(\\d+)", [this](const auto& req, auto& res) { handle_delete_memory(req, res); });

    server_->Get(prefix + "/providers", [this](const auto& req, auto& res) { handle_get_providers(req, res); });
    server_->Post(prefix + "/providers/switch", [this](const auto& req, auto& res) { handle_post_providers_switch(req, res); });
    server_->Get(prefix + "/providers/status", [this](const auto& req, auto& res) { handle_get_providers_status(req, res); });

    server_->Get(prefix + "/stats", [this](const auto& req, auto& res) { handle_get_stats(req, res); });
    server_->Get(prefix + "/health", [this](const auto& req, auto& res) { handle_get_health(req, res); });
    server_->Post(prefix + "/backup", [this](const auto& req, auto& res) { handle_post_backup(req, res); });
    server_->Post(prefix + "/restore", [this](const auto& req, auto& res) { handle_post_restore(req, res); });
    server_->Post(prefix + "/vacuum", [this](const auto& req, auto& res) { handle_post_vacuum(req, res); });
    server_->Post(prefix + "/cleanup", [this](const auto& req, auto& res) { handle_post_cleanup(req, res); });
    server_->Get(prefix + "/config", [this](const auto& req, auto& res) { handle_get_config(req, res); });
    server_->Put(prefix + "/config", [this](const auto& req, auto& res) { handle_put_config(req, res); });

    if (cfg_.enable_sse) {
        server_->Get(prefix + "/events", [this](const auto& req, auto& res) { handle_sse_events(req, res); });
    }

    server_->set_error_handler([](const auto&, auto& res) {
        res.status = 404;
        res.set_content(R"({"error":"Not Found"})", "application/json");
    });
}

void HttpServer::set_json_response(httplib::Response& res, const protocol::json& j, int status) {
    res.status = status;
    res.set_content(j.dump(), "application/json");
    if (cfg_.enable_cors) res.set_header("Access-Control-Allow-Origin", "*");
}

void HttpServer::set_error_response(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    protocol::json j = {{"error", message}, {"status", status}};
    res.set_content(j.dump(), "application/json");
    if (cfg_.enable_cors) res.set_header("Access-Control-Allow-Origin", "*");
}

protocol::json HttpServer::search_results_to_json(const std::vector<SearchResult>& results) {
    protocol::json arr = protocol::json::array();
    for (const auto& r : results) {
        arr.push_back({
            {"msg_id", r.msg_id},
            {"group_id", r.group_id},
            {"qq_id", r.qq_id},
            {"nickname", r.nickname},
            {"content", r.content},
            {"timestamp", r.timestamp},
            {"score", r.relevance_score},
            {"semantic", r.is_semantic_match},
            {"fulltext", r.is_fulltext_match}
        });
    }
    return arr;
}

void HttpServer::handle_post_messages(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    try {
        auto j = protocol::json::parse(req.body);
        db_->receive_message(
            j.value("group_id", 0),
            j.value("qq_id", 0),
            j.value("nickname", ""),
            j.value("content", ""),
            j.value("msg_type", 1),
            j.value("timestamp", 0)
        );
        set_json_response(res, protocol::json{{"status", "received"}, {"msg_id", db_->count_messages()}});
    } catch (...) {
        set_error_response(res, 400, "Invalid JSON");
    }
}

void HttpServer::handle_post_messages_batch(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    try {
        auto j = protocol::json::parse(req.body);
        auto msgs = j.value("messages", protocol::json::array());
        std::vector<RawMessage> batch;
        for (auto& m : msgs) {
            batch.push_back({m.value("group_id", 0), m.value("qq_id", 0),
                             m.value("nickname", ""), m.value("content", ""),
                             m.value("msg_type", 1), m.value("timestamp", 0)});
        }
        db_->receive_messages(std::move(batch));
        set_json_response(res, protocol::json{{"status", "received"}, {"count", batch.size()}});
    } catch (...) {
        set_error_response(res, 400, "Invalid JSON");
    }
}

void HttpServer::handle_post_messages_recall(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    try {
        auto j = protocol::json::parse(req.body);
        db_->handle_recall(j.value("group_id", 0), j.value("msg_id", 0),
                           j.value("content", ""), j.value("important", false));
        set_json_response(res, protocol::json{{"status", "recall_handled"}});
    } catch (...) {
        set_error_response(res, 400, "Invalid JSON");
    }
}

void HttpServer::handle_get_search(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    auto query = req.get_param_value("q");
    auto gid_opt = safe_stoll(get_param_or_default(req, "group_id", "0"));
    auto limit_opt = safe_stoi(get_param_or_default(req, "limit", "20"));
    if (!gid_opt || !limit_opt) { set_error_response(res, 400, "Invalid parameters"); return; }
    auto group_id = *gid_opt;
    auto limit = *limit_opt;
    auto results = db_->search_fulltext(query, group_id, limit);
    set_json_response(res, protocol::json{{"results", search_results_to_json(results)}, {"type", "fulltext"}});
}

void HttpServer::handle_get_search_semantic(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    auto query = req.get_param_value("q");
    auto gid_opt = safe_stoll(get_param_or_default(req, "group_id", "0"));
    auto limit_opt = safe_stoi(get_param_or_default(req, "limit", "20"));
    if (!gid_opt || !limit_opt) { set_error_response(res, 400, "Invalid parameters"); return; }
    auto group_id = *gid_opt;
    auto limit = *limit_opt;
    auto results = db_->search_semantic(query, group_id, limit);
    set_json_response(res, protocol::json{{"results", search_results_to_json(results)}, {"type", "semantic"}});
}

void HttpServer::handle_get_search_hybrid(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    auto query = req.get_param_value("q");
    auto gid_opt = safe_stoll(get_param_or_default(req, "group_id", "0"));
    auto limit_opt = safe_stoi(get_param_or_default(req, "limit", "20"));
    if (!gid_opt || !limit_opt) { set_error_response(res, 400, "Invalid parameters"); return; }
    auto group_id = *gid_opt;
    auto limit = *limit_opt;
    auto results = db_->search_hybrid(query, group_id, limit);
    set_json_response(res, protocol::json{{"results", search_results_to_json(results)}, {"type", "hybrid"}});
}

void HttpServer::handle_get_recent(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    auto gid_opt = safe_stoll(req.matches[1]);
    auto limit_opt = safe_stoi(get_param_or_default(req, "limit", "50"));
    if (!gid_opt || !limit_opt) { set_error_response(res, 400, "Invalid parameters"); return; }
    auto group_id = *gid_opt;
    auto limit = *limit_opt;
    auto results = db_->get_recent(group_id, limit);
    set_json_response(res, protocol::json{{"results", search_results_to_json(results)}});
}

void HttpServer::handle_get_context(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    auto mid_opt = safe_stoll(req.matches[1]);
    auto radius_opt = safe_stoi(get_param_or_default(req, "radius", "5"));
    if (!mid_opt || !radius_opt) { set_error_response(res, 400, "Invalid parameters"); return; }
    auto msg_id = *mid_opt;
    auto radius = *radius_opt;
    auto results = db_->query()->get_context(msg_id, radius);
    set_json_response(res, protocol::json{{"results", search_results_to_json(results)}, {"msg_id", msg_id}});
}

void HttpServer::handle_get_memories(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    auto gid_opt = safe_stoll(get_param_or_default(req, "group_id", "0"));
    auto limit_opt = safe_stoi(get_param_or_default(req, "limit", "50"));
    if (!gid_opt || !limit_opt) { set_error_response(res, 400, "Invalid parameters"); return; }
    auto group_id = *gid_opt;
    auto level = req.get_param_value("level");
    auto limit = *limit_opt;
    auto mems = db_->get_memories(group_id, level, limit);
    protocol::json arr = protocol::json::array();
    for (auto& m : mems) arr.push_back(m);
    set_json_response(res, protocol::json{{"memories", arr}});
}

void HttpServer::handle_post_memories_summarize(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    try {
        auto j = protocol::json::parse(req.body);
        auto id = db_->summarize_now(j.value("group_id", 0), j.value("level", "24h"));
        set_json_response(res, protocol::json{{"task_id", id}, {"status", "queued"}});
    } catch (...) {
        set_error_response(res, 400, "Invalid JSON");
    }
}

void HttpServer::handle_post_memories_importance(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    try {
        auto j = protocol::json::parse(req.body);
        auto msg_id = j.value("msg_id", 0);
        auto score = j.value("score", 0.5f);
        auto reason = j.value("reason", "");
        if (db_->summarizer()) {
            db_->summarizer()->mark_importance(msg_id, score, reason);
        }
        set_json_response(res, protocol::json{{"status", "marked"}});
    } catch (...) {
        set_error_response(res, 400, "Invalid JSON");
    }
}

void HttpServer::handle_post_memories_merge(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    try {
        auto j = protocol::json::parse(req.body);
        auto ids = j.value("mem_ids", std::vector<int64_t>{});
        auto new_summary = j.value("new_summary", "");
        bool ok = false;
        if (db_->summarizer()) {
            ok = db_->summarizer()->merge_memories(ids, new_summary);
        }
        set_json_response(res, protocol::json{{"status", ok ? "merged" : "failed"}});
    } catch (...) {
        set_error_response(res, 400, "Invalid JSON");
    }
}

void HttpServer::handle_delete_memory(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    auto mid_opt = safe_stoll(req.matches[1]);
    if (!mid_opt) { set_error_response(res, 400, "Invalid memory ID"); return; }
    auto mem_id = *mid_opt;
    bool ok = false;
    if (db_->summarizer()) {
        ok = db_->summarizer()->delete_memory(mem_id);
    }
    set_json_response(res, protocol::json{{"status", ok ? "deleted" : "failed"}, {"mem_id", mem_id}});
}

void HttpServer::handle_get_providers(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    (void)req;
    auto names = db_->list_providers();
    protocol::json arr = protocol::json::array();
    for (auto& n : names) arr.push_back(n);
    set_json_response(res, protocol::json{{"providers", arr}});
}

void HttpServer::handle_post_providers_switch(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    try {
        auto j = protocol::json::parse(req.body);
        auto name = j.value("name", "ollama");
        bool ok = db_->switch_provider(name);
        set_json_response(res, protocol::json{{"switched", ok}, {"provider", name}});
    } catch (...) {
        set_error_response(res, 400, "Invalid JSON");
    }
}

void HttpServer::handle_get_providers_status(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    (void)req;
    auto current = db_->provider_manager() ? db_->provider_manager()->current() : nullptr;
    set_json_response(res, protocol::json{
        {"current", current ? current->name() : "none"},
        {"available", db_->list_providers()}
    });
}

void HttpServer::handle_get_stats(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    (void)req;
    auto stats = db_->get_stats();
    set_json_response(res, protocol::json{{"stats", stats}});
}

void HttpServer::handle_get_health(const httplib::Request& req, httplib::Response& res) {
    (void)req;
    set_json_response(res, protocol::json{{"status", "ok"}, {"version", "2.1.0"}, {"timestamp", time(nullptr)}});
}

void HttpServer::handle_post_backup(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    (void)req;
    bool ok = db_->backup_index();
    set_json_response(res, protocol::json{{"backup", ok ? "success" : "failed"}});
}

void HttpServer::handle_post_restore(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    try {
        auto j = protocol::json::parse(req.body);
        auto name = j.value("name", "");
        bool ok = db_->restore_index(name);
        set_json_response(res, protocol::json{{"restore", ok ? "success" : "failed"}, {"name", name}});
    } catch (...) {
        set_error_response(res, 400, "Invalid JSON");
    }
}

void HttpServer::handle_post_vacuum(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    (void)req;
    db_->vacuum();
    set_json_response(res, protocol::json{{"vacuum", "done"}});
}

void HttpServer::handle_post_cleanup(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    try {
        auto j = protocol::json::parse(req.body);
        int days = j.value("days", 7);
        int deleted = db_->cleanup_old_data(days);
        set_json_response(res, protocol::json{{"deleted", deleted}});
    } catch (...) {
        set_error_response(res, 400, "Invalid JSON");
    }
}

void HttpServer::handle_get_config(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    (void)req;
    set_json_response(res, protocol::json{{"status", "TODO"}});
}

void HttpServer::handle_put_config(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    (void)req;
    set_json_response(res, protocol::json{{"status", "TODO"}});
}

void HttpServer::handle_sse_events(const httplib::Request& req, httplib::Response& res) {
    if (!check_auth(req, res)) return;
    (void)req;

    auto client = std::make_shared<SseClient>();
    client->id = fmt::format("sse_{}", std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        sse_clients_.erase(
            std::remove_if(sse_clients_.begin(), sse_clients_.end(),
                           [](const std::weak_ptr<SseClient>& wp) { return wp.expired(); }),
            sse_clients_.end());
        sse_clients_.push_back(client);
    }

    res.set_header("Content-Type", "text/event-stream");
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    if (cfg_.enable_cors) res.set_header("Access-Control-Allow-Origin", "*");

    client->push(fmt::format("event: connected\ndata: {{\"client_id\":\"{}\"}}\n\n", client->id));

    res.set_content_provider("text/event-stream",
        [client, this](size_t /*offset*/, httplib::DataSink& sink) {
            std::string event;
            while (client->active.load() && running_.load()) {
                if (client->pop(event, std::chrono::milliseconds(30000))) {
                    auto n = sink.write(event.c_str(), event.size());
                    if (n != event.size()) {
                        client->active = false;
                        break;
                    }
                }
                sink.write(":\n\n", 3);
            }
            sink.done();
            client->active = false;
            return false;
        }
    );
}

void HttpServer::broadcast_event(const std::string& event_name, const protocol::json& data) {
    std::string payload = fmt::format("event: {}\ndata: {}\n\n", event_name, data.dump());

    std::lock_guard<std::mutex> lock(sse_mutex_);
    sse_clients_.erase(
        std::remove_if(sse_clients_.begin(), sse_clients_.end(),
                       [](const std::weak_ptr<SseClient>& wp) { return wp.expired(); }),
        sse_clients_.end());

    for (auto& wp : sse_clients_) {
        if (auto sp = wp.lock()) {
            sp->push(payload);
        }
    }
    spdlog::debug("Broadcast event '{}' to {} clients", event_name, sse_clients_.size());
}

} // namespace chatdb
