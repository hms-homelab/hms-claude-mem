# SDD-002 — Embedded local store (default), Redis optional

**Status:** Draft
**Author:** Albin + Claude
**Date:** 2026-06-25
**Target version:** 1.3.1 (new default store, backward compatible)

## 1. Problem / Motivation

After SDD-001, embeddings run in-process with no external service. The remaining
hard dependency is **Redis 8 + the vectorset module** for storage and search. That
is still a barrier: a new user must stand up Redis 8 (vectorsets are very new) before
the MCP does anything. To finish the "just the binary" story, storage should also work
**in-process by default**, with Redis kept as an opt-in for shared/multi-machine setups.

Same pattern as SDD-001: **self-contained by default, external opt-in.**

## 2. Key insight — it must persist

An in-memory cache alone loses every memory on process restart, which breaks the core
"persistent semantic memory" promise. So the default is not a pure cache: it is an
**in-memory vector index + metadata, backed by a local file** (loaded on startup,
durably written on each mutation). "In-memory" describes the hot path (search); "local
file" provides durability across sessions.

## 3. Goals / Non-goals

**Goals**
- Add an `EmbeddedStore`: in-memory vectors + metadata, persisted to a local file.
- Make it the **default** (`STORE_PROVIDER=local`). Redis stays available via
  `STORE_PROVIDER=redis`.
- Brute-force cosine search in C++ (no index server). Fine for the target scale
  (hundreds to tens of thousands of memories).
- Durable across restarts: atomic persistence, crash-safe.
- Per-namespace isolation on disk (mirrors Redis `NAMESPACE`).
- Keep all scoring/decay logic provider-agnostic (it already lives in `MemoryTools`).

**Non-goals**
- Removing Redis (it stays for multi-machine / shared-team setups).
- A networked or multi-process store (single MCP process owns the file).
- An ANN index (HNSW) now — brute force is enough at this scale; note as a later option.
- Auto-migrating the existing `.15` Redis corpus (Redis users just keep
  `STORE_PROVIDER=redis`). An optional one-shot import tool is a stretch goal (§7).

## 4. Current state (what we're refactoring)

- `MemoryTools` (`src/tools.cpp`) holds a `RedisClient&` and calls a fixed set of
  storage ops: `vectorAdd`, `vectorRemove`, `vectorSearch`, `hashSet`, `hashGet`,
  `hashDelete`, `scanKeys`, plus `getNamespace()`. **These are the entire storage
  surface** — a clean seam for an interface.
- `RedisClient` (`src/redis_client.{h,cpp}`) implements them over hiredis (VADD/VSIM +
  HSET/HGETALL/SCAN), with the v1.2.1 lazy-connect + backoff.
- `MemoryEntry` / `SearchResult` structs already model the data provider-agnostically.

## 5. Design

### 5.1 Extract a storage interface
Introduce `IMemoryStore` (`src/memory_store.h`) with exactly the methods `MemoryTools`
already uses:
```cpp
class IMemoryStore {
public:
  virtual ~IMemoryStore() = default;
  virtual bool connect() = 0;                  // EmbeddedStore: load file; Redis: connect
  virtual bool vectorAdd(const std::string& key, const std::vector<float>&) = 0;
  virtual bool vectorRemove(const std::string& key) = 0;
  virtual std::vector<std::pair<std::string,double>> vectorSearch(
            const std::vector<float>& query, int top_k) = 0;
  virtual bool hashSet(const std::string& key, const MemoryEntry&) = 0;
  virtual std::optional<MemoryEntry> hashGet(const std::string& key) = 0;
  virtual bool hashDelete(const std::string& key) = 0;
  virtual std::vector<std::string> scanKeys(const std::string& pattern, int count) = 0;
  virtual const std::string& getNamespace() const = 0;
};
```
`RedisClient` gets `: public IMemoryStore` (signatures already match — minimal change).
`MemoryTools` takes `IMemoryStore&` instead of `RedisClient&`. `MemoryEntry`/`SearchResult`
move to `memory_store.h` (or it includes `redis_client.h`'s structs — prefer moving them
to a neutral header).

### 5.2 EmbeddedStore (`src/embedded_store.{h,cpp}`)
- In-memory state, keyed by memory key:
  ```cpp
  struct Record { MemoryEntry meta; std::vector<float> vec; };
  std::unordered_map<std::string, Record> records_;
  std::mutex mutex_;
  ```
- `vectorAdd` / `hashSet`: upsert the record, then **persist**.
- `vectorRemove` / `hashDelete`: erase, then persist.
- `hashGet`: map lookup.
- `scanKeys(pattern, count)`: iterate keys (support the `*` glob `MemoryTools` actually
  uses; a simple prefix/`*` matcher covers current usage — it only ever passes `"*"`).
- `vectorSearch(query, top_k)`: brute-force cosine over all `records_` (vectors are
  L2-normalized by the embedder, so dot product = cosine), partial-sort top_k. O(N·768);
  at N=10k that's ~10M fl/ query, sub-millisecond. `MemoryTools` already re-ranks with
  freshness/pinned on top, so this just returns similarity pairs like Redis VSIM.

### 5.3 Persistence
- One file per namespace: `~/.hms-claude-mem/store/<namespace>.db` (override with
  `STORE_PATH`). Created on first write.
- **Format:** compact binary — header `{magic, version, count}` then per record
  `{keylen,key, vlen, vec(float32[]), meta fields}`. Binary avoids the ~10 KB/vector
  bloat of JSON for 768-dim floats. (A JSONL variant is simpler to debug; decide at impl
  — leaning binary for size, with a `--dump` to inspect.)
- **Crash-safe writes:** write the full state to `<file>.tmp`, `fsync`, then
  `rename()` over the real file (atomic on POSIX/Windows). Mutations are infrequent
  (user stores occasionally), so full rewrite is acceptable; revisit append-log +
  compaction only if profiling shows it matters.
- `connect()` loads the file into `records_` (or starts empty). Corrupt/partial file →
  log to stderr, start empty rather than crash (the tmp+rename invariant means the main
  file is always a complete prior state).

### 5.4 Selection / config (the flag)
| Var | Default | Notes |
|-----|---------|-------|
| `STORE_PROVIDER` | `local` | `local` (embedded file) or `redis` |
| `STORE_PATH` | `~/.hms-claude-mem/store/<ns>.db` | embedded store file override |
| `REDIS_HOST`/`REDIS_PORT` | as today | used only when `STORE_PROVIDER=redis` |

`main.cpp` constructs a `std::unique_ptr<IMemoryStore>` based on `STORE_PROVIDER` and
passes it to `MemoryTools`. Lazy load stays lazy (load file on first op / `connect()`
deferred), preserving the instant handshake.

## 6. Risks / validation

| Risk | Mitigation |
|------|-----------|
| Data loss on crash mid-write | tmp + fsync + atomic rename; main file always a complete prior state |
| Search parity vs Redis VSIM | Bench: same vectors into both, assert identical top_k ordering / scores within fp tolerance (reuse the SDD-001 harness shape) |
| Memory footprint | 768×4 B = ~3 KB/vector → 10k = ~30 MB RAM. Fine; note the ceiling |
| Brute-force at very large N | Sub-ms to ~50k; document. HNSW is a later option if anyone needs 100k+ |
| Default flip surprises Redis users | Existing `.15`/team setups set `STORE_PROVIDER=redis` (+ `REDIS_HOST`). Call out in README/CHANGELOG; this machine's `.mcp.json` keeps `redis` to stay on the shared `global` corpus |
| Concurrent access | Single MCP process owns the file; `std::mutex` guards in-memory state; warn against pointing two processes at one file |
| `scanKeys` glob breadth | Only `"*"` is used today; implement that + simple prefix, not full glob |

## 7. Rollout (stages)
1. Extract `IMemoryStore`; `RedisClient` implements it; `MemoryTools` takes the interface.
   No behavior change, Redis still default. Build + existing e2e green.
2. Implement `EmbeddedStore` + persistence. Unit tests (add/search/delete/persist-reload)
   + a Redis-vs-embedded search-parity bench on shared vectors.
3. Wire `STORE_PROVIDER` (default still `redis`); manual e2e on embedded.
4. **Flip default to `local`**; README + CHANGELOG migration note; bump **1.3.1**.
   This machine's `.mcp.json` pins `STORE_PROVIDER=redis` (keep the `global` corpus on .15).
5. (Stretch) `--import-redis` one-shot: read a Redis namespace → write an embedded file.

## 8. Open questions
- Binary vs JSONL persistence (size vs debuggability)? Leaning binary + a `--dump`.
- Full-rewrite vs append-log+compaction — start simple (rewrite); revisit on profile.
- Combine with SDD-001 so "binary + nothing else" is the headline 1.3.1 story, or ship
  storage separately from embeddings?
- Do we want a tiny file lock to prevent two processes clobbering one store file?
