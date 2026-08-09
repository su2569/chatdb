#include "chatdb/sqlite_storage.hpp"
#include <sqlite3.h>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

namespace chatdb {

// ========== 索引备份/恢复 ==========
bool SQLiteStorage::backup_index(const std::string& name, const std::string& data) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!stmt_backup_insert_) return false;

    sqlite3_reset(stmt_backup_insert_);
    sqlite3_bind_text(stmt_backup_insert_, 1, name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt_backup_insert_, 2, data.data(), static_cast<int>(data.size()), SQLITE_STATIC);
    sqlite3_bind_int64(stmt_backup_insert_, 3, static_cast<int64_t>(data.size()));

    // 简单 checksum: SHA1 前8位
    unsigned char hash[SHA_DIGEST_LENGTH];
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, hash, &len);
    EVP_MD_CTX_free(ctx);
    std::string checksum = fmt::format("{:02x}{:02x}{:02x}{:02x}", hash[0], hash[1], hash[2], hash[3]);
    sqlite3_bind_text(stmt_backup_insert_, 4, checksum.c_str(), -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt_backup_insert_);
    return rc == SQLITE_DONE;
}

std::optional<std::string> SQLiteStorage::restore_index(const std::string& name) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!stmt_backup_get_) return std::nullopt;

    sqlite3_reset(stmt_backup_get_);
    sqlite3_bind_text(stmt_backup_get_, 1, name.c_str(), -1, SQLITE_STATIC);

    std::optional<std::string> result;
    if (sqlite3_step(stmt_backup_get_) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt_backup_get_, 0);
        int size = sqlite3_column_bytes(stmt_backup_get_, 0);
        if (blob && size > 0) {
            result = std::string(static_cast<const char*>(blob), size);
        }
    }
    return result;
}

std::vector<std::string> SQLiteStorage::list_backups() {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<std::string> names;
    if (!stmt_backup_list_) return names;

    sqlite3_reset(stmt_backup_list_);
    while (sqlite3_step(stmt_backup_list_) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt_backup_list_, 0));
        int64_t ts = sqlite3_column_int64(stmt_backup_list_, 1);
        int64_t sz = sqlite3_column_int64(stmt_backup_list_, 2);
        names.push_back(fmt::format("{} ({} bytes, {})", name ? name : "", sz, ts));
    }
    return names;
}

bool SQLiteStorage::delete_backup(const std::string& name) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!stmt_backup_delete_) return false;

    sqlite3_reset(stmt_backup_delete_);
    sqlite3_bind_text(stmt_backup_delete_, 1, name.c_str(), -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt_backup_delete_);
    return rc == SQLITE_DONE;
}



} // namespace chatdb
