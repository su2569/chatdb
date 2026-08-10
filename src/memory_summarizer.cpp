#include "chatdb/memory_summarizer.hpp"
#include <sqlite3.h>
#include "chatdb/embedding_provider.hpp"
#include "chatdb/redis_client.hpp"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <unordered_set>
#include <chrono>
#include <algorithm>

namespace chatdb {

MemorySummarizer::MemorySummarizer(SQLiteStorage* sqlite, RedisClient* redis,
                                    EmbeddingProviderManager* provider_mgr)
    : sqlite_(sqlite), redis_(redis), provider_mgr_(provider_mgr) {}

MemorySummarizer::~MemorySummarizer() { stop(); }

void MemorySummarizer::start() {
    if (running_) return;
    running_ = true;
    create_memory_tables();
    worker_thread_ = std::thread(&MemorySummarizer::worker_loop, this);
    cron_thread_ = std::thread(&MemorySummarizer::cron_loop, this);
    spdlog::info("MemorySummarizer started");
}

void MemorySummarizer::stop() {
    running_ = false;
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) worker_thread_.join();
    if (cron_thread_.joinable()) cron_thread_.join();
}

bool MemorySummarizer::create_memory_tables() {
    // 记忆表
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS memories (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            group_id INTEGER NOT NULL,
            summary TEXT NOT NULL,
            detail TEXT,
            category TEXT DEFAULT 'daily',
            level TEXT DEFAULT '24h',
            start_time INTEGER NOT NULL,
            end_time INTEGER NOT NULL,
            created_at INTEGER DEFAULT (strftime('%s','now')),
            importance_score REAL DEFAULT 0.5,
            is_manual INTEGER DEFAULT 0,
            source_provider TEXT,
            tags TEXT
        );
        CREATE INDEX IF NOT EXISTS idx_mem_group ON memories(group_id);
        CREATE INDEX IF NOT EXISTS idx_mem_level ON memories(level);
        CREATE INDEX IF NOT EXISTS idx_mem_time ON memories(start_time, end_time);
        CREATE INDEX IF NOT EXISTS idx_mem_category ON memories(category);

        CREATE TABLE IF NOT EXISTS memory_msg_links (
            memory_id INTEGER,
            msg_id INTEGER,
            PRIMARY KEY (memory_id, msg_id)
        );

        CREATE TABLE IF NOT EXISTS msg_importance (
            msg_id INTEGER PRIMARY KEY,
            score REAL DEFAULT 0.5,
            reason TEXT,
            marked_at INTEGER DEFAULT (strftime('%s','now'))
        );
    )";

    // 通过 sqlite_ 的底层 db 执行（需要暴露接口或直接用 SQLiteStorage 的方法）
    // 这里简化：假设 sqlite_ 已创建这些表，或在 sqlite_storage.cpp 中统一管理
    // 实际应在 SQLiteStorage::create_tables() 中加入这些表
    return true;
}

void MemorySummarizer::worker_loop() {
    while (running_) {
        SummaryTask task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return !task_queue_.empty() || !running_; });
            if (!running_) break;
            task = std::move(task_queue_.front());
            task_queue_.pop();
        }
        try {
            do_summarize(task);
            ++total_summaries_;
        } catch (const std::exception& e) {
            spdlog::error("Summarize error: {}", e.what());
        }
    }
}

void MemorySummarizer::cron_loop() {
    auto last_3h = std::chrono::system_clock::now();
    auto last_12h = last_3h;
    auto last_24h = last_3h;
    auto last_month = last_3h;
    auto last_year = last_3h;

    while (running_) {
        std::this_thread::sleep_for(std::chrono::minutes(1));
        if (!running_) break;

        auto now = std::chrono::system_clock::now();
        auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

        // 获取所有活跃的群（从 Redis 或 SQLite 中查询）
        // 简化：假设有 group_ids 列表
        std::vector<int64_t> group_ids = {10001, 10002, 10003};  // 实际应动态获取

        for (auto gid : group_ids) {
            // 3小时总结
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_3h).count() >= cfg_.summary_3h_interval) {
                auto end = now_ts;
                auto start = end - cfg_.summary_3h_interval;
                SummaryTask t{gid, "3h", start, end, false, "cron"};
                task_queue_.push(t);
            }
            // 12小时
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_12h).count() >= cfg_.summary_12h_interval) {
                auto end = now_ts;
                auto start = end - cfg_.summary_12h_interval;
                SummaryTask t{gid, "12h", start, end, false, "cron"};
                task_queue_.push(t);
            }
            // 24小时
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_24h).count() >= cfg_.summary_24h_interval) {
                auto end = now_ts;
                auto start = end - cfg_.summary_24h_interval;
                SummaryTask t{gid, "24h", start, end, false, "cron"};
                task_queue_.push(t);
            }
        }

        if (!task_queue_.empty()) queue_cv_.notify_one();

        last_3h = now;
        last_12h = now;
        last_24h = now;
    }
}

void MemorySummarizer::do_summarize(const SummaryTask& task) {
    auto msgs = sqlite_->query_by_group_time(task.group_id, task.start_time, task.end_time, 10000);
    if (msgs.empty()) return;

    if (task.level == "3h") do_3h_summary(task.group_id, task.start_time, task.end_time);
    else if (task.level == "12h") do_12h_summary(task.group_id, task.start_time, task.end_time);
    else if (task.level == "24h") do_24h_summary(task.group_id, task.start_time, task.end_time);
    else if (task.level == "month") do_month_summary(task.group_id, task.start_time, task.end_time);
    else if (task.level == "year") do_year_summary(task.group_id, task.start_time, task.end_time);
    else if (task.level == "3year") do_3year_summary(task.group_id, task.start_time, task.end_time);
}

void MemorySummarizer::do_3h_summary(int64_t group_id, int64_t start, int64_t end) {
    auto msgs = sqlite_->query_by_group_time(group_id, start, end, 5000);
    if (msgs.size() < 5) return;  // 消息太少不总结

    // 3小时：模糊零碎记忆，只保留重要片段和统计
    std::string summary = fmt::format("[3h] 群{} 在 {} ~ {} 共有 {} 条消息",
                                       group_id, start, end, msgs.size());

    // 提取高频词/关键话题（简化：取出现最多的发送者和内容关键词）
    std::unordered_map<int64_t, int> qq_count;
    for (const auto& m : msgs) qq_count[m.qq_id]++;

    protocol::MemoryEntry mem;
    mem.group_id = group_id;
    mem.summary = summary;
    mem.detail = generate_summary(msgs, "3h");
    mem.level = "3h";
    mem.category = "daily";
    mem.start_time = start;
    mem.end_time = end;
    mem.importance_score = 0.3f;
    for (const auto& m : msgs) mem.msg_ids.push_back(m.id);

    save_memory(mem);
    spdlog::info("3h summary created for group {}", group_id);
}

void MemorySummarizer::do_12h_summary(int64_t group_id, int64_t start, int64_t end) {
    // 12小时：基于3小时总结，进一步模糊不重要的记忆
    // 先获取该时间段内的3h总结
    auto mems = get_memories(group_id, "3h", "", 100);

    std::string summary = fmt::format("[12h] 群{} 半日回顾：共 {} 个时段",
                                       group_id, mems.size());

    protocol::MemoryEntry mem;
    mem.group_id = group_id;
    mem.summary = summary;
    mem.detail = generate_summary({}, "12h");  // 实际应基于3h总结再总结
    mem.level = "12h";
    mem.category = "daily";
    mem.start_time = start;
    mem.end_time = end;
    mem.importance_score = 0.4f;

    save_memory(mem);
}

void MemorySummarizer::do_24h_summary(int64_t group_id, int64_t start, int64_t end) {
    auto msgs = sqlite_->query_by_group_time(group_id, start, end, 20000);

    std::string summary = fmt::format("[24h] 群{} 今日总结：{} 条消息",
                                       group_id, msgs.size());

    protocol::MemoryEntry mem;
    mem.group_id = group_id;
    mem.summary = summary;
    mem.detail = generate_summary(msgs, "24h");
    mem.level = "24h";
    mem.category = "daily";
    mem.start_time = start;
    mem.end_time = end;
    mem.importance_score = 0.5f;
    for (const auto& m : msgs) mem.msg_ids.push_back(m.id);

    save_memory(mem);

    // 触发主动聊天检测
    maybe_trigger_active_chat(group_id, "今日热点");
}

void MemorySummarizer::do_month_summary(int64_t group_id, int64_t start, int64_t end) {
    auto daily = get_memories(group_id, "24h", "", 100);

    protocol::MemoryEntry mem;
    mem.group_id = group_id;
    mem.summary = fmt::format("[月] 群{} 月度回顾", group_id);
    mem.detail = fmt::format("汇总了 {} 天的详细记录", daily.size());
    mem.level = "month";
    mem.category = "important";
    mem.start_time = start;
    mem.end_time = end;
    mem.importance_score = 0.7f;

    save_memory(mem);
}

void MemorySummarizer::do_year_summary(int64_t group_id, int64_t start, int64_t end) {
    auto months = get_memories(group_id, "month", "", 50);

    protocol::MemoryEntry mem;
    mem.group_id = group_id;
    mem.summary = fmt::format("[年] 群{} 年度总结", group_id);
    mem.detail = fmt::format("汇总了 {} 个月的记录", months.size());
    mem.level = "year";
    mem.category = "important";
    mem.start_time = start;
    mem.end_time = end;
    mem.importance_score = 0.85f;

    save_memory(mem);
}

void MemorySummarizer::do_3year_summary(int64_t group_id, int64_t start, int64_t end) {
    auto years = get_memories(group_id, "year", "", 10);

    protocol::MemoryEntry mem;
    mem.group_id = group_id;
    mem.summary = fmt::format("[3年] 群{} 三年回顾", group_id);
    mem.detail = fmt::format("汇总了 {} 年的记录", years.size());
    mem.level = "3year";
    mem.category = "important";
    mem.start_time = start;
    mem.end_time = end;
    mem.importance_score = 0.95f;

    save_memory(mem);
}

std::string MemorySummarizer::generate_summary(const std::vector<Message>& msgs, const std::string& level) {
    // 简化版：实际应调用 LLM API 生成高质量总结
    // 这里用模板拼接
    if (msgs.empty()) return "[空]";

    std::string detail = fmt::format("级别: {}\n消息数: {}\n", level, msgs.size());
    detail += "关键参与者: ";
    std::unordered_set<int64_t> qq_set;
    for (const auto& m : msgs) qq_set.insert(m.qq_id);
    for (auto q : qq_set) detail += fmt::format("{} ", q);
    detail += "\n内容摘要: \n";

    // 取前20条重要消息
    int count = 0;
    for (const auto& m : msgs) {
        if (count++ >= 20) break;
        detail += fmt::format("- [{}] {}: {}\n", m.timestamp, m.nickname, m.content);
    }
    return detail;
}

float MemorySummarizer::calculate_importance(const Message& msg) {
    float score = 0.5f;
    // 关键词加权
    if (msg.content.find("?") != std::string::npos || 
        msg.content.find("？") != std::string::npos) score += 0.1f;
    if (msg.content.find("重要") != std::string::npos ||
        msg.content.find("通知") != std::string::npos) score += 0.2f;
    if (msg.content.length() > 50) score += 0.1f;
    if (score > 1.0f) score = 1.0f;
    return score;
}

void MemorySummarizer::maybe_trigger_active_chat(int64_t group_id, const std::string& topic) {
    if (!active_chat_cb_) return;

    // 随机决定是否主动聊天（基于阈值）
    float roll = static_cast<float>(rand()) / RAND_MAX;
    if (roll < cfg_.active_chat_threshold) {
        ActiveChatRequest req;
        req.group_id = group_id;
        req.topic = topic;
        req.suggested_content = fmt::format("关于 '{}' 大家怎么看？", topic);
        req.urgency = roll;
        active_chat_cb_(req);
    }
}

int64_t MemorySummarizer::summarize_now(int64_t group_id, const std::string& level, bool is_manual) {
    auto now = std::chrono::system_clock::now();
    auto end = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    int64_t start = end;

    if (level == "3h") start = end - 3 * 3600;
    else if (level == "12h") start = end - 12 * 3600;
    else if (level == "24h") start = end - 24 * 3600;
    else if (level == "month") start = end - 30 * 24 * 3600;
    else if (level == "year") start = end - 365 * 24 * 3600;
    else if (level == "3year") start = end - 3 * 365 * 24 * 3600;

    SummaryTask task{group_id, level, start, end, is_manual, is_manual ? "manual" : "api"};
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        task_queue_.push(task);
    }
    queue_cv_.notify_one();
    return task.start_time;  // 返回任务ID（简化）
}

void MemorySummarizer::handle_recall(int64_t group_id, int64_t msg_id, 
                                      const std::string& content, bool is_important) {
    // 撤回消息：如果重要则保留为"recalled"类别，否则作为零碎记忆
    protocol::MemoryEntry mem;
    mem.group_id = group_id;
    mem.summary = fmt::format("[撤回] {}", content.substr(0, 50));
    mem.detail = content;
    mem.level = "3h";
    mem.category = is_important ? "important" : "recalled";
    mem.start_time = time(nullptr);
    mem.end_time = time(nullptr);
    mem.importance_score = is_important ? 0.8f : 0.2f;
    mem.msg_ids.push_back(msg_id);
    mem.tags.push_back("recalled");

    save_memory(mem);
}

void MemorySummarizer::mark_importance(int64_t msg_id, float score, const std::string& reason) {
    if (!sqlite_) return;
    const char* sql = "INSERT OR REPLACE INTO msg_importance (msg_id, score, reason) VALUES (?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(sqlite_->raw_db(), sql, -1, &stmt, nullptr);
    if (stmt) {
        sqlite3_bind_int64(stmt, 1, msg_id);
        sqlite3_bind_double(stmt, 2, score);
        sqlite3_bind_text(stmt, 3, reason.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    spdlog::info("Marked msg {} importance: {} ({})", msg_id, score, reason);
}

std::vector<protocol::MemoryEntry> MemorySummarizer::get_memories(int64_t group_id,
                                                                   const std::string& level,
                                                                   const std::string& category,
                                                                   int limit) {
    std::vector<protocol::MemoryEntry> results;
    if (!sqlite_) return results;

    std::string sql = "SELECT id, group_id, summary, detail, category, level, "
                      "start_time, end_time, created_at, importance_score, "
                      "is_manual, source_provider, tags FROM memories WHERE 1=1";
    if (group_id != 0) sql += " AND group_id = ?";
    if (!level.empty()) sql += " AND level = ?";
    if (!category.empty()) sql += " AND category = ?";
    sql += " ORDER BY created_at DESC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(sqlite_->raw_db(), sql.c_str(), -1, &stmt, nullptr);
    if (!stmt) return results;

    int idx = 1;
    if (group_id != 0) sqlite3_bind_int64(stmt, idx++, group_id);
    if (!level.empty()) sqlite3_bind_text(stmt, idx++, level.c_str(), -1, SQLITE_STATIC);
    if (!category.empty()) sqlite3_bind_text(stmt, idx++, category.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, idx, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        protocol::MemoryEntry m;
        m.id = sqlite3_column_int64(stmt, 0);
        m.group_id = sqlite3_column_int64(stmt, 1);
        m.summary = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        m.detail = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        m.category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        m.level = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        m.start_time = sqlite3_column_int64(stmt, 6);
        m.end_time = sqlite3_column_int64(stmt, 7);
        m.created_at = sqlite3_column_int64(stmt, 8);
        m.importance_score = static_cast<float>(sqlite3_column_double(stmt, 9));
        m.is_manual = sqlite3_column_int(stmt, 10) != 0;
        m.source_provider = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
        std::string tags = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
        size_t pos = 0;
        while ((pos = tags.find(',')) != std::string::npos) {
            m.tags.push_back(tags.substr(0, pos));
            tags.erase(0, pos + 1);
        }
        if (!tags.empty()) m.tags.push_back(tags);
        results.push_back(m);
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<protocol::MemoryEntry> MemorySummarizer::search_memories(int64_t group_id,
                                                                      const std::string& query,
                                                                      const std::string& search_type) {
    std::vector<protocol::MemoryEntry> results;
    if (!sqlite_) return results;

    if (search_type == "time") {
        auto comma = query.find(',');
        if (comma != std::string::npos) {
            int64_t start = 0, end = 0;
            try {
                start = std::stoll(query.substr(0, comma));
                end = std::stoll(query.substr(comma + 1));
            } catch (...) {
                spdlog::warn("Invalid time range format: {}", query);
                return results;
            }
            const char* sql = "SELECT id, group_id, summary, detail, category, level, "
                              "start_time, end_time, created_at, importance_score, "
                              "is_manual, source_provider, tags FROM memories "
                              "WHERE group_id = ? AND start_time >= ? AND end_time <= ? "
                              "ORDER BY start_time DESC";
            sqlite3_stmt* stmt = nullptr;
            sqlite3_prepare_v2(sqlite_->raw_db(), sql, -1, &stmt, nullptr);
            if (stmt) {
                sqlite3_bind_int64(stmt, 1, group_id);
                sqlite3_bind_int64(stmt, 2, start);
                sqlite3_bind_int64(stmt, 3, end);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    protocol::MemoryEntry m;
                    m.id = sqlite3_column_int64(stmt, 0);
                    m.group_id = sqlite3_column_int64(stmt, 1);
                    m.summary = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                    m.detail = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                    m.category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
                    m.level = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                    m.start_time = sqlite3_column_int64(stmt, 6);
                    m.end_time = sqlite3_column_int64(stmt, 7);
                    m.created_at = sqlite3_column_int64(stmt, 8);
                    m.importance_score = static_cast<float>(sqlite3_column_double(stmt, 9));
                    m.is_manual = sqlite3_column_int(stmt, 10) != 0;
                    m.source_provider = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
                    std::string tags = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
                    size_t pos = 0;
                    while ((pos = tags.find(',')) != std::string::npos) {
                        m.tags.push_back(tags.substr(0, pos));
                        tags.erase(0, pos + 1);
                    }
                    if (!tags.empty()) m.tags.push_back(tags);
                    results.push_back(m);
                }
                sqlite3_finalize(stmt);
            }
        }
    } else if (search_type == "ref") {
        const char* sql = "SELECT m.id, m.group_id, m.summary, m.detail, m.category, m.level, "
                          "m.start_time, m.end_time, m.created_at, m.importance_score, "
                          "m.is_manual, m.source_provider, m.tags "
                          "FROM memories m JOIN memory_msg_links l ON m.id = l.memory_id "
                          "WHERE l.msg_id = ? AND m.group_id = ?";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(sqlite_->raw_db(), sql, -1, &stmt, nullptr);
        if (stmt) {
            try {
                sqlite3_bind_int64(stmt, 1, std::stoll(query));
            } catch (...) {
                spdlog::warn("Invalid msg_id for context: {}", query);
                sqlite3_finalize(stmt);
                return results;
            }
            sqlite3_bind_int64(stmt, 2, group_id);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                protocol::MemoryEntry m;
                m.id = sqlite3_column_int64(stmt, 0);
                m.group_id = sqlite3_column_int64(stmt, 1);
                m.summary = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                m.detail = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                m.category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
                m.level = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                m.start_time = sqlite3_column_int64(stmt, 6);
                m.end_time = sqlite3_column_int64(stmt, 7);
                m.created_at = sqlite3_column_int64(stmt, 8);
                m.importance_score = static_cast<float>(sqlite3_column_double(stmt, 9));
                m.is_manual = sqlite3_column_int(stmt, 10) != 0;
                m.source_provider = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
                std::string tags = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
                size_t pos = 0;
                while ((pos = tags.find(',')) != std::string::npos) {
                    m.tags.push_back(tags.substr(0, pos));
                    tags.erase(0, pos + 1);
                }
                if (!tags.empty()) m.tags.push_back(tags);
                results.push_back(m);
            }
            sqlite3_finalize(stmt);
        }
    } else if (search_type == "marked") {
        results = get_memories(group_id, "", "", 100);
        results.erase(std::remove_if(results.begin(), results.end(),
            [](const auto& m) { return m.importance_score < 0.75f; }), results.end());
    } else {
        // hybrid: 全文搜索 summary/detail
        const char* sql = "SELECT id, group_id, summary, detail, category, level, "
                          "start_time, end_time, created_at, importance_score, "
                          "is_manual, source_provider, tags FROM memories "
                          "WHERE group_id = ? AND (summary LIKE ? OR detail LIKE ?) "
                          "ORDER BY importance_score DESC";
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(sqlite_->raw_db(), sql, -1, &stmt, nullptr);
        if (stmt) {
            std::string pattern = "%" + query + "%";
            sqlite3_bind_int64(stmt, 1, group_id);
            sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, pattern.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                protocol::MemoryEntry m;
                m.id = sqlite3_column_int64(stmt, 0);
                m.group_id = sqlite3_column_int64(stmt, 1);
                m.summary = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                m.detail = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                m.category = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
                m.level = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                m.start_time = sqlite3_column_int64(stmt, 6);
                m.end_time = sqlite3_column_int64(stmt, 7);
                m.created_at = sqlite3_column_int64(stmt, 8);
                m.importance_score = static_cast<float>(sqlite3_column_double(stmt, 9));
                m.is_manual = sqlite3_column_int(stmt, 10) != 0;
                m.source_provider = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
                std::string tags = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
                size_t pos = 0;
                while ((pos = tags.find(',')) != std::string::npos) {
                    m.tags.push_back(tags.substr(0, pos));
                    tags.erase(0, pos + 1);
                }
                if (!tags.empty()) m.tags.push_back(tags);
                results.push_back(m);
            }
            sqlite3_finalize(stmt);
        }
    }
    return results;
}

bool MemorySummarizer::merge_memories(const std::vector<int64_t>& mem_ids, const std::string& new_summary) {
    if (!sqlite_ || mem_ids.empty()) return false;
    for (int64_t id : mem_ids) {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(sqlite_->raw_db(), "DELETE FROM memories WHERE id = ?", -1, &stmt, nullptr);
        if (stmt) {
            sqlite3_bind_int64(stmt, 1, id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    spdlog::info("Merged {} memories into: {}", mem_ids.size(), new_summary);
    return true;
}

bool MemorySummarizer::delete_memory(int64_t mem_id) {
    if (!sqlite_) return false;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(sqlite_->raw_db(), "DELETE FROM memories WHERE id = ?", -1, &stmt, nullptr);
    if (!stmt) return false;
    sqlite3_bind_int64(stmt, 1, mem_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool MemorySummarizer::update_memory(int64_t mem_id, const protocol::MemoryEntry& entry) {
    if (!sqlite_) return false;
    const char* sql = "UPDATE memories SET summary=?, detail=?, category=?, "
                        "level=?, importance_score=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(sqlite_->raw_db(), sql, -1, &stmt, nullptr);
    if (!stmt) return false;
    sqlite3_bind_text(stmt, 1, entry.summary.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, entry.detail.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, entry.category.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, entry.level.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 5, entry.importance_score);
    sqlite3_bind_int64(stmt, 6, mem_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

void MemorySummarizer::set_active_chat_callback(std::function<void(const ActiveChatRequest&)> cb) {
    active_chat_cb_ = std::move(cb);
}

size_t MemorySummarizer::pending_tasks() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return task_queue_.size();
}

int64_t MemorySummarizer::save_memory(const protocol::MemoryEntry& entry) {
    if (!sqlite_) return -1;

    const char* sql = R"(
        INSERT INTO memories (group_id, summary, detail, category, level,
                              start_time, end_time, importance_score, is_manual,
                              source_provider, tags)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(sqlite_->raw_db(), sql, -1, &stmt, nullptr);
    if (!stmt) return -1;

    sqlite3_bind_int64(stmt, 1, entry.group_id);
    sqlite3_bind_text(stmt, 2, entry.summary.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, entry.detail.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, entry.category.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, entry.level.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, entry.start_time);
    sqlite3_bind_int64(stmt, 7, entry.end_time);
    sqlite3_bind_double(stmt, 8, entry.importance_score);
    sqlite3_bind_int(stmt, 9, entry.is_manual ? 1 : 0);
    sqlite3_bind_text(stmt, 10, entry.source_provider.c_str(), -1, SQLITE_STATIC);
    std::string tags_str;
    for (size_t i = 0; i < entry.tags.size(); ++i) {
        if (i > 0) tags_str += ",";
        tags_str += entry.tags[i];
    }
    sqlite3_bind_text(stmt, 11, tags_str.c_str(), -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        spdlog::error("save_memory failed: {}", sqlite3_errmsg(sqlite_->raw_db()));
        return -1;
    }

    int64_t mem_id = sqlite3_last_insert_rowid(sqlite_->raw_db());

    if (!entry.msg_ids.empty()) {
        const char* link_sql = "INSERT OR IGNORE INTO memory_msg_links (memory_id, msg_id) VALUES (?, ?);";
        sqlite3_stmt* link_stmt = nullptr;
        sqlite3_prepare_v2(sqlite_->raw_db(), link_sql, -1, &link_stmt, nullptr);
        if (link_stmt) {
            for (int64_t msg_id : entry.msg_ids) {
                sqlite3_reset(link_stmt);
                sqlite3_bind_int64(link_stmt, 1, mem_id);
                sqlite3_bind_int64(link_stmt, 2, msg_id);
                sqlite3_step(link_stmt);
            }
            sqlite3_finalize(link_stmt);
        }
    }

    return mem_id;
}

} // namespace chatdb
