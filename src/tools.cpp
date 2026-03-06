#include "tools.h"
#include <chrono>
#include <iomanip>
#include <sstream>

MemoryTools::MemoryTools(RedisClient& redis, EmbeddingClient& embedder)
    : redis_(redis), embedder_(embedder) {}

std::string MemoryTools::now() {
    auto t = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(t);
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

json MemoryTools::store(const std::string& key, const std::string& value,
                        const std::string& category) {
    // Embed the category:key combination for semantic search
    std::string embed_text = category + ": " + key;
    std::vector<float> embedding;
    try {
        embedding = embedder_.embed(embed_text);
    } catch (const std::exception& e) {
        return {{"error", std::string("Embedding failed: ") + e.what()}};
    }

    // Check if key exists (update vs create)
    auto existing = redis_.hashGet(key);
    std::string created = existing ? existing->created_at : now();

    // Store vector
    if (!redis_.vectorAdd(key, embedding)) {
        return {{"error", "Failed to store vector"}};
    }

    // Store metadata
    MemoryEntry entry{key, value, category, created, now()};
    if (!redis_.hashSet(key, entry)) {
        redis_.vectorRemove(key); // rollback
        return {{"error", "Failed to store metadata"}};
    }

    return {{"status", "stored"}, {"key", key}, {"category", category},
            {"is_update", existing.has_value()}};
}

json MemoryTools::search(const std::string& query, int top_k) {
    std::vector<float> embedding;
    try {
        embedding = embedder_.embed(query);
    } catch (const std::exception& e) {
        return {{"error", std::string("Embedding failed: ") + e.what()}};
    }

    auto results = redis_.vectorSearch(embedding, top_k);

    json matches = json::array();
    for (auto& [key, score] : results) {
        auto entry = redis_.hashGet(key);
        if (entry) {
            matches.push_back({
                {"key", key},
                {"value", entry->value},
                {"category", entry->category},
                {"similarity", score}
            });
        }
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
            {"updated_at", entry->updated_at}};
}

json MemoryTools::remove(const std::string& key) {
    redis_.vectorRemove(key);
    redis_.hashDelete(key);
    return {{"status", "deleted"}, {"key", key}};
}

json MemoryTools::list(const std::string& category, int limit) {
    auto keys = redis_.scanKeys("*", limit * 2); // over-fetch for filtering

    json items = json::array();
    for (auto& key : keys) {
        if (static_cast<int>(items.size()) >= limit) break;

        auto entry = redis_.hashGet(key);
        if (!entry) continue;
        if (!category.empty() && entry->category != category) continue;

        items.push_back({
            {"key", key},
            {"category", entry->category},
            {"created_at", entry->created_at},
            {"updated_at", entry->updated_at}
        });
    }

    return {{"items", items}, {"count", items.size()}};
}
