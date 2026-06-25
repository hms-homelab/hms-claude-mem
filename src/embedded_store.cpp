#include "embedded_store.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>

#if defined(_WIN32)
#include <io.h>
#define hms_fsync(fd) _commit(fd)
#define hms_fileno _fileno
#else
#include <unistd.h>
#define hms_fsync(fd) ::fsync(fd)
#define hms_fileno fileno
#endif

namespace fs = std::filesystem;

namespace {
constexpr char kMagic[4] = {'H', 'C', 'M', '1'};
constexpr uint32_t kVersion = 1;

void logErr(const std::string& msg) {
    std::cerr << "[claude-mem][store] " << msg << "\n";
}

int envInt(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    try { return std::stoi(v); } catch (...) { return fallback; }
}

std::string homeDir() {
    const char* h = std::getenv("HOME");
    if (h && *h) return h;
#if defined(_WIN32)
    const char* up = std::getenv("USERPROFILE");
    if (up && *up) return up;
#endif
    return ".";
}

// --- buffer append helpers ---
void putU8(std::vector<char>& b, uint8_t v) { b.push_back((char)v); }
void putU32(std::vector<char>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((char)((v >> (8 * i)) & 0xff));
}
void putStr(std::vector<char>& b, const std::string& s) {
    putU32(b, (uint32_t)s.size());
    b.insert(b.end(), s.begin(), s.end());
}
void putVec(std::vector<char>& b, const std::vector<float>& v) {
    putU32(b, (uint32_t)v.size());
    const char* p = reinterpret_cast<const char*>(v.data());
    b.insert(b.end(), p, p + v.size() * sizeof(float));
}

// --- buffer read helpers (bounds-checked; throw on underrun) ---
struct Reader {
    const char* p;
    const char* end;
    void need(size_t n) { if ((size_t)(end - p) < n) throw std::runtime_error("truncated store file"); }
    uint8_t u8() { need(1); return (uint8_t)*p++; }
    uint32_t u32() {
        need(4);
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= ((uint32_t)(uint8_t)p[i]) << (8 * i);
        p += 4;
        return v;
    }
    std::string str() {
        uint32_t n = u32();
        need(n);
        std::string s(p, p + n);
        p += n;
        return s;
    }
    std::vector<float> vec() {
        uint32_t n = u32();
        need((size_t)n * sizeof(float));
        std::vector<float> v(n);
        std::memcpy(v.data(), p, (size_t)n * sizeof(float));
        p += (size_t)n * sizeof(float);
        return v;
    }
};
} // namespace

EmbeddedStore::EmbeddedStore(const std::string& ns, const std::string& path_override,
                             int flush_idle_ms)
    : namespace_(ns), path_override_(path_override) {
    flush_idle_ms_ = envInt("STORE_FLUSH_IDLE_MS", flush_idle_ms);
    if (flush_idle_ms_ < 50) flush_idle_ms_ = 50;
}

EmbeddedStore::~EmbeddedStore() {
    stop_.store(true);
    cv_.notify_all();
    if (flusher_.joinable()) flusher_.join();
    flushNow(); // final durability on clean shutdown
}

std::string EmbeddedStore::resolvePath() const {
    if (!path_override_.empty()) return path_override_;
    return (fs::path(homeDir()) / ".hms-claude-mem" / "store" / (namespace_ + ".db")).string();
}

void EmbeddedStore::ensureLoaded() {
    std::call_once(init_flag_, [this]() {
        path_ = resolvePath();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loadFromDisk();
            loaded_ = true;
        }
        flusher_ = std::thread([this]() { flusherLoop(); });
    });
}

void EmbeddedStore::loadFromDisk() {
    std::error_code ec;
    if (!fs::exists(path_, ec) || ec) {
        logErr("no store file yet at " + path_ + " (starting empty)");
        return;
    }
    FILE* f = fopen(path_.c_str(), "rb");
    if (!f) { logErr("cannot open " + path_ + " (starting empty)"); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<char> buf((size_t)std::max(0L, sz));
    size_t got = sz > 0 ? fread(buf.data(), 1, (size_t)sz, f) : 0;
    fclose(f);
    if (got != (size_t)sz) { logErr("short read on " + path_ + " (starting empty)"); return; }

    try {
        Reader r{buf.data(), buf.data() + buf.size()};
        r.need(4);
        if (std::memcmp(r.p, kMagic, 4) != 0) throw std::runtime_error("bad magic");
        r.p += 4;
        uint32_t ver = r.u32();
        if (ver != kVersion) throw std::runtime_error("unsupported version");
        uint32_t count = r.u32();
        for (uint32_t i = 0; i < count; ++i) {
            Record rec;
            rec.meta.key = r.str();
            rec.meta.value = r.str();
            rec.meta.category = r.str();
            rec.meta.created_at = r.str();
            rec.meta.updated_at = r.str();
            rec.meta.pinned = r.u8() != 0;
            rec.vec = r.vec();
            records_[rec.meta.key] = std::move(rec);
        }
        logErr("loaded " + std::to_string(records_.size()) + " memories from " + path_);
    } catch (const std::exception& e) {
        records_.clear();
        logErr(std::string("corrupt store file (") + e.what() + "); starting empty");
    }
}

std::vector<char> EmbeddedStore::serializeLocked() const {
    std::vector<char> b;
    b.insert(b.end(), kMagic, kMagic + 4);
    putU32(b, kVersion);
    putU32(b, (uint32_t)records_.size());
    for (const auto& [key, rec] : records_) {
        putStr(b, rec.meta.key);
        putStr(b, rec.meta.value);
        putStr(b, rec.meta.category);
        putStr(b, rec.meta.created_at);
        putStr(b, rec.meta.updated_at);
        putU8(b, rec.meta.pinned ? 1 : 0);
        putVec(b, rec.vec);
    }
    return b;
}

bool EmbeddedStore::writeAtomic(const std::vector<char>& buf) const {
    std::error_code ec;
    fs::path parent = fs::path(path_).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);

    std::string tmp = path_ + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { logErr("cannot open " + tmp + " for write"); return false; }
    bool ok = (fwrite(buf.data(), 1, buf.size(), f) == buf.size());
    fflush(f);
    hms_fsync(hms_fileno(f));
    fclose(f);
    if (!ok) { logErr("short write to " + tmp); return false; }

    fs::rename(tmp, path_, ec);
    if (ec) {
        // Windows: rename can fail if target exists — replace explicitly.
        fs::remove(path_, ec);
        fs::rename(tmp, path_, ec);
    }
    if (ec) { logErr("rename failed: " + ec.message()); return false; }
    return true;
}

void EmbeddedStore::markDirtyLocked() {
    dirty_ = true;
    last_mutation_ = std::chrono::steady_clock::now();
}

void EmbeddedStore::flusherLoop() {
    int poll = std::min(flush_idle_ms_, 500);
    std::unique_lock<std::mutex> lk(mutex_);
    while (!stop_.load()) {
        cv_.wait_for(lk, std::chrono::milliseconds(poll));
        if (stop_.load()) break;
        if (!dirty_) continue;
        auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_mutation_).count();
        if (idle < flush_idle_ms_) continue;

        auto snapshot = serializeLocked();
        auto before = last_mutation_;
        lk.unlock();
        bool ok = writeAtomic(snapshot);
        lk.lock();
        // Only mark clean if no mutation happened while we were writing.
        if (ok && last_mutation_ == before) dirty_ = false;
    }
}

void EmbeddedStore::flushNow() {
    std::unique_lock<std::mutex> lk(mutex_);
    if (!dirty_) return;
    auto snapshot = serializeLocked();
    auto before = last_mutation_;
    lk.unlock();
    bool ok = writeAtomic(snapshot);
    lk.lock();
    if (ok && last_mutation_ == before) dirty_ = false;
}

bool EmbeddedStore::connect() {
    ensureLoaded();
    return true;
}

bool EmbeddedStore::isConnected() const {
    return loaded_;
}

bool EmbeddedStore::vectorAdd(const std::string& key, const std::vector<float>& embedding) {
    ensureLoaded();
    std::lock_guard<std::mutex> lock(mutex_);
    records_[key].vec = embedding; // upsert vector; meta filled by hashSet
    records_[key].meta.key = key;
    markDirtyLocked();
    return true;
}

bool EmbeddedStore::vectorRemove(const std::string& key) {
    ensureLoaded();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(key);
    if (it != records_.end()) {
        it->second.vec.clear();
        if (it->second.meta.value.empty() && it->second.meta.category.empty())
            records_.erase(it);
        markDirtyLocked();
    }
    return true;
}

std::vector<std::pair<std::string, double>> EmbeddedStore::vectorSearch(
    const std::vector<float>& query, int top_k) {
    ensureLoaded();
    std::lock_guard<std::mutex> lock(mutex_);

    // Brute-force cosine. Vectors are L2-normalized, so cos == dot; map to [0,1]
    // via (1+cos)/2 to match Redis VSIM's score convention.
    std::vector<std::pair<std::string, double>> scored;
    scored.reserve(records_.size());
    for (const auto& [key, rec] : records_) {
        if (rec.vec.size() != query.size() || rec.vec.empty()) continue;
        double dot = 0.0;
        for (size_t i = 0; i < query.size(); ++i) dot += (double)query[i] * rec.vec[i];
        scored.emplace_back(key, (1.0 + dot) / 2.0);
    }

    if ((int)scored.size() > top_k) {
        std::partial_sort(scored.begin(), scored.begin() + top_k, scored.end(),
                          [](const auto& a, const auto& b) { return a.second > b.second; });
        scored.resize(top_k);
    } else {
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
    }
    return scored;
}

bool EmbeddedStore::hashSet(const std::string& key, const MemoryEntry& entry) {
    ensureLoaded();
    std::lock_guard<std::mutex> lock(mutex_);
    Record& rec = records_[key];
    rec.meta = entry;
    rec.meta.key = key;
    markDirtyLocked();
    return true;
}

std::optional<MemoryEntry> EmbeddedStore::hashGet(const std::string& key) {
    ensureLoaded();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(key);
    if (it == records_.end()) return std::nullopt;
    // A vector-only stub (no hashSet yet) counts as absent, mirroring Redis.
    if (it->second.meta.created_at.empty()) return std::nullopt;
    return it->second.meta;
}

bool EmbeddedStore::hashDelete(const std::string& key) {
    ensureLoaded();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(key);
    if (it != records_.end()) {
        records_.erase(it);
        markDirtyLocked();
    }
    return true;
}

std::vector<std::string> EmbeddedStore::scanKeys(const std::string& pattern, int count) {
    ensureLoaded();
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> keys;
    // Only "*" and simple "prefix*" are used by MemoryTools.
    std::string prefix;
    bool match_all = (pattern == "*" || pattern.empty());
    if (!match_all) {
        prefix = pattern;
        if (!prefix.empty() && prefix.back() == '*') prefix.pop_back();
    }
    for (const auto& [key, rec] : records_) {
        if ((int)keys.size() >= count) break;
        if (rec.meta.created_at.empty()) continue; // skip vector-only stubs
        if (match_all || key.rfind(prefix, 0) == 0) keys.push_back(key);
    }
    return keys;
}
