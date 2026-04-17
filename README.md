# hms-claude-mem

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![GHCR](https://img.shields.io/badge/ghcr.io-hms--claude--mem-blue?logo=docker)](https://github.com/hms-homelab/hms-claude-mem/pkgs/container/hms-claude-mem)
[![Build](https://github.com/hms-homelab/hms-claude-mem/actions/workflows/docker-build.yml/badge.svg)](https://github.com/hms-homelab/hms-claude-mem/actions)
[![Release](https://img.shields.io/github/v/release/hms-homelab/hms-claude-mem?sort=semver&label=release)](https://github.com/hms-homelab/hms-claude-mem/releases/latest)
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
                                        └── Embedding Provider (Ollama, OpenAI, vLLM, etc.)
                                             └── nomic-embed-text / text-embedding-3-small / etc.
```

## How It Works

Each memory is stored in **two places**:

- **Vectorset** (`claude:mem:vectors`) — the key gets embedded as a 768-dim vector via nomic-embed-text. Enables semantic similarity search with `VSIM`.
- **Hash** (`claude:mem:data:{key}`) — the actual value, category, and timestamps. Enables exact retrieval with `HGETALL`.

What gets **embedded** is `"{category}: {key}"`, not the value. Keys should be descriptive sentences so embeddings capture the semantic meaning:

```
key:      "hms-cpap deploy process to raspberry pi"
category: "project:hms-cpap"
value:    "Use ./deploy_to_pi.sh for ARM build + deploy ..."
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
- **Any embedding provider**: Ollama, OpenAI, vLLM, LiteLLM, LocalAI, or anything OpenAI-compatible
- **C++17** compiler
- **libhiredis-dev**, **libcurl4-openssl-dev**, **nlohmann-json3-dev**

```bash
# Install dependencies (Debian/Ubuntu)
sudo apt install -y libhiredis-dev libcurl4-openssl-dev nlohmann-json3-dev

# Option A: Ollama (local, free)
ollama pull nomic-embed-text

# Option B: OpenAI-compatible (any provider)
export EMBED_PROVIDER=openai
export EMBED_HOST=https://api.openai.com
export EMBED_MODEL=text-embedding-3-small
export EMBED_API_KEY=sk-...
```

## Download

Prebuilt binaries are attached to every [GitHub Release](https://github.com/hms-homelab/hms-claude-mem/releases/latest):

| Platform | Archive |
|----------|---------|
| Linux (amd64) | [hms_claude_mem-linux-amd64.tar.gz](https://github.com/hms-homelab/hms-claude-mem/releases/latest/download/hms_claude_mem-linux-amd64.tar.gz) |
| Windows (amd64) | [hms_claude_mem-windows-amd64.zip](https://github.com/hms-homelab/hms-claude-mem/releases/latest/download/hms_claude_mem-windows-amd64.zip) |
| macOS (arm64) | [hms_claude_mem-macos-arm64.tar.gz](https://github.com/hms-homelab/hms-claude-mem/releases/latest/download/hms_claude_mem-macos-arm64.tar.gz) |

Each archive bundles the binary, `README.md`, `LICENSE`, and `VERSION`. The Windows zip also ships the runtime DLLs (hiredis, libcurl, zlib, openssl) next to the `.exe`. Prefer the Docker image or build from source if your distro's `hiredis` / `libcurl` ABI differs from the runner's.

## Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Test

14 integration tests (require Redis + embedding provider running):

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
| `NAMESPACE` | `default` | Memory namespace (isolates per project/user) |
| `EMBED_PROVIDER` | `ollama` | Embedding provider: `ollama` or `openai` |
| `EMBED_HOST` | `http://localhost:11434` | Embedding API endpoint (falls back to `OLLAMA_HOST`) |
| `EMBED_MODEL` | `nomic-embed-text` | Embedding model name |
| `EMBED_API_KEY` | *(empty)* | Bearer token for authenticated providers |
| `DECAY_RATE` | `0.01` | Recency decay per day (0.01 = 1%/day, 0 = disabled) |

## Register with Claude Code

### Project-Scoped (recommended starting point)

Add to `.mcp.json` in your project root. Memories are isolated to this project:

**Ollama (local, free):**
```json
{
  "mcpServers": {
    "claude-mem": {
      "command": "/path/to/build/hms_claude_mem",
      "args": [],
      "env": {
        "REDIS_HOST": "127.0.0.1",
        "REDIS_PORT": "6379",
        "EMBED_PROVIDER": "ollama",
        "EMBED_HOST": "http://localhost:11434",
        "EMBED_MODEL": "nomic-embed-text",
        "EMBED_API_KEY": "",
        "NAMESPACE": "my-project",
        "DECAY_RATE": "0.01"
      }
    }
  }
}
```

**OpenAI / OpenAI-compatible (vLLM, LiteLLM, LocalAI, etc.):**
```json
{
  "mcpServers": {
    "claude-mem": {
      "command": "/path/to/build/hms_claude_mem",
      "args": [],
      "env": {
        "REDIS_HOST": "127.0.0.1",
        "REDIS_PORT": "6379",
        "EMBED_PROVIDER": "openai",
        "EMBED_HOST": "https://api.openai.com",
        "EMBED_MODEL": "text-embedding-3-small",
        "EMBED_API_KEY": "sk-...",
        "NAMESPACE": "my-project",
        "DECAY_RATE": "0.01"
      }
    }
  }
}
```

### Global (all sessions, all projects)

Add to `~/.claude/settings.json` so Claude remembers across every project:

```json
{
  "mcpServers": {
    "claude-mem": {
      "command": "/path/to/build/hms_claude_mem",
      "args": [],
      "env": {
        "REDIS_HOST": "127.0.0.1",
        "REDIS_PORT": "6379",
        "EMBED_PROVIDER": "ollama",
        "EMBED_HOST": "http://localhost:11434",
        "EMBED_MODEL": "nomic-embed-text",
        "NAMESPACE": "global",
        "DECAY_RATE": "0.01"
      }
    }
  }
}
```

### Docker as MCP Server

If you built or pulled the Docker image, point Claude Code at it:

```json
{
  "mcpServers": {
    "claude-mem": {
      "command": "docker",
      "args": ["run", "--rm", "-i",
        "-e", "REDIS_HOST=host.docker.internal",
        "-e", "EMBED_HOST=http://host.docker.internal:11434",
        "-e", "NAMESPACE=my-project",
        "ghcr.io/hms-homelab/hms-claude-mem:latest"
      ]
    }
  }
}
```

### Notes

- **Namespace isolation:** Different `NAMESPACE` values create fully separate memory pools. A project `.mcp.json` overrides `~/.claude/settings.json` when both exist.
- **Minimal config:** Only `NAMESPACE` and `EMBED_HOST` are typically needed — everything else has sensible defaults.
- **Restart required:** Claude Code must be restarted after changing MCP configuration.
- **Remote Redis:** Set `REDIS_HOST` to your Redis server IP for shared memory across machines.

## Teach Claude to Use This MCP

The first time you connect claude-mem to a new project, paste the block below into your Claude Code session. Claude will store these usage conventions as memories — so every future session retrieves them via `mem_search` instead of you having to re-explain.

````markdown
You now have the `claude-mem` MCP available (tools: `mem_store`, `mem_search`, `mem_get`, `mem_delete`, `mem_list`). Please store the following usage conventions as memories so future sessions can retrieve them.

Store each of these with `mem_store`:

1. key: "how and when to use mem_store"
   category: "meta:claude-mem"
   value: "Store a memory when you learn: a build/deploy command, a fix for a non-obvious bug, a user preference, a project decision with a durable why, or a reference to an external system. Do NOT store: ephemeral task state, info already in the code/git history, or duplicates — run mem_search first."

2. key: "how and when to use mem_search"
   category: "meta:claude-mem"
   value: "Search semantically at the start of a task and before asking the user questions the codebase can't answer. Use descriptive natural-language queries (e.g. 'how do I deploy the CPAP service'). Top-k=5 is a reasonable default."

3. key: "memory category conventions"
   category: "meta:claude-mem"
   value: "Use these category prefixes: 'project:<name>' for project-specific facts, 'user:preferences' for how the user likes to work, 'feedback:<topic>' for corrections/validations that should change future behavior, 'reference:<system>' for pointers to external systems, 'meta:claude-mem' for self-documentation of this MCP."

4. key: "memory key naming convention"
   category: "meta:claude-mem"
   value: "Keys are what gets embedded and searched. Write full descriptive sentences ('deploy process for hms-cpap on raspberry pi'), not short slugs ('deploy-cpap'). The value holds the actual content."

5. key: "avoid duplicate memories"
   category: "meta:claude-mem"
   value: "Before mem_store, run mem_search with the same phrasing. If a close match exists, mem_delete the old one or skip the new store. Drifting duplicates poison search results."

After storing, confirm with `mem_list category=meta:claude-mem` that all five are present.
````

From then on, at the start of any session Claude can run `mem_search query="how to use claude-mem"` and these conventions will surface.

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
│   ├── embedding_client.cpp/h  # Multi-provider embedding client (Ollama, OpenAI)
│   └── tools.cpp/h         # Tool implementations (store, search, get, delete, list)
├── tests/unit/
│   └── test_mcp_server.cpp # 14 integration tests
└── .github/workflows/
    └── docker-build.yml    # CI: build, test, push to GHCR
```

## Support

[![Buy Me A Coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/aamat09)

## License

MIT
