#include "chatdb/embedding_provider.hpp"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include "json.hpp"
#include <httplib.h>

namespace chatdb {

using json = nlohmann::json;

// ========== OllamaProvider ==========
class OllamaProvider::Impl {
public:
    explicit Impl(const Config& cfg) : cfg_(cfg) {
        client_ = std::make_unique<httplib::Client>(fmt::format("{}:{}", cfg_.host, cfg_.port));
        client_->set_connection_timeout(cfg_.timeout_ms / 1000.0);
        client_->set_read_timeout(cfg_.timeout_ms / 1000.0);
    }
    Config cfg_;
    std::unique_ptr<httplib::Client> client_;
};

OllamaProvider::OllamaProvider(const Config& cfg) : cfg_(cfg), impl_(std::make_unique<Impl>(cfg)) {}
OllamaProvider::~OllamaProvider() = default;

bool OllamaProvider::test_connection() {
    try {
        auto res = impl_->client_->Get("/api/tags");
        return res && res->status == 200;
    } catch (...) { return false; }
}

std::vector<float> OllamaProvider::embed(const std::string& text) {
    json req = {{"model", cfg_.model}, {"input", text}};
    for (int i = 0; i < cfg_.max_retries; ++i) {
        auto res = impl_->client_->Post("/api/embed", req.dump(), "application/json");
        if (res && res->status == 200) {
            auto j = json::parse(res->body);
            if (j.contains("embeddings") && !j["embeddings"].empty()) {
                std::vector<float> vec;
                for (auto& v : j["embeddings"][0]) vec.push_back(v.get<float>());
                return vec;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    throw std::runtime_error("Ollama embed failed");
}

std::vector<std::vector<float>> OllamaProvider::embed_batch(const std::vector<std::string>& texts) {
    json req = {{"model", cfg_.model}, {"input", texts}};
    auto res = impl_->client_->Post("/api/embed", req.dump(), "application/json");
    if (!res || res->status != 200) throw std::runtime_error("Ollama batch embed failed");

    auto j = json::parse(res->body);
    std::vector<std::vector<float>> results;
    for (auto& emb : j["embeddings"]) {
        std::vector<float> vec;
        for (auto& v : emb) vec.push_back(v.get<float>());
        results.push_back(std::move(vec));
    }
    return results;
}

// ========== OpenAIProvider ==========
class OpenAIProvider::Impl {
public:
    explicit Impl(const Config& cfg) : cfg_(cfg) {
        client_ = std::make_unique<httplib::Client>(cfg_.api_base);
        client_->set_connection_timeout(cfg_.timeout_ms / 1000.0);
        client_->set_read_timeout(cfg_.timeout_ms / 1000.0);
        if (!cfg_.api_key.empty()) {
            client_->set_default_headers({{"Authorization", fmt::format("Bearer {}", cfg_.api_key)}});
        }
    }
    Config cfg_;
    std::unique_ptr<httplib::Client> client_;
};

OpenAIProvider::OpenAIProvider(const Config& cfg) : cfg_(cfg), impl_(std::make_unique<Impl>(cfg)) {}
OpenAIProvider::~OpenAIProvider() = default;

bool OpenAIProvider::test_connection() {
    try {
        auto res = impl_->client_->Get("/models");
        return res && (res->status == 200 || res->status == 401);  // 401 也代表服务器在线
    } catch (...) { return false; }
}

std::vector<float> OpenAIProvider::embed(const std::string& text) {
    json req = {{"model", cfg_.model}, {"input", text}};
    for (int i = 0; i < cfg_.max_retries; ++i) {
        auto res = impl_->client_->Post("/embeddings", req.dump(), "application/json");
        if (res && res->status == 200) {
            auto j = json::parse(res->body);
            if (j.contains("data") && !j["data"].empty()) {
                std::vector<float> vec;
                for (auto& v : j["data"][0]["embedding"]) vec.push_back(v.get<float>());
                return vec;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    throw std::runtime_error(fmt::format("{} embed failed", cfg_.name));
}

std::vector<std::vector<float>> OpenAIProvider::embed_batch(const std::vector<std::string>& texts) {
    json req = {{"model", cfg_.model}, {"input", texts}};
    auto res = impl_->client_->Post("/embeddings", req.dump(), "application/json");
    if (!res || res->status != 200) throw std::runtime_error("OpenAI batch embed failed");

    auto j = json::parse(res->body);
    std::vector<std::vector<float>> results;
    for (auto& item : j["data"]) {
        std::vector<float> vec;
        for (auto& v : item["embedding"]) vec.push_back(v.get<float>());
        results.push_back(std::move(vec));
    }
    return results;
}

// ========== EmbeddingProviderManager ==========
EmbeddingProviderManager& EmbeddingProviderManager::instance() {
    static EmbeddingProviderManager inst;
    return inst;
}

void EmbeddingProviderManager::register_provider(std::shared_ptr<EmbeddingProvider> provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    providers_[provider->name()] = provider;
    if (!current_) {
        current_ = provider;
    }
}

void EmbeddingProviderManager::remove_provider(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    providers_.erase(name);
    if (current_ && current_->name() == name) {
        current_ = providers_.empty() ? nullptr : providers_.begin()->second;
    }
}

bool EmbeddingProviderManager::switch_to(const std::string& name) {
    std::shared_ptr<EmbeddingProvider> new_provider;
    std::shared_ptr<EmbeddingProvider> old_provider;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = providers_.find(name);
        if (it == providers_.end()) {
            spdlog::error("Provider '{}' not found", name);
            return false;
        }
        if (current_ && current_->name() == name) {
            spdlog::info("Provider '{}' is already active", name);
            return true;
        }
        old_provider = current_;
        new_provider = it->second;
        current_ = new_provider;
    }

    spdlog::info("Switched embedding provider: {} -> {}", 
                 old_provider ? old_provider->name() : "none", 
                 new_provider->name());

    if (switch_cb_) {
        switch_cb_(old_provider ? old_provider->name() : "", new_provider->name());
    }

    // 触发索引重建信号
    if (on_rebuild_index) {
        on_rebuild_index();
    }

    return true;
}

std::shared_ptr<EmbeddingProvider> EmbeddingProviderManager::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

std::shared_ptr<EmbeddingProvider> EmbeddingProviderManager::get(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = providers_.find(name);
    return it != providers_.end() ? it->second : nullptr;
}

std::vector<std::string> EmbeddingProviderManager::list_names() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (const auto& [k, v] : providers_) names.push_back(k);
    return names;
}

std::vector<protocol::EmbeddingProviderConfig> EmbeddingProviderManager::list_configs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<protocol::EmbeddingProviderConfig> configs;
    for (const auto& [k, p] : providers_) {
        protocol::EmbeddingProviderConfig c;
        c.name = p->name();
        c.display_name = p->display_name();
        c.model = "unknown";  // 实际应从内部获取
        c.embedding_dim = p->dimension();
        c.timeout_ms = p->timeout_ms();
        c.is_active = (current_ && current_->name() == p->name());
        configs.push_back(c);
    }
    return configs;
}

void EmbeddingProviderManager::on_switch(std::function<void(const std::string&, const std::string&)> cb) {
    switch_cb_ = std::move(cb);
}

} // namespace chatdb
