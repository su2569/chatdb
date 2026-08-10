#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <optional>
#include <mutex>

namespace chatdb {

struct VectorQueryResult {
    int64_t msg_id = -1;
    float similarity = 0.0f;
    std::string content;
    int64_t group_id = 0;
    int64_t qq_id = 0;
    int64_t timestamp = 0;
};

class RedisClient {
public:
    struct Config {
        std::string host = "127.0.0.1";
        int port = 6379;
        std::string password;
        int db = 0;
        int pool_size = 4;
        int connection_timeout_ms = 3000;
        int socket_timeout_ms = 3000;
    };

    explicit RedisClient(const Config& cfg);
    ~RedisClient();

    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;

    bool connect();
    void disconnect();
    bool is_connected() const;

    // 去重
    bool is_duplicate(int64_t group_id, const std::string& content, int64_t window_seconds = 300);
    void mark_duplicate(int64_t group_id, const std::string& content, int64_t window_seconds = 300);

    // 向量操作
    bool add_vector(int64_t msg_id, int64_t group_id, int64_t qq_id, 
                    const std::string& content, int64_t timestamp,
                    const std::vector<float>& embedding);

    std::vector<VectorQueryResult> search_similar(int64_t group_id, 
                                                   const std::vector<float>& query_embedding,
                                                   int top_k = 10,
                                                   float min_similarity = 0.75f);

    // 删除过期向量
    int cleanup_expired_vectors(int retention_days);

    // 缓存
    void cache_set(const std::string& key, const std::string& value, int ttl_seconds);
    std::optional<std::string> cache_get(const std::string& key);
    void cache_del(const std::string& key);

    // 状态管理
    void set_group_state(int64_t group_id, const std::string& field, const std::string& value);
    std::optional<std::string> get_group_state(int64_t group_id, const std::string& field);

    // 队列（用于极端高并发缓冲）
    void queue_push(const std::string& queue_name, const std::string& value);
    std::optional<std::string> queue_pop(const std::string& queue_name, int timeout_seconds = 1);
    int queue_length(const std::string& queue_name);

    // 统计
    int64_t get_memory_usage() const;
    std::string get_info() const;

    // 创建向量索引（启动时调用一次）
    bool create_vector_index(int dim = 768);
    bool drop_vector_index();
    bool index_exists() const;

private:
    Config cfg_;
    class Impl;
    std::unique_ptr<Impl> impl_;

    std::string serialize_vector(const std::vector<float>& vec);
    std::vector<float> deserialize_vector(const std::string& s);
    std::string make_vector_key(int64_t msg_id);
    std::string make_dup_key(int64_t group_id);
    std::string make_state_key(int64_t group_id);

    mutable std::mutex mutex_;
    bool connected_ = false;
    bool index_created_ = false;
};

} // namespace chatdb
