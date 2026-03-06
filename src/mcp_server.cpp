#include "mcp_server.h"
#include <iostream>
#include <string>

McpServer::McpServer(MemoryTools& tools) : tools_(tools) {}

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
            {"version", "1.2.0"}
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

void McpServer::run() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        try {
            auto request = json::parse(line);
            auto response = handleRequest(request);
            if (!response.is_null()) {
                std::cout << response.dump() << "\n";
                std::cout.flush();
            }
        } catch (const json::exception& e) {
            json err = makeError(nullptr, -32700, std::string("Parse error: ") + e.what());
            std::cout << err.dump() << "\n";
            std::cout.flush();
        } catch (const std::exception& e) {
            json err = makeError(nullptr, -32603, std::string("Internal error: ") + e.what());
            std::cout << err.dump() << "\n";
            std::cout.flush();
        }
    }
}
