#pragma once
#include <string>
#include <vector>
#include <memory>

class LocalEmbedder; // forward decl; only used under HMS_WITH_LOCAL_EMBED

enum class EmbedProvider {
    Local,   // in-process llama.cpp, bundled nomic-embed-text GGUF (default)
    Ollama,  // POST /api/embed  {"model":"...", "input":"..."}
    OpenAI   // POST /v1/embeddings  {"model":"...", "input":"..."} (OpenAI, vLLM, LiteLLM, LocalAI, etc.)
};

class EmbeddingClient {
public:
    // local_model_path is the LOCAL_EMBED_MODEL hint, used only by the Local provider.
    EmbeddingClient(const std::string& host, const std::string& model,
                    EmbedProvider provider = EmbedProvider::Ollama,
                    const std::string& api_key = "",
                    const std::string& local_model_path = "");
    ~EmbeddingClient();

    std::vector<float> embed(const std::string& text);
    bool isHealthy();

private:
    std::vector<float> embedOllama(const std::string& text);
    std::vector<float> embedOpenAI(const std::string& text);
    std::vector<float> embedLocal(const std::string& text);
    std::string httpPost(const std::string& url, const std::string& body);

    std::string host_;
    std::string model_;
    EmbedProvider provider_;
    std::string api_key_;
    std::string local_model_path_;
#ifdef HMS_WITH_LOCAL_EMBED
    std::unique_ptr<LocalEmbedder> local_;
#endif
};
