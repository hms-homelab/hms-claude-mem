// Transport parity for SDD-003 stage 5.
//
// stdio and HTTP must be two drivers over one handler. This sends the same
// JSON-RPC requests down both paths and diffs the replies, so the modes cannot
// drift as either driver changes.
//
// Hermetic on purpose (decision 8.3 deletes stdio later, so tests must not lean
// on it): backed by EmbeddedStore in a temp dir, and restricted to protocol
// methods that never reach the embedder, so no Redis and no Ollama are needed.

#include <gtest/gtest.h>
#include "mcp_server.h"
#include "http_transport.h"
#include "embedded_store.h"
#include "embedding_client.h"
#include "tools.h"
#include "httplib.h"

#include <csignal>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kParityPort = 18901;

class TransportParityTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("hms-parity-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir_);

        store_ = std::make_unique<EmbeddedStore>("parity", (dir_ / "store.bin").string());
        // Never actually called: every request below is protocol-level.
        embedder_ = std::make_unique<EmbeddingClient>("http://127.0.0.1:1", "unused");
        tools_ = std::make_unique<MemoryTools>(*store_, *embedder_, 0.01);
        server_ = std::make_unique<McpServer>(*tools_);

        cfg_.bind_addr = "127.0.0.1";
        cfg_.port = kParityPort;
        cfg_.version = "test";
        stop_ = 0;
        http_ = std::thread([this] { runHttpTransport(*server_, cfg_, &stop_); });

        // Wait for the listener rather than sleeping a fixed amount.
        httplib::Client probe("127.0.0.1", kParityPort);
        probe.set_connection_timeout(0, 200000);
        for (int i = 0; i < 100; ++i) {
            if (probe.Get("/health")) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ADD_FAILURE() << "HTTP transport never came up on port " << kParityPort;
    }

    void TearDown() override {
        stop_ = 1;
        if (http_.joinable()) http_.join();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    // Same request, both drivers. stdio's driver is handleRequest() itself.
    json viaStdio(const json& req) { return server_->handleRequest(req); }

    json viaHttp(const json& req) {
        httplib::Client cli("127.0.0.1", kParityPort);
        cli.set_read_timeout(10, 0);
        auto res = cli.Post("/mcp", req.dump(), "application/json");
        EXPECT_TRUE(res != nullptr) << "no HTTP response";
        if (!res) return json();
        if (res->status == 204) return json();       // notification
        return json::parse(res->body);
    }

    std::filesystem::path dir_;
    std::unique_ptr<EmbeddedStore> store_;
    std::unique_ptr<EmbeddingClient> embedder_;
    std::unique_ptr<MemoryTools> tools_;
    std::unique_ptr<McpServer> server_;
    HttpTransportConfig cfg_;
    volatile sig_atomic_t stop_ = 0;
    std::thread http_;
};

TEST_F(TransportParityTest, ProtocolRepliesAreIdentical) {
    const std::vector<json> requests = {
        {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
         {"params", {{"protocolVersion", "2024-11-05"}, {"clientInfo", {{"name", "parity"}}}}}},
        {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}},
        {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "ping"}},
        {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "no/such/method"}},
    };

    for (const auto& req : requests) {
        const json a = viaStdio(req);
        const json b = viaHttp(req);
        EXPECT_EQ(a, b) << "drivers disagreed on method "
                        << req.value("method", "?") << "\n stdio: " << a.dump()
                        << "\n http : " << b.dump();
    }
}

TEST_F(TransportParityTest, NotificationYields204AndNoBody) {
    httplib::Client cli("127.0.0.1", kParityPort);
    json note = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    auto res = cli.Post("/mcp", note.dump(), "application/json");
    ASSERT_TRUE(res != nullptr);
    EXPECT_EQ(res->status, 204);
    EXPECT_TRUE(res->body.empty());
}

TEST_F(TransportParityTest, BatchMatchesIndividualCalls) {
    json batch = json::array({
        {{"jsonrpc", "2.0"}, {"id", 10}, {"method", "ping"}},
        {{"jsonrpc", "2.0"}, {"id", 11}, {"method", "tools/list"}},
    });

    httplib::Client cli("127.0.0.1", kParityPort);
    cli.set_read_timeout(10, 0);
    auto res = cli.Post("/mcp", batch.dump(), "application/json");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);

    const json got = json::parse(res->body);
    ASSERT_TRUE(got.is_array());
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], viaStdio(batch[0]));
    EXPECT_EQ(got[1], viaStdio(batch[1]));
}

TEST_F(TransportParityTest, MalformedJsonIsRejectedWithParseError) {
    httplib::Client cli("127.0.0.1", kParityPort);
    auto res = cli.Post("/mcp", "{ not json", "application/json");
    ASSERT_TRUE(res != nullptr);
    EXPECT_EQ(res->status, 400);
    const json err = json::parse(res->body);
    EXPECT_EQ(err["error"]["code"], -32700);
}

TEST_F(TransportParityTest, HealthReportsWiring) {
    httplib::Client cli("127.0.0.1", kParityPort);
    auto res = cli.Get("/health");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    const json h = json::parse(res->body);
    EXPECT_EQ(h["status"], "ok");
    EXPECT_EQ(h["service"], "hms-claude-mem");
    EXPECT_EQ(h["version"], "test");
}

} // namespace
