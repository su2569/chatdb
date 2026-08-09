#include "chatdb/message_processor.hpp"
#include "chatdb/sqlite_storage.hpp"
#include "chatdb/redis_client.hpp"
#include "chatdb/ollama_client.hpp"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <chrono>

namespace chatdb {

MessageProcessor::MessageProcessor(SQLiteStorage* sqlite, RedisClient* redis, 
                                    OllamaClient* ollama)
    : sqlite_(sqlite), redis_(redis), ollama_(ollama) {}

MessageProcessor::~MessageProcessor() {
    stop();
}

void MessageProcessor::start() {
    if (running_) return;
    running_ = true;

    int threads = 2;  // 默认2个处理线程
    for (int i = 0; i < threads; ++i) {
        workers_.emplace_back(&MessageProcessor::worker_loop, this);
    }
    spdlog::info("MessageProcessor started with {} threads", threads);
}

void MessageProcessor::stop() {
    running_ = false;
    queue_cv_.notify_all();

    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
    spdlog::info("MessageProcessor stopped.");
}

void MessageProcessor::receive_message(RawMessage msg) {
    if (msg.timestamp == 0) {
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::lock_guard<std::mutex> lock(queue_mutex_);
    input_queue_.push(std::move(msg));
    queue_cv_.notify_one();
}

void MessageProcessor::receive_messages(std::vector<RawMessage> msgs) {
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(queue_mutex_);
    for (auto& msg : msgs) {
        if (msg.timestamp == 0) msg.timestamp = now;
        input_queue_.push(std::move(msg));
    }
    queue_cv_.notify_one();
}

ProcessedMessage MessageProcessor::process_sync(const RawMessage& raw) {
    return do_process(raw);
}

void MessageProcessor::worker_loop() {
    while (running_) {
        RawMessage raw;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return !input_queue_.empty() || !running_; });
            if (!running_) break;

            raw = std::move(input_queue_.front());
            input_queue_.pop();
        }

        try {
            auto processed = do_process(raw);
            ++processed_count_;

            if (processed.is_duplicate) {
                ++duplicate_count_;
                continue;
            }

            if (callback_) {
                callback_(processed);
            }
        } catch (const std::exception& e) {
            ++error_count_;
            spdlog::error("Message processing error: {}", e.what());
            if (error_cb_) error_cb_(e.what());
        }
    }
}

ProcessedMessage MessageProcessor::do_process(const RawMessage& raw) {
    ProcessedMessage result;
    result.group_id = raw.group_id;
    result.qq_id = raw.qq_id;
    result.nickname = raw.nickname;
    result.content = raw.content;
    result.msg_type = raw.msg_type;
    result.timestamp = raw.timestamp;
    result.msg_hash = compute_hash(raw);
    result.is_duplicate = false;

    // 1. 去重检查（Redis优先，SQLite兜底）
    bool dup = false;
    if (redis_ && redis_->is_connected()) {
        dup = redis_->is_duplicate(raw.group_id, raw.content, dedup_window_seconds_);
    } else {
        dup = sqlite_->message_exists(result.msg_hash);
    }

    if (dup) {
        result.is_duplicate = true;
        return result;
    }

    // 2. 写入 SQLite
    Message msg;
    msg.group_id = raw.group_id;
    msg.qq_id = raw.qq_id;
    msg.nickname = raw.nickname;
    msg.content = raw.content;
    msg.msg_type = raw.msg_type;
    msg.timestamp = raw.timestamp;
    msg.msg_hash = result.msg_hash;

    sqlite_->enqueue_message(msg);
    result.id = sqlite_->last_insert_id();

    // 3. 生成 Embedding 并写入 Redis（异步）
    if (ollama_ && redis_ && redis_->is_connected()) {
        try {
            auto embedding = ollama_->embed(raw.content);
            result.embedding = embedding;

            redis_->add_vector(result.id, raw.group_id, raw.qq_id, 
                              raw.content, raw.timestamp, embedding);
            redis_->mark_duplicate(raw.group_id, raw.content, dedup_window_seconds_);
        } catch (const std::exception& e) {
            spdlog::warn("Embedding/Redis store failed: {}", e.what());
            // 不影响主流程，继续
        }
    }

    return result;
}

std::string MessageProcessor::compute_hash(const RawMessage& msg) {
    // 组合群号+QQ号+内容+时间窗口（5分钟）的hash
    auto window = msg.timestamp / dedup_window_seconds_;
    std::string data = fmt::format("{}:{}:{}:{}", msg.group_id, msg.qq_id, msg.content, window);

    // 简单hash，生产环境可用SHA256
    size_t h = std::hash<std::string>{}(data);
    return fmt::format("{:x}", h);
}

size_t MessageProcessor::queue_size() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return input_queue_.size();
}

} // namespace chatdb
