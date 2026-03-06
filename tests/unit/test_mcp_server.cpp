#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "mcp_server.h"
#include "tools.h"
#include "redis_client.h"
#include "embedding_client.h"

// Test MCP protocol handling with a real Redis + Ollama
// These are integration-ish tests since we need actual services

class McpServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        redis_ = std::make_unique<RedisClient>("127.0.0.1", 6379);
        if (!redis_->connect()) {
            GTEST_SKIP() << "Redis not available";
        }
        embedder_ = std::make_unique<EmbeddingClient>(
            "http://192.168.2.5:11434", "nomic-embed-text");
        tools_ = std::make_unique<MemoryTools>(*redis_, *embedder_);
        server_ = std::make_unique<McpServer>(*tools_);
    }

    void TearDown() override {
        // Clean up test keys
        redis_->vectorRemove("test:unit:key1");
        redis_->vectorRemove("test:unit:key2");
        redis_->hashDelete("test:unit:key1");
        redis_->hashDelete("test:unit:key2");
    }

    std::unique_ptr<RedisClient> redis_;
    std::unique_ptr<EmbeddingClient> embedder_;
    std::unique_ptr<MemoryTools> tools_;
    std::unique_ptr<McpServer> server_;
};

TEST_F(McpServerTest, InitializeReturnsProtocolVersion) {
    json req = {
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", {{"protocolVersion", "2024-11-05"},
                    {"clientInfo", {{"name", "test"}}}}}
    };
    auto resp = server_->handleRequest(req);
    EXPECT_EQ(resp["result"]["protocolVersion"], "2024-11-05");
    EXPECT_EQ(resp["result"]["serverInfo"]["name"], "claude-mem");
}

TEST_F(McpServerTest, ToolsListReturns5Tools) {
    json req = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}};
    auto resp = server_->handleRequest(req);
    EXPECT_EQ(resp["result"]["tools"].size(), 5);

    // Verify tool names
    std::vector<std::string> names;
    for (auto& t : resp["result"]["tools"]) {
        names.push_back(t["name"]);
    }
    EXPECT_THAT(names, ::testing::UnorderedElementsAre(
        "mem_store", "mem_search", "mem_get", "mem_delete", "mem_list"));
}

TEST_F(McpServerTest, StoreAndRetrieveMemory) {
    // Store
    json store_req = {
        {"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
        {"params", {{"name", "mem_store"},
                    {"arguments", {{"key", "test:unit:key1"},
                                   {"value", "Redis runs on port 6379"},
                                   {"category", "test"}}}}}
    };
    auto store_resp = server_->handleRequest(store_req);
    auto store_text = json::parse(store_resp["result"]["content"][0]["text"].get<std::string>());
    EXPECT_EQ(store_text["status"], "stored");

    // Get
    json get_req = {
        {"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"},
        {"params", {{"name", "mem_get"},
                    {"arguments", {{"key", "test:unit:key1"}}}}}
    };
    auto get_resp = server_->handleRequest(get_req);
    auto get_text = json::parse(get_resp["result"]["content"][0]["text"].get<std::string>());
    EXPECT_EQ(get_text["value"], "Redis runs on port 6379");
    EXPECT_EQ(get_text["category"], "test");
}

TEST_F(McpServerTest, SearchFindsRelevantMemory) {
    // Store two memories
    json store1 = {
        {"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"},
        {"params", {{"name", "mem_store"},
                    {"arguments", {{"key", "test:unit:key1"},
                                   {"value", "PostgreSQL password is maestro_postgres_2026"},
                                   {"category", "infra:postgres"}}}}}
    };
    json store2 = {
        {"jsonrpc", "2.0"}, {"id", 6}, {"method", "tools/call"},
        {"params", {{"name", "mem_store"},
                    {"arguments", {{"key", "test:unit:key2"},
                                   {"value", "MQTT broker at port 1883"},
                                   {"category", "infra:mqtt"}}}}}
    };
    server_->handleRequest(store1);
    server_->handleRequest(store2);

    // Search for database-related memory
    json search_req = {
        {"jsonrpc", "2.0"}, {"id", 7}, {"method", "tools/call"},
        {"params", {{"name", "mem_search"},
                    {"arguments", {{"query", "database password credentials"},
                                   {"top_k", 2}}}}}
    };
    auto search_resp = server_->handleRequest(search_req);
    auto search_text = json::parse(search_resp["result"]["content"][0]["text"].get<std::string>());
    EXPECT_GE(search_text["count"].get<int>(), 1);
    // The postgres key should rank higher than mqtt for "database password"
    EXPECT_EQ(search_text["results"][0]["key"], "test:unit:key1");
}

TEST_F(McpServerTest, DeleteRemovesMemory) {
    // Store then delete
    json store_req = {
        {"jsonrpc", "2.0"}, {"id", 8}, {"method", "tools/call"},
        {"params", {{"name", "mem_store"},
                    {"arguments", {{"key", "test:unit:key1"},
                                   {"value", "temporary data"},
                                   {"category", "test"}}}}}
    };
    server_->handleRequest(store_req);

    json del_req = {
        {"jsonrpc", "2.0"}, {"id", 9}, {"method", "tools/call"},
        {"params", {{"name", "mem_delete"},
                    {"arguments", {{"key", "test:unit:key1"}}}}}
    };
    auto del_resp = server_->handleRequest(del_req);
    auto del_text = json::parse(del_resp["result"]["content"][0]["text"].get<std::string>());
    EXPECT_EQ(del_text["status"], "deleted");

    // Verify gone
    json get_req = {
        {"jsonrpc", "2.0"}, {"id", 10}, {"method", "tools/call"},
        {"params", {{"name", "mem_get"},
                    {"arguments", {{"key", "test:unit:key1"}}}}}
    };
    auto get_resp = server_->handleRequest(get_req);
    auto get_text = json::parse(get_resp["result"]["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(get_text.contains("error"));
}

TEST_F(McpServerTest, UnknownMethodReturnsError) {
    json req = {{"jsonrpc", "2.0"}, {"id", 11}, {"method", "unknown/method"}};
    auto resp = server_->handleRequest(req);
    EXPECT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32601);
}

TEST_F(McpServerTest, PingReturnsEmpty) {
    json req = {{"jsonrpc", "2.0"}, {"id", 12}, {"method", "ping"}};
    auto resp = server_->handleRequest(req);
    EXPECT_TRUE(resp["result"].is_object());
}
