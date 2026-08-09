#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <mutex>
#include <deque>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <optional>

struct sqlite3;
struct sqlite3_stmt;

namespace chatdb {

struct Message {
    int64_t id = -1;
    int64_t group_id = 0;
    int64_t qq_id = 0;
    std::string nickname;
    std::string content;
    int msg_type = 1;  // 1=文本 2=图片 3=表情 4=合并转发
    int64_t timestamp = 0;
    std::string msg_hash;  // 用于去重
};

struct MessageBatch {
    std::vector<Message> messages;
};

class SQLiteStorage {
public:
    explicit SQLiteStorage(const std::string& db_path);
    ~SQLiteStorage();

    // 禁止拷贝
    SQLiteStorage(const SQLiteStorage&) = delete;
    SQLiteStorage& operator=(const SQLiteStorage&) = delete;

    bool initialize();
    void shutdown();

    // 同步插入（用于低延迟场景）
    int64_t insert_message(const Message& msg);

    // 批量异步插入（高性能）
    void enqueue_message(Message msg);
    void enqueue_messages(std::vector<Message> msgs);

    // 查询
    std::vector<Message> query_by_group(int64_t group_id, int limit = 100, int offset = 0);
    std::vector<Message> query_by_group_time(int64_t group_id, int64_t start_ts, int64_t end_ts, int limit = 100);
    std::vector<Message> query_by_qq(int64_t qq_id, int limit = 100);
    std::vector<Message> fulltext_search(const std::string& keyword, int64_t group_id = 0, int limit = 50);
    std::optional<Message> get_message_by_id(int64_t id);

    // 统计
    int64_t count_messages(int64_t group_id = 0);
    int64_t count_messages_by_time(int64_t group_id, int64_t start_ts, int64_t end_ts);

    // 清理旧数据（保留N天）
    int cleanup_old_messages(int retain_days);

    // 获取最后插入ID
    int64_t last_insert_id() const;

    // 执行VACUUM（压缩数据库）
    void vacuum();

    // 检查消息是否存在（通过hash）
    bool message_exists(const std::string& msg_hash);

    // 暴露原始 db 句柄（供 MemorySummarizer 等使用）
    sqlite3* raw_db() const { return db_; }

    // 索引备份/恢复（BLOB 存储）
    bool backup_index(const std::string& name, const std::string& data);
    std::optional<std::string> restore_index(const std::string& name);
    std::vector<std::string> list_backups();
    bool delete_backup(const std::string& name);

private:
    void worker_loop();
    bool flush_batch();
    bool prepare_statements();
    void finalize_statements();
    bool create_tables();
    bool create_indexes();
    bool create_fts();

    sqlite3* db_ = nullptr;
    std::string db_path_;

    // 预编译语句
    sqlite3_stmt* stmt_insert_ = nullptr;
    sqlite3_stmt* stmt_query_group_ = nullptr;
    sqlite3_stmt* stmt_query_group_time_ = nullptr;
    sqlite3_stmt* stmt_query_qq_ = nullptr;
    sqlite3_stmt* stmt_fts_search_ = nullptr;
    sqlite3_stmt* stmt_get_by_id_ = nullptr;
    sqlite3_stmt* stmt_count_ = nullptr;
    sqlite3_stmt* stmt_count_time_ = nullptr;
    sqlite3_stmt* stmt_cleanup_ = nullptr;
    sqlite3_stmt* stmt_exists_ = nullptr;
    sqlite3_stmt* stmt_backup_insert_ = nullptr;
    sqlite3_stmt* stmt_backup_get_ = nullptr;
    sqlite3_stmt* stmt_backup_list_ = nullptr;
    sqlite3_stmt* stmt_backup_delete_ = nullptr;

    // 批量写入队列
    std::deque<Message> queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread worker_thread_;
    std::atomic<bool> running_{false};

    int batch_size_ = 100;
    int flush_interval_ms_ = 500;

    mutable std::mutex db_mutex_;  // 保护非批量操作
};

} // namespace chatdb
