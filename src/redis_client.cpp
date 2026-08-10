#include "chatdb/redis_client.hpp"
#include "chatdb/embedding_provider.hpp"
#include <hiredis/hiredis.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <cstring>
#include <chrono>

namespace chatdb {

struct RedisClient::Impl {
    redisContext* ctx = nullptr;
};

RedisClient::RedisClient(const Config& cfg) : cfg_(cfg), impl_(std::make_unique<Impl>()) {}

RedisClient::~RedisClient() { disconnect(); }

bool RedisClient::connect() {
    disconnect();
    struct timeval timeout = {cfg_.connection_timeout_ms / 1000, (cfg_.connection_timeout_ms % 1000) * 1000};
    impl_->ctx = redisConnectWithTimeout(cfg_.host.c_str(), cfg_.port, timeout);
    if (!impl_->ctx || impl_->ctx->err) {
        spdlog::error("Redis connect failed: {}", impl_->ctx ? impl_->ctx->errstr : "null");
        disconnect();
        return false;
    }
    if (!cfg_.password.empty()) {
        auto* reply = (redisReply*)redisCommand(impl_->ctx, "AUTH %s", cfg_.password.c_str());
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            spdlog::error("Redis AUTH failed");
            if (reply) freeReplyObject(reply);
            disconnect();
            return false;
        }
        if (reply) freeReplyObject(reply);
    }
    if (cfg_.db != 0) {
        auto* reply = (redisReply*)redisCommand(impl_->ctx, "SELECT %d", cfg_.db);
        if (reply) freeReplyObject(reply);
    }
    connected_ = true;
    spdlog::info("Redis connected: {}:{}", cfg_.host, cfg_.port);
    return true;
}

void RedisClient::disconnect() {
    connected_ = false;
    if (impl_->ctx) {
        redisFree(impl_->ctx);
        impl_->ctx = nullptr;
    }
}

bool RedisClient::is_connected() const {
    return connected_ && impl_->ctx;
}

// ========== 去重 ==========
bool RedisClient::is_duplicate(int64_t group_id, const std::string& content, int64_t window_seconds) {
    if (!is_connected()) return false;
    auto key = make_dup_key(group_id);
    std::hash<std::string> hasher;
    auto hash = std::to_string(hasher(content));
    auto* reply = (redisReply*)redisCommand(impl_->ctx, "SISMEMBER %s %s", key.c_str(), hash.c_str());
    bool dup = false;
    if (reply && reply->type == REDIS_REPLY_INTEGER) {
        dup = reply->integer == 1;
    }
    if (reply) freeReplyObject(reply);
    return dup;
}

void RedisClient::mark_duplicate(int64_t group_id, const std::string& content, int64_t window_seconds) {
    if (!is_connected()) return;
    auto key = make_dup_key(group_id);
    std::hash<std::string> hasher;
    auto hash = std::to_string(hasher(content));
    auto* reply = (redisReply*)redisCommand(impl_->ctx, "SADD %s %s", key.c_str(), hash.c_str());
    if (reply) freeReplyObject(reply);
    reply = (redisReply*)redisCommand(impl_->ctx, "EXPIRE %s %lld", key.c_str(), window_seconds * 2);
    if (reply) freeReplyObject(reply);
}

// ========== 向量序列化 ==========
std::string RedisClient::serialize_vector(const std::vector<float>& vec) {
    std::string s;
    s.reserve(vec.size() * sizeof(float));
    for (float f : vec) {
        s.append(reinterpret_cast<const char*>(&f), sizeof(float));
    }
    return s;
}

std::vector<float> RedisClient::deserialize_vector(const std::string& s) {
    std::vector<float> vec;
    vec.reserve(s.size() / sizeof(float));
    for (size_t i = 0; i + sizeof(float) <= s.size(); i += sizeof(float)) {
        float f;
        memcpy(&f, s.data() + i, sizeof(float));
        vec.push_back(f);
    }
    return vec;
}

// ========== 向量索引 ==========
bool RedisClient::add_vector(int64_t msg_id, int64_t group_id, int64_t qq_id,
                              const std::string& content, int64_t timestamp,
                              const std::vector<float>& embedding) {
    if (!is_connected()) return false;
    // 算法辅助：存储前归一化，加速后续余弦相似度计算
    auto norm_embedding = embedding;
    EmbeddingProvider::normalize(norm_embedding);

    auto key = make_vector_key(msg_id);
    auto* reply = (redisReply*)redisCommand(impl_->ctx,
        "HMSET %s msg_id %lld group_id %lld qq_id %lld content %s timestamp %lld embedding %b",
        key.c_str(), msg_id, group_id, qq_id, content.c_str(), timestamp,
        serialize_vector(norm_embedding).c_str(), norm_embedding.size() * sizeof(float));
    bool ok = reply && reply->type != REDIS_REPLY_ERROR;
    if (reply) freeReplyObject(reply);
    return ok;
}

std::vector<VectorQueryResult> RedisClient::search_similar(int64_t group_id,
                                                              const std::vector<float>& query_vec,
                                                              int limit, float min_similarity) {
    std::vector<VectorQueryResult> results;
    if (!is_connected() || !index_created_) return results;

    // 使用 RedisJSON + RediSearch 的 FT.SEARCH
    // 简化：遍历所有 vec:* key 计算余弦相似度
    auto* reply = (redisReply*)redisCommand(impl_->ctx, "KEYS vec:*");
    if (!reply || reply->type != REDIS_REPLY_ARRAY) {
        if (reply) freeReplyObject(reply);
        return results;
    }

    std::vector<VectorQueryResult> candidates;
    for (size_t i = 0; i < reply->elements; ++i) {
        if (reply->element[i]->type != REDIS_REPLY_STRING) continue;
        std::string key = reply->element[i]->str;

        auto* hreply = (redisReply*)redisCommand(impl_->ctx, "HMGET %s msg_id group_id qq_id content timestamp embedding", key.c_str());
        if (!hreply || hreply->type != REDIS_REPLY_ARRAY || hreply->elements < 6) {
            if (hreply) freeReplyObject(hreply);
            continue;
        }

        VectorQueryResult vr;
        if (hreply->element[0] && hreply->element[0]->type == REDIS_REPLY_STRING) {
            try { vr.msg_id = std::stoll(hreply->element[0]->str); } catch (...) { vr.msg_id = 0; }
        }
        if (hreply->element[1] && hreply->element[1]->type == REDIS_REPLY_STRING) {
            try { vr.group_id = std::stoll(hreply->element[1]->str); } catch (...) { vr.group_id = 0; }
        }
        if (hreply->element[2] && hreply->element[2]->type == REDIS_REPLY_STRING) {
            try { vr.qq_id = std::stoll(hreply->element[2]->str); } catch (...) { vr.qq_id = 0; }
        }
        if (hreply->element[3] && hreply->element[3]->type == REDIS_REPLY_STRING)
            vr.content = hreply->element[3]->str;
        if (hreply->element[4] && hreply->element[4]->type == REDIS_REPLY_STRING) {
            try { vr.timestamp = std::stoll(hreply->element[4]->str); } catch (...) { vr.timestamp = 0; }
        }

        if (hreply->element[5] && hreply->element[5]->type == REDIS_REPLY_STRING) {
            auto emb = deserialize_vector(std::string(hreply->element[5]->str, hreply->element[5]->len));
            // 算法辅助：向量已归一化，只需计算点积即得余弦相似度
            if (emb.size() == query_vec.size() && !emb.empty()) {
                vr.similarity = EmbeddingProvider::cosine_similarity_fast(emb, query_vec);
            }
        }
        if (hreply) freeReplyObject(hreply);

        if (group_id != 0 && vr.group_id != group_id) continue;
        if (vr.similarity >= min_similarity) {
            candidates.push_back(vr);
        }
    }
    if (reply) freeReplyObject(reply);

    // 按相似度排序取 top N
    std::sort(candidates.begin(), candidates.end(),
        [](const auto& a, const auto& b) { return a.similarity > b.similarity; });
    if ((size_t)limit < candidates.size()) candidates.resize(limit);
    return candidates;
}

bool RedisClient::create_vector_index(int dim) {
    if (!is_connected()) return false;
    // 尝试删除旧索引
    auto* reply = (redisReply*)redisCommand(impl_->ctx, "FT.DROPINDEX msg_vec_idx DD");
    if (reply) freeReplyObject(reply);

    // 创建 RediSearch 索引（如果 Redis 安装了 RediSearch 模块）
    reply = (redisReply*)redisCommand(impl_->ctx,
        "FT.CREATE msg_vec_idx ON HASH PREFIX 1 vec: SCHEMA msg_id TAG group_id TAG qq_id TAG content TEXT timestamp NUMERIC embedding VECTOR FLAT 6 DIM %d DISTANCE_METRIC COSINE TYPE FLOAT32",
        dim);
    bool ok = reply && reply->type != REDIS_REPLY_ERROR;
    if (!ok && reply) {
        spdlog::warn("FT.CREATE failed (RediSearch module may not be loaded): {}", reply->str ? reply->str : "unknown");
    }
    if (reply) freeReplyObject(reply);
    index_created_ = ok;
    return ok;
}

bool RedisClient::drop_vector_index() {
    if (!is_connected()) return false;
    auto* reply = (redisReply*)redisCommand(impl_->ctx, "FT.DROPINDEX msg_vec_idx DD");
    if (reply) freeReplyObject(reply);
    index_created_ = false;
    return true;
}

bool RedisClient::index_exists() const {
    return index_created_;
}

int RedisClient::cleanup_expired_vectors(int retention_days) {
    if (!is_connected()) return 0;
    int64_t cutoff = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - retention_days * 86400;

    auto* reply = (redisReply*)redisCommand(impl_->ctx, "KEYS vec:*");
    if (!reply || reply->type != REDIS_REPLY_ARRAY) {
        if (reply) freeReplyObject(reply);
        return 0;
    }

    int deleted = 0;
    for (size_t i = 0; i < reply->elements; ++i) {
        if (reply->element[i]->type != REDIS_REPLY_STRING) continue;
        std::string key = reply->element[i]->str;
        auto* ts_reply = (redisReply*)redisCommand(impl_->ctx, "HGET %s timestamp", key.c_str());
        if (ts_reply && ts_reply->type == REDIS_REPLY_STRING) {
            int64_t ts = 0;
            try { ts = std::stoll(ts_reply->str); } catch (...) { ts = 0; }
            if (ts < cutoff) {
                auto* del_reply = (redisReply*)redisCommand(impl_->ctx, "DEL %s", key.c_str());
                if (del_reply) freeReplyObject(del_reply);
                ++deleted;
            }
        }
        if (ts_reply) freeReplyObject(ts_reply);
    }
    if (reply) freeReplyObject(reply);
    return deleted;
}

// ========== 缓存 ==========
void RedisClient::cache_set(const std::string& key, const std::string& value, int ttl_seconds) {
    if (!is_connected()) return;
    auto* reply = (redisReply*)redisCommand(impl_->ctx, "SETEX %s %d %s", key.c_str(), ttl_seconds, value.c_str());
    if (reply) freeReplyObject(reply);
}

std::optional<std::string> RedisClient::cache_get(const std::string& key) {
    if (!is_connected()) return std::nullopt;
    auto* reply = (redisReply*)redisCommand(impl_->ctx, "GET %s", key.c_str());
    std::optional<std::string> result;
    if (reply && reply->type == REDIS_REPLY_STRING) {
        result = std::string(reply->str, reply->len);
    }
    if (reply) freeReplyObject(reply);
    return result;
}

void RedisClient::cache_del(const std::string& key) {
    if (!is_connected()) return;
    auto* reply = (redisReply*)redisCommand(impl_->ctx, "DEL %s", key.c_str());
    if (reply) freeReplyObject(reply);
}

// ========== 状态 ==========
void RedisClient::set_group_state(int64_t group_id, const std::string& field, const std::string& value) {
    if (!is_connected()) return;
    auto* reply = (redisReply*)redisCommand(impl_->ctx, "HSET %s %s %s",
        make_state_key(group_id).c_str(), field.c_str(), value.c_str());
    if (reply) freeReplyObject(reply);
}

std::optional<std::string> RedisClient::get_group_state(int64_t group_id, const std::string& field) {
    if (!is_connected()) return std::nullopt;
    auto* reply = (redisReply*)redisCommand(impl_->ctx, "HGET %s %s",
        make_state_key(group_id).c_str(), field.c_str());
    std::optional<std::string> result;
    if (reply && reply->type == REDIS_REPLY_STRING) {
        result = std::string(reply->str, reply->len);
    }
    if (reply) freeReplyObject(reply);
    return result;
}

// ========== 队列 ==========
void RedisClient::queue_push(const std::string& queue_name, const std::string& value) {
    if (!is_connected()) return;
    auto* reply = (redisReply*)redisCommand(impl_->ctx, "LPUSH %s %s", queue_name.c_str(), value.c_str());
    if (reply) freeReplyObject(reply);
}

std::optional<std::string> RedisClient::queue_pop(const std::string& queue_name, int timeout_seconds) {
    if (!is_connected()) return std::nullopt;
    auto* reply = (redisReply*)redisCommand(impl_->ctx, "BRPOP %s %d", queue_name.c_str(), timeout_seconds);
    std::optional<std::string> result;
    if (reply && reply->type == REDIS_REPLY_ARRAY && reply->elements >= 2) {
        if (reply->element[1] && reply->element[1]->type == REDIS_REPLY_STRING) {
            result = std::string(reply->element[1]->str, reply->element[1]->len);
        }
    }
    if (reply) freeReplyObject(reply);
    return result;
}

int RedisClient::queue_length(const std::string& queue_name) {
    if (!is_connected()) return 0;
    auto* reply = (redisReply*)redisCommand(impl_->ctx, "LLEN %s", queue_name.c_str());
    int len = 0;
    if (reply && reply->type == REDIS_REPLY_INTEGER) {
        len = static_cast<int>(reply->integer);
    }
    if (reply) freeReplyObject(reply);
    return len;
}

// ========== 统计 ==========
int64_t RedisClient::get_memory_usage() const {
    if (!is_connected()) return 0;
    auto* reply = (redisReply*)redisCommand(impl_->ctx, "INFO memory");
    int64_t mem = 0;
    if (reply && reply->type == REDIS_REPLY_STRING) {
        std::string info(reply->str, reply->len);
        auto pos = info.find("used_memory:");
        if (pos != std::string::npos) {
            try { mem = std::stoll(info.substr(pos + 12)); } catch (...) { mem = 0; }
        }
    }
    if (reply) freeReplyObject(reply);
    return mem;
}

std::string RedisClient::get_info() const {
    if (!is_connected()) return "";
    auto* reply = (redisReply*)redisCommand(impl_->ctx, "INFO");
    std::string info;
    if (reply && reply->type == REDIS_REPLY_STRING) {
        info = std::string(reply->str, reply->len);
    }
    if (reply) freeReplyObject(reply);
    return info;
}

// ========== Key 生成 ==========
std::string RedisClient::make_vector_key(int64_t msg_id) {
    return fmt::format("vec:{}", msg_id);
}

std::string RedisClient::make_dup_key(int64_t group_id) {
    return fmt::format("dup:{}", group_id);
}

std::string RedisClient::make_state_key(int64_t group_id) {
    return fmt::format("state:{}", group_id);
}

} // namespace chatdb
