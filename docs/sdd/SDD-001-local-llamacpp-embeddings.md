# SDD-001 — In-process embeddings via llama.cpp (default, no Ollama required)

**Status:** Draft
**Author:** Albin + Claude
**Date:** 2026-06-25
**Target version:** 1.3.0 (minor — new default provider, backward compatible)

## 1. Problem / Motivation

Today `hms-claude-mem` always embeds against an **external** service (`EMBED_PROVIDER=ollama`
or `openai`). That means the MCP server is useless unless the user already runs Ollama (or
pays for an API). For a tool whose whole value is "drop-in persistent memory for Claude
Code," that external dependency is the single biggest adoption barrier — and, as the v1.2.1
work showed, the source of the cold-load / startup-disconnect failure mode.

**Goal:** make the server embed **in-process** by default, with the embedding model shipped
alongside the binary, so a fresh user needs *nothing but the binary + Redis*. Ollama and
OpenAI stay as opt-in providers behind a flag.

This is a product statement as much as a technical one: **you do not need Ollama to use the
MCP.** The bundled model is the default; Ollama becomes a power-user opt-in.

## 2. Goals / Non-goals

**Goals**
- Add `EmbedProvider::Local` backed by **llama.cpp / libllama** loading a bundled
  `nomic-embed-text` GGUF.
- **Make `local` the default** `EMBED_PROVIDER`. Ollama/OpenAI remain available, selected
  explicitly via `EMBED_PROVIDER=ollama|openai`.
- Preserve the v1.2.1 **lazy-init** property: the model loads on the *first embed call*,
  never during `main()`/MCP `initialize`. The handshake stays instant.
- Stay **vector-space compatible** with existing Ollama-embedded corpora (same model,
  precision, and task prefixes) so the `global` namespace (~1951 vectors) keeps working
  without a re-embed.
- Keep a build escape hatch: `WITH_LOCAL_EMBED=OFF` produces a lean Ollama/OpenAI-only
  binary (no llama.cpp link).

**Non-goals**
- Removing the Ollama or OpenAI providers (they stay, just no longer default).
- GPU acceleration for the local path (CPU is plenty for 768-dim nomic; GPU is a later option).
- Re-embedding / migrating existing corpora (only needed if §6 compatibility check fails).
- Swapping the embedding model — nomic-embed-text stays the canonical model for space parity.

## 3. Current state (what we're extending)

- `src/embedding_client.{h,cpp}`: `enum class EmbedProvider { Ollama, OpenAI }`; `embed()`
  switches on it; HTTP via libcurl; `keep_alive` added in v1.2.1 (Ollama only).
- `src/main.cpp`: reads `EMBED_PROVIDER` (default `"ollama"`), `EMBED_HOST`, `EMBED_MODEL`,
  `EMBED_API_KEY`; constructs one `EmbeddingClient`.
- `MemoryTools::store/search` call `embedder_.embed(text)` and already handle exceptions
  by returning a JSON `error`.

## 4. Design

### 4.1 Provider enum + dispatch
```cpp
enum class EmbedProvider { Local, Ollama, OpenAI };
```
`embed()` gains a `case Local: return embedLocal(text);`. `embedLocal` lives behind
`#ifdef HMS_WITH_LOCAL_EMBED` so the lean build compiles cleanly without llama.cpp; if
`Local` is selected in a lean build, throw a clear "built without local embeddings, set
EMBED_PROVIDER=ollama" error.

### 4.2 Local backend (`src/local_embedder.{h,cpp}`)
Thin RAII wrapper over libllama:
- Lazy: first `embed()` triggers `llama_load_model_from_file()` + `llama_new_context()`.
  Guard with `std::once_flag`. Loading is ~1-2s on CPU — acceptable on first tool call,
  never at handshake.
- Tokenize input, run a single forward pass, read pooled embedding
  (`llama_get_embeddings_seq`, mean pooling — nomic uses mean pooling), L2-normalize to
  match Ollama's output.
- Single context guarded by a `std::mutex` (MCP server is single-request-at-a-time over
  stdio, so no real contention; the mutex is just correctness insurance).
- Model resident for process lifetime (≈140-280MB RAM depending on quant) — the in-process
  analogue of `keep_alive=-1`.

### 4.3 Model file: shipping + location
The GGUF travels **with the artifact**, not git history (no 140MB+ blob in the repo):
- **Resolution order** at load: `LOCAL_EMBED_MODEL` env → `<dir-of-executable>/models/<file>`
  → `~/.hms-claude-mem/models/<file>`. Clear error listing the searched paths if absent.
- **CMake** (`WITH_LOCAL_EMBED=ON`, default): `FetchContent`/`file(DOWNLOAD)` the GGUF into
  `build/models/` at configure time with a SHA256 check; copy next to the binary on install.
- **Release binaries** (`release.yml`): include `models/<file>` in each `tar.gz`/`zip`.
- **Docker** (`Dockerfile` → GHCR): `COPY`/download the GGUF into the image so the published
  container is self-contained.
- `.gitignore` the `models/` dir.

**Model choice (decided 2026-06-25):** ship **`nomic-embed-text v1.5` Q8_0** (~146MB) as the
bundled default. Quantized keeps the artifact/image small while Q8_0 is the
quality-preserving quant (avoid Q4 — quantization hurts embeddings more than generation).
f16 is *not* shipped; it's only a debugging reference if the §6 parity gate is marginal.

**Model is a setting** (`LOCAL_EMBED_MODEL`): the loader is generic — llama.cpp reads the
embedding dimension and pooling from the GGUF, so a user can point at *any* embedding GGUF
("hey, let's use this model") without recompiling. Caveat (the model-dependent part): the
task-prefix convention in §4.4 is **nomic-tuned**, and swapping models changes the vector
space, so the *supported, parity-validated* path is the bundled Q8_0 nomic. Other models are
"advanced — expect to re-embed the corpus and possibly adjust prefixes." We optimize for the
bundled quantized nomic; the setting exists, model-independence is best-effort.

### 4.4 Task prefixes (compatibility-critical)
nomic-embed-text is trained with task prefixes (`search_query:` / `search_document:`).
Ollama applies these via its template; to land in the **same vector space** the local path
must apply the identical convention:
- `MemoryTools::store` → embed as a **document** (`search_document: <text>`).
- `MemoryTools::search` → embed the query as a **query** (`search_query: <text>`).

This is a behavioral detail that currently lives implicitly inside Ollama. It must become
explicit and shared so Local and Ollama produce comparable vectors. **Verify** what Ollama
0.30.x actually injects for `nomic-embed-text` before locking this in (§6).

### 4.5 Config / defaults (the "flag")
| Var | Old default | New default | Notes |
|-----|-------------|-------------|-------|
| `EMBED_PROVIDER` | `ollama` | **`local`** | `ollama` / `openai` opt-in via this flag |
| `LOCAL_EMBED_MODEL` | — | *(bundled Q8_0 nomic, auto-resolved)* | **the model setting** — point at any embedding GGUF; non-default = advanced, may need re-embed |
| `EMBED_HOST` | localhost:11434 | *(unused for local)* | still used by ollama/openai |

So "use Ollama" = set `EMBED_PROVIDER=ollama` in `.mcp.json` env. Default (unset) = bundled
local model.

## 5. Build / CI impact
- `CMakeLists.txt`: `option(WITH_LOCAL_EMBED "Build in-process llama.cpp embeddings" ON)`;
  when ON, `FetchContent` llama.cpp (pinned tag), link `llama` + `ggml`, define
  `HMS_WITH_LOCAL_EMBED`, fetch the GGUF. Compile time goes up (llama.cpp is sizeable) —
  mitigate with the GHA build cache already in use.
- `release.yml`: bundle `models/` per-arch; the macOS/Linux/Windows matrix must each produce
  a working local build (llama.cpp is portable; Windows via vcpkg as today).
- `docker-build.yml` / `Dockerfile`: add the GGUF to the image; image grows ~150-280MB.
- Tests: add a `LocalEmbedder` unit test (load tiny/real model, assert 768-dim, L2-norm≈1)
  guarded so it skips when the model isn't present.

## 6. Risks / validation gates

| Risk | Mitigation / gate |
|------|-------------------|
| **Vector-space drift** vs existing Ollama corpus (now using **Q8_0**, not f16) | **GATE:** cosine(localQ8(text), ollama(text)) ≥ 0.98 across a sample of stored keys before flipping the default. Q8_0 adds a small quant delta on top of any prefix mismatch — measure the combined drift. If it fails, align prefixes first; if still marginal, either ship f16 or provide a one-shot `--reembed` over the namespace. |
| Wrong/missing task prefixes | Confirm Ollama's nomic template behavior empirically; mirror it (§4.4). |
| Model file missing at runtime | Explicit resolution-order error message; CMake/CI guarantee it's bundled. |
| llama.cpp build weight / portability | `WITH_LOCAL_EMBED=OFF` lean build; pin a known-good llama.cpp tag; reuse GHA cache. |
| Reintroducing startup block | Lazy `std::once_flag` load on first embed; assert no model load in `initialize` (smoke test like v1.2.1's). |
| Image/artifact size bloat | Accept (~150MB); offer Q8_0 if §6 passes; document. |
| Default flip surprises current users | This repo's `.mcp.json` must add `EMBED_PROVIDER=ollama` to keep `.15`, **or** drop it to adopt local. Call out in CHANGELOG + README migration note. |

## 7. Rollout (stages)
1. `LocalEmbedder` + provider plumbing behind `WITH_LOCAL_EMBED`, default still `ollama`.
   Bench parity (§6 gate) against `.15` Ollama on the `global` corpus.
2. CMake/CI model bundling (release matrix + Docker).
3. **Flip default to `local`**; update README + CHANGELOG migration note; bump to **1.3.0**.
4. Update this repo's `.mcp.json`: either adopt local (drop EMBED_HOST) or pin
   `EMBED_PROVIDER=ollama` to keep `.15`. (Decision deferred to implementation — depends on
   §6 parity result.)

## 8. Open questions
- ~~f16 vs Q8_0 for the shipped GGUF~~ → **decided: Q8_0** (small artifact; quality-preserving
  quant). f16 kept only as a parity-debug reference if §6 is marginal.
- ~~Model swappable?~~ → **decided: yes, `LOCAL_EMBED_MODEL` is a setting**; bundled Q8_0
  nomic is the supported/validated default, other models are advanced (§4.3).
- Download-at-build vs git-lfs vs release-asset-only — which is least friction for the three
  install paths (build-from-source / release binary / Docker)?
- Do we expose `LOCAL_EMBED_THREADS` / context size, or keep them auto?
