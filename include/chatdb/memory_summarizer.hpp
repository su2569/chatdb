#pragma once
#include "chatdb/protocol.hpp"

namespace chatdb {
class EmbeddingProviderManager;
}
#include "chatdb/sqlite_storage.hpp"
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>

namespace chatdb {

class EmbeddingProvider;
class RedisClient;

// 记忆总结任务
struct SummaryTask {
    int64_t group_id = 0;
    std::string level;        // "3h" | "12h" | "24h" | "month" | "year" | "3year"
    int64_t start_time = 0;
    int64_t end_time = 0;
    bool is_manual = false;
    std::string triggered_by; // "cron" | "manual" | "ai"
};

// 主动聊天请求
struct ActiveChatRequest {
    int64_t group_id = 0;
    std::string topic;
    std::string suggested_content;
    float urgency = 0.5f;     // 0.0~1.0，决定是否真的发送
};

class MemorySummarizer {
public:
    struct Config {
        int summary_3h_interval = 3 * 3600;       // 3小时
        int summary_12h_interval = 12 * 3600;     // 12小时
        int summary_24h_interval = 24 * 3600;     // 24小时
        int summary_month_interval = 30 * 24 * 3600;  // 1月
        int summary_year_interval = 365 * 24 * 3600;  // 1年
        int summary_3year_interval = 3 * 365 * 24 * 3600; // 3年

        float active_chat_threshold = 0.7f;       // 主动聊天阈值
        int max_daily_active_chat = 3;            // 每天最多主动聊天次数

        // 分类阈值
        float important_threshold = 0.75f;        // 重要消息分数阈值
    };

    MemorySummarizer(SQLiteStorage* sqlite, RedisClient* redis, 
                     EmbeddingProviderManager* provider_mgr);
    ~MemorySummarizer();

    MemorySummarizer(const MemorySummarizer&) = delete;
    MemorySummarizer& operator=(const MemorySummarizer&) = delete;

    void start();
    void stop();

    // 手动触发总结
    int64_t summarize_now(int64_t group_id, const std::string& level, bool is_manual = true);

    // 处理撤回消息
    void handle_recall(int64_t group_id, int64_t msg_id, const std::string& content, 
                       bool is_important);

    // 设置消息重要性（人工或AI标记）
    void mark_importance(int64_t msg_id, float score, const std::string& reason);

    // 获取记忆
    std::vector<protocol::MemoryEntry> get_memories(int64_t group_id, 
                                                     const std::string& level = "",
                                                     const std::string& category = "",
                                                     int limit = 50);

    // 搜索记忆（索引、时间、引用、标记）
    std::vector<protocol::MemoryEntry> search_memories(int64_t group_id,
                                                        const std::string& query,
                                                        const std::string& search_type = "hybrid");

    // 合并/删除记忆（人工整理）
    bool merge_memories(const std::vector<int64_t>& mem_ids, const std::string& new_summary);
    bool delete_memory(int64_t mem_id);
    bool update_memory(int64_t mem_id, const protocol::MemoryEntry& entry);

    // 主动聊天
    void set_active_chat_callback(std::function<void(const ActiveChatRequest&)> cb);

    // 获取待处理任务数
    size_t pending_tasks() const;

    // 统计
    uint64_t total_summaries() const { return total_summaries_.load(); }

private:
    void worker_loop();
    void cron_loop();
    void do_summarize(const SummaryTask& task);
    void do_3h_summary(int64_t group_id, int64_t start, int64_t end);
    void do_12h_summary(int64_t group_id, int64_t start, int64_t end);
    void do_24h_summary(int64_t group_id, int64_t start, int64_t end);
    void do_month_summary(int64_t group_id, int64_t start, int64_t end);
    void do_year_summary(int64_t group_id, int64_t start, int64_t end);
    void do_3year_summary(int64_t group_id, int64_t start, int64_t end);

    std::string generate_summary(const std::vector<Message>& msgs, const std::string& level);
    float calculate_importance(const Message& msg);
    void maybe_trigger_active_chat(int64_t group_id, const std::string& topic);

    bool create_memory_tables();
    int64_t save_memory(const protocol::MemoryEntry& entry);

    SQLiteStorage* sqlite_;
    RedisClient* redis_;
    EmbeddingProviderManager* provider_mgr_;
    Config cfg_;

    std::queue<SummaryTask> task_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread worker_thread_;
    std::thread cron_thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> total_summaries_{0};

    std::function<void(const ActiveChatRequest&)> active_chat_cb_;
};

} // namespace chatdb
