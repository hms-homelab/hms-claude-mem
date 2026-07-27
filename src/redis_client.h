#pragma once
#include "memory_store.h"
#include <string>
#include <vector>
#include <optional>
#include <mutex>

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

    // ── Locking discipline ────────────────────────────────────────────────
    // hiredis redisContext is NOT thread safe and there is exactly one per
    // client, so every public entry point takes mutex_ for its whole duration.
    //
    // The internal helpers below assume the caller already holds it. That split
    // exists because ensureConnected() calls connect() and isConnected(), which
    // are themselves public: locking in those too would self-deadlock on a
    // non-recursive mutex. Public connect()/isConnected() are thin locking
    // wrappers over the *Unlocked variants.
    //
    // Rule: anything named *Unlocked, plus ensureConnected/dropConnection, must
    // only ever be called with mutex_ held.
    bool connectUnlocked();
    bool isConnectedUnlocked() const;
    bool ensureConnected();     // caller holds mutex_
    void dropConnection();      // caller holds mutex_

    mutable std::mutex mutex_;

    std::string host_;
    int port_;
    std::string namespace_;
    void* ctx_; // redisContext*, guarded by mutex_
    int max_retries_; // backoff attempts before giving up (>= 10)
};
