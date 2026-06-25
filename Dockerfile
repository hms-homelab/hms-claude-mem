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

# Configure fetches llama.cpp and downloads the bundled GGUF (SHA256-verified)
# into build/models. WITH_LOCAL_EMBED is ON by default.
RUN mkdir build && cd build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DWITH_LOCAL_EMBED=ON \
    && make -j$(nproc) \
    && strip hms_claude_mem

# Runtime
FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libhiredis-dev \
    libcurl4-openssl-dev \
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
