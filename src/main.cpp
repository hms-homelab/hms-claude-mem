#include "mcp_server.h"
#include "redis_client.h"
#include "embedding_client.h"
#include "tools.h"
#include <cstdlib>
#include <iostream>
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
    std::string embed_provider_str = getEnv("EMBED_PROVIDER", "ollama");
    std::string embed_api_key = getEnv("EMBED_API_KEY", "");
    std::string ns = getEnv("NAMESPACE", "default");
    double decay_rate = std::stod(getEnv("DECAY_RATE", "0.01"));

    EmbedProvider provider = EmbedProvider::Ollama;
    if (embed_provider_str == "openai") {
        provider = EmbedProvider::OpenAI;
    }

    RedisClient redis(redis_host, redis_port, ns);
    if (!redis.connect()) {
        std::cerr << "Failed to connect to Redis at "
                  << redis_host << ":" << redis_port << "\n";
        return 1;
    }

    EmbeddingClient embedder(embed_host, embed_model, provider, embed_api_key);
    MemoryTools tools(redis, embedder, decay_rate);
    McpServer server(tools);

    server.run();

    return 0;
}
