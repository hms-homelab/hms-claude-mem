#pragma once
#include "mcp_server.h"
#include <string>
#include <csignal>

// HTTP transport for SDD-003. Drives the same McpServer::handleRequest() the
// stdio loop drives, so the two modes cannot diverge.
//
// Endpoints:
//   POST /mcp     JSON-RPC request (or a batch array) in, reply out
//   GET  /health  liveness plus what the process is wired to
//
// Stateless by decision 8.2: no session table. Mcp-Session-Id is echoed back if
// a client sends one, otherwise ignored.
struct HttpTransportConfig {
    std::string bind_addr = "127.0.0.1";
    int         port      = 8901;
    std::string auth_token;      // empty disables auth (only safe on loopback)
    std::string version;         // reported by /health
    std::string store_provider;  // reported by /health
    std::string embed_provider;  // reported by /health
    std::string ns;              // reported by /health
};

// Blocks until the process is signalled. Returns 0 on clean shutdown,
// non-zero if the listener could not be bound.
int runHttpTransport(McpServer& server, const HttpTransportConfig& cfg,
                     const volatile sig_atomic_t* stop_flag);
