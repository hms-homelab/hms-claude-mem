# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

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
