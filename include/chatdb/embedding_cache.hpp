#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include <list>
#include <shared_mutex>
#include <optional>
#include <atomic>

namespace chatdb {

class EmbeddingCache {
public:
    explicit EmbeddingCache(size_t max_size = 1000) : max_size_(max_size) {}

    std::optional<std::vector<float>> get(const std::string& text) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = map_.find(text);
        if (it != map_.end()) {
            list_.splice(list_.end(), list_, it->second);
            ++hits_;
            return it->second->second;
        }
        ++misses_;
        return std::nullopt;
    }

    void put(const std::string& text, const std::vector<float>& embedding) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = map_.find(text);
        if (it != map_.end()) {
            it->second->second = embedding;
            list_.splice(list_.end(), list_, it->second);
            return;
        }

        if (map_.size() >= max_size_) {
            map_.erase(list_.front().first);
            list_.pop_front();
        }

        list_.emplace_back(text, embedding);
        map_[text] = std::prev(list_.end());
    }

    void clear() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        map_.clear();
        list_.clear();
    }

    size_t size() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return map_.size();
    }

    size_t hit_count() const { return hits_.load(); }
    size_t miss_count() const { return misses_.load(); }
    double hit_rate() const {
        size_t total = hits_.load() + misses_.load();
        return total > 0 ? static_cast<double>(hits_.load()) / total : 0.0;
    }

private:
    size_t max_size_;
    std::list<std::pair<std::string, std::vector<float>>> list_;
    std::unordered_map<std::string, decltype(list_)::iterator> map_;
    mutable std::shared_mutex mutex_;
    std::atomic<size_t> hits_{0};
    std::atomic<size_t> misses_{0};
};

} // namespace chatdb
