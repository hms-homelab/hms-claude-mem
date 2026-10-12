FROM debian:trixie-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    ca-certificates \
    libhiredis-dev \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY CMakeLists.txt VERSION ./
COPY src/ src/
# Vendored single-header cpp-httplib, needed by src/http_transport.cpp.
COPY third_party/ third_party/

# Configure fetches llama.cpp and downloads the bundled GGUF (SHA256-verified)
# into build/models. WITH_LOCAL_EMBED is ON by default.
RUN mkdir build && cd build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DWITH_LOCAL_EMBED=ON \
    && make -j$(nproc) \
    && strip hms_claude_mem

# Runtime
FROM debian:trixie-slim

# libgomp1: llama.cpp (the bundled embedder) links OpenMP. Without it the
# binary does not start at all ("libgomp.so.1: cannot open shared object
# file"), which is how the 2.0.0 image shipped. (Its other 2.0.0 fault, a
# runner-CPU-tuned llama.cpp, is fixed in CMakeLists.txt: HMS_NATIVE_CPU.)
RUN apt-get update && apt-get install -y --no-install-recommends \
    libhiredis-dev \
    libcurl4-openssl-dev \
    libgomp1 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -s /bin/false claude-mem

COPY --from=builder /app/build/hms_claude_mem /usr/local/bin/
# Bundle the embedding model so EMBED_PROVIDER=local works with no external Ollama.
COPY --from=builder /app/build/models /usr/local/bin/models

USER claude-mem

# Storage + embeddings default to in-process (no Redis/Ollama needed).
# Persist the embedded store by mounting a volume at the store path, e.g.:
#   docker run -e STORE_PATH=/data/mem.db -v hms-mem:/data ghcr.io/hms-homelab/hms-claude-mem
# To use Redis instead: -e STORE_PROVIDER=redis -e REDIS_HOST=host
# To use Ollama embeddings: -e EMBED_PROVIDER=ollama -e EMBED_HOST=http://host:11434
ENV EMBED_MODEL=nomic-embed-text

ENTRYPOINT ["hms_claude_mem"]
