# SDD-003: Always-up HTTP daemon transport

**Status:** Implemented (stages 0-9 complete 2026-07-26)
**Author:** Albin + Claude
**Date:** 2026-07-26
**Target version:** 1.4.0 (new transport, backward compatible)

## 1. Problem / Motivation

`hms_claude_mem` is a stdio MCP binary: Claude Code spawns one process per session and
talks JSON-RPC over stdin/stdout. That has three consequences we hit in practice on
2026-07-26:

1. **A process per session, accumulating with long-lived clients.** Four
   `hms_claude_mem` processes were alive on the M5 at once, dating back several days.
   Investigated: these are **not orphans and not a leak**. Each has a live parent
   (`claude bg-spare` daemons from Jul 21, 22 and 24) and is correctly blocked reading
   stdin, exactly as designed. The cost is structural rather than buggy: one resident
   process per client session, each with its own store handle and embedder, for as long
   as any Claude Code daemon lives.
2. **The connection can die mid-session and there is no recovery.** A `mem_store` call
   returned `MCP error -32000: Connection closed`; a retry worked, but later the server
   dropped out of the session entirely and `mem_search` / `mem_store` were unavailable
   for the rest of it. Nothing was lost (Redis on `.15` held all 880 keys), but the
   assistant was blind to memory from that point on. Checked whether this was simply a
   stale binary missing commit `28f8643` ("bound Redis commands with a timeout so a dead
   socket can't hang mem_store forever"): it is not. `strings build/hms_claude_mem`
   contains both `REDIS_CONNECT_TIMEOUT_MS` and `REDIS_CMD_TIMEOUT_MS`, so the running
   binary already had the timeout fix and the failure happened anyway. Comparing mtimes
   suggested otherwise and was misleading, since a commit does not touch working-tree
   timestamps. Root cause of the drop is therefore still unexplained, which is itself an
   argument for a supervised long-lived process over a per-session child.
3. **It is Mac-local.** Every machine that wants memory needs the binary, the model or
   Ollama reachability, and Redis reachability. OpenClaw on `.72`, the VPS, and any
   future host each repeat that setup.

A single always-up HTTP server addresses all three: one process, restarted by systemd
rather than by session lifecycle, reachable by every client on the network.

## 2. Key insight: the transport is already separable

`McpServer` (`src/mcp_server.h`) is written as:

```cpp
json handleRequest(const json& request);   // pure JSON-RPC in, JSON-RPC out
void run();                                // the stdio loop, blocks until EOF
```

`handleRequest` has no knowledge of stdio. `run()` is a thin loop around it. So adding
HTTP is **a second driver for an existing handler**, not a rewrite of the protocol layer.
The work that is genuinely new is concurrency (section 5.3), not MCP plumbing.

## 3. Goals / Non-goals

**Goals**
- Add a streamable HTTP transport: `POST /mcp` carrying JSON-RPC, same semantics as stdio.
- Ship it as a daemon (systemd unit) so one process serves every client.
- Make the backend safe for concurrent callers.
- Keep **stdio mode** working and default *during the transition*, so nothing existing breaks.
  Per 8.3 it is removed once the daemon is proven, so tests target `handleRequest` directly.
- Bearer token auth, because it listens on the LAN.

**Non-goals**
- Changing the tool surface. `mem_search` / `mem_store` / `mem_get` / `mem_list` /
  `mem_delete` keep their current signatures and behaviour.
- Changing storage or embedding providers. SDD-001 and SDD-002 stand as they are.
- Multi-tenancy or per-user isolation beyond the existing `NAMESPACE`.
- TLS termination in-process (ruled out by decision 8.4: LAN only, bearer token).

## 4. Current state (what we are building on)

| Piece | File | Concurrency today |
|---|---|---|
| JSON-RPC handler | `src/mcp_server.cpp` | stateless per call, fine |
| Tools | `src/tools.cpp` | holds refs to store + embedder |
| Redis backend | `src/redis_client.{h,cpp}` | **not thread safe**: raw `void* ctx_` (a `redisContext`), no mutex |
| Embedded store | `src/embedded_store.{h,cpp}` | **already safe**: `mutable std::mutex mutex_`, atomic stop flag, background flusher thread |
| Local embedder | `src/local_embedder.{h,cpp}` | **already safe**: `std::mutex mutex_` serialises `embed()` over the single llama ctx |
| Remote embedder | `src/embedding_client.cpp` | libcurl `curl_easy_*` per call; needs review for shared handle reuse |

Runtime config in use on the M5 today (`cool_shit/.mcp.json`):

```
STORE_PROVIDER=redis   REDIS_HOST=192.168.2.15
EMBED_PROVIDER=ollama  OLLAMA_HOST=http://192.168.2.15:11434
EMBED_MODEL=nomic-embed-text   NAMESPACE=global
```

Worth noting for scoping: because embeddings already go to Ollama on `.15`, the daemon
does **not** save a per-session model load in this configuration. That benefit only
applies to `EMBED_PROVIDER=local` deployments.

`src/main.cpp` also carries a deliberate constraint we must preserve:

> Lazy by design: do NOT connect to Redis, load the store file, or warm the embed model
> here. The MCP initialize handshake must return instantly, otherwise a cold load or slow
> round-trip can blow Claude Code's startup timeout.

In daemon mode that constraint **inverts**: the process is already warm when a client
connects, so eager init at boot is not only safe, it is desirable. The lazy path stays for stdio
mode until stdio is removed (8.3), after which eager init is the only path.

## 5. Design

### 5.1 Transport selection

`MODE=stdio` (default) or `MODE=http`. `main.cpp` builds the same
`store -> embedder -> MemoryTools -> McpServer` graph, then dispatches:

```cpp
if (mode == "http") server.runHttp(bind_addr, port, auth_token);
else                server.run();          // unchanged
```

Everything before that line is identical, so the two modes cannot drift apart.

### 5.2 HTTP transport (`src/http_transport.{h,cpp}`)

- `POST /mcp` with `Content-Type: application/json`, body is a JSON-RPC request,
  response is the JSON-RPC reply. This is the MCP streamable HTTP shape.
- `GET /health` returning `{"status":"ok","version":...,"store":...,"embed":...}`,
  matching the convention used across the other HMS services.
- Batching: accept a JSON array of requests and reply with an array, per JSON-RPC 2.0.
- Notifications (no `id`) get `204 No Content`.

The server is **stateless**: every tool call is self-contained, so no session store is
required. `Mcp-Session-Id` is accepted and echoed if a client sends it, and otherwise
ignored (decision 8.2).

### 5.3 Concurrency (the actual work)

`RedisClient` is the blocker. hiredis `redisContext` is not thread safe, and today there
is exactly one per process.

Per decision 8.1 this is solved with a **single `std::mutex`** guarding the context, the
same shape `EmbeddedStore` and `LocalEmbedder` already use, so all three backends share
one concurrency story. All Redis traffic serialises; for a handful of Claude sessions that
is not a meaningful cost. A connection pool was considered and explicitly deferred until a
measurement justifies the extra reconnect and health-check machinery.

`EmbeddingClient` needs the same review: if it holds a shared `CURL*`, it needs either a
handle per call or a mutex.

### 5.4 Auth

`AUTH_TOKEN` env var. If set, `POST /mcp` requires `Authorization: Bearer <token>` and
returns `401` otherwise. If unset, no auth (acceptable only for `127.0.0.1` binds).
Default bind is `127.0.0.1`; exposing on the LAN is an explicit `BIND_ADDR=0.0.0.0`.

### 5.5 Deployment

systemd unit on `.15`, next to Redis, following the pattern of the other HMS services:

```
Environment=MODE=http BIND_ADDR=0.0.0.0 HTTP_PORT=8901
Environment=STORE_PROVIDER=redis REDIS_HOST=127.0.0.1
Environment=EMBED_PROVIDER=ollama OLLAMA_HOST=http://127.0.0.1:11434
Environment=NAMESPACE=global
Restart=always
```

Redis and Ollama both become loopback, removing two network hops per call.

Client registration then becomes, in `.mcp.json`:

```json
"claude-mem": { "type": "http", "url": "http://192.168.2.15:8901/mcp" }
```

which is the same shape the `atlassian` entry already uses.

## 6. Risks / validation

| Risk | Mitigation |
|---|---|
| Concurrent writes corrupt the store | Mutex (5.3) plus a test that hammers `mem_store` from N threads and verifies key count and content |
| Daemon dies and every client loses memory at once | `Restart=always`, `/health`, and stdio stays available as a fallback registration |
| Token leaks into a repo | `AUTH_TOKEN` from the systemd unit or `~/.secrets`, never a committed default. Kconfig-style blank defaults, per the hms-esp-apc lesson |
| Silent divergence between stdio and http | Both call the identical `handleRequest`; add a test that runs the same request list through both drivers and diffs the replies |
| Redis reconnect after a restart of `.15` | Existing reconnect path needs a test with Redis bounced mid-run |

Validation: existing `run_tests` must stay green, plus new cases for concurrent access,
auth accept/reject, batching, and stdio/http parity.

## 7. Rollout (stages)

1. Extract `runHttp()` behind `MODE`, no behaviour change when unset.
2. Make `RedisClient` thread safe, with the concurrency test.
3. Auth plus `/health`.
4. systemd unit on `.15`, run it alongside the existing stdio setup.
5. Switch the M5 `.mcp.json` to `type: http`, keep the stdio entry commented for fallback.
6. Point `.72` and any other host at the same daemon.
7. Once proven, delete `run()` and the stdio driver (8.3), leaving one code path. **Done in 2.0.0.**

Stage 5 is the only one that changes day-to-day behaviour, and it is one line to revert.
Stage 7 is the point of no return, so it should wait until the daemon has run without
incident for a while.

The orphan-cleanup fix (8.7) lands as its own commit before stage 1.

## 8. Decisions (locked 2026-07-26)

**8.1 Concurrency: single mutex.** One `std::mutex` guarding the `redisContext`, matching
`EmbeddedStore` and `LocalEmbedder` so all three backends share one concurrency story.
All Redis traffic serialises, which is acceptable for a handful of Claude sessions. A
connection pool is explicitly deferred until a measurement justifies it.

**8.2 Sessions: stateless.** Every tool call is self-contained. `Mcp-Session-Id` is
echoed if a client sends it and otherwise ignored. No session table, no expiry, no
reaper. A daemon restart therefore has zero client impact.

**8.3 stdio: the daemon replaces it once proven.** stdio stays only until the daemon is
trusted, then `run()` is deleted and there is one code path. Consequence accepted: `.15`
becomes a hard dependency for memory on every host, including an offline laptop.
`Restart=always` and `/health` cover process death, not host death. Tests must therefore
exercise `handleRequest` directly rather than relying on the stdio driver, so they stay
hermetic after the removal.

**8.4 Transport: LAN only, bearer token, plain HTTP.** `BIND_ADDR=0.0.0.0` on the LAN with
`AUTH_TOKEN` required, no TLS, consistent with the other HMS services on `.15`. The token
lives in the systemd unit or `~/.secrets` and is never committed; the Kconfig default
stays blank. This deliberately rules out an off-LAN client: the VPS cannot use the daemon
safely without revisiting this.

**8.5 Namespace: one `global`.** All clients continue to share `NAMESPACE=global`, which
is where the existing 880 memories live. No migration, and cross-machine recall is
preserved, which is the point of a shared store.

**8.6 Port: `HTTP_PORT`, default `8901`.** The port is configurable by env and must stay
that way: `.15` is crowded, with `8877`, `8888`, `8889`, `8890`, `8891`, `8892`, `8893`,
and `8895` through `8899` all bound as of 2026-07-26. `8901` was verified free. Nothing in
the code or the unit file may hardcode it.

**8.7 Orphan cleanup: DROPPED, there is no bug.** Investigated on 2026-07-26. The
long-lived `hms_claude_mem` processes are not orphans: each has a live `claude bg-spare`
parent and is correctly blocked on `std::getline(std::cin, ...)`, which is exactly what
the stdio contract requires. `ps` showed `PPID` values pointing at running processes, not
`1`, and `lsof` showed no held sockets. Nothing in this repo leaks. The accumulation is a
property of Claude Code keeping background daemons alive across days, and it resolves on
its own when the daemon transport removes the process-per-session model entirely.
