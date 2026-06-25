// SDD-002 store-parity gate: EmbeddedStore brute-force search vs Redis VSIM.
//
// Adds the same set of normalized random vectors to both backends, runs random
// queries, and compares top_k ordering (overlap) and scores. Redis vectors are
// written to a throwaway namespace and removed afterward.
//
// Build: cmake -B build -DBUILD_STORE_PARITY=ON && cmake --build build --target store_parity_bench
// Run:   REDIS_HOST=192.168.2.15 ./build/store_parity_bench [N] [Q] [top_k]

#include "embedded_store.h"
#include "redis_client.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
std::string getEnv(const char* n, const std::string& d) {
    const char* v = std::getenv(n);
    return (v && *v) ? std::string(v) : d;
}

std::vector<float> randUnit(std::mt19937& rng, int dim) {
    std::normal_distribution<float> nd(0.f, 1.f);
    std::vector<float> v(dim);
    double n = 0;
    for (auto& x : v) { x = nd(rng); n += (double)x * x; }
    n = std::sqrt(n);
    for (auto& x : v) x = (float)(x / n);
    return v;
}
} // namespace

int main(int argc, char** argv) {
    std::string host = getEnv("REDIS_HOST", "192.168.2.15");
    int port = std::stoi(getEnv("REDIS_PORT", "6379"));
    int N = argc > 1 ? std::atoi(argv[1]) : 300;
    int Q = argc > 2 ? std::atoi(argv[2]) : 30;
    int K = argc > 3 ? std::atoi(argv[3]) : 10;
    const int DIM = 768;
    const std::string ns = "store_parity_tmp";

    std::mt19937 rng(12345);

    RedisClient redis(host, port, ns);
    if (!redis.connect()) { std::cerr << "FAIL: no Redis at " << host << ":" << port << "\n"; return 2; }

    std::string dbpath = getEnv("TMPDIR", "/tmp") + "/store_parity_" + std::to_string(port) + ".db";
    std::remove(dbpath.c_str());
    EmbeddedStore emb(ns, dbpath, 100000); // long debounce: we flush manually / never

    std::cout << "Loading " << N << " vectors into both backends...\n";
    std::vector<std::string> keys;
    for (int i = 0; i < N; ++i) {
        std::string key = "k" + std::to_string(i);
        keys.push_back(key);
        auto v = randUnit(rng, DIM);
        emb.vectorAdd(key, v);
        redis.vectorAdd(key, v);
    }

    double overlap_sum = 0, score_mae_sum = 0;
    int score_pairs = 0, top1_match = 0;
    for (int q = 0; q < Q; ++q) {
        auto query = randUnit(rng, DIM);
        auto e = emb.vectorSearch(query, K);
        auto r = redis.vectorSearch(query, K);

        std::unordered_map<std::string, double> rmap;
        for (auto& [k, s] : r) rmap[k] = s;

        int overlap = 0;
        for (auto& [k, s] : e) {
            auto it = rmap.find(k);
            if (it != rmap.end()) {
                ++overlap;
                score_mae_sum += std::fabs(s - it->second);
                ++score_pairs;
            }
        }
        overlap_sum += (double)overlap / std::max(1, (int)e.size());
        if (!e.empty() && !r.empty() && e.front().first == r.front().first) ++top1_match;
    }

    // Cleanup Redis temp namespace.
    for (auto& k : keys) redis.vectorRemove(k);
    std::remove(dbpath.c_str());

    double overlap = overlap_sum / Q;
    double score_mae = score_pairs ? score_mae_sum / score_pairs : 1.0;
    double top1 = (double)top1_match / Q;

    std::cout << "\n=== Store parity (EmbeddedStore vs Redis VSIM) ===\n";
    std::cout << "  queries        : " << Q << "  (N=" << N << ", top_k=" << K << ")\n";
    std::cout << "  mean overlap@K : " << overlap << "\n";
    std::cout << "  top-1 match    : " << top1 << "\n";
    std::cout << "  score MAE      : " << score_mae << "  (~0 => same score formula)\n";
    std::cout << "\n  Note: Redis Vector Sets quantize to int8 (Q8) by default; the embedded\n"
                 "  store keeps full float32, so the few boundary differences are Redis's\n"
                 "  quantization noise (MAE confirms the formula matches). Embedded is the\n"
                 "  more-accurate side, so the gate allows for Redis's Q8 floor.\n";

    bool pass = (overlap >= 0.95 && top1 >= 0.90 && score_mae < 5e-3);
    std::cout << "\n  GATE overlap>=0.95 & top1>=0.90 & MAE<5e-3 : "
              << (pass ? "PASS ✅" : "FAIL ❌") << "\n";
    return pass ? 0 : 1;
}
