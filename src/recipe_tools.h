#pragma once
#include "memory_store.h"
#include "embedding_client.h"
#include <nlohmann/json.hpp>
#include <mutex>
#include <optional>
#include <string>

using json = nlohmann::json;

// SDD-004: recipes, named ready-to-run scripts. Runs over its own store
// instance (the recipe pool), so no mem_* tool sees a recipe and no recipe_*
// tool sees a memory. Shares the embedder with MemoryTools.
class RecipeTools {
public:
    static constexpr size_t kMaxScriptBytes = 64 * 1024;

    RecipeTools(IMemoryStore& store, EmbeddingClient& embedder);

    // Each takes the tool call's arguments object and returns the JSON reply;
    // a refusal is {"error": "..."}.
    json create(const json& args);
    json update(const json& args);
    json remove(const json& args);
    json get(const json& args);
    json search(const json& args);

    // D3 secret guard over one field's text. Returns the refusal message, which
    // names the field and line and never contains any of the text itself.
    static std::optional<std::string> findSecret(const std::string& field,
                                                 const std::string& text);

    // The embedded text: "<language>: <name>. <description> [tags]".
    static std::string embedText(const json& recipe);

private:
    std::optional<json> load(const std::string& name);
    bool save(const json& recipe, const std::vector<float>* embedding);
    static json summary(const json& recipe);
    static std::string now();
    static std::string today();

    IMemoryStore& store_;
    EmbeddingClient& embedder_;
    // Serialises create/update/delete so "refuse if name exists" holds when
    // two HTTP clients write the same name at once.
    std::mutex write_mutex_;
};
