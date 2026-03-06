#pragma once
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>

struct MemoryEntry {
    std::string key;
    std::string value;
    std::string category;
    std::string created_at;
    std::string updated_at;
};

struct SearchResult {
    std::string key;
    std::string value;
    std::string category;
    double score;
};

class RedisClient {
public:
    RedisClient(const std::string& host = "127.0.0.1", int port = 6379);
    ~RedisClient();

    bool connect();
    bool isConnected() const;

    // Vector operations
    bool vectorAdd(const std::string& key, const std::vector<float>& embedding);
    bool vectorRemove(const std::string& key);
    std::vector<std::pair<std::string, double>> vectorSearch(
        const std::vector<float>& query, int top_k);

    // Hash operations (metadata + value storage)
    bool hashSet(const std::string& key, const MemoryEntry& entry);
    std::optional<MemoryEntry> hashGet(const std::string& key);
    bool hashDelete(const std::string& key);
    std::vector<std::string> scanKeys(const std::string& pattern, int count);

private:
    static constexpr const char* VECTOR_KEY = "claude:mem:vectors";
    static constexpr const char* DATA_PREFIX = "claude:mem:data:";

    std::string host_;
    int port_;
    void* ctx_; // redisContext*
};
