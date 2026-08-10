#include "chatdb/sqlite_storage.hpp"
#include <sqlite3.h>
#include <optional>
#include <spdlog/spdlog.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <fmt/format.h>
#include <cstring>
#include <chrono>

namespace chatdb {

SQLiteStorage::SQLiteStorage(const std::string& db_path) 
    : db_path_(db_path) {}

SQLiteStorage::~SQLiteStorage() {
    shutdown();
}

bool SQLiteStorage::initialize() {
    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        spdlog::error("SQLite open failed: {}", sqlite3_errmsg(db_));
        return false;
    }

    // 性能优化设置
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA cache_size=-65536;", nullptr, nullptr, nullptr);  // 64MB cache
    sqlite3_exec(db_, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA mmap_size=268435456;", nullptr, nullptr, nullptr);  // 256MB mmap
    sqlite3_exec(db_, "PRAGMA page_size=4096;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA auto_vacuum=INCREMENTAL;", nullptr, nullptr, nullptr);

    if (!create_tables()) return false;
    if (!create_indexes()) return false;
    if (!create_fts()) return false;
    if (!prepare_statements()) return false;

    running_ = true;
    worker_thread_ = std::thread(&SQLiteStorage::worker_loop, this);

    spdlog::info("SQLiteStorage initialized: {}", db_path_);
    return true;
}

void SQLiteStorage::shutdown() {
    running_ = false;
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    flush_batch();
    finalize_statements();
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    spdlog::info("SQLiteStorage shutdown.");
}

bool SQLiteStorage::create_tables() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            group_id INTEGER NOT NULL,
            qq_id INTEGER NOT NULL,
            nickname TEXT,
            content TEXT NOT NULL,
            msg_type INTEGER DEFAULT 1,
            timestamp INTEGER NOT NULL,
            msg_hash TEXT UNIQUE,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );

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

        CREATE TABLE IF NOT EXISTS memory_msg_links (
            memory_id INTEGER NOT NULL,
            msg_id INTEGER NOT NULL,
            PRIMARY KEY (memory_id, msg_id)
        );

        CREATE TABLE IF NOT EXISTS msg_importance (
            msg_id INTEGER PRIMARY KEY,
            score REAL DEFAULT 0.5,
            reason TEXT,
            marked_at INTEGER DEFAULT (strftime('%s','now'))
        );

        CREATE TABLE IF NOT EXISTS index_backups (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            backup_name TEXT NOT NULL,
            backup_data BLOB NOT NULL,
            created_at INTEGER DEFAULT (strftime('%s','now')),
            size_bytes INTEGER,
            checksum TEXT
        );
    )";

    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        spdlog::error("Create table failed: {}", err);
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool SQLiteStorage::create_indexes() {
    const char* indexes[] = {
        "CREATE INDEX IF NOT EXISTS idx_msg_group_time ON messages(group_id, timestamp DESC);",
        "CREATE INDEX IF NOT EXISTS idx_msg_qq ON messages(qq_id, timestamp DESC);",
        "CREATE INDEX IF NOT EXISTS idx_msg_hash ON messages(msg_hash);",
        "CREATE INDEX IF NOT EXISTS idx_msg_time ON messages(timestamp DESC);",
        "CREATE INDEX IF NOT EXISTS idx_mem_group ON memories(group_id);",
        "CREATE INDEX IF NOT EXISTS idx_mem_level ON memories(level);",
        "CREATE INDEX IF NOT EXISTS idx_mem_time ON memories(start_time, end_time);",
        "CREATE INDEX IF NOT EXISTS idx_mem_category ON memories(category);",
        "CREATE INDEX IF NOT EXISTS idx_mem_importance ON memories(importance_score DESC);",
        nullptr
    };

    for (int i = 0; indexes[i]; ++i) {
        char* err = nullptr;
        sqlite3_exec(db_, indexes[i], nullptr, nullptr, &err);
        if (err) {
            spdlog::warn("Create index warning: {}", err);
            sqlite3_free(err);
        }
    }
    return true;
}

bool SQLiteStorage::create_fts() {
    // FTS5 全文搜索虚拟表
    const char* sql = R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS msg_fts USING fts5(
            content, 
            content='messages', 
            content_rowid='id'
        );
        CREATE TRIGGER IF NOT EXISTS msg_fts_insert AFTER INSERT ON messages BEGIN
            INSERT INTO msg_fts(rowid, content) VALUES (new.id, new.content);
        END;
        CREATE TRIGGER IF NOT EXISTS msg_fts_delete AFTER DELETE ON messages BEGIN
            INSERT INTO msg_fts(msg_fts, rowid, content) VALUES ('delete', old.id, old.content);
        END;
    )";

    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        spdlog::warn("FTS5 creation warning: {} (FTS5 extension may not be available)", err ? err : "unknown");
        if (err) sqlite3_free(err);
        // FTS5 不是致命错误，继续运行
    }
    return true;
}

bool SQLiteStorage::prepare_statements() {
    const char* sqls[] = {
        "INSERT INTO messages (group_id, qq_id, nickname, content, msg_type, timestamp, msg_hash) VALUES (?, ?, ?, ?, ?, ?, ?);",
        "SELECT id, group_id, qq_id, nickname, content, msg_type, timestamp FROM messages WHERE group_id = ? ORDER BY timestamp DESC LIMIT ? OFFSET ?;",
        "SELECT id, group_id, qq_id, nickname, content, msg_type, timestamp FROM messages WHERE group_id = ? AND timestamp >= ? AND timestamp <= ? ORDER BY timestamp DESC LIMIT ?;",
        "SELECT id, group_id, qq_id, nickname, content, msg_type, timestamp FROM messages WHERE qq_id = ? ORDER BY timestamp DESC LIMIT ?;",
        "SELECT m.id, m.group_id, m.qq_id, m.nickname, m.content, m.msg_type, m.timestamp FROM messages m JOIN msg_fts f ON m.id = f.rowid WHERE f.content MATCH ? ORDER BY rank LIMIT ?;",
        "SELECT id, group_id, qq_id, nickname, content, msg_type, timestamp FROM messages WHERE id = ?;",
        "SELECT COUNT(*) FROM messages WHERE (? = 0 OR group_id = ?);",
        "SELECT COUNT(*) FROM messages WHERE group_id = ? AND timestamp >= ? AND timestamp <= ?;",
        "DELETE FROM messages WHERE timestamp < ?;",
        "SELECT 1 FROM messages WHERE msg_hash = ? LIMIT 1;",
        nullptr
    };

    sqlite3_stmt** stmts[] = {
        &stmt_insert_, &stmt_query_group_, &stmt_query_group_time_,
        &stmt_query_qq_, &stmt_fts_search_, &stmt_get_by_id_,
        &stmt_count_, &stmt_count_time_, &stmt_cleanup_, &stmt_exists_,
        nullptr
    };

    for (int i = 0; sqls[i]; ++i) {
        if (sqlite3_prepare_v2(db_, sqls[i], -1, stmts[i], nullptr) != SQLITE_OK) {
            spdlog::error("Prepare statement failed: {}", sqlite3_errmsg(db_));
            return false;
        }
    }
    return true;
}

void SQLiteStorage::finalize_statements() {
    sqlite3_stmt** stmts[] = {
        &stmt_insert_, &stmt_query_group_, &stmt_query_group_time_,
        &stmt_query_qq_, &stmt_fts_search_, &stmt_get_by_id_,
        &stmt_count_, &stmt_count_time_, &stmt_cleanup_, &stmt_exists_,
        nullptr
    };
    for (int i = 0; stmts[i]; ++i) {
        if (*stmts[i]) {
            sqlite3_finalize(*stmts[i]);
            *stmts[i] = nullptr;
        }
    }
}

void SQLiteStorage::worker_loop() {
    while (running_) {
        try {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            bool has_data = queue_cv_.wait_for(lock, std::chrono::milliseconds(flush_interval_ms_), 
                [this] { return !queue_.empty() || !running_; });

            if (!running_) break;

            if (queue_.size() >= static_cast<size_t>(batch_size_) || 
                (has_data && !queue_.empty())) {
                lock.unlock();
                flush_batch();
            }
        } catch (const std::exception& e) {
            spdlog::error("SQLiteStorage worker_loop error: {}", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    // 退出前刷新剩余
    flush_batch();
}

bool SQLiteStorage::flush_batch() {
    std::vector<Message> batch;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.empty()) return true;
        batch.reserve(queue_.size());
        while (!queue_.empty() && batch.size() < static_cast<size_t>(batch_size_)) {
            batch.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
    }

    if (batch.empty()) return true;

    std::lock_guard<std::mutex> db_lock(db_mutex_);

    sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    for (const auto& msg : batch) {
        sqlite3_reset(stmt_insert_);
        sqlite3_bind_int64(stmt_insert_, 1, msg.group_id);
        sqlite3_bind_int64(stmt_insert_, 2, msg.qq_id);
        sqlite3_bind_text(stmt_insert_, 3, msg.nickname.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt_insert_, 4, msg.content.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt_insert_, 5, msg.msg_type);
        sqlite3_bind_int64(stmt_insert_, 6, msg.timestamp);
        sqlite3_bind_text(stmt_insert_, 7, msg.msg_hash.c_str(), -1, SQLITE_STATIC);

        if (sqlite3_step(stmt_insert_) != SQLITE_DONE) {
            spdlog::warn("Insert failed: {}", sqlite3_errmsg(db_));
        }
    }

    sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
    spdlog::debug("Flushed {} messages to SQLite", batch.size());
    return true;
}

int64_t SQLiteStorage::insert_message(const Message& msg) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_insert_);
    sqlite3_bind_int64(stmt_insert_, 1, msg.group_id);
    sqlite3_bind_int64(stmt_insert_, 2, msg.qq_id);
    sqlite3_bind_text(stmt_insert_, 3, msg.nickname.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt_insert_, 4, msg.content.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt_insert_, 5, msg.msg_type);
    sqlite3_bind_int64(stmt_insert_, 6, msg.timestamp);
    sqlite3_bind_text(stmt_insert_, 7, msg.msg_hash.c_str(), -1, SQLITE_STATIC);

    if (sqlite3_step(stmt_insert_) != SQLITE_DONE) {
        spdlog::error("Direct insert failed: {}", sqlite3_errmsg(db_));
        return -1;
    }

    return sqlite3_last_insert_rowid(db_);
}

void SQLiteStorage::enqueue_message(Message msg) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push_back(std::move(msg));
    if (queue_.size() >= static_cast<size_t>(batch_size_)) {
        queue_cv_.notify_one();
    }
}

void SQLiteStorage::enqueue_messages(std::vector<Message> msgs) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    for (auto& msg : msgs) {
        queue_.push_back(std::move(msg));
    }
    if (queue_.size() >= static_cast<size_t>(batch_size_)) {
        queue_cv_.notify_one();
    }
}

std::vector<Message> SQLiteStorage::query_by_group(int64_t group_id, int limit, int offset) {
    std::vector<Message> results;
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_query_group_);
    sqlite3_bind_int64(stmt_query_group_, 1, group_id);
    sqlite3_bind_int(stmt_query_group_, 2, limit);
    sqlite3_bind_int(stmt_query_group_, 3, offset);

    while (sqlite3_step(stmt_query_group_) == SQLITE_ROW) {
        Message m;
        m.id = sqlite3_column_int64(stmt_query_group_, 0);
        m.group_id = sqlite3_column_int64(stmt_query_group_, 1);
        m.qq_id = sqlite3_column_int64(stmt_query_group_, 2);
        m.nickname = reinterpret_cast<const char*>(sqlite3_column_text(stmt_query_group_, 3));
        m.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt_query_group_, 4));
        m.msg_type = sqlite3_column_int(stmt_query_group_, 5);
        m.timestamp = sqlite3_column_int64(stmt_query_group_, 6);
        results.push_back(m);
    }
    return results;
}

std::vector<Message> SQLiteStorage::query_by_group_time(int64_t group_id, int64_t start_ts, int64_t end_ts, int limit) {
    std::vector<Message> results;
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_query_group_time_);
    sqlite3_bind_int64(stmt_query_group_time_, 1, group_id);
    sqlite3_bind_int64(stmt_query_group_time_, 2, start_ts);
    sqlite3_bind_int64(stmt_query_group_time_, 3, end_ts);
    sqlite3_bind_int(stmt_query_group_time_, 4, limit);

    while (sqlite3_step(stmt_query_group_time_) == SQLITE_ROW) {
        Message m;
        m.id = sqlite3_column_int64(stmt_query_group_time_, 0);
        m.group_id = sqlite3_column_int64(stmt_query_group_time_, 1);
        m.qq_id = sqlite3_column_int64(stmt_query_group_time_, 2);
        m.nickname = reinterpret_cast<const char*>(sqlite3_column_text(stmt_query_group_time_, 3));
        m.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt_query_group_time_, 4));
        m.msg_type = sqlite3_column_int(stmt_query_group_time_, 5);
        m.timestamp = sqlite3_column_int64(stmt_query_group_time_, 6);
        results.push_back(m);
    }
    return results;
}

std::vector<Message> SQLiteStorage::fulltext_search(const std::string& keyword, int64_t group_id, int limit) {
    std::vector<Message> results;
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_fts_search_);
    sqlite3_bind_text(stmt_fts_search_, 1, keyword.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt_fts_search_, 2, limit);

    while (sqlite3_step(stmt_fts_search_) == SQLITE_ROW) {
        Message m;
        m.id = sqlite3_column_int64(stmt_fts_search_, 0);
        m.group_id = sqlite3_column_int64(stmt_fts_search_, 1);
        m.qq_id = sqlite3_column_int64(stmt_fts_search_, 2);
        m.nickname = reinterpret_cast<const char*>(sqlite3_column_text(stmt_fts_search_, 3));
        m.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt_fts_search_, 4));
        m.msg_type = sqlite3_column_int(stmt_fts_search_, 5);
        m.timestamp = sqlite3_column_int64(stmt_fts_search_, 6);
        results.push_back(m);
    }
    return results;
}

std::optional<Message> SQLiteStorage::get_message_by_id(int64_t id) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_get_by_id_);
    sqlite3_bind_int64(stmt_get_by_id_, 1, id);

    if (sqlite3_step(stmt_get_by_id_) == SQLITE_ROW) {
        Message m;
        m.id = sqlite3_column_int64(stmt_get_by_id_, 0);
        m.group_id = sqlite3_column_int64(stmt_get_by_id_, 1);
        m.qq_id = sqlite3_column_int64(stmt_get_by_id_, 2);
        m.nickname = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_by_id_, 3));
        m.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_by_id_, 4));
        m.msg_type = sqlite3_column_int(stmt_get_by_id_, 5);
        m.timestamp = sqlite3_column_int64(stmt_get_by_id_, 6);
        return m;
    }
    return std::nullopt;
}

int64_t SQLiteStorage::count_messages(int64_t group_id) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_count_);
    sqlite3_bind_int64(stmt_count_, 1, group_id);
    sqlite3_bind_int64(stmt_count_, 2, group_id);

    if (sqlite3_step(stmt_count_) == SQLITE_ROW) {
        return sqlite3_column_int64(stmt_count_, 0);
    }
    return 0;
}

bool SQLiteStorage::message_exists(const std::string& msg_hash) {
    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_exists_);
    sqlite3_bind_text(stmt_exists_, 1, msg_hash.c_str(), -1, SQLITE_STATIC);

    bool exists = (sqlite3_step(stmt_exists_) == SQLITE_ROW);
    sqlite3_reset(stmt_exists_);
    return exists;
}

int SQLiteStorage::cleanup_old_messages(int retain_days) {
    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(24 * retain_days);
    int64_t cutoff_ts = std::chrono::duration_cast<std::chrono::seconds>(cutoff.time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(db_mutex_);

    sqlite3_reset(stmt_cleanup_);
    sqlite3_bind_int64(stmt_cleanup_, 1, cutoff_ts);

    sqlite3_step(stmt_cleanup_);
    int deleted = sqlite3_changes(db_);

    // 清理FTS碎片
    sqlite3_exec(db_, "INSERT INTO msg_fts(msg_fts) VALUES('optimize');", nullptr, nullptr, nullptr);

    spdlog::info("Cleaned up {} old messages", deleted);
    return deleted;
}

void SQLiteStorage::vacuum() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    sqlite3_exec(db_, "VACUUM;", nullptr, nullptr, nullptr);
    spdlog::info("SQLite VACUUM completed.");
}

int64_t SQLiteStorage::last_insert_id() const {
    return sqlite3_last_insert_rowid(db_);
}



} // namespace chatdb
