#include "http_transport.h"
#include "httplib.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {

void logLine(const std::string& msg) {
    // stderr: stdout stays clean in case anything ever pipes this process.
    std::cerr << "[http] " << msg << std::endl;
}

bool authorised(const httplib::Request& req, const std::string& token) {
    if (token.empty()) return true;               // auth disabled
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) return false;
    const std::string& v = it->second;
    const std::string prefix = "Bearer ";
    if (v.size() <= prefix.size()) return false;
    if (v.compare(0, prefix.size(), prefix) != 0) return false;
    // Length-independent compare is overkill for a LAN token, but cheap.
    const std::string got = v.substr(prefix.size());
    if (got.size() != token.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < token.size(); ++i)
        diff |= static_cast<unsigned char>(got[i] ^ token[i]);
    return diff == 0;
}

void echoSessionId(const httplib::Request& req, httplib::Response& res) {
    auto it = req.headers.find("Mcp-Session-Id");
    if (it != req.headers.end()) res.set_header("Mcp-Session-Id", it->second);
}

// A JSON-RPC request with no "id" is a notification: handled, never answered.
bool isNotification(const json& j) {
    return j.is_object() && !j.contains("id");
}

} // namespace

int runHttpTransport(McpServer& server, const HttpTransportConfig& cfg,
                     const volatile sig_atomic_t* stop_flag) {
    httplib::Server svr;

    svr.set_payload_max_length(16 * 1024 * 1024);   // memory values can be long
    svr.set_read_timeout(30, 0);
    svr.set_write_timeout(30, 0);

    svr.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
        json h = {
            {"status",  "ok"},
            {"service", "hms-claude-mem"},
            {"version", cfg.version},
            {"store",   cfg.store_provider},
            {"embed",   cfg.embed_provider},
            {"namespace", cfg.ns},
        };
        res.set_content(h.dump(), "application/json");
    });

    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        echoSessionId(req, res);

        if (!authorised(req, cfg.auth_token)) {
            res.status = 401;
            res.set_content(R"({"error":"unauthorized"})", "application/json");
            return;
        }

        json parsed;
        try {
            parsed = json::parse(req.body);
        } catch (const json::exception& e) {
            res.status = 400;
            json err = {{"jsonrpc", "2.0"}, {"id", nullptr},
                        {"error", {{"code", -32700},
                                   {"message", std::string("Parse error: ") + e.what()}}}};
            res.set_content(err.dump(), "application/json");
            return;
        }

        try {
            // Batch: array in, array out, notifications omitted from the reply.
            if (parsed.is_array()) {
                json replies = json::array();
                for (const auto& one : parsed) {
                    auto r = server.handleRequest(one);
                    if (!isNotification(one) && !r.is_null()) replies.push_back(r);
                }
                if (replies.empty()) { res.status = 204; return; }
                res.set_content(replies.dump(), "application/json");
                return;
            }

            auto reply = server.handleRequest(parsed);
            if (isNotification(parsed) || reply.is_null()) { res.status = 204; return; }
            res.set_content(reply.dump(), "application/json");

        } catch (const std::exception& e) {
            res.status = 500;
            json err = {{"jsonrpc", "2.0"},
                        {"id", parsed.is_object() && parsed.contains("id") ? parsed["id"] : json(nullptr)},
                        {"error", {{"code", -32603},
                                   {"message", std::string("Internal error: ") + e.what()}}}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // Signal handlers cannot safely call into httplib, so watch the flag the
    // handler sets and stop the listener from here instead. `done` exists so a
    // listen() that returns on its own (bind failure) can release the watcher;
    // without it the join() below would block forever waiting on a signal.
    std::atomic<bool> done{false};
    std::thread watcher;
    if (stop_flag) {
        watcher = std::thread([&] {
            while (!*stop_flag && !done.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (!done.load()) logLine("shutdown requested, stopping listener");
            svr.stop();
        });
    }

    logLine("listening on " + cfg.bind_addr + ":" + std::to_string(cfg.port) +
            (cfg.auth_token.empty() ? "  (no auth)" : "  (bearer auth)"));
    if (cfg.auth_token.empty() && cfg.bind_addr != "127.0.0.1" && cfg.bind_addr != "localhost") {
        logLine("WARNING: bound beyond loopback with no AUTH_TOKEN set");
    }

    const bool ok = svr.listen(cfg.bind_addr.c_str(), cfg.port);

    done = true;                 // release the watcher whatever ended listen()
    if (watcher.joinable()) {
        svr.stop();
        watcher.join();
    }

    if (!ok) {
        logLine("failed to bind " + cfg.bind_addr + ":" + std::to_string(cfg.port));
        return 1;
    }
    return 0;
}
