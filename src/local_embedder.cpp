#ifdef HMS_WITH_LOCAL_EMBED

#include "local_embedder.h"

#include <llama.h>

#include <cstdlib>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <iostream>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

void logErr(const std::string& msg) {
    // stderr only — stdout is reserved for JSON-RPC framing.
    std::cerr << "[claude-mem][local] " << msg << "\n";
}

// Silence llama.cpp's verbose info/warn chatter; surface only real errors,
// routed to stderr (never stdout).
void llamaLogCallback(ggml_log_level level, const char* text, void* /*user*/) {
    if (level >= GGML_LOG_LEVEL_ERROR && text) std::cerr << text;
}

int envInt(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    try { return std::stoi(v); } catch (...) { return fallback; }
}

// Directory containing the running executable, or "" if it can't be resolved.
std::string exeDir() {
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return "";
    std::error_code ec;
    fs::path p = fs::canonical(buf, ec);
    if (ec) p = fs::path(buf);
    return p.parent_path().string();
#elif defined(_WIN32)
    char buf[4096];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (n == 0 || n == sizeof(buf)) return "";
    return fs::path(std::string(buf, n)).parent_path().string();
#else
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    return fs::path(buf).parent_path().string();
#endif
}

std::string homeDir() {
    const char* h = std::getenv("HOME");
    if (h && *h) return h;
#if defined(_WIN32)
    const char* up = std::getenv("USERPROFILE");
    if (up && *up) return up;
#endif
    return "";
}

} // namespace

LocalEmbedder::LocalEmbedder(std::string model_path_hint)
    : model_path_hint_(std::move(model_path_hint)) {}

LocalEmbedder::~LocalEmbedder() {
    if (ctx_) llama_free(ctx_);
    if (model_) llama_model_free(model_);
}

const char* LocalEmbedder::defaultModelFile() {
    return "nomic-embed-text-v1.5.Q8_0.gguf";
}

std::string LocalEmbedder::resolveModelPath() const {
    std::vector<std::string> candidates;

    // 1. Explicit override (LOCAL_EMBED_MODEL).
    if (!model_path_hint_.empty()) candidates.push_back(model_path_hint_);

    // 2. <exe-dir>/models/<file>
    const std::string file = defaultModelFile();
    std::string ed = exeDir();
    if (!ed.empty()) candidates.push_back((fs::path(ed) / "models" / file).string());

    // 3. ~/.hms-claude-mem/models/<file>
    std::string home = homeDir();
    if (!home.empty())
        candidates.push_back((fs::path(home) / ".hms-claude-mem" / "models" / file).string());

    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec) && !ec) return c;
    }

    std::string msg = "embedding model not found. Searched:";
    for (const auto& c : candidates) msg += "\n  - " + c;
    msg += "\nSet LOCAL_EMBED_MODEL to a GGUF path, or use EMBED_PROVIDER=ollama.";
    throw std::runtime_error(msg);
}

void LocalEmbedder::ensureLoaded() {
    std::call_once(init_flag_, [this]() {
        try {
            llama_log_set(llamaLogCallback, nullptr);
            llama_backend_init();

            std::string path = resolveModelPath();
            logErr("loading embedding model: " + path);

            llama_model_params mp = llama_model_default_params();
            // CPU by default (deterministic, no Metal/CUDA dependency at runtime);
            // override with LOCAL_EMBED_GPU_LAYERS to offload.
            mp.n_gpu_layers = envInt("LOCAL_EMBED_GPU_LAYERS", 0);

            model_ = llama_model_load_from_file(path.c_str(), mp);
            if (!model_) throw std::runtime_error("llama_model_load_from_file failed: " + path);

            n_embd_ = llama_model_n_embd(model_);
            if (n_embd_ <= 0) throw std::runtime_error("model reports non-positive embedding dim");

            int n_ctx = envInt("LOCAL_EMBED_CTX", 2048);

            llama_context_params cp = llama_context_default_params();
            cp.embeddings = true;
            cp.pooling_type = LLAMA_POOLING_TYPE_MEAN; // nomic uses mean pooling
            cp.n_ctx = n_ctx;
            cp.n_batch = n_ctx;
            cp.n_ubatch = n_ctx; // pooled embeddings require the whole seq in one ubatch
            int threads = envInt("LOCAL_EMBED_THREADS", 0);
            if (threads > 0) {
                cp.n_threads = threads;
                cp.n_threads_batch = threads;
            }

            ctx_ = llama_init_from_model(model_, cp);
            if (!ctx_) throw std::runtime_error("llama_init_from_model failed");

            logErr("embedding model ready (dim=" + std::to_string(n_embd_) + ")");
        } catch (const std::exception& e) {
            init_error_ = e.what();
            if (ctx_) { llama_free(ctx_); ctx_ = nullptr; }
            if (model_) { llama_model_free(model_); model_ = nullptr; }
        }
    });

    if (!ctx_) {
        throw std::runtime_error(init_error_.empty() ? "local embedder init failed" : init_error_);
    }
}

std::vector<float> LocalEmbedder::embed(const std::string& text) {
    ensureLoaded();
    std::lock_guard<std::mutex> lock(mutex_);

    const llama_vocab* vocab = llama_model_get_vocab(model_);

    // Two-pass tokenize (add_special=true → [CLS]/[SEP]; parse_special=false).
    int needed = -llama_tokenize(vocab, text.c_str(), (int)text.size(),
                                 nullptr, 0, /*add_special=*/true, /*parse_special=*/false);
    if (needed <= 0) throw std::runtime_error("tokenization produced no tokens");

    int n_ctx = (int)llama_n_ctx(ctx_);
    if (needed > n_ctx) needed = n_ctx; // clamp overly long inputs

    std::vector<llama_token> tokens(needed);
    int n = llama_tokenize(vocab, text.c_str(), (int)text.size(),
                           tokens.data(), needed, true, false);
    if (n < 0) n = needed; // truncated to buffer
    if (n == 0) throw std::runtime_error("tokenization produced no tokens");

    // Fresh sequence each call — clear the cache so positions don't accumulate.
    llama_memory_clear(llama_get_memory(ctx_), true);

    llama_batch batch = llama_batch_init(n, 0, 1);
    for (int i = 0; i < n; ++i) {
        batch.token[i] = tokens[i];
        batch.pos[i] = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = 1; // MEAN pooling needs every token marked as an output
    }
    batch.n_tokens = n;

    // nomic-bert is an encoder (non-causal) model — use encode, not decode.
    int rc = llama_encode(ctx_, batch);
    if (rc != 0) {
        llama_batch_free(batch);
        throw std::runtime_error("llama_encode failed (rc=" + std::to_string(rc) + ")");
    }

    const float* emb = llama_get_embeddings_seq(ctx_, 0);
    if (!emb) {
        llama_batch_free(batch);
        throw std::runtime_error("llama_get_embeddings_seq returned null");
    }

    std::vector<float> out(emb, emb + n_embd_);
    llama_batch_free(batch);

    // L2-normalize to match Ollama's normalized nomic output (Redis VSIM cosine).
    double norm = 0.0;
    for (float v : out) norm += (double)v * v;
    norm = std::sqrt(norm);
    if (norm > 0.0) {
        for (auto& v : out) v = (float)(v / norm);
    }
    return out;
}

#endif // HMS_WITH_LOCAL_EMBED
