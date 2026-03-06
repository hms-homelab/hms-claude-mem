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
    // Config from environment (with sensible defaults)
    std::string redis_host = getEnv("REDIS_HOST", "127.0.0.1");
    int redis_port = std::stoi(getEnv("REDIS_PORT", "6379"));
    std::string ollama_host = getEnv("OLLAMA_HOST", "http://192.168.2.5:11434");
    std::string embed_model = getEnv("EMBED_MODEL", "nomic-embed-text");

    // Connect to Redis
    RedisClient redis(redis_host, redis_port);
    if (!redis.connect()) {
        std::cerr << "Failed to connect to Redis at "
                  << redis_host << ":" << redis_port << "\n";
        return 1;
    }

    // Init embedding client
    EmbeddingClient embedder(ollama_host, embed_model);

    // Create tools and server
    MemoryTools tools(redis, embedder);
    McpServer server(tools);

    // Run stdio MCP loop
    server.run();

    return 0;
}
