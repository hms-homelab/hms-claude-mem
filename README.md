# hms-claude-mem

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-%23FFDD00.svg?logo=buy-me-a-coffee)](https://www.buymeacoffee.com/aamat09)

Persistent semantic memory for Claude Code via Redis 8 vectorsets. A C++ MCP server that gives Claude the ability to store, search, and retrieve context across sessions — surgically, on-demand, without bloating the context window.

## The Problem

LLMs have limited context windows. As conversations grow, early context gets compressed and lost. File-based memory (like `MEMORY.md`) loads everything upfront, wasting context on things that aren't relevant right now.

## The Solution

A Redis-backed semantic memory system that Claude manages itself:

1. **Store** what matters as it discovers it (build commands, debug solutions, user preferences)
2. **Search** semantically when it needs context later ("how do I deploy to the Pi?" finds the right memory even if those exact words were never stored)
3. **Retrieve** only what's relevant, in small surgical batches

```
Claude Code  <──stdio JSON-RPC──>  hms_claude_mem (C++ binary)
                                        │
                                        ├── Redis 8 (vectorset module)
                                        │    ├── VADD/VSIM  → semantic vector search
                                        │    └── HSET/HGET  → key-value storage
                                        │
                                        └── Ollama
                                             └── nomic-embed-text (768-dim embeddings)
```

## How It Works

Each memory is stored in **two places**:

- **Vectorset** (`claude:mem:vectors`) — the key gets embedded as a 768-dim vector via nomic-embed-text. Enables semantic similarity search with `VSIM`.
- **Hash** (`claude:mem:data:{key}`) — the actual value, category, and timestamps. Enables exact retrieval with `HGETALL`.

What gets **embedded** is `"{category}: {key}"`, not the value. Keys should be descriptive sentences so embeddings capture the semantic meaning:

```
key:      "hms-cpap deploy process to raspberry pi"
category: "project:hms-cpap"
value:    "Pi at 192.168.2.73. ALWAYS use ./deploy_to_pi.sh ..."
```

Searching for "how do I push code to the Pi" will find this memory via cosine similarity, even though the words don't match.

## MCP Tools

| Tool | Description |
|------|-------------|
| `mem_store` | Store key + value + category. Embeds and indexes the key. |
| `mem_search` | Semantic search. Returns top-k matches with similarity scores. |
| `mem_get` | Exact key lookup. Returns value, category, timestamps. |
| `mem_delete` | Removes from both vectorset and hash. |
| `mem_list` | Lists all keys, optionally filtered by category. |

## Prerequisites

- **Redis 8+** with vectorset module (built-in since Redis 8.0)
- **Ollama** with `nomic-embed-text` model
- **C++17** compiler
- **libhiredis-dev**, **libcurl4-openssl-dev**, **nlohmann-json3-dev**

```bash
# Install dependencies (Debian/Ubuntu)
sudo apt install -y libhiredis-dev libcurl4-openssl-dev nlohmann-json3-dev

# Ensure Ollama has the embedding model
ollama pull nomic-embed-text
```

## Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Test

7 integration tests (require Redis + Ollama running):

```bash
cd build
./run_tests
```

## Configuration

Environment variables with sensible defaults:

| Variable | Default | Description |
|----------|---------|-------------|
| `REDIS_HOST` | `127.0.0.1` | Redis server address |
| `REDIS_PORT` | `6379` | Redis server port |
| `OLLAMA_HOST` | `http://localhost:11434` | Ollama API endpoint |
| `EMBED_MODEL` | `nomic-embed-text` | Embedding model name |

## Register with Claude Code

Add to `.mcp.json` in your project root:

```json
{
  "mcpServers": {
    "claude-mem": {
      "command": "/path/to/build/hms_claude_mem",
      "args": [],
      "env": {
        "OLLAMA_HOST": "http://localhost:11434"
      }
    }
  }
}
```

Restart Claude Code to pick up the new MCP server.

## Docker

```bash
# Build
docker build -t hms-claude-mem .

# Run (needs access to Redis and Ollama)
docker run --rm \
  -e REDIS_HOST=host.docker.internal \
  -e OLLAMA_HOST=http://host.docker.internal:11434 \
  hms-claude-mem
```

Or use the published image:

```bash
docker pull ghcr.io/hms-homelab/hms-claude-mem:latest
```

## Performance

| Operation | Latency | Notes |
|-----------|---------|-------|
| `mem_store` | ~60ms | Embedding generation dominates |
| `mem_search` | ~280ms | Embedding + VSIM |
| `mem_get` | <1ms | Direct hash lookup |
| `mem_delete` | <1ms | VREM + DEL |

## Project Structure

```
hms-claude-mem/
├── CMakeLists.txt          # C++17, hiredis + curl + nlohmann-json
├── Dockerfile              # Multi-stage (debian:trixie-slim)
├── VERSION                 # Semantic version
├── CHANGELOG.md
├── src/
│   ├── main.cpp            # Env config, wiring, stdio loop
│   ├── mcp_server.cpp/h    # JSON-RPC 2.0 MCP protocol handler
│   ├── redis_client.cpp/h  # hiredis wrapper (VADD, VSIM, HSET, SCAN)
│   ├── embedding_client.cpp/h  # Ollama HTTP client for embeddings
│   └── tools.cpp/h         # Tool implementations (store, search, get, delete, list)
├── tests/unit/
│   └── test_mcp_server.cpp # 7 integration tests
└── .github/workflows/
    └── docker-build.yml    # CI: build, test, push to GHCR
```

## Support

[![Buy Me A Coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/aamat09)

## License

MIT
