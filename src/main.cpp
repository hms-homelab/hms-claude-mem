#include "mcp_server.h"
#include "redis_client.h"
#include "embedding_client.h"
#include "tools.h"
#include <cstdlib>
#include <string>

namespace {
std::string getEnv(const char* name, const std::string& fallback) {
    const char* val = std::getenv(name);
    return val ? std::string(val) : fallback;
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

    EmbedProvider provider = EmbedProvider::Local;
    if (embed_provider_str == "ollama") {
        provider = EmbedProvider::Ollama;
    } else if (embed_provider_str == "openai") {
        provider = EmbedProvider::OpenAI;
    }

    // Lazy by design: do NOT connect to Redis or warm the embed model here.
    // The MCP initialize handshake must return instantly, otherwise a cold
    // Ollama load or a slow Redis round-trip can blow Claude Code's startup
    // timeout and the server gets marked disconnected. The first actual tool
    // call establishes the Redis connection (with backoff) and the embed.
    RedisClient redis(redis_host, redis_port, ns);

    EmbeddingClient embedder(embed_host, embed_model, provider, embed_api_key, local_model_path);
    MemoryTools tools(redis, embedder, decay_rate);
    McpServer server(tools);

    server.run();

    return 0;
}
