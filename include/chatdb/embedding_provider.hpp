#pragma once
#include "chatdb/protocol.hpp"
#include <string>
#include <vector>
#include <cmath>
#include <memory>
#include <mutex>
#include <functional>
#include <future>

namespace chatdb {

// Embedding Provider 基类
class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;
    virtual std::string name() const = 0;
    virtual std::string display_name() const = 0;
    virtual bool test_connection() = 0;
    virtual std::vector<float> embed(const std::string& text) = 0;
    virtual std::vector<std::vector<float>> embed_batch(const std::vector<std::string>& texts) = 0;
    virtual int dimension() const = 0;
    virtual int timeout_ms() const = 0;
    virtual void set_timeout(int ms) = 0;
};

// Ollama Provider
class OllamaProvider : public EmbeddingProvider {
public:
    struct Config {
        std::string host = "127.0.0.1";
        int port = 11434;
        std::string model = "nomic-embed-text";
        int timeout_ms = 300000;  // Ollama 默认 300s
        int embedding_dim = 768;
        int max_retries = 3;
    };
    explicit OllamaProvider(const Config& cfg);
    ~OllamaProvider() override;
    std::string name() const override { return "ollama"; }
    std::string display_name() const override { return "Ollama (Local)"; }
    bool test_connection() override;
    std::vector<float> embed(const std::string& text) override;
    std::vector<std::vector<float>> embed_batch(const std::vector<std::string>& texts) override;
    int dimension() const override { return cfg_.embedding_dim; }
    int timeout_ms() const override { return cfg_.timeout_ms; }
    void set_timeout(int ms) override { cfg_.timeout_ms = ms; }
private:
    Config cfg_;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// OpenAI Compatible Provider (OpenAI / 阿里云 / 其他)
class OpenAIProvider : public EmbeddingProvider {
public:
    struct Config {
        std::string api_base;      // e.g. "https://api.openai.com/v1"
        std::string api_key;
        std::string model = "text-embedding-3-small";
        int timeout_ms = 60000;    // 其他 Provider 默认 60s
        int embedding_dim = 1536;
        int max_retries = 3;
        std::string name = "openai";
        std::string display_name = "OpenAI";
    };
    explicit OpenAIProvider(const Config& cfg);
    ~OpenAIProvider() override;
    std::string name() const override { return cfg_.name; }
    std::string display_name() const override { return cfg_.display_name; }
    bool test_connection() override;
    std::vector<float> embed(const std::string& text) override;
    std::vector<std::vector<float>> embed_batch(const std::vector<std::string>& texts) override;
    int dimension() const override { return cfg_.embedding_dim; }
    int timeout_ms() const override { return cfg_.timeout_ms; }
    void set_timeout(int ms) override { cfg_.timeout_ms = ms; }
private:
    Config cfg_;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Provider 管理器（支持热切换）
class EmbeddingProviderManager {
public:
    EmbeddingProviderManager() = default;
    static EmbeddingProviderManager& instance();

    // 注册 Provider
    void register_provider(std::shared_ptr<EmbeddingProvider> provider);
    void remove_provider(const std::string& name);

    // 切换 Provider（会触发索引重建）
    bool switch_to(const std::string& name);
    std::shared_ptr<EmbeddingProvider> current() const;
    std::shared_ptr<EmbeddingProvider> get(const std::string& name) const;

    // 列表
    std::vector<std::string> list_names() const;
    std::vector<protocol::EmbeddingProviderConfig> list_configs() const;

    // 事件回调：Provider 切换时通知
    void on_switch(std::function<void(const std::string& old_name, const std::string& new_name)> cb);

    // 重建索引信号（外部监听后执行重建）
    std::function<void()> on_rebuild_index;

private:

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<EmbeddingProvider>> providers_;
    std::shared_ptr<EmbeddingProvider> current_;
    std::function<void(const std::string&, const std::string&)> switch_cb_;
};

} // namespace chatdb
