#pragma once
#include "tools.h"
#include "recipe_tools.h"
#include <nlohmann/json.hpp>
#include <string>
#include <functional>

using json = nlohmann::json;

class McpServer {
public:
    // recipes is optional so a server can be built over memories alone; the
    // daemon always passes it (SDD-004).
    McpServer(MemoryTools& tools, RecipeTools* recipes = nullptr);

    // Process a JSON-RPC request and return a response
    json handleRequest(const json& request);


private:
    json handleInitialize(const json& request);
    json handleToolsList(const json& request);
    json handleToolsCall(const json& request);

    json makeResponse(const json& id, const json& result);
    json makeError(const json& id, int code, const std::string& message);
    json toolResult(const std::string& text, bool is_error = false);

    MemoryTools& tools_;
    RecipeTools* recipes_;
};
