#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>

namespace chatdb {

class SQLiteStorage;
class RedisClient;
class OllamaClient;

struct RawMessage {
    int64_t group_id;
    int64_t qq_id;
    std::string nickname;
    std::string content;
    int msg_type;
    int64_t timestamp;
};

struct ProcessedMessage {
    int64_t id;
    int64_t group_id;
    int64_t qq_id;
    std::string nickname;
    std::string content;
    int msg_type;
    int64_t timestamp;
    std::string msg_hash;
    std::vector<float> embedding;
    bool is_duplicate;
};

using MessageCallback = std::function<void(const ProcessedMessage&)>;

class MessageProcessor {
public:
    MessageProcessor(SQLiteStorage* sqlite, RedisClient* redis, 
                     OllamaClient* ollama);
    ~MessageProcessor();

    MessageProcessor(const MessageProcessor&) = delete;
    MessageProcessor& operator=(const MessageProcessor&) = delete;

    void start();
    void stop();

    // 接收原始消息（非阻塞，入队处理）
    void receive_message(RawMessage msg);
    void receive_messages(std::vector<RawMessage> msgs);

    // 同步处理单条（用于需要立即确认的场景）
    ProcessedMessage process_sync(const RawMessage& msg);

    // 设置回调
    void on_processed(MessageCallback cb) { callback_ = std::move(cb); }
    void on_error(std::function<void(const std::string&)> cb) { error_cb_ = std::move(cb); }

    // 统计
    size_t queue_size() const;
    uint64_t processed_count() const { return processed_count_.load(); }
    uint64_t duplicate_count() const { return duplicate_count_.load(); }
    uint64_t error_count() const { return error_count_.load(); }

private:
    void worker_loop();
    ProcessedMessage do_process(const RawMessage& raw);
    std::string compute_hash(const RawMessage& msg);

    SQLiteStorage* sqlite_;
    RedisClient* redis_;
    OllamaClient* ollama_;

    std::queue<RawMessage> input_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};

    std::atomic<uint64_t> processed_count_{0};
    std::atomic<uint64_t> duplicate_count_{0};
    std::atomic<uint64_t> error_count_{0};

    MessageCallback callback_;
    std::function<void(const std::string&)> error_cb_;

    int dedup_window_seconds_ = 300;
};

} // namespace chatdb
