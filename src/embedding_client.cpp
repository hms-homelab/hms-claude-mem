#include "embedding_client.h"
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <stdexcept>

using json = nlohmann::json;

namespace {
size_t writeCallback(char* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(ptr, size * nmemb);
    return size * nmemb;
}
} // namespace

EmbeddingClient::EmbeddingClient(const std::string& host, const std::string& model,
                                 EmbedProvider provider, const std::string& api_key)
    : host_(host), model_(model), provider_(provider), api_key_(api_key) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

EmbeddingClient::~EmbeddingClient() {
    curl_global_cleanup();
}

std::string EmbeddingClient::httpPost(const std::string& url, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Failed to init curl");

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    if (!api_key_.empty()) {
        std::string auth = "Authorization: Bearer " + api_key_;
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("HTTP request failed: ") + curl_easy_strerror(res));
    }

    return response;
}

std::vector<float> EmbeddingClient::embedOllama(const std::string& text) {
    // POST /api/embed  {"model":"...", "input":"..."}
    // Response: {"embeddings": [[0.1, 0.2, ...]]}
    std::string url = host_ + "/api/embed";
    json req = {{"model", model_}, {"input", text}};
    std::string response = httpPost(url, req.dump());

    auto j = json::parse(response);
    if (!j.contains("embeddings") || j["embeddings"].empty()) {
        throw std::runtime_error("Invalid Ollama response: " + response.substr(0, 200));
    }
    return j["embeddings"][0].get<std::vector<float>>();
}

std::vector<float> EmbeddingClient::embedOpenAI(const std::string& text) {
    // POST /v1/embeddings  {"model":"...", "input":"..."}
    // Response: {"data": [{"embedding": [0.1, 0.2, ...]}]}
    std::string url = host_ + "/v1/embeddings";
    json req = {{"model", model_}, {"input", text}};
    std::string response = httpPost(url, req.dump());

    auto j = json::parse(response);
    if (!j.contains("data") || j["data"].empty() ||
        !j["data"][0].contains("embedding")) {
        throw std::runtime_error("Invalid OpenAI response: " + response.substr(0, 200));
    }
    return j["data"][0]["embedding"].get<std::vector<float>>();
}

std::vector<float> EmbeddingClient::embed(const std::string& text) {
    switch (provider_) {
        case EmbedProvider::Ollama: return embedOllama(text);
        case EmbedProvider::OpenAI: return embedOpenAI(text);
    }
    throw std::runtime_error("Unknown embedding provider");
}

bool EmbeddingClient::isHealthy() {
    try {
        auto vec = embed("health check");
        return !vec.empty();
    } catch (...) {
        return false;
    }
}
