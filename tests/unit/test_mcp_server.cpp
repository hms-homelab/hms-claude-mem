#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "mcp_server.h"
#include "tools.h"
#include "redis_client.h"
#include "embedding_client.h"

class McpServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Use "test" namespace to isolate from production data
        redis_ = std::make_unique<RedisClient>("127.0.0.1", 6379, "test");
        if (!redis_->connect()) {
            GTEST_SKIP() << "Redis not available";
        }
        embedder_ = std::make_unique<EmbeddingClient>(
            "http://192.168.2.5:11434", "nomic-embed-text");
        tools_ = std::make_unique<MemoryTools>(*redis_, *embedder_, 0.01);
        server_ = std::make_unique<McpServer>(*tools_);
    }

    void TearDown() override {
        if (!redis_ || !redis_->isConnected()) return;
        for (auto& k : test_keys_) {
            redis_->vectorRemove(k);
            redis_->hashDelete(k);
        }
    }

    // Helper to call a tool and parse the result JSON
    json callTool(const std::string& name, const json& arguments, int id = 1) {
        json req = {
            {"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"},
            {"params", {{"name", name}, {"arguments", arguments}}}
        };
        auto resp = server_->handleRequest(req);
        return json::parse(resp["result"]["content"][0]["text"].get<std::string>());
    }

    void trackKey(const std::string& key) { test_keys_.push_back(key); }

    std::unique_ptr<RedisClient> redis_;
    std::unique_ptr<EmbeddingClient> embedder_;
    std::unique_ptr<MemoryTools> tools_;
    std::unique_ptr<McpServer> server_;
    std::vector<std::string> test_keys_;
};

// === Protocol Tests ===

TEST_F(McpServerTest, InitializeReturnsVersion110) {
    json req = {
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", {{"protocolVersion", "2024-11-05"},
                    {"clientInfo", {{"name", "test"}}}}}
    };
    auto resp = server_->handleRequest(req);
    EXPECT_EQ(resp["result"]["protocolVersion"], "2024-11-05");
    EXPECT_EQ(resp["result"]["serverInfo"]["version"], "1.1.0");
}

TEST_F(McpServerTest, ToolsListReturns5Tools) {
    json req = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}};
    auto resp = server_->handleRequest(req);
    EXPECT_EQ(resp["result"]["tools"].size(), 5);
}

TEST_F(McpServerTest, PingReturnsEmpty) {
    json req = {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "ping"}};
    auto resp = server_->handleRequest(req);
    EXPECT_TRUE(resp["result"].is_object());
}

TEST_F(McpServerTest, UnknownMethodReturnsError) {
    json req = {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "unknown/method"}};
    auto resp = server_->handleRequest(req);
    EXPECT_EQ(resp["error"]["code"], -32601);
}

// === Store & Retrieve Tests ===

TEST_F(McpServerTest, StoreAndRetrieveMemory) {
    trackKey("test:v11:basic");
    auto stored = callTool("mem_store", {
        {"key", "test:v11:basic"},
        {"value", "Redis runs on port 6379"},
        {"category", "test"}
    });
    EXPECT_EQ(stored["status"], "stored");
    EXPECT_FALSE(stored["is_update"].get<bool>());

    auto got = callTool("mem_get", {{"key", "test:v11:basic"}});
    EXPECT_EQ(got["value"], "Redis runs on port 6379");
    EXPECT_EQ(got["category"], "test");
    EXPECT_FALSE(got["pinned"].get<bool>());
}

TEST_F(McpServerTest, StoreWithPinned) {
    trackKey("test:v11:pinned");
    auto stored = callTool("mem_store", {
        {"key", "test:v11:pinned"},
        {"value", "Critical fact"},
        {"category", "user:preference"},
        {"pinned", true}
    });
    EXPECT_EQ(stored["status"], "stored");
    EXPECT_TRUE(stored["pinned"].get<bool>());

    auto got = callTool("mem_get", {{"key", "test:v11:pinned"}});
    EXPECT_TRUE(got["pinned"].get<bool>());
}

TEST_F(McpServerTest, UpdatePreservesCreatedAt) {
    trackKey("test:v11:update");
    callTool("mem_store", {
        {"key", "test:v11:update"},
        {"value", "original value"},
        {"category", "test"}
    });
    auto first = callTool("mem_get", {{"key", "test:v11:update"}});
    std::string original_created = first["created_at"];

    auto updated = callTool("mem_store", {
        {"key", "test:v11:update"},
        {"value", "updated value"},
        {"category", "test"}
    });
    EXPECT_TRUE(updated["is_update"].get<bool>());

    auto got = callTool("mem_get", {{"key", "test:v11:update"}});
    EXPECT_EQ(got["value"], "updated value");
    EXPECT_EQ(got["created_at"], original_created);
}

// === Search Tests ===

TEST_F(McpServerTest, SearchFindsRelevantMemory) {
    trackKey("test:v11:postgres");
    trackKey("test:v11:mqtt");
    callTool("mem_store", {
        {"key", "test:v11:postgres"},
        {"value", "PostgreSQL password is secret123"},
        {"category", "infra:postgres"}
    });
    callTool("mem_store", {
        {"key", "test:v11:mqtt"},
        {"value", "MQTT broker at port 1883"},
        {"category", "infra:mqtt"}
    });

    auto results = callTool("mem_search", {
        {"query", "database password credentials"},
        {"top_k", 2}
    });
    EXPECT_GE(results["count"].get<int>(), 1);
    EXPECT_EQ(results["results"][0]["key"], "test:v11:postgres");
    // v1.1.0: results include final_score, age_days, pinned
    EXPECT_TRUE(results["results"][0].contains("final_score"));
    EXPECT_TRUE(results["results"][0].contains("age_days"));
    EXPECT_TRUE(results["results"][0].contains("pinned"));
}

TEST_F(McpServerTest, SearchWithCategoryFilter) {
    trackKey("test:v11:cat_a");
    trackKey("test:v11:cat_b");
    callTool("mem_store", {
        {"key", "test:v11:cat_a"},
        {"value", "This is about deployment"},
        {"category", "project:alpha"}
    });
    callTool("mem_store", {
        {"key", "test:v11:cat_b"},
        {"value", "This is also about deployment"},
        {"category", "project:beta"}
    });

    // Search with category filter — should only return alpha
    auto results = callTool("mem_search", {
        {"query", "deployment process"},
        {"top_k", 5},
        {"category", "project:alpha"}
    });
    EXPECT_EQ(results["count"].get<int>(), 1);
    EXPECT_EQ(results["results"][0]["category"], "project:alpha");
}

TEST_F(McpServerTest, SearchRecencyWeighting) {
    trackKey("test:v11:recent");
    // Store a memory — it's brand new so age_days ~ 0
    callTool("mem_store", {
        {"key", "test:v11:recent"},
        {"value", "Fresh information"},
        {"category", "test"}
    });

    auto results = callTool("mem_search", {
        {"query", "fresh information"},
        {"top_k", 1}
    });
    EXPECT_GE(results["count"].get<int>(), 1);
    auto& first = results["results"][0];
    // For a brand new memory, final_score should be very close to similarity
    double sim = first["similarity"].get<double>();
    double final_score = first["final_score"].get<double>();
    EXPECT_NEAR(sim, final_score, 0.01); // age ~ 0 means no decay
}

// === Delete Tests ===

TEST_F(McpServerTest, DeleteRemovesMemory) {
    trackKey("test:v11:del");
    callTool("mem_store", {
        {"key", "test:v11:del"},
        {"value", "temporary"},
        {"category", "test"}
    });

    auto del = callTool("mem_delete", {{"key", "test:v11:del"}});
    EXPECT_EQ(del["status"], "deleted");

    auto got = callTool("mem_get", {{"key", "test:v11:del"}});
    EXPECT_TRUE(got.contains("error"));
}

// === List Tests ===

TEST_F(McpServerTest, ListFiltersByCategory) {
    trackKey("test:v11:list_a");
    trackKey("test:v11:list_b");
    callTool("mem_store", {
        {"key", "test:v11:list_a"},
        {"value", "alpha item"},
        {"category", "list:alpha"}
    });
    callTool("mem_store", {
        {"key", "test:v11:list_b"},
        {"value", "beta item"},
        {"category", "list:beta"}
    });

    auto results = callTool("mem_list", {{"category", "list:alpha"}});
    for (auto& item : results["items"]) {
        EXPECT_EQ(item["category"], "list:alpha");
    }
}

TEST_F(McpServerTest, ListShowsPinnedField) {
    trackKey("test:v11:list_pin");
    callTool("mem_store", {
        {"key", "test:v11:list_pin"},
        {"value", "pinned item"},
        {"category", "test"},
        {"pinned", true}
    });

    auto results = callTool("mem_list", {{"category", "test"}});
    bool found = false;
    for (auto& item : results["items"]) {
        if (item["key"] == "test:v11:list_pin") {
            EXPECT_TRUE(item["pinned"].get<bool>());
            found = true;
        }
    }
    EXPECT_TRUE(found) << "Pinned item not found in list";
}

// === Namespace Isolation Test ===

TEST_F(McpServerTest, NamespaceIsolation) {
    // Our test fixture uses "test" namespace
    EXPECT_EQ(redis_->getNamespace(), "test");

    trackKey("test:v11:ns");
    callTool("mem_store", {
        {"key", "test:v11:ns"},
        {"value", "namespace test"},
        {"category", "test"}
    });

    // Create a second client with different namespace
    RedisClient other_redis("127.0.0.1", 6379, "other");
    ASSERT_TRUE(other_redis.connect());

    // Should NOT find the key in "other" namespace
    auto entry = other_redis.hashGet("test:v11:ns");
    EXPECT_FALSE(entry.has_value());
}
