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

EmbeddingClient::EmbeddingClient(const std::string& ollama_host, const std::string& model)
    : ollama_url_(ollama_host + "/api/embed"), model_(model) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

EmbeddingClient::~EmbeddingClient() {
    curl_global_cleanup();
}

std::vector<float> EmbeddingClient::embed(const std::string& text) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Failed to init curl");

    json req = {{"model", model_}, {"input", text}};
    std::string body = req.dump();
    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, ollama_url_.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("Embedding request failed: ") + curl_easy_strerror(res));
    }

    auto j = json::parse(response);
    if (!j.contains("embeddings") || j["embeddings"].empty()) {
        throw std::runtime_error("Invalid embedding response: " + response.substr(0, 200));
    }

    return j["embeddings"][0].get<std::vector<float>>();
}

bool EmbeddingClient::isHealthy() {
    try {
        auto vec = embed("health check");
        return !vec.empty();
    } catch (...) {
        return false;
    }
}
