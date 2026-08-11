#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace chatdb {

// ===== 共享数据结构（从各模块头文件统一提取，避免重复定义） =====

struct RawMessage {
    int64_t group_id = 0;
    int64_t qq_id = 0;
    std::string nickname;
    std::string content;
    int msg_type = 1;
    int64_t timestamp = 0;
};

struct ProcessedMessage {
    int64_t id = 0;
    int64_t group_id = 0;
    int64_t qq_id = 0;
    std::string nickname;
    std::string content;
    int msg_type = 1;
    int64_t timestamp = 0;
    std::string msg_hash;
    std::vector<float> embedding;
    bool is_duplicate = false;
};

struct ActiveChatRequest {
    int64_t group_id = 0;
    std::string topic;
    std::string suggested_content;
    float urgency = 0.5f; // 0.0~1.0，决定是否真的发送
};

struct SearchResult {
    int64_t msg_id = 0;
    int64_t group_id = 0;
    int64_t qq_id = 0;
    std::string nickname;
    std::string content;
    int64_t timestamp = 0;
    float relevance_score = 0.0f; // 综合相关度分数
    bool is_semantic_match = false;
    bool is_fulltext_match = false;
};

namespace protocol {

using json = nlohmann::json;

struct Request {
    std::string id;
    std::string method;
    json params;
    int64_t timestamp = 0;
};

struct Response {
    std::string id;
    bool success = false;
    json result;
    std::string error;
    int64_t timestamp = 0;
};

inline std::string serialize_request(const Request& req) {
    json j = {{"id", req.id}, {"method", req.method}, {"params", req.params}, {"ts", req.timestamp}};
    return j.dump();
}

inline std::string serialize_response(const Response& resp) {
    json j = {{"id", resp.id}, {"success", resp.success}, {"result", resp.result}, {"error", resp.error}, {"ts", resp.timestamp}};
    return j.dump();
}

inline Request parse_request(const std::string& data) {
    auto j = json::parse(data, nullptr, false);
    Request req;
    if (!j.is_discarded()) {
        req.id = j.value("id", "");
        req.method = j.value("method", "");
        req.params = j.value("params", json::object());
        req.timestamp = j.value("ts", 0);
    }
    return req;
}

inline Response parse_response(const std::string& data) {
    auto j = json::parse(data, nullptr, false);
    Response resp;
    if (!j.is_discarded()) {
        resp.id = j.value("id", "");
        resp.success = j.value("success", false);
        resp.result = j.value("result", json::object());
        resp.error = j.value("error", "");
        resp.timestamp = j.value("ts", 0);
    }
    return resp;
}

namespace methods {
    constexpr const char* MSG_RECEIVE = "msg.receive";
    constexpr const char* MSG_BATCH = "msg.batch";
    constexpr const char* MSG_RECALL = "msg.recall";
    constexpr const char* MSG_GET = "msg.get";
    constexpr const char* MSG_CONTEXT = "msg.context";
    constexpr const char* SEARCH_FULLTEXT = "search.fulltext";
    constexpr const char* SEARCH_SEMANTIC = "search.semantic";
    constexpr const char* SEARCH_HYBRID = "search.hybrid";
    constexpr const char* SEARCH_TIME = "search.time";
    constexpr const char* SEARCH_MARKED = "search.marked";
    constexpr const char* SEARCH_REF = "search.ref";
    constexpr const char* MEM_GET = "mem.get";
    constexpr const char* MEM_LIST = "mem.list";
    constexpr const char* MEM_IMPORTANCE = "mem.set_importance";
    constexpr const char* MEM_MERGE = "mem.merge";
    constexpr const char* MEM_DELETE = "mem.delete";
    constexpr const char* MEM_SUMMARIZE = "mem.summarize";
    constexpr const char* MEM_ACTIVE_CHAT = "mem.active_chat";
    constexpr const char* CFG_GET = "cfg.get";
    constexpr const char* CFG_SET = "cfg.set";
    constexpr const char* CFG_RELOAD = "cfg.reload";
    constexpr const char* PROVIDER_LIST = "provider.list";
    constexpr const char* PROVIDER_SWITCH = "provider.switch";
    constexpr const char* PROVIDER_STATUS = "provider.status";
    constexpr const char* SYS_STATS = "sys.stats";
    constexpr const char* SYS_HEALTH = "sys.health";
    constexpr const char* SYS_BACKUP = "sys.backup";
    constexpr const char* SYS_RESTORE = "sys.restore";
    constexpr const char* SYS_VACUUM = "sys.vacuum";
    constexpr const char* SYS_CLEANUP = "sys.cleanup";
}

namespace events {
    constexpr const char* MSG_NEW = "evt.msg_new";
    constexpr const char* MEM_SUMMARY = "evt.mem_summary";
    constexpr const char* MEM_ACTIVE = "evt.mem_active";
    constexpr const char* SYS_ALERT = "evt.sys_alert";
    constexpr const char* PROVIDER_CHANGED = "evt.provider_changed";
}

struct MemoryEntry {
    int64_t id = -1;
    int64_t group_id = 0;
    std::string summary;
    std::string detail;
    std::string category;
    std::string level;
    int64_t start_time = 0;
    int64_t end_time = 0;
    int64_t created_at = 0;
    std::vector<int64_t> msg_ids;
    std::vector<std::string> tags;
    float importance_score = 0.5f;
    bool is_manual = false;
    std::string source_provider;
};

inline void to_json(json& j, const MemoryEntry& m) {
    j = json{{"id", m.id}, {"group_id", m.group_id}, {"summary", m.summary},
             {"detail", m.detail}, {"category", m.category}, {"level", m.level},
             {"start_time", m.start_time}, {"end_time", m.end_time},
             {"created_at", m.created_at}, {"msg_ids", m.msg_ids},
             {"tags", m.tags}, {"importance_score", m.importance_score},
             {"is_manual", m.is_manual}, {"source_provider", m.source_provider}};
}

struct EmbeddingProviderConfig {
    std::string name;
    std::string display_name;
    std::string api_base;
    std::string api_key;
    std::string model;
    int timeout_ms = 60000;
    int embedding_dim = 768;
    int max_retries = 3;
    bool is_active = false;
};

inline void to_json(json& j, const EmbeddingProviderConfig& p) {
    j = json{{"name", p.name}, {"display_name", p.display_name},
             {"api_base", p.api_base}, {"model", p.model},
             {"timeout_ms", p.timeout_ms}, {"embedding_dim", p.embedding_dim},
             {"is_active", p.is_active}};
}

} // namespace protocol
} // namespace chatdb
