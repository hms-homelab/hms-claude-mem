#pragma once
#ifdef HMS_WITH_LOCAL_EMBED

#include <string>
#include <vector>
#include <mutex>

// Opaque llama.cpp types — forward-declared so this header stays llama-free.
struct llama_model;
struct llama_context;

// In-process embeddings via llama.cpp / libllama loading a bundled
// nomic-embed-text GGUF. Replicates Ollama's behaviour exactly (raw text →
// MEAN pooling → L2-normalize, no task prefixes) so vectors land in the same
// space as the existing Ollama-embedded corpus, no re-embed required.
//
// LAZY by design: the constructor only records the model-path hint; the model
// is loaded on the FIRST embed() call. Nothing loads during main()/MCP
// initialize, preserving the instant-handshake property (see v1.2.1).
class LocalEmbedder {
public:
    explicit LocalEmbedder(std::string model_path_hint = "");
    ~LocalEmbedder();

    LocalEmbedder(const LocalEmbedder&) = delete;
    LocalEmbedder& operator=(const LocalEmbedder&) = delete;

    // Throws std::runtime_error if the model can't be loaded.
    std::vector<float> embed(const std::string& text);

    // Default bundled model filename, searched for under the resolution paths.
    static const char* defaultModelFile();

private:
    void ensureLoaded();              // std::call_once guard
    std::string resolveModelPath() const;

    std::string model_path_hint_;     // LOCAL_EMBED_MODEL (explicit path) or ""
    std::once_flag init_flag_;
    std::string init_error_;          // captured if load fails
    std::mutex mutex_;                // serialize embed() over the single ctx
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    int n_embd_ = 0;
};

#endif // HMS_WITH_LOCAL_EMBED
