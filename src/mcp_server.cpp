#include "mcp_server.h"
#include <string>

McpServer::McpServer(MemoryTools& tools, RecipeTools* recipes)
    : tools_(tools), recipes_(recipes) {}

namespace {
// SDD-004 section 4.1, carried in the tool descriptions so every session
// reads them before it writes or runs a recipe.
const char* const kRecipeRules =
    " Rules: a recipe never holds a secret value; it names the secrets file and "
    "variable it reads (source ~/.secrets; $SERVICE_TOKEN), and the server refuses "
    "a script, prereqs or params that look like they hold one. mutates:true changes "
    "nothing about permission: ask the user before running it, as for any action "
    "that changes data or a live system. A broken recipe is fixed in place with "
    "recipe_update once the fix is proven, never copied.";

json recipeFieldProps() {
    return {
        {"description", {{"type", "string"}, {"description", "One or two sentences: what it does and when to use it. Embedded for search."}}},
        {"language", {{"type", "string"}, {"description", "bash, zsh, sql, python, powershell, js, ..."}}},
        {"script", {{"type", "string"}, {"description", "The script, runnable as is once {{param}} placeholders are filled. Max 64 KB. No secret values."}}},
        {"runs_on", {{"type", "array"}, {"items", {{"type", "string"}}},
                     {"description", "Host labels where it runs: mac, hub, vps, windows, docker, any"}}},
        {"mutates", {{"type", "boolean"}, {"description", "true if it changes data or a live system (write SQL, deploy, delete, restart). Never assumed: say it."}}},
        {"params", {{"type", "array"},
                    {"items", {{"type", "object"},
                               {"properties", {{"name", {{"type", "string"}}},
                                               {"description", {{"type", "string"}}},
                                               {"example", {{"type", "string"}}}}},
                               {"required", {"name"}}}},
                    {"description", "Parameters; the script refers to each as {{name}}"}}},
        {"prereqs", {{"type", "array"}, {"items", {{"type", "string"}}},
                     {"description", "What must be true first: tools installed, a secrets file sourced (variable NAME, never value), a tunnel up"}}},
        {"tags", {{"type", "array"}, {"items", {{"type", "string"}}},
                  {"description", "Free labels for exact filtering"}}},
    };
}
} // namespace

json McpServer::makeResponse(const json& id, const json& result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

json McpServer::makeError(const json& id, int code, const std::string& message) {
    return {{"jsonrpc", "2.0"}, {"id", id},
            {"error", {{"code", code}, {"message", message}}}};
}

json McpServer::toolResult(const std::string& text, bool is_error) {
    return {{"content", json::array({{{"type", "text"}, {"text", text}}})},
            {"isError", is_error}};
}

json McpServer::handleInitialize(const json& request) {
    json result = {
        {"protocolVersion", "2024-11-05"},
        {"capabilities", {
            {"tools", json::object()}
        }},
        {"serverInfo", {
            {"name", "claude-mem"},
            {"version", HMS_CLAUDE_MEM_VERSION}
        }}
    };
    return makeResponse(request["id"], result);
}

json McpServer::handleToolsList(const json& request) {
    json tools = json::array({
        {
            {"name", "mem_store"},
            {"description", "Store a memory with semantic key. Keys should be descriptive sentences like 'hms-cpap deploy process and steps' so semantic search works well. Use pinned=true for critical facts that should never decay."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"key", {{"type", "string"}, {"description", "Descriptive key (sentence-like for good embeddings)"}}},
                    {"value", {{"type", "string"}, {"description", "The information to store"}}},
                    {"category", {{"type", "string"}, {"description", "Category for filtering (e.g. project:hms-cpap, infra:redis, user:preference)"},
                                  {"default", "general"}}},
                    {"pinned", {{"type", "boolean"}, {"description", "Pin this memory so it never decays in search ranking (default false)"},
                                {"default", false}}}
                }},
                {"required", {"key", "value"}}
            }}
        },
        {
            {"name", "mem_search"},
            {"description", "Semantic search for memories with recency weighting. Recent and pinned memories rank higher. Optionally filter by category for hybrid retrieval."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"query", {{"type", "string"}, {"description", "Natural language search query"}}},
                    {"top_k", {{"type", "integer"}, {"description", "Number of results (default 5)"}, {"default", 5}}},
                    {"category", {{"type", "string"}, {"description", "Filter results to this category only (e.g. project:hms-cpap)"}}}
                }},
                {"required", {"query"}}
            }}
        },
        {
            {"name", "mem_get"},
            {"description", "Get a specific memory by exact key."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"key", {{"type", "string"}, {"description", "Exact key to retrieve"}}}
                }},
                {"required", {"key"}}
            }}
        },
        {
            {"name", "mem_delete"},
            {"description", "Delete a memory by exact key."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"key", {{"type", "string"}, {"description", "Exact key to delete"}}}
                }},
                {"required", {"key"}}
            }}
        },
        {
            {"name", "mem_list"},
            {"description", "List stored memories, optionally filtered by category."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"category", {{"type", "string"}, {"description", "Filter by category (e.g. project:hms-cpap)"}}},
                    {"limit", {{"type", "integer"}, {"description", "Max results (default 20)"}, {"default", 20}}}
                }}
            }}
        }
    });

    if (recipes_) {
        const std::string rules = kRecipeRules;
        json name_prop = {{"type", "string"}, {"description", "Unique kebab-case name, e.g. ingest-nights-for-user"}};

        json create_props = recipeFieldProps();
        create_props["name"] = name_prop;
        tools.push_back({
            {"name", "recipe_create"},
            {"description", "Store a recipe: a named, ready-to-run script shared by every session. "
                            "Before writing a script, recipe_search for it; after writing one that worked "
                            "and will be needed again, recipe_create it. Refuses an existing name (use "
                            "recipe_update) and a script over 64 KB." + rules},
            {"inputSchema", {{"type", "object"}, {"properties", create_props},
                             {"required", {"name", "description", "language", "script", "runs_on", "mutates"}}}}
        });

        json update_props = recipeFieldProps();
        update_props["name"] = {{"type", "string"}, {"description", "Exact name of the recipe to change"}};
        update_props["new_name"] = {{"type", "string"}, {"description", "Rename the recipe to this kebab-case name"}};
        update_props["verified"] = {{"type", "boolean"}, {"description", "true stamps verified_at with today: the recipe was just confirmed working"}};
        tools.push_back({
            {"name", "recipe_update"},
            {"description", "Change a recipe: only the fields given change. verified:true stamps verified_at "
                            "with today. Refuses a name that does not exist (use recipe_create). Use it to fix "
                            "a broken recipe in place once the fix is proven." + rules},
            {"inputSchema", {{"type", "object"}, {"properties", update_props}, {"required", {"name"}}}}
        });

        tools.push_back({
            {"name", "recipe_delete"},
            {"description", "Delete a recipe by exact name."},
            {"inputSchema", {{"type", "object"},
                             {"properties", {{"name", {{"type", "string"}, {"description", "Exact recipe name"}}}}},
                             {"required", {"name"}}}}
        });

        tools.push_back({
            {"name", "recipe_get"},
            {"description", "Get a whole recipe, script included, by exact name." + rules},
            {"inputSchema", {{"type", "object"},
                             {"properties", {{"name", {{"type", "string"}, {"description", "Exact recipe name"}}}}},
                             {"required", {"name"}}}}
        });

        tools.push_back({
            {"name", "recipe_search"},
            {"description", "Find recipes. Search here before writing a script. With name: exact match, "
                            "same as recipe_get. With only filters (tag, language, runs_on, mutates): every "
                            "recipe matching all of them, by name. With query: semantic search, then the "
                            "filters. Results carry name, description, language, runs_on, mutates, "
                            "verified_at; fetch the script with recipe_get." + rules},
            {"inputSchema", {{"type", "object"},
                             {"properties", {
                                 {"query", {{"type", "string"}, {"description", "Natural language: what the script should do"}}},
                                 {"name", {{"type", "string"}, {"description", "Exact recipe name"}}},
                                 {"tag", {{"type", "string"}, {"description", "Only recipes with this tag"}}},
                                 {"language", {{"type", "string"}, {"description", "Only recipes in this language"}}},
                                 {"runs_on", {{"type", "string"}, {"description", "Only recipes that list this host label"}}},
                                 {"mutates", {{"type", "boolean"}, {"description", "Only recipes whose mutates flag equals this"}}},
                                 {"top_k", {{"type", "integer"}, {"description", "Max semantic results (default 5)"}, {"default", 5}}}
                             }}}}
        });
    }

    return makeResponse(request["id"], {{"tools", tools}});
}

json McpServer::handleToolsCall(const json& request) {
    auto& params = request["params"];
    std::string name = params["name"];
    json args = params.contains("arguments") ? params["arguments"] : json::object();

    json result;

    if (name == "mem_store") {
        std::string key = args.at("key");
        std::string value = args.at("value");
        std::string category = args.value("category", "general");
        bool pinned = args.value("pinned", false);
        result = tools_.store(key, value, category, pinned);
    } else if (name == "mem_search") {
        std::string query = args.at("query");
        int top_k = args.value("top_k", 5);
        std::string category = args.value("category", "");
        result = tools_.search(query, top_k, category);
    } else if (name == "mem_get") {
        std::string key = args.at("key");
        result = tools_.get(key);
    } else if (name == "mem_delete") {
        std::string key = args.at("key");
        result = tools_.remove(key);
    } else if (name == "mem_list") {
        std::string category = args.value("category", "");
        int limit = args.value("limit", 20);
        result = tools_.list(category, limit);
    } else if (recipes_ && name.rfind("recipe_", 0) == 0) {
        if (!args.is_object()) {
            result = {{"error", "arguments must be an object"}};
        } else if (name == "recipe_create") {
            result = recipes_->create(args);
        } else if (name == "recipe_update") {
            result = recipes_->update(args);
        } else if (name == "recipe_delete") {
            result = recipes_->remove(args);
        } else if (name == "recipe_get") {
            result = recipes_->get(args);
        } else if (name == "recipe_search") {
            result = recipes_->search(args);
        } else {
            return makeError(request["id"], -32601, "Unknown tool: " + name);
        }
    } else {
        return makeError(request["id"], -32601, "Unknown tool: " + name);
    }

    bool is_error = result.contains("error");
    std::string text = result.dump(2);
    return makeResponse(request["id"], toolResult(text, is_error));
}

json McpServer::handleRequest(const json& request) {
    std::string method = request.value("method", "");

    if (method == "initialize") {
        return handleInitialize(request);
    } else if (method == "notifications/initialized") {
        return nullptr;
    } else if (method == "tools/list") {
        return handleToolsList(request);
    } else if (method == "tools/call") {
        return handleToolsCall(request);
    } else if (method == "ping") {
        return makeResponse(request["id"], json::object());
    } else {
        return makeError(request.value("id", json(nullptr)), -32601,
                         "Method not found: " + method);
    }
}
