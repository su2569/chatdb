#include "chatdb/ollama_client.hpp"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

// cpp-httplib header-only
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

namespace chatdb {

using json = nlohmann::json;

class OllamaClient::HttpClientImpl {
public:
    explicit HttpClientImpl(const std::string& host, int port) 
        : client_(fmt::format("{}:{}", host, port)) {}

    httplib::Client client_;
};

OllamaClient::OllamaClient(const Config& cfg) : cfg_(cfg) {
    http_ = std::make_unique<HttpClientImpl>(cfg_.host, cfg_.port);
    http_->client_.set_connection_timeout(cfg_.timeout_ms / 1000.0);
    http_->client_.set_read_timeout(cfg_.timeout_ms / 1000.0);
    http_->client_.set_write_timeout(cfg_.timeout_ms / 1000.0);
}

OllamaClient::~OllamaClient() {
    stop();
}

bool OllamaClient::test_connection() {
    try {
        auto res = http_->client_.Get("/api/tags");
        if (res && res->status == 200) {
            spdlog::info("Ollama connection OK: {}:{}", cfg_.host, cfg_.port);
            return true;
        }
    } catch (const std::exception& e) {
        spdlog::error("Ollama connection test failed: {}", e.what());
    }
    return false;
}

std::string OllamaClient::get_model_info() {
    try {
        auto res = http_->client_.Get("/api/tags");
        if (res && res->status == 200) {
            return res->body;
        }
    } catch (...) {}
    return "{}";
}

std::vector<float> OllamaClient::embed(const std::string& text) {
    return do_embed(text);
}

std::vector<std::vector<float>> OllamaClient::embed_batch(const std::vector<std::string>& texts) {
    std::vector<std::vector<float>> results;
    results.reserve(texts.size());

    // 批量请求
    json req = {
        {"model", cfg_.model},
        {"input", texts}
    };

    for (int retry = 0; retry < cfg_.max_retries; ++retry) {
        try {
            auto res = http_->client_.Post("/api/embed", 
                req.dump(), "application/json");

            if (res && res->status == 200) {
                auto j = json::parse(res->body);
                if (j.contains("embeddings")) {
                    for (const auto& emb : j["embeddings"]) {
                        std::vector<float> vec;
                        for (const auto& v : emb) {
                            vec.push_back(v.get<float>());
                        }
                        results.push_back(std::move(vec));
                    }
                    return results;
                }
            }
        } catch (const std::exception& e) {
            spdlog::warn("Ollama embed_batch retry {}/{}: {}", retry + 1, cfg_.max_retries, e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.retry_delay_ms));
        }
    }

    spdlog::error("Ollama embed_batch failed after {} retries", cfg_.max_retries);
    return results;
}

std::future<std::vector<float>> OllamaClient::embed_async(const std::string& text, int64_t msg_id) {
    std::promise<std::vector<float>> promise;
    auto future = promise.get_future();

    if (!running_) {
        // 同步执行
        try {
            auto result = do_embed(text);
            promise.set_value(std::move(result));
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
        return future;
    }

    // 入队异步处理
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        task_queue_.push({msg_id, text, std::move(promise)});
    }
    queue_cv_.notify_one();

    return future;
}

void OllamaClient::start() {
    if (running_) return;
    running_ = true;

    for (int i = 0; i < cfg_.worker_threads; ++i) {
        workers_.emplace_back(&OllamaClient::worker_loop, this);
    }
    spdlog::info("OllamaClient started with {} worker threads", cfg_.worker_threads);
}

void OllamaClient::stop() {
    running_ = false;
    queue_cv_.notify_all();

    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

void OllamaClient::worker_loop() {
    while (running_) {
        EmbeddingTask task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return !task_queue_.empty() || !running_; });
            if (!running_) break;

            task = std::move(task_queue_.front());
            task_queue_.pop();
        }

        try {
            auto result = do_embed(task.text);
            task.promise.set_value(std::move(result));
        } catch (...) {
            task.promise.set_exception(std::current_exception());
        }
    }
}

std::vector<float> OllamaClient::do_embed(const std::string& text) {
    json req = {
        {"model", cfg_.model},
        {"input", text}
    };

    for (int retry = 0; retry < cfg_.max_retries; ++retry) {
        try {
            auto res = http_->client_.Post("/api/embed",
                req.dump(), "application/json");

            if (res && res->status == 200) {
                auto j = json::parse(res->body);
                if (j.contains("embeddings") && !j["embeddings"].empty()) {
                    std::vector<float> vec;
                    for (const auto& v : j["embeddings"][0]) {
                        vec.push_back(v.get<float>());
                    }
                    return vec;
                }
            } else {
                spdlog::warn("Ollama embed HTTP {}", res ? res->status : 0);
            }
        } catch (const std::exception& e) {
            spdlog::warn("Ollama embed retry {}/{}: {}", retry + 1, cfg_.max_retries, e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.retry_delay_ms));
        }
    }

    throw std::runtime_error(fmt::format("Ollama embed failed after {} retries", cfg_.max_retries));
}

std::vector<int8_t> OllamaClient::quantize_int8(const std::vector<float>& vec) {
    std::vector<int8_t> result;
    result.reserve(vec.size());
    for (float v : vec) {
        // 映射 [-1, 1] -> [-127, 127]
        int val = static_cast<int>(v * 127.0f);
        if (val > 127) val = 127;
        if (val < -127) val = -127;
        result.push_back(static_cast<int8_t>(val));
    }
    return result;
}

std::vector<float> OllamaClient::dequantize_int8(const std::vector<int8_t>& vec) {
    std::vector<float> result;
    result.reserve(vec.size());
    for (int8_t v : vec) {
        result.push_back(static_cast<float>(v) / 127.0f);
    }
    return result;
}

std::vector<float> OllamaClient::reduce_dim(const std::vector<float>& vec, int target_dim) {
    if (vec.size() <= static_cast<size_t>(target_dim)) return vec;

    // 简单平均池化降维
    std::vector<float> result;
    result.reserve(target_dim);

    int pool_size = static_cast<int>(vec.size()) / target_dim;
    for (int i = 0; i < target_dim; ++i) {
        float sum = 0.0f;
        for (int j = 0; j < pool_size; ++j) {
            sum += vec[i * pool_size + j];
        }
        result.push_back(sum / pool_size);
    }
    return result;
}

} // namespace chatdb
