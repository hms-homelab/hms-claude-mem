# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

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
