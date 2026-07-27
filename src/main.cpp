#include "mcp_server.h"
#include "http_transport.h"
#include "memory_store.h"
#include "redis_client.h"
#include "embedded_store.h"
#include "embedding_client.h"
#include "tools.h"
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>

namespace {
std::string getEnv(const char* name, const std::string& fallback) {
    const char* val = std::getenv(name);
    return val ? std::string(val) : fallback;
}

// For shutdown durability: a signal handler installs a non-default disposition
// (without SA_RESTART) so a blocked std::getline returns on SIGTERM/SIGINT,
// letting main() return and the store's destructor flush.
volatile std::sig_atomic_t g_signaled = 0;
void onSignal(int) { g_signaled = 1; }

void installShutdownSignals() {
#ifndef _WIN32
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART: interrupt the blocking read
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
#else
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);
#endif
}
} // namespace

int main() {
    std::string redis_host = getEnv("REDIS_HOST", "127.0.0.1");
    int redis_port = std::stoi(getEnv("REDIS_PORT", "6379"));
    std::string embed_host = getEnv("EMBED_HOST", getEnv("OLLAMA_HOST", "http://localhost:11434"));
    std::string embed_model = getEnv("EMBED_MODEL", "nomic-embed-text");
    // Default provider is "local" (bundled in-process model — no Ollama needed).
    // Set EMBED_PROVIDER=ollama|openai to use an external service instead.
    // Lean builds (WITH_LOCAL_EMBED=OFF) have no local model, so default to ollama.
#ifdef HMS_WITH_LOCAL_EMBED
    std::string embed_provider_str = getEnv("EMBED_PROVIDER", "local");
#else
    std::string embed_provider_str = getEnv("EMBED_PROVIDER", "ollama");
#endif
    std::string embed_api_key = getEnv("EMBED_API_KEY", "");
    std::string local_model_path = getEnv("LOCAL_EMBED_MODEL", "");
    std::string ns = getEnv("NAMESPACE", "default");
    double decay_rate = std::stod(getEnv("DECAY_RATE", "0.01"));

    // Storage backend. Default is "local" (embedded file-backed store — no Redis
    // needed). Set STORE_PROVIDER=redis for a shared/multi-machine Redis backend.
    std::string store_provider = getEnv("STORE_PROVIDER", "local");
    std::string store_path = getEnv("STORE_PATH", "");

    EmbedProvider provider = EmbedProvider::Local;
    if (embed_provider_str == "ollama") {
        provider = EmbedProvider::Ollama;
    } else if (embed_provider_str == "openai") {
        provider = EmbedProvider::OpenAI;
    }

    // Lazy by design: do NOT connect to Redis, load the store file, or warm the
    // embed model here. The MCP initialize handshake must return instantly,
    // otherwise a cold load or slow round-trip can blow Claude Code's startup
    // timeout. The first actual tool call establishes the backend + embed.
    std::unique_ptr<IMemoryStore> store;
    if (store_provider == "local") {
        store = std::make_unique<EmbeddedStore>(ns, store_path);
    } else {
        store = std::make_unique<RedisClient>(redis_host, redis_port, ns);
    }

    installShutdownSignals();

    EmbeddingClient embedder(embed_host, embed_model, provider, embed_api_key, local_model_path);
    MemoryTools tools(*store, embedder, decay_rate);
    McpServer server(tools);

    // SDD-003: same object graph either way, so the two transports cannot drift.
    // MODE=stdio (default) keeps the historical behaviour exactly.
    if (getEnv("MODE", "stdio") == "http") {
        HttpTransportConfig http_cfg;
        http_cfg.bind_addr      = getEnv("BIND_ADDR", "127.0.0.1");
        http_cfg.port           = std::stoi(getEnv("HTTP_PORT", "8901"));
        http_cfg.auth_token     = getEnv("AUTH_TOKEN", "");
        http_cfg.version        = HMS_CLAUDE_MEM_VERSION;
        http_cfg.store_provider = store_provider;
        http_cfg.embed_provider = embed_provider_str;
        http_cfg.ns             = ns;
        return runHttpTransport(server, http_cfg, &g_signaled);
    }

    server.run();

    // store's destructor flushes any pending writes (EmbeddedStore write-back).
    return 0;
}
