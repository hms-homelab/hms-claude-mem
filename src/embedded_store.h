#pragma once
#include "memory_store.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>

// In-process, file-backed memory store. No external service needed.
//
// Write-back model: in-memory is the source of truth at runtime. Mutations mark
// the store dirty; a background flusher writes a crash-safe snapshot to disk once
// the store has been idle for `flush_idle_ms_`. A final flush runs on shutdown
// (destructor + flushNow() from a signal handler). Search is brute-force cosine
// over the in-memory vectors — sub-millisecond at the personal-memory scale.
//
// Vectors are assumed L2-normalized (the embedder normalizes), so similarity is
// the dot product mapped to [0,1] via (1+cos)/2 — matching Redis VSIM's score.
class EmbeddedStore : public IMemoryStore {
public:
    EmbeddedStore(const std::string& ns, const std::string& path_override = "",
                  int flush_idle_ms = 2000);
    ~EmbeddedStore() override;

    bool connect() override;            // lazily loads the file (idempotent)
    bool isConnected() const override;

    bool vectorAdd(const std::string& key, const std::vector<float>& embedding) override;
    bool vectorRemove(const std::string& key) override;
    std::vector<std::pair<std::string, double>> vectorSearch(
        const std::vector<float>& query, int top_k) override;

    bool hashSet(const std::string& key, const MemoryEntry& entry) override;
    std::optional<MemoryEntry> hashGet(const std::string& key) override;
    bool hashDelete(const std::string& key) override;
    std::vector<std::string> scanKeys(const std::string& pattern, int count) override;

    const std::string& getNamespace() const override { return namespace_; }

    // Force a synchronous flush of pending changes (signal handler / shutdown).
    void flushNow();

private:
    struct Record { MemoryEntry meta; std::vector<float> vec; };

    void ensureLoaded();                // load file + start flusher, once
    std::string resolvePath() const;
    void loadFromDisk();                // caller holds mutex_
    std::vector<char> serializeLocked() const; // caller holds mutex_
    bool writeAtomic(const std::vector<char>& buf) const;
    void markDirtyLocked();             // caller holds mutex_
    void flusherLoop();

    std::string namespace_;
    std::string path_override_;
    std::string path_;
    int flush_idle_ms_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Record> records_;
    std::once_flag init_flag_;
    bool loaded_ = false;

    bool dirty_ = false;
    std::chrono::steady_clock::time_point last_mutation_;

    std::thread flusher_;
    std::atomic<bool> stop_{false};
    std::condition_variable cv_;
};
