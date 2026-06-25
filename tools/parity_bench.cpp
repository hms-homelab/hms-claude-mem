// SDD-001 §6 parity gate.
//
// Reconstructs each memory's embed_text exactly as MemoryTools::store does
// (category + ": " + key), then compares the in-process local Q8 embedding
// against the live Ollama embedding for the same text. Reports the cosine
// distribution and PASS/FAIL at the 0.98 p5 gate. Read-only against Redis.
//
// Build:  cmake -B build -DWITH_LOCAL_EMBED=ON -DBUILD_PARITY_BENCH=ON && cmake --build build --target parity_bench
// Run:    REDIS_HOST=192.168.2.15 NAMESPACE=global \
//         EMBED_HOST=http://192.168.2.15:11434 \
//         LOCAL_EMBED_MODEL=/path/to/nomic-embed-text-v1.5.Q8_0.gguf \
//         ./build/parity_bench [sample_size]

#include "redis_client.h"
#include "embedding_client.h"
#include "local_embedder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::string getEnv(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

double cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return -2.0;
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += (double)a[i] * b[i];
        na  += (double)a[i] * a[i];
        nb  += (double)b[i] * b[i];
    }
    if (na == 0 || nb == 0) return -2.0;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}
} // namespace

int main(int argc, char** argv) {
    std::string redis_host = getEnv("REDIS_HOST", "192.168.2.15");
    int redis_port = std::stoi(getEnv("REDIS_PORT", "6379"));
    std::string ns = getEnv("NAMESPACE", "global");
    std::string embed_host = getEnv("EMBED_HOST", "http://192.168.2.15:11434");
    std::string embed_model = getEnv("EMBED_MODEL", "nomic-embed-text");
    std::string local_model = getEnv("LOCAL_EMBED_MODEL", "");
    int sample = (argc > 1) ? std::atoi(argv[1]) : 200;

    RedisClient redis(redis_host, redis_port, ns);
    if (!redis.connect()) {
        std::cerr << "FAIL: cannot connect to Redis at " << redis_host << ":" << redis_port << "\n";
        return 2;
    }

    auto keys = redis.scanKeys("*", std::max(sample * 4, 2000));
    if (keys.empty()) {
        std::cerr << "FAIL: no memories found in namespace '" << ns << "'\n";
        return 2;
    }
    std::cout << "Scanned " << keys.size() << " keys in namespace '" << ns << "'\n";

    LocalEmbedder local(local_model);
    EmbeddingClient ollama(embed_host, embed_model, EmbedProvider::Ollama);

    std::vector<double> cosines;
    int compared = 0, errors = 0;
    for (const auto& key : keys) {
        if (compared >= sample) break;
        auto entry = redis.hashGet(key);
        if (!entry) continue;
        std::string text = entry->category + ": " + key; // matches MemoryTools::store
        try {
            auto a = local.embed(text);
            auto b = ollama.embed(text);
            double c = cosine(a, b);
            if (c < -1.5) { ++errors; continue; }
            cosines.push_back(c);
            ++compared;
            if (compared % 25 == 0) std::cout << "  ..." << compared << " compared\n";
        } catch (const std::exception& e) {
            if (++errors <= 3) std::cerr << "  embed error: " << e.what() << "\n";
        }
    }

    if (cosines.empty()) {
        std::cerr << "FAIL: no successful comparisons (" << errors << " errors)\n";
        return 2;
    }

    std::sort(cosines.begin(), cosines.end());
    double sum = 0; for (double c : cosines) sum += c;
    double mean = sum / cosines.size();
    double minv = cosines.front();
    double p5 = cosines[(size_t)(0.05 * cosines.size())];

    std::cout << "\n=== Parity (local Q8 vs Ollama) ===\n";
    std::cout << "  compared : " << cosines.size() << "  (errors: " << errors << ")\n";
    std::cout << "  min      : " << minv << "\n";
    std::cout << "  p5       : " << p5 << "\n";
    std::cout << "  mean     : " << mean << "\n";

    const double GATE = 0.98;
    bool pass = (p5 >= GATE);
    std::cout << "\n  GATE p5 >= " << GATE << " : " << (pass ? "PASS ✅" : "FAIL ❌") << "\n";
    return pass ? 0 : 1;
}
