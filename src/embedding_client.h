#pragma once
#include <string>
#include <vector>

class EmbeddingClient {
public:
    EmbeddingClient(const std::string& ollama_host, const std::string& model);
    ~EmbeddingClient();

    // Returns 768-dim vector for the given text
    std::vector<float> embed(const std::string& text);

    bool isHealthy();

private:
    std::string ollama_url_;
    std::string model_;
};
