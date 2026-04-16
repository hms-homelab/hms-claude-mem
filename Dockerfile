FROM debian:trixie-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    libhiredis-dev \
    libcurl4-openssl-dev \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY CMakeLists.txt VERSION ./
COPY src/ src/

RUN mkdir build && cd build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
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

USER claude-mem

ENV REDIS_HOST=127.0.0.1
ENV REDIS_PORT=6379
ENV OLLAMA_HOST=http://localhost:11434
ENV EMBED_MODEL=nomic-embed-text

ENTRYPOINT ["hms_claude_mem"]
