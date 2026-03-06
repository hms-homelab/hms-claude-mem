#pragma once
#include <string>
#include <vector>

enum class EmbedProvider {
    Ollama,  // POST /api/embed  {"model":"...", "input":"..."}
    OpenAI   // POST /v1/embeddings  {"model":"...", "input":"..."} (OpenAI, vLLM, LiteLLM, LocalAI, etc.)
};

class EmbeddingClient {
public:
    EmbeddingClient(const std::string& host, const std::string& model,
                    EmbedProvider provider = EmbedProvider::Ollama,
                    const std::string& api_key = "");
    ~EmbeddingClient();

    std::vector<float> embed(const std::string& text);
    bool isHealthy();

private:
    std::vector<float> embedOllama(const std::string& text);
    std::vector<float> embedOpenAI(const std::string& text);
    std::string httpPost(const std::string& url, const std::string& body);

    std::string host_;
    std::string model_;
    EmbedProvider provider_;
    std::string api_key_;
};
