#include "chatdb/query_engine.hpp"
#include "chatdb/sqlite_storage.hpp"
#include "chatdb/redis_client.hpp"
#include "chatdb/ollama_client.hpp"
#include <optional>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace chatdb {

QueryEngine::QueryEngine(SQLiteStorage* sqlite, RedisClient* redis, OllamaClient* ollama)
    : sqlite_(sqlite), redis_(redis), ollama_(ollama) {}

std::vector<SearchResult> QueryEngine::search(const SearchRequest& req) {
    switch (req.mode) {
        case SearchMode::FULLTEXT_ONLY:
            return search_fulltext(req.query, req.group_id, req.limit);
        case SearchMode::SEMANTIC_ONLY:
            return search_semantic(req.query, req.group_id, req.limit, req.min_similarity);
        case SearchMode::HYBRID:
            return search_hybrid(req.query, req.group_id, req.limit);
        case SearchMode::TIME_RANGE:
            return get_by_time_range(req.group_id, req.start_time, req.end_time, req.limit);
        case SearchMode::RECENT:
            return get_recent(req.group_id, req.limit);
        default:
            return search_hybrid(req.query, req.group_id, req.limit);
    }
}

std::vector<SearchResult> QueryEngine::search_fulltext(const std::string& keyword, 
                                                        int64_t group_id, 
                                                        int limit) {
    auto msgs = sqlite_->fulltext_search(keyword, group_id, limit);

    std::vector<SearchResult> results;
    results.reserve(msgs.size());
    for (const auto& m : msgs) {
        SearchResult r;
        r.msg_id = m.id;
        r.group_id = m.group_id;
        r.qq_id = m.qq_id;
        r.nickname = m.nickname;
        r.content = m.content;
        r.timestamp = m.timestamp;
        r.relevance_score = 1.0f;
        r.is_fulltext_match = true;
        r.is_semantic_match = false;
        results.push_back(r);
    }
    return results;
}

std::vector<SearchResult> QueryEngine::search_semantic(const std::string& sentence,
                                                        int64_t group_id,
                                                        int limit,
                                                        float min_sim) {
    std::vector<SearchResult> results;
    if (!ollama_ || !redis_ || !redis_->is_connected() || !redis_->index_exists()) {
        spdlog::warn("Semantic search unavailable: Ollama/Redis not ready or no index");
        return results;
    }

    try {
        auto query_vec = ollama_->embed(sentence);
        if (query_vec.empty()) {
            spdlog::warn("Empty embedding returned from Ollama");
            return results;
        }

        auto vec_results = redis_->search_similar(group_id, query_vec, limit, min_sim);

        results.reserve(vec_results.size());
        for (const auto& vr : vec_results) {
            auto msg_opt = sqlite_->get_message_by_id(vr.msg_id);
            if (!msg_opt) continue;

            SearchResult r;
            r.msg_id = vr.msg_id;
            r.group_id = msg_opt->group_id;
            r.qq_id = msg_opt->qq_id;
            r.nickname = msg_opt->nickname;
            r.content = msg_opt->content;
            r.timestamp = msg_opt->timestamp;
            r.relevance_score = vr.similarity;
            r.is_semantic_match = true;
            r.is_fulltext_match = false;
            results.push_back(r);
        }

        spdlog::debug("Semantic search found {} results", results.size());
    } catch (const std::exception& e) {
        spdlog::error("Semantic search failed: {}", e.what());
    }

    return results;
}

std::vector<SearchResult> QueryEngine::search_hybrid(const std::string& query,
                                                      int64_t group_id,
                                                      int limit) {
    // 同时执行全文搜索和语义搜索（并行）
    auto fts_future = std::async(std::launch::async, [this, query, group_id, limit]() {
        return sqlite_->fulltext_search(query, group_id, limit * 2);
    });

    auto semantic_results = search_semantic(query, group_id, limit * 2, 0.60f);
    auto fts_results = fts_future.get();

    return merge_results(fts_results, semantic_results, 0.6f, 0.4f, limit);
}

std::vector<SearchResult> QueryEngine::get_recent(int64_t group_id, int limit) {
    auto msgs = sqlite_->query_by_group(group_id, limit, 0);

    std::vector<SearchResult> results;
    results.reserve(msgs.size());
    for (const auto& m : msgs) {
        SearchResult r;
        r.msg_id = m.id;
        r.group_id = m.group_id;
        r.qq_id = m.qq_id;
        r.nickname = m.nickname;
        r.content = m.content;
        r.timestamp = m.timestamp;
        r.relevance_score = 0.0f;
        r.is_fulltext_match = false;
        r.is_semantic_match = false;
        results.push_back(r);
    }
    return results;
}

std::vector<SearchResult> QueryEngine::get_by_time_range(int64_t group_id, 
                                                          int64_t start, 
                                                          int64_t end, 
                                                          int limit) {
    auto msgs = sqlite_->query_by_group_time(group_id, start, end, limit);

    std::vector<SearchResult> results;
    results.reserve(msgs.size());
    for (const auto& m : msgs) {
        SearchResult r;
        r.msg_id = m.id;
        r.group_id = m.group_id;
        r.qq_id = m.qq_id;
        r.nickname = m.nickname;
        r.content = m.content;
        r.timestamp = m.timestamp;
        r.relevance_score = 0.0f;
        r.is_fulltext_match = false;
        r.is_semantic_match = false;
        results.push_back(r);
    }
    return results;
}

std::optional<SearchResult> QueryEngine::get_message(int64_t msg_id) {
    auto msg_opt = sqlite_->get_message_by_id(msg_id);
    if (!msg_opt) return std::nullopt;

    SearchResult r;
    r.msg_id = msg_opt->id;
    r.group_id = msg_opt->group_id;
    r.qq_id = msg_opt->qq_id;
    r.nickname = msg_opt->nickname;
    r.content = msg_opt->content;
    r.timestamp = msg_opt->timestamp;
    r.relevance_score = 1.0f;
    r.is_fulltext_match = false;
    r.is_semantic_match = false;
    return r;
}

std::vector<SearchResult> QueryEngine::get_context(int64_t msg_id, int radius) {
    auto msg_opt = sqlite_->get_message_by_id(msg_id);
    if (!msg_opt) return {};

    auto all = sqlite_->query_by_group(msg_opt->group_id, radius * 2 + 1, 0);

    std::vector<SearchResult> results;
    for (const auto& m : all) {
        if (std::llabs(m.timestamp - msg_opt->timestamp) <= radius * 60) {
            SearchResult r;
            r.msg_id = m.id;
            r.group_id = m.group_id;
            r.qq_id = m.qq_id;
            r.nickname = m.nickname;
            r.content = m.content;
            r.timestamp = m.timestamp;
            results.push_back(r);
        }
    }
    return results;
}

int64_t QueryEngine::count_messages(int64_t group_id) {
    return sqlite_->count_messages(group_id);
}

int64_t QueryEngine::count_today(int64_t group_id) {
    auto now = std::chrono::system_clock::now();
    auto start = now - std::chrono::hours(24);
    int64_t start_ts = std::chrono::duration_cast<std::chrono::seconds>(start.time_since_epoch()).count();
    int64_t end_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    return sqlite_->count_messages_by_time(group_id, start_ts, end_ts);
}

std::vector<SearchResult> QueryEngine::merge_results(
    const std::vector<Message>& fts_results,
    const std::vector<SearchResult>& semantic_results,
    float semantic_weight,
    float fulltext_weight,
    int limit) {

    std::unordered_map<int64_t, SearchResult> merged;

    // 加入全文结果
    for (const auto& m : fts_results) {
        SearchResult r;
        r.msg_id = m.id;
        r.group_id = m.group_id;
        r.qq_id = m.qq_id;
        r.nickname = m.nickname;
        r.content = m.content;
        r.timestamp = m.timestamp;
        r.relevance_score = fulltext_weight;
        r.is_fulltext_match = true;
        r.is_semantic_match = false;
        merged[m.id] = r;
    }

    // 合并语义结果
    for (const auto& sr : semantic_results) {
        auto it = merged.find(sr.msg_id);
        if (it != merged.end()) {
            // 同时命中全文和语义
            it->second.relevance_score = semantic_weight * sr.relevance_score 
                                         + fulltext_weight * it->second.relevance_score;
            it->second.is_semantic_match = true;
        } else {
            merged[sr.msg_id] = sr;
        }
    }

    // 转 vector 并排序
    std::vector<SearchResult> results;
    results.reserve(merged.size());
    for (auto& [id, r] : merged) {
        results.push_back(std::move(r));
    }

    std::sort(results.begin(), results.end(), 
              [](const auto& a, const auto& b) { return a.relevance_score > b.relevance_score; });

    if (results.size() > static_cast<size_t>(limit)) {
        results.resize(limit);
    }
    return results;
}

} // namespace chatdb
