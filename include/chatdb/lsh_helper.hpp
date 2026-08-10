#pragma once
#include <vector>
#include <cstdint>
#include <random>
#include <cmath>
#include <string>

namespace chatdb {

class LSHHelper {
public:
    explicit LSHHelper(int dim, int num_planes = 16) : dim_(dim), num_planes_(num_planes) {
        std::mt19937 gen(42);
        std::normal_distribution<float> dist(0.0f, 1.0f);
        planes_.reserve(num_planes * dim);
        for (int i = 0; i < num_planes * dim; ++i) {
            planes_.push_back(dist(gen));
        }
    }

    // 生成 LSH 签名
    uint64_t signature(const std::vector<float>& vec) const {
        uint64_t sig = 0;
        for (int i = 0; i < num_planes_; ++i) {
            float dot = 0.0f;
            for (size_t j = 0; j < vec.size() && j < static_cast<size_t>(dim_); ++j) {
                dot += vec[j] * planes_[i * dim_ + j];
            }
            if (dot >= 0) sig |= (1ULL << i);
        }
        return sig;
    }

    // 汉明距离
    static int hamming_distance(uint64_t a, uint64_t b) {
        return __builtin_popcountll(a ^ b);
    }

    // 快速近似去重：签名相同或汉明距离 <= max_hamming
    bool is_approximate_duplicate(const std::vector<float>& a, const std::vector<float>& b,
                                   int max_hamming = 1) const {
        auto sig_a = signature(a);
        auto sig_b = signature(b);
        return hamming_distance(sig_a, sig_b) <= max_hamming;
    }

    // 批量签名
    std::vector<uint64_t> signature_batch(const std::vector<std::vector<float>>& vecs) const {
        std::vector<uint64_t> sigs;
        sigs.reserve(vecs.size());
        for (const auto& v : vecs) sigs.push_back(signature(v));
        return sigs;
    }

private:
    int dim_;
    int num_planes_;
    std::vector<float> planes_;
};

} // namespace chatdb
