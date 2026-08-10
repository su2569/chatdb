#include <optional>
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace chatdb {

class SQLiteStorage;
class RedisClient;
class EmbeddingProviderManager;
class OllamaClient;
struct Message;
struct VectorQueryResult;

enum class SearchMode {
    FULLTEXT_ONLY,      // 仅SQLite FTS5全文搜索
    SEMANTIC_ONLY,      // 仅Redis向量语义搜索
    HYBRID,             // 混合：全文+语义，加权排序
    TIME_RANGE,         // 按时间范围查询
    RECENT              // 最近N条
};

struct SearchResult {
    int64_t msg_id;
    int64_t group_id;
    int64_t qq_id;
    std::string nickname;
    std::string content;
    int64_t timestamp;
    float relevance_score;  // 综合相关度分数
    bool is_semantic_match;
    bool is_fulltext_match;
};

struct SearchRequest {
    SearchMode mode = SearchMode::HYBRID;
    std::string query;           // 搜索关键词/句子
    int64_t group_id = 0;        // 0=所有群
    int64_t qq_id = 0;           // 0=所有用户
    int64_t start_time = 0;      // 时间范围开始
    int64_t end_time = 0;        // 时间范围结束
    int limit = 20;              // 返回条数
    float min_similarity = 0.70f; // 语义相似度阈值
    float semantic_weight = 0.6f; // 混合搜索中语义权重
    float fulltext_weight = 0.4f; // 混合搜索中全文权重
};

class QueryEngine {
public:
    QueryEngine(SQLiteStorage* sqlite, RedisClient* redis, EmbeddingProviderManager* provider_mgr);
    ~QueryEngine() = default;

    // 主搜索接口
    std::vector<SearchResult> search(const SearchRequest& req);

    // 快捷查询
    std::vector<SearchResult> search_fulltext(const std::string& keyword, 
                                               int64_t group_id = 0, 
                                               int limit = 20);

    std::vector<SearchResult> search_semantic(const std::string& sentence,
                                               int64_t group_id = 0,
                                               int limit = 20,
                                               float min_sim = 0.75f);

    std::vector<SearchResult> search_hybrid(const std::string& query,
                                             int64_t group_id = 0,
                                             int limit = 20);

    std::vector<SearchResult> get_recent(int64_t group_id, int limit = 50);
    std::vector<SearchResult> get_by_time_range(int64_t group_id, 
                                                 int64_t start, 
                                                 int64_t end, 
                                                 int limit = 100);

    // 获取单条消息详情
    std::optional<SearchResult> get_message(int64_t msg_id);

    // 获取上下文（某条消息前后N条）
    std::vector<SearchResult> get_context(int64_t msg_id, int radius = 5);

    // 统计
    int64_t count_messages(int64_t group_id = 0);
    int64_t count_today(int64_t group_id = 0);

private:
    std::vector<SearchResult> merge_results(
        const std::vector<Message>& fts_results,
        const std::vector<SearchResult>& semantic_results,
        float semantic_weight,
        float fulltext_weight,
        int limit);

    float normalize_score(float score, float min_val, float max_val);

    SQLiteStorage* sqlite_;
    RedisClient* redis_;
    EmbeddingProviderManager* provider_mgr_;
    OllamaClient* ollama_;  // deprecated, kept for compat
};

} // namespace chatdb
