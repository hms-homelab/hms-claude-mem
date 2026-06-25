#pragma once
#include <string>
#include <vector>
#include <optional>
#include <utility>

// A stored memory's metadata + value (vector lives separately in the index).
struct MemoryEntry {
    std::string key;
    std::string value;
    std::string category;
    std::string created_at;
    std::string updated_at;
    bool pinned = false;
};

struct SearchResult {
    std::string key;
    std::string value;
    std::string category;
    double score;
};

// Storage backend abstraction. Implemented by RedisClient (external) and
// EmbeddedStore (in-process, file-backed). This is the entire surface that
// MemoryTools depends on.
class IMemoryStore {
public:
    virtual ~IMemoryStore() = default;

    // Establish/verify the backend (connect to Redis, or load the file).
    virtual bool connect() = 0;
    virtual bool isConnected() const = 0;

    // Vector index
    virtual bool vectorAdd(const std::string& key, const std::vector<float>& embedding) = 0;
    virtual bool vectorRemove(const std::string& key) = 0;
    virtual std::vector<std::pair<std::string, double>> vectorSearch(
        const std::vector<float>& query, int top_k) = 0;

    // Metadata + value storage
    virtual bool hashSet(const std::string& key, const MemoryEntry& entry) = 0;
    virtual std::optional<MemoryEntry> hashGet(const std::string& key) = 0;
    virtual bool hashDelete(const std::string& key) = 0;
    virtual std::vector<std::string> scanKeys(const std::string& pattern, int count) = 0;

    virtual const std::string& getNamespace() const = 0;
};
