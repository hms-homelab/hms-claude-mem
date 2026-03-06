#pragma once
#include "redis_client.h"
#include "embedding_client.h"
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

class MemoryTools {
public:
    MemoryTools(RedisClient& redis, EmbeddingClient& embedder);

    json store(const std::string& key, const std::string& value,
               const std::string& category = "general");
    json search(const std::string& query, int top_k = 5);
    json get(const std::string& key);
    json remove(const std::string& key);
    json list(const std::string& category = "", int limit = 20);

private:
    std::string now();
    RedisClient& redis_;
    EmbeddingClient& embedder_;
};
