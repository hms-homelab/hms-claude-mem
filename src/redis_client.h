#pragma once
#include "memory_store.h"
#include <string>
#include <vector>
#include <optional>

class RedisClient : public IMemoryStore {
public:
    RedisClient(const std::string& host = "127.0.0.1", int port = 6379,
                const std::string& ns = "default");
    ~RedisClient() override;

    // Single connection attempt. Returns true on success.
    bool connect() override;
    bool isConnected() const override;

    // Vector operations
    bool vectorAdd(const std::string& key, const std::vector<float>& embedding) override;
    bool vectorRemove(const std::string& key) override;
    std::vector<std::pair<std::string, double>> vectorSearch(
        const std::vector<float>& query, int top_k) override;

    // Hash operations (metadata + value storage)
    bool hashSet(const std::string& key, const MemoryEntry& entry) override;
    std::optional<MemoryEntry> hashGet(const std::string& key) override;
    bool hashDelete(const std::string& key) override;
    std::vector<std::string> scanKeys(const std::string& pattern, int count) override;

    const std::string& getNamespace() const override { return namespace_; }

private:
    std::string vectorKey() const;
    std::string dataKey(const std::string& key) const;
    std::string dataPrefix() const;

    // Lazily (re)establish the connection with exponential backoff.
    // Returns true if a live context is available, false after giving up.
    bool ensureConnected();
    // Tear down a dead/errored context so the next op reconnects.
    void dropConnection();

    std::string host_;
    int port_;
    std::string namespace_;
    void* ctx_; // redisContext*
    int max_retries_; // backoff attempts before giving up (>= 10)
};
