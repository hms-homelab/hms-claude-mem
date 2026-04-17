#include "tools.h"
#include <chrono>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <ctime>

#ifdef _WIN32
  #define hms_timegm _mkgmtime
#else
  #define hms_timegm timegm
#endif

MemoryTools::MemoryTools(RedisClient& redis, EmbeddingClient& embedder,
                         double decay_rate)
    : redis_(redis), embedder_(embedder), decay_rate_(decay_rate) {}

std::string MemoryTools::now() {
    auto t = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(t);
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

double MemoryTools::ageDays(const std::string& timestamp) {
    std::tm tm = {};
    std::istringstream ss(timestamp);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    if (ss.fail()) return 0.0;

    auto stored = std::chrono::system_clock::from_time_t(hms_timegm(&tm));
    auto current = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::hours>(current - stored);
    return duration.count() / 24.0;
}

double MemoryTools::computeFinalScore(double similarity, double age_days, bool pinned) {
    if (pinned) return similarity; // pinned memories don't decay
    double freshness = std::max(0.0, 1.0 - decay_rate_ * age_days);
    return similarity * freshness;
}

json MemoryTools::store(const std::string& key, const std::string& value,
                        const std::string& category, bool pinned) {
    std::string embed_text = category + ": " + key;
    std::vector<float> embedding;
    try {
        embedding = embedder_.embed(embed_text);
    } catch (const std::exception& e) {
        return {{"error", std::string("Embedding failed: ") + e.what()}};
    }

    auto existing = redis_.hashGet(key);
    std::string created = existing ? existing->created_at : now();

    if (!redis_.vectorAdd(key, embedding)) {
        return {{"error", "Failed to store vector"}};
    }

    MemoryEntry entry{key, value, category, created, now(), pinned};
    if (!redis_.hashSet(key, entry)) {
        redis_.vectorRemove(key);
        return {{"error", "Failed to store metadata"}};
    }

    return {{"status", "stored"}, {"key", key}, {"category", category},
            {"pinned", pinned}, {"is_update", existing.has_value()}};
}

json MemoryTools::search(const std::string& query, int top_k,
                         const std::string& category) {
    std::vector<float> embedding;
    try {
        embedding = embedder_.embed(query);
    } catch (const std::exception& e) {
        return {{"error", std::string("Embedding failed: ") + e.what()}};
    }

    // Over-fetch when filtering by category since some results will be dropped
    int fetch_count = category.empty() ? top_k : top_k * 3;
    auto results = redis_.vectorSearch(embedding, fetch_count);

    struct ScoredResult {
        std::string key;
        std::string value;
        std::string category;
        double similarity;
        double final_score;
        double age_days;
        bool pinned;
    };

    std::vector<ScoredResult> scored;
    for (auto& [key, sim] : results) {
        auto entry = redis_.hashGet(key);
        if (!entry) continue;

        // Category filter
        if (!category.empty() && entry->category != category) continue;

        double age = ageDays(entry->updated_at);
        double final_score = computeFinalScore(sim, age, entry->pinned);

        scored.push_back({key, entry->value, entry->category,
                         sim, final_score, age, entry->pinned});
    }

    // Re-sort by final score (similarity * freshness)
    std::sort(scored.begin(), scored.end(),
              [](const ScoredResult& a, const ScoredResult& b) {
                  return a.final_score > b.final_score;
              });

    // Trim to top_k
    if (static_cast<int>(scored.size()) > top_k) {
        scored.resize(top_k);
    }

    json matches = json::array();
    for (auto& r : scored) {
        matches.push_back({
            {"key", r.key},
            {"value", r.value},
            {"category", r.category},
            {"similarity", r.similarity},
            {"final_score", r.final_score},
            {"age_days", std::round(r.age_days * 10) / 10},
            {"pinned", r.pinned}
        });
    }

    return {{"results", matches}, {"count", matches.size()}};
}

json MemoryTools::get(const std::string& key) {
    auto entry = redis_.hashGet(key);
    if (!entry) {
        return {{"error", "Key not found: " + key}};
    }
    return {{"key", entry->key}, {"value", entry->value},
            {"category", entry->category}, {"created_at", entry->created_at},
            {"updated_at", entry->updated_at}, {"pinned", entry->pinned}};
}

json MemoryTools::remove(const std::string& key) {
    redis_.vectorRemove(key);
    redis_.hashDelete(key);
    return {{"status", "deleted"}, {"key", key}};
}

json MemoryTools::list(const std::string& category, int limit) {
    auto keys = redis_.scanKeys("*", limit * 2);

    json items = json::array();
    for (auto& key : keys) {
        if (static_cast<int>(items.size()) >= limit) break;

        auto entry = redis_.hashGet(key);
        if (!entry) continue;
        if (!category.empty() && entry->category != category) continue;

        items.push_back({
            {"key", key},
            {"category", entry->category},
            {"pinned", entry->pinned},
            {"created_at", entry->created_at},
            {"updated_at", entry->updated_at}
        });
    }

    return {{"items", items}, {"count", items.size()}};
}
