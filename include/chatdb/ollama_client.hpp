#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <future>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

namespace chatdb {

struct EmbeddingTask {
    int64_t msg_id;
    std::string text;
    std::promise<std::vector<float>> promise;
};

class OllamaClient {
public:
    struct Config {
        std::string host = "127.0.0.1";
        int port = 11434;
        std::string model = "nomic-embed-text";
        int timeout_ms = 30000;
        int embedding_dim = 768;
        int batch_size = 8;           // 批量embed大小
        int max_retries = 3;
        int retry_delay_ms = 1000;
        int worker_threads = 2;       // 后台embed线程数
    };

    explicit OllamaClient(const Config& cfg);
    ~OllamaClient();

    OllamaClient(const OllamaClient&) = delete;
    OllamaClient& operator=(const OllamaClient&) = delete;

    bool test_connection();

    // 同步获取embedding（阻塞）
    std::vector<float> embed(const std::string& text);

    // 批量同步获取
    std::vector<std::vector<float>> embed_batch(const std::vector<std::string>& texts);

    // 异步获取（返回future）
    std::future<std::vector<float>> embed_async(const std::string& text, int64_t msg_id = -1);

    // 启动/停止后台工作线程
    void start();
    void stop();

    // 获取模型信息
    std::string get_model_info();

    // 量化：float32 -> int8（用于Redis内存优化）
    static std::vector<int8_t> quantize_int8(const std::vector<float>& vec);
    static std::vector<float> dequantize_int8(const std::vector<int8_t>& vec);

    // 降维：768维 -> 384维（简单平均池化，可选）
    static std::vector<float> reduce_dim(const std::vector<float>& vec, int target_dim = 384);

    const Config& config() const { return cfg_; }

private:
    void worker_loop();
    std::vector<float> do_embed(const std::string& text);
    std::string build_url(const std::string& endpoint) const;

    Config cfg_;

    // 异步任务队列
    std::queue<EmbeddingTask> task_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};

    // HTTP客户端（cpp-httplib，lazy init）
    class HttpClientImpl;
    std::unique_ptr<HttpClientImpl> http_;
};

} // namespace chatdb
