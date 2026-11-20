// SDD-004 recipes: the section 10 unit suite, run against both store providers.
//
// Embedded store: always, in a temp dir. Redis: only when one answers at
// REDIS_HOST:REDIS_PORT (default 127.0.0.1:6379), under a throwaway prefix
// (claude:test:recipe:<pid>:) and memory namespace (test-recipes-<pid>), never
// the real claude:recipe: pool. TearDown deletes every key the test made.
//
// The embedder is a stand-in (hashed bag of words): offline, deterministic,
// and it counts calls, which is how re-embedding is asserted.

#include <gtest/gtest.h>
#include "mcp_server.h"
#include "tools.h"
#include "recipe_tools.h"
#include "redis_client.h"
#include "embedded_store.h"
#include "embedding_client.h"

#include <unistd.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string envOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

class FakeEmbedder : public EmbeddingClient {
public:
    FakeEmbedder() : EmbeddingClient("http://127.0.0.1:1", "unused") {}
    std::vector<float> embed(const std::string& text) override {
        ++calls;
        std::vector<float> v(64, 0.0f);
        std::string word;
        auto flush = [&]() {
            if (word.empty()) return;
            uint32_t h = 2166136261u;
            for (unsigned char c : word) { h ^= c; h *= 16777619u; }
            v[h % 64] += 1.0f;
            word.clear();
        };
        for (unsigned char c : text) {
            if (std::isalnum(c)) word.push_back(static_cast<char>(std::tolower(c)));
            else flush();
        }
        flush();
        double n = 0;
        for (float x : v) n += x * x;
        n = std::sqrt(n);
        if (n > 0)
            for (float& x : v) x = static_cast<float>(x / n);
        return v;
    }
    int calls = 0;
};

enum class Provider { Embedded, Redis };

std::string providerName(const ::testing::TestParamInfo<Provider>& info) {
    return info.param == Provider::Embedded ? "Embedded" : "Redis";
}

json baseRecipe(const std::string& name) {
    return {
        {"name", name},
        {"description", "Count the rows of a table in the local database"},
        {"language", "sql"},
        {"script", "SELECT count(*) FROM {{table}};"},
        {"runs_on", {"mac", "hub"}},
        {"mutates", false},
        {"params", {{{"name", "table"}, {"description", "Table to count"}, {"example", "nights"}}}},
        {"prereqs", {"psql installed", "source ~/.secrets for $DB_URL"}},
        {"tags", {"postgres", "stats"}},
    };
}

} // namespace

class RecipeTest : public ::testing::TestWithParam<Provider> {
protected:
    void SetUp() override {
        const std::string tag = std::to_string(::getpid());
        if (GetParam() == Provider::Embedded) {
            dir_ = fs::temp_directory_path() / ("hms-recipes-" + tag + "-" +
                   ::testing::UnitTest::GetInstance()->current_test_info()->name());
            fs::remove_all(dir_);
            fs::create_directories(dir_);
            const std::string mem_path = (dir_ / "test.db").string();
            mem_store_ = std::make_unique<EmbeddedStore>("test", mem_path);
            recipe_store_ = std::make_unique<EmbeddedStore>(
                "recipes", EmbeddedStore::recipesPathBeside("test", mem_path));
        } else {
            const std::string host = envOr("REDIS_HOST", "127.0.0.1");
            const int port = std::stoi(envOr("REDIS_PORT", "6379"));
            auto probe = std::make_unique<RedisClient>(host, port, "probe");
            if (!probe->connect()) GTEST_SKIP() << "Redis not available at " << host << ":" << port;
            mem_store_ = std::make_unique<RedisClient>(host, port, "test-recipes-" + tag);
            recipe_store_ = std::make_unique<RedisClient>(host, port, "unused",
                                                          "claude:test:recipe:" + tag + ":");
        }
        recipe_store_->connect();
        mem_store_->connect();
        memories_ = std::make_unique<MemoryTools>(*mem_store_, embedder_, 0.01);
        recipes_ = std::make_unique<RecipeTools>(*recipe_store_, embedder_);
        server_ = std::make_unique<McpServer>(*memories_, recipes_.get());
    }

    void TearDown() override {
        for (IMemoryStore* s : {recipe_store_.get(), mem_store_.get()}) {
            if (!s) continue;
            for (const auto& k : s->scanKeys("*", 100000)) {
                s->vectorRemove(k);
                s->hashDelete(k);
            }
        }
        server_.reset();
        recipes_.reset();
        memories_.reset();
        recipe_store_.reset();
        mem_store_.reset();
        if (!dir_.empty()) fs::remove_all(dir_);
    }

    json call(const std::string& tool, const json& args) {
        json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                    {"params", {{"name", tool}, {"arguments", args}}}};
        auto resp = server_->handleRequest(req);
        return json::parse(resp["result"]["content"][0]["text"].get<std::string>());
    }

    bool isError(const json& j) { return j.contains("error"); }

    fs::path dir_;
    FakeEmbedder embedder_;
    std::unique_ptr<IMemoryStore> mem_store_;
    std::unique_ptr<IMemoryStore> recipe_store_;
    std::unique_ptr<MemoryTools> memories_;
    std::unique_ptr<RecipeTools> recipes_;
    std::unique_ptr<McpServer> server_;
};

// === create / get ===

TEST_P(RecipeTest, CreateThenGetReturnsEveryField) {
    auto c = call("recipe_create", baseRecipe("count-rows"));
    ASSERT_FALSE(isError(c)) << c.dump();
    EXPECT_EQ(c["status"], "created");

    auto g = call("recipe_get", {{"name", "count-rows"}});
    ASSERT_FALSE(isError(g)) << g.dump();
    json want = baseRecipe("count-rows");
    for (const char* f : {"name", "description", "language", "script", "runs_on",
                          "mutates", "params", "prereqs", "tags"})
        EXPECT_EQ(g[f], want[f]) << f;
    EXPECT_TRUE(g["verified_at"].is_null());
    EXPECT_FALSE(g["created_at"].get<std::string>().empty());
    EXPECT_EQ(g["created_at"], g["updated_at"]);
}

TEST_P(RecipeTest, CreateWithOnlyRequiredFields) {
    json r = {{"name", "hello"}, {"description", "Print hello"}, {"language", "bash"},
              {"script", "echo hello"}, {"runs_on", {"any"}}, {"mutates", false}};
    ASSERT_FALSE(isError(call("recipe_create", r)));
    auto g = call("recipe_get", {{"name", "hello"}});
    EXPECT_EQ(g["params"], json::array());
    EXPECT_EQ(g["prereqs"], json::array());
    EXPECT_EQ(g["tags"], json::array());
}

TEST_P(RecipeTest, CreateRefusesDuplicateName) {
    ASSERT_FALSE(isError(call("recipe_create", baseRecipe("dup"))));
    auto again = call("recipe_create", baseRecipe("dup"));
    ASSERT_TRUE(isError(again));
    EXPECT_NE(again["error"].get<std::string>().find("recipe_update"), std::string::npos);
}

TEST_P(RecipeTest, CreateRequiresMutatesToBeSaid) {
    json r = baseRecipe("no-mutates");
    r.erase("mutates");
    auto c = call("recipe_create", r);
    ASSERT_TRUE(isError(c));
    EXPECT_NE(c["error"].get<std::string>().find("mutates"), std::string::npos);
    EXPECT_TRUE(isError(call("recipe_get", {{"name", "no-mutates"}})));
}

TEST_P(RecipeTest, CreateRefusesNonKebabName) {
    for (const char* bad : {"Count_Rows", "count rows", "-lead", "trail-", "a--b", ""}) {
        json r = baseRecipe("x");
        r["name"] = bad;
        EXPECT_TRUE(isError(call("recipe_create", r))) << bad;
    }
}

TEST_P(RecipeTest, CreateRefusesScriptOver64KB) {
    json r = baseRecipe("big");
    r["script"] = std::string(64 * 1024 + 1, 'a');
    auto c = call("recipe_create", r);
    ASSERT_TRUE(isError(c));
    EXPECT_NE(c["error"].get<std::string>().find("64 KB"), std::string::npos);

    r["script"] = std::string(64 * 1024, 'a'); // exactly at the cap is fine
    EXPECT_FALSE(isError(call("recipe_create", r)));
}

// === update ===

TEST_P(RecipeTest, UpdateChangesOnlyGivenFields) {
    ASSERT_FALSE(isError(call("recipe_create", baseRecipe("upd"))));
    auto before = call("recipe_get", {{"name", "upd"}});

    auto u = call("recipe_update", {{"name", "upd"}, {"script", "SELECT 1;"}, {"mutates", true}});
    ASSERT_FALSE(isError(u)) << u.dump();

    auto after = call("recipe_get", {{"name", "upd"}});
    EXPECT_EQ(after["script"], "SELECT 1;");
    EXPECT_EQ(after["mutates"], true);
    for (const char* f : {"description", "language", "runs_on", "params", "prereqs", "tags", "created_at"})
        EXPECT_EQ(after[f], before[f]) << f;
    EXPECT_TRUE(after["verified_at"].is_null());
}

TEST_P(RecipeTest, UpdateRefusesMissingName) {
    auto u = call("recipe_update", {{"name", "never-made"}, {"script", "x"}});
    ASSERT_TRUE(isError(u));
    EXPECT_NE(u["error"].get<std::string>().find("recipe_create"), std::string::npos);
}

TEST_P(RecipeTest, UpdateRefusesOversizedScript) {
    ASSERT_FALSE(isError(call("recipe_create", baseRecipe("upd-big"))));
    EXPECT_TRUE(isError(call("recipe_update", {{"name", "upd-big"},
                                               {"script", std::string(64 * 1024 + 1, 'b')}})));
}

TEST_P(RecipeTest, VerifiedTrueStampsToday) {
    ASSERT_FALSE(isError(call("recipe_create", baseRecipe("ver"))));
    auto u = call("recipe_update", {{"name", "ver"}, {"verified", true}});
    ASSERT_FALSE(isError(u)) << u.dump();

    std::time_t t = std::time(nullptr);
    char today[11];
    std::strftime(today, sizeof(today), "%Y-%m-%d", std::gmtime(&t));
    EXPECT_EQ(call("recipe_get", {{"name", "ver"}})["verified_at"], today);
}

TEST_P(RecipeTest, ReembedsOnDescriptionChangeNotOnScriptChange) {
    ASSERT_FALSE(isError(call("recipe_create", baseRecipe("emb"))));
    EXPECT_EQ(embedder_.calls, 1);

    ASSERT_FALSE(isError(call("recipe_update", {{"name", "emb"}, {"script", "SELECT 2;"}})));
    ASSERT_FALSE(isError(call("recipe_update", {{"name", "emb"}, {"mutates", true}, {"verified", true}})));
    EXPECT_EQ(embedder_.calls, 1) << "script/mutates/verified must not re-embed";

    ASSERT_FALSE(isError(call("recipe_update", {{"name", "emb"},
                                                {"description", "Remove stale rows from a table"}})));
    EXPECT_EQ(embedder_.calls, 2);

    // The new description is what search now finds.
    auto s = call("recipe_search", {{"query", "remove stale rows"}, {"top_k", 1}});
    ASSERT_EQ(s["count"], 1);
    EXPECT_EQ(s["results"][0]["name"], "emb");
}

TEST_P(RecipeTest, ReembedsOnTagsChange) {
    ASSERT_FALSE(isError(call("recipe_create", baseRecipe("tagged"))));
    ASSERT_FALSE(isError(call("recipe_update", {{"name", "tagged"}, {"tags", {"redis"}}})));
    EXPECT_EQ(embedder_.calls, 2);
}

TEST_P(RecipeTest, RenameReembedsAndMoves) {
    ASSERT_FALSE(isError(call("recipe_create", baseRecipe("old-name"))));
    auto u = call("recipe_update", {{"name", "old-name"}, {"new_name", "new-name"}});
    ASSERT_FALSE(isError(u)) << u.dump();
    EXPECT_EQ(embedder_.calls, 2);
    EXPECT_TRUE(isError(call("recipe_get", {{"name", "old-name"}})));
    auto g = call("recipe_get", {{"name", "new-name"}});
    ASSERT_FALSE(isError(g));
    EXPECT_EQ(g["name"], "new-name");

    auto s = call("recipe_search", {{"query", "count rows"}, {"top_k", 5}});
    ASSERT_EQ(s["count"], 1);
    EXPECT_EQ(s["results"][0]["name"], "new-name");
}

TEST_P(RecipeTest, RenameRefusesExistingTarget) {
    ASSERT_FALSE(isError(call("recipe_create", baseRecipe("a-one"))));
    ASSERT_FALSE(isError(call("recipe_create", baseRecipe("a-two"))));
    EXPECT_TRUE(isError(call("recipe_update", {{"name", "a-one"}, {"new_name", "a-two"}})));
    EXPECT_FALSE(isError(call("recipe_get", {{"name", "a-one"}})));
}

// === delete ===

TEST_P(RecipeTest, DeleteRemovesRecipeAndVector) {
    ASSERT_FALSE(isError(call("recipe_create", baseRecipe("gone"))));
    auto d = call("recipe_delete", {{"name", "gone"}});
    EXPECT_EQ(d["status"], "deleted");
    EXPECT_TRUE(isError(call("recipe_get", {{"name", "gone"}})));
    EXPECT_EQ(call("recipe_search", {{"query", "count rows table"}})["count"], 0);
    EXPECT_EQ(call("recipe_search", json::object())["count"], 0);
    EXPECT_TRUE(isError(call("recipe_delete", {{"name", "gone"}})));
}

// === search ===

class RecipeSearchTest : public RecipeTest {
protected:
    void SetUp() override {
        RecipeTest::SetUp();
        if (IsSkipped()) return;
        auto mk = [&](const std::string& name, const std::string& desc, const std::string& lang,
                      json runs_on, bool mutates, json tags) {
            json r = {{"name", name}, {"description", desc}, {"language", lang},
                      {"script", "echo " + name}, {"runs_on", runs_on},
                      {"mutates", mutates}, {"tags", tags}};
            ASSERT_FALSE(isError(call("recipe_create", r))) << name;
        };
        mk("restart-ingest", "Restart the ingest service container", "bash", {"vps"}, true, {"ingest", "docker"});
        mk("tail-ingest-logs", "Follow the ingest service logs", "bash", {"vps"}, false, {"ingest", "logs"});
        mk("nights-per-user", "Count nights stored for one user", "sql", {"vps", "mac"}, false, {"ingest", "postgres"});
        mk("flush-redis-cache", "Delete cached keys from redis", "bash", {"hub"}, true, {"redis"});
    }
};

TEST_P(RecipeSearchTest, NameIsExactLikeGet) {
    auto s = call("recipe_search", {{"name", "tail-ingest-logs"}});
    EXPECT_EQ(s["name"], "tail-ingest-logs");
    EXPECT_EQ(s["script"], "echo tail-ingest-logs");
    EXPECT_TRUE(isError(call("recipe_search", {{"name", "tail-ingest"}})));
}

TEST_P(RecipeSearchTest, FiltersAloneMatchAllByName) {
    auto s = call("recipe_search", {{"tag", "ingest"}, {"language", "bash"}});
    ASSERT_EQ(s["count"], 2);
    EXPECT_EQ(s["results"][0]["name"], "restart-ingest");
    EXPECT_EQ(s["results"][1]["name"], "tail-ingest-logs");
    for (auto& r : s["results"]) {
        EXPECT_FALSE(r.contains("script"));
        for (const char* f : {"name", "description", "language", "runs_on", "mutates", "verified_at"})
            EXPECT_TRUE(r.contains(f)) << f;
    }

    auto m = call("recipe_search", {{"mutates", true}});
    ASSERT_EQ(m["count"], 2);
    EXPECT_EQ(m["results"][0]["name"], "flush-redis-cache");
    EXPECT_EQ(m["results"][1]["name"], "restart-ingest");

    auto r = call("recipe_search", {{"runs_on", "mac"}});
    ASSERT_EQ(r["count"], 1);
    EXPECT_EQ(r["results"][0]["name"], "nights-per-user");

    // Filters ignore top_k: every match.
    EXPECT_EQ(call("recipe_search", {{"tag", "ingest"}, {"top_k", 1}})["count"], 3);
    EXPECT_EQ(call("recipe_search", {{"tag", "nope"}})["count"], 0);
}

TEST_P(RecipeSearchTest, QueryAloneIsSemantic) {
    auto s = call("recipe_search", {{"query", "follow logs of the ingest service"}, {"top_k", 2}});
    ASSERT_EQ(s["count"], 2);
    EXPECT_EQ(s["results"][0]["name"], "tail-ingest-logs");
    EXPECT_TRUE(s["results"][0].contains("similarity"));
    EXPECT_FALSE(s["results"][0].contains("script"));
}

TEST_P(RecipeSearchTest, QueryThenFilters) {
    auto s = call("recipe_search", {{"query", "ingest service"}, {"mutates", false}, {"top_k", 5}});
    ASSERT_GE(s["count"].get<int>(), 1);
    for (auto& r : s["results"]) EXPECT_EQ(r["mutates"], false);
    EXPECT_EQ(s["results"][0]["name"], "tail-ingest-logs");

    // A filter that drops the best semantic hits still finds the match further down.
    auto h = call("recipe_search", {{"query", "ingest service logs"}, {"runs_on", "hub"}, {"top_k", 1}});
    ASSERT_EQ(h["count"], 1);
    EXPECT_EQ(h["results"][0]["name"], "flush-redis-cache");
}

// === separation from memories ===

TEST_P(RecipeTest, RecipesAndMemoriesNeverSeeEachOther) {
    ASSERT_FALSE(isError(call("recipe_create", baseRecipe("count-rows"))));
    auto m = call("mem_store", {{"key", "count rows of a table rule"},
                                {"value", "Use the replica for counts"},
                                {"category", "test"}});
    ASSERT_EQ(m["status"], "stored");

    auto list = call("mem_list", {{"limit", 100}});
    for (auto& i : list["items"]) EXPECT_NE(i["key"], "count-rows");
    EXPECT_EQ(list["count"], 1);

    auto ms = call("mem_search", {{"query", "count rows table"}, {"top_k", 10}});
    for (auto& r : ms["results"]) EXPECT_NE(r["key"], "count-rows");
    EXPECT_TRUE(isError(call("mem_get", {{"key", "count-rows"}})));

    auto rs = call("recipe_search", {{"query", "count rows of a table rule"}, {"top_k", 10}});
    ASSERT_EQ(rs["count"], 1);
    EXPECT_EQ(rs["results"][0]["name"], "count-rows");
    EXPECT_EQ(call("recipe_search", json::object())["count"], 1);
    EXPECT_TRUE(isError(call("recipe_get", {{"name", "count rows of a table rule"}})));
}

// === D3 secret guard through the tools ===

TEST_P(RecipeTest, SecretGuardRefusesAndNeverEchoes) {
    const std::string secret = std::string("gh") + "p_" + "Zq7Rk2Lm9Xw4Tn6Vb8Yc1Hd3Jf5Gs0Pa1Bc2";
    json r = baseRecipe("leaky");
    r["script"] = "#!/bin/bash\ncurl -H \"Authorization: token " + secret + "\" https://example.invalid";
    auto c = call("recipe_create", r);
    ASSERT_TRUE(isError(c));
    const std::string err = c["error"];
    EXPECT_NE(err.find("script line 2"), std::string::npos) << err;
    EXPECT_NE(err.find("secrets file"), std::string::npos) << err;
    EXPECT_EQ(err.find(secret), std::string::npos) << "the value must never be echoed";
    EXPECT_EQ(err.find("Zq7Rk2"), std::string::npos);
    EXPECT_TRUE(isError(call("recipe_get", {{"name", "leaky"}})));

    // Prereqs and params are guarded too, on create and update.
    ASSERT_FALSE(isError(call("recipe_create", baseRecipe("clean"))));
    auto u = call("recipe_update", {{"name", "clean"},
                                    {"prereqs", {"ok", "export DB_PASSWORD=Hk38fn2LxQ0pz7Wm4Rt9"}}});
    ASSERT_TRUE(isError(u));
    EXPECT_NE(u["error"].get<std::string>().find("prereqs[1] line 1"), std::string::npos) << u.dump();
    EXPECT_EQ(u["error"].get<std::string>().find("Hk38fn2LxQ0pz7Wm4Rt9"), std::string::npos);

    auto p = call("recipe_update", {{"name", "clean"},
                                    {"params", {{{"name", "key"}, {"example", std::string("sk-ant-") + "api03-AbCdEfGhIjKlMnOpQrStUv"}}}}});
    ASSERT_TRUE(isError(p));
    EXPECT_NE(p["error"].get<std::string>().find("params[0].example"), std::string::npos) << p.dump();

    // The refused update changed nothing.
    EXPECT_EQ(call("recipe_get", {{"name", "clean"}})["prereqs"], baseRecipe("clean")["prereqs"]);

    // References pass.
    auto ok = call("recipe_update", {{"name", "clean"},
                                     {"script", "source ~/.secrets\ncurl -H \"Authorization: Bearer $SERVICE_TOKEN\" "
                                                "-d api_key=${API_KEY} -d token={{token}}"}});
    EXPECT_FALSE(isError(ok)) << ok.dump();
}

// === tools/list ===

TEST_P(RecipeTest, ToolsListCarriesRecipeToolsAndRules) {
    json req = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}};
    auto tools = server_->handleRequest(req)["result"]["tools"];
    EXPECT_EQ(tools.size(), 10u);
    std::map<std::string, std::string> desc;
    for (auto& t : tools) desc[t["name"]] = t["description"];
    for (const char* n : {"mem_store", "mem_search", "mem_get", "mem_delete", "mem_list",
                          "recipe_create", "recipe_update", "recipe_delete", "recipe_get", "recipe_search"})
        EXPECT_TRUE(desc.count(n)) << n;
    const std::string& c = desc["recipe_create"];
    EXPECT_NE(c.find("secret"), std::string::npos);
    EXPECT_NE(c.find("mutates:true"), std::string::npos);
    EXPECT_NE(c.find("recipe_search"), std::string::npos);
    EXPECT_NE(c.find("fixed in place"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(Providers, RecipeTest,
                         ::testing::Values(Provider::Embedded, Provider::Redis), providerName);
INSTANTIATE_TEST_SUITE_P(Providers, RecipeSearchTest,
                         ::testing::Values(Provider::Embedded, Provider::Redis), providerName);

// === D3 secret guard, directly ===

TEST(RecipeSecretGuard, RefusesWellKnownKeys) {
    const std::vector<std::string> secrets = {
        std::string("AK") + "IA" + "QWERTYUIOPASDFGH",
        std::string("AS") + "IA" + "ZXCVBNMLKJHGFDSA",
        std::string("gh") + "o_" + "a1B2c3D4e5F6g7H8i9J0k1L2",
        std::string("gh") + "s_" + "a1B2c3D4e5F6g7H8i9J0k1L2",
        std::string("github") + "_pat_" + "11ABCDEFG0123456789_abcdefghij",
        std::string("sk") + "_live_" + "a1B2c3D4e5F6g7H8",
        std::string("rk") + "_live_" + "a1B2c3D4e5F6g7H8",
        std::string("xo") + "xb-" + "1234567890-abcdefghij",
        std::string("xo") + "xp-" + "1234567890-abcdefghij",
        std::string("sk-") + "proj-" + "a1B2c3D4e5F6g7H8i9J0k1L2",
        std::string("sk-") + "a1B2c3D4e5F6g7H8i9J0k1L2m3",
        std::string("sk-ant-") + "api03-a1B2c3D4e5F6g7H8i9J0",
        std::string("AI") + "za" + "SyA1b2C3d4E5f6G7h8I9j0K1l2M3n4O5p6Q",
        std::string("-----BEGIN ") + "RSA PRIVATE KEY-----",
        std::string("-----BEGIN ") + "OPENSSH PRIVATE KEY-----",
        std::string("-----BEGIN ") + "PRIVATE KEY-----",
    };
    for (const auto& s : secrets) {
        auto r = RecipeTools::findSecret("script", "echo start\nX=" + s + "\n");
        ASSERT_TRUE(r.has_value()) << "not caught: " << s;
        EXPECT_NE(r->find("script line 2"), std::string::npos) << *r;
        EXPECT_EQ(r->find(s), std::string::npos) << "echoed: " << s;
    }
}

TEST(RecipeSecretGuard, RefusesLongLiteralAssignedToSecretName) {
    const std::string v = "Hk38fn2LxQ0pz7Wm4Rt9";
    for (const std::string& line : {
             "DB_PASSWORD=" + v,
             "export API_KEY=\"" + v + "\"",
             "client_secret: " + v,
             "\"accessToken\": \"" + v + "\"",
             "auth_token = '" + v + "'",
             "mysql --password=" + v,
             "PRIVATEKEY=" + v + "==",
         }) {
        auto r = RecipeTools::findSecret("prereqs[0]", line);
        ASSERT_TRUE(r.has_value()) << "not caught: " << line;
        EXPECT_NE(r->find("prereqs[0] line 1"), std::string::npos);
        EXPECT_EQ(r->find(v), std::string::npos);
    }
}

TEST(RecipeSecretGuard, PassesReferencesAndOrdinaryCode) {
    for (const std::string& line : {
             std::string("curl -H \"Authorization: Bearer $SERVICE_TOKEN\" https://example.invalid"),
             std::string("export API_KEY=${API_KEY}"),
             std::string("TOKEN=$(cat ~/.secrets/service_token)"),
             std::string("password={{db_password}}"),
             std::string("secret_file=/Users/someone/.secrets/service.env"),
             std::string("token_url=https://auth.example.invalid/oauth2/token/v1"),
             std::string("api_key = os.environ[\"API_KEY\"]"),
             std::string("SET key = 'short1'"),
             std::string("primary_key: user_settings_identifier_name"),
             std::string("echo task-runner-for-the-whole-nightly-batch"),
             std::string("SELECT count(*) FROM nights WHERE user_id = {{user_id}};"),
             std::string("ghp_ is the prefix GitHub uses"),
             std::string("-----BEGIN PUBLIC KEY-----"),
         }) {
        auto r = RecipeTools::findSecret("script", line);
        EXPECT_FALSE(r.has_value()) << "false positive: " << line << " -> " << *r;
    }
}

// === embedded store file format ===

TEST(RecipeEmbeddedFile, RecipesSurviveReloadAndMemoryFileStaysV1) {
    const fs::path dir = fs::temp_directory_path() / ("hms-recipes-file-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string mem_path = (dir / "default.db").string();
    const std::string rec_path = EmbeddedStore::recipesPathBeside("default", mem_path);
    EXPECT_EQ(fs::path(rec_path), dir / "recipes.db");

    FakeEmbedder emb;
    {
        EmbeddedStore mem("default", mem_path);
        EmbeddedStore rec("recipes", rec_path);
        MemoryTools mt(mem, emb, 0.01);
        RecipeTools rt(rec, emb);
        ASSERT_EQ(mt.store("a memory", "value", "test")["status"], "stored");
        ASSERT_FALSE(rt.create(baseRecipe("persisted")).contains("error"));
        mem.flushNow();
        rec.flushNow();
    }
    auto version = [](const std::string& p) {
        std::ifstream f(p, std::ios::binary);
        char hdr[8] = {};
        f.read(hdr, 8);
        return static_cast<int>(static_cast<unsigned char>(hdr[4]));
    };
    EXPECT_EQ(version(mem_path), 1);
    EXPECT_EQ(version(rec_path), 2);

    EmbeddedStore rec("recipes", rec_path);
    RecipeTools rt(rec, emb);
    auto g = rt.get({{"name", "persisted"}});
    ASSERT_FALSE(g.contains("error")) << g.dump();
    EXPECT_EQ(g["script"], baseRecipe("persisted")["script"]);
    EXPECT_EQ(g["params"], baseRecipe("persisted")["params"]);
    EXPECT_EQ(rt.search({{"query", "count rows"}})["count"], 1);
    fs::remove_all(dir);
}
