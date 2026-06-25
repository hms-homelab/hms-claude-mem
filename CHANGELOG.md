# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [1.3.1] - 2026-06-25

### Added
- **Embedded local store (no Redis required)** — `STORE_PROVIDER=local`, now the
  **default**. An in-process vector index + metadata persisted to a local file
  (`~/.hms-claude-mem/store/<namespace>.db`, override with `STORE_PATH`). With
  in-process embeddings (1.3.0), the server now needs **no external service** at all.
- **Write-back persistence**: in-memory is the source of truth; mutations flush to
  disk on idle (debounced, `STORE_FLUSH_IDLE_MS`, default 2000 ms) and on clean
  shutdown (stdin close, `SIGTERM`/`SIGINT`). Crash-safe writes (tmp + fsync +
  atomic rename). A hard crash loses only writes since the last flush.
- `IMemoryStore` interface abstracts the backend; `RedisClient` and the new
  `EmbeddedStore` both implement it.

### Changed
- Default `STORE_PROVIDER` flips from `redis` to `local`. Set `STORE_PROVIDER=redis`
  (+ `REDIS_HOST`) to keep using Redis (shared/multi-machine setups).
- Search is brute-force cosine in-process; scores match Redis VSIM's `(1+cos)/2`
  (validated: overlap 0.98, top-1 0.97, score MAE 1.6e-4 — within Redis's own int8
  quantization noise).

### Migration
- Existing Redis users: set `STORE_PROVIDER=redis` to stay on your Redis data.
  Otherwise a fresh embedded store file is used. (An optional `--import-redis` is
  planned for moving a Redis namespace into an embedded file.)

## [1.3.0] - 2026-06-25

### Added
- **In-process embeddings (no Ollama required)** via llama.cpp, loading a bundled
  `nomic-embed-text-v1.5` Q8_0 GGUF. This is now the **default** provider
  (`EMBED_PROVIDER=local`). Ollama and OpenAI remain available via the flag.
- `LOCAL_EMBED_MODEL` env to point at any embedding GGUF (advanced; the bundled
  nomic is the supported, parity-validated default). Also `LOCAL_EMBED_CTX`,
  `LOCAL_EMBED_THREADS`, `LOCAL_EMBED_GPU_LAYERS`.
- The model is bundled with every artifact (release tarballs/zip, Docker image)
  and downloaded SHA256-verified at build time (`HMS_DOWNLOAD_MODEL`).
- `WITH_LOCAL_EMBED` CMake option (default ON); OFF builds a lean
  Ollama/OpenAI-only binary with no llama.cpp dependency.

### Changed
- Default `EMBED_PROVIDER` flips from `ollama` to `local`. The local path
  replicates Ollama exactly (raw text → MEAN pool → L2-normalize), so existing
  Ollama-embedded corpora stay valid — **no re-embed needed** (validated: p5
  cosine ≥ 0.998 vs Ollama over the live corpus).
- Local model loads lazily on first embed (not at startup), preserving the
  instant MCP handshake from 1.2.1.

### Migration
- To keep using Ollama, set `EMBED_PROVIDER=ollama` (and `EMBED_HOST`) in your
  MCP server env. Otherwise the bundled local model is used automatically.

## [1.2.1] - 2026-06-25

### Changed
- **Lazy startup**: no Redis connect or embed warm-up during `main()`/MCP `initialize`. The handshake now returns instantly so a cold Ollama load or slow Redis round-trip can't blow Claude Code's startup timeout and mark the server disconnected. The first tool call establishes the connection.
- **Resilient Redis**: connection is (re)established lazily with exponential backoff (100ms doubling, capped 2s) for at least 10 attempts (`REDIS_MAX_RETRIES`, never below 10) before giving up gracefully — a transient blip no longer `exit()`s the process. A dead/errored context is dropped and reconnected on the next op.

### Added
- `keep_alive` on Ollama embed requests (`OLLAMA_KEEP_ALIVE`, default `-1` = pin in VRAM forever) so the embed model never cold-loads after the first call. Integer values are sent as JSON numbers; unit strings (e.g. `24h`) are passed through.

### Notes
- All diagnostics go to stderr only; stdout stays pure JSON-RPC.

## [1.2.0] - 2026-03-06

### Added
- **Multi-provider embeddings**: Support for Ollama and OpenAI-compatible APIs (OpenAI, vLLM, LiteLLM, LocalAI, etc.)
- `EMBED_PROVIDER` env var: `ollama` (default) or `openai`
- `EMBED_HOST` env var: embedding API endpoint (falls back to `OLLAMA_HOST` for backward compatibility)
- `EMBED_API_KEY` env var: Bearer token for authenticated providers (OpenAI, cloud APIs)

### Changed
- `EmbeddingClient` refactored with `httpPost()` shared helper and provider-specific `embedOllama()`/`embedOpenAI()` parsers

## [1.1.0] - 2026-03-06

### Added
- **Recency weighting**: Search results scored by `similarity * freshness`. Configurable decay rate via `DECAY_RATE` env var (default 0.01 = 1% per day). Old memories naturally sink.
- **Pinned memories**: `pinned=true` on mem_store makes a memory immune to decay. Use for critical facts (credentials, deploy commands, user preferences).
- **Hybrid search**: mem_search accepts optional `category` filter. Combines semantic similarity with exact category matching for more dependable retrieval at scale.
- **Namespaces**: `NAMESPACE` env var isolates memory per project/user/branch. Keys are prefixed with `claude:mem:{namespace}:`. Default namespace: `default`.
- **Search observability**: Results now include `final_score`, `age_days`, and `pinned` fields so you can see why a memory ranked where it did.
- 14 integration tests (up from 7)

### Changed
- Redis key format: `claude:mem:{namespace}:vectors` and `claude:mem:{namespace}:data:{key}` (was `claude:mem:vectors` / `claude:mem:data:{key}`)
- Default OLLAMA_HOST changed from hardcoded IP to `http://localhost:11434`

## [1.0.0] - 2026-03-06

### Added
- MCP server with stdio JSON-RPC 2.0 transport
- 5 tools: mem_store, mem_search, mem_get, mem_delete, mem_list
- Redis 8 vectorset integration (VADD/VSIM for semantic search)
- Redis hash storage for metadata and values
- Ollama embedding client (nomic-embed-text, 768-dim)
- Category-based filtering for mem_list
- Store with rollback (cleans up vector if hash write fails)
- Update detection (preserves created_at on re-store)
- Environment-based configuration (REDIS_HOST, OLLAMA_HOST, EMBED_MODEL)
- Docker multi-stage build (debian:trixie-slim)
- GitHub Actions CI/CD (build, test, push to GHCR)
- 7 integration tests (GTest/GMock)
