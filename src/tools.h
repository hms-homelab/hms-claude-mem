#pragma once
#include "redis_client.h"
#include "embedding_client.h"
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

class MemoryTools {
public:
    MemoryTools(RedisClient& redis, EmbeddingClient& embedder,
                double decay_rate = 0.01);

    json store(const std::string& key, const std::string& value,
               const std::string& category = "general", bool pinned = false);
    json search(const std::string& query, int top_k = 5,
                const std::string& category = "");
    json get(const std::string& key);
    json remove(const std::string& key);
    json list(const std::string& category = "", int limit = 20);

private:
    std::string now();
    double ageDays(const std::string& timestamp);
    double computeFinalScore(double similarity, double age_days, bool pinned);

    RedisClient& redis_;
    EmbeddingClient& embedder_;
    double decay_rate_; // score penalty per day (0.01 = 1% per day)
};
