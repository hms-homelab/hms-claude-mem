#pragma once
#include "tools.h"
#include <nlohmann/json.hpp>
#include <string>
#include <functional>

using json = nlohmann::json;

class McpServer {
public:
    McpServer(MemoryTools& tools);

    // Process a JSON-RPC request and return a response
    json handleRequest(const json& request);

    // Run the stdio loop (blocks until EOF)
    void run();

private:
    json handleInitialize(const json& request);
    json handleToolsList(const json& request);
    json handleToolsCall(const json& request);

    json makeResponse(const json& id, const json& result);
    json makeError(const json& id, int code, const std::string& message);
    json toolResult(const std::string& text, bool is_error = false);

    MemoryTools& tools_;
};
