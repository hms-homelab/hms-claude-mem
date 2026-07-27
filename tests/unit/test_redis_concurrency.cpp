// Concurrency cover for SDD-003 stage 2.
//
// hiredis redisContext is not thread safe and RedisClient holds exactly one.
// Once the HTTP daemon serves several clients at once, every public entry point
// can be reached concurrently, so these tests hammer a single shared client from
// many threads and assert nothing is lost, corrupted, or crashed.
//
// Point at a reachable Redis, same as the rest of the suite:
//   REDIS_HOST=192.168.2.15 ./run_tests
// Uses the isolated "concurrency" namespace and removes every key it creates.

#include <gtest/gtest.h>
#include "redis_client.h"
#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string envOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

// Deterministic stand-in for a real embedding: unique per key, correct width.
std::vector<float> fakeEmbedding(int seed, size_t dim = 768) {
    std::vector<float> v(dim);
    for (size_t i = 0; i < dim; ++i) {
        v[i] = static_cast<float>((seed + static_cast<int>(i)) % 97) / 97.0f;
    }
    return v;
}

MemoryEntry makeEntry(const std::string& value) {
    MemoryEntry e;
    e.value = value;
    e.category = "concurrency";
    e.created_at = "2026-07-26T00:00:00Z";
    e.updated_at = "2026-07-26T00:00:00Z";
    e.pinned = false;
    return e;
}

class RedisConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        host_ = envOr("REDIS_HOST", "127.0.0.1");
        port_ = std::stoi(envOr("REDIS_PORT", "6379"));
        client_ = std::make_unique<RedisClient>(host_, port_, "concurrency");
        if (!client_->connect()) {
            GTEST_SKIP() << "Redis not available at " << host_ << ":" << port_;
        }
    }

    void TearDown() override {
        if (!client_) return;
        for (const auto& k : keys_) {
            client_->vectorRemove(k);
            client_->hashDelete(k);
        }
    }

    std::string host_;
    int port_ = 6379;
    std::unique_ptr<RedisClient> client_;
    std::vector<std::string> keys_;
};

constexpr int kThreads = 8;
constexpr int kPerThread = 25;

// Every write from every thread must land, with the right value.
TEST_F(RedisConcurrencyTest, ConcurrentHashSetLosesNothing) {
    for (int t = 0; t < kThreads; ++t)
        for (int i = 0; i < kPerThread; ++i)
            keys_.push_back("conc:set:" + std::to_string(t) + ":" + std::to_string(i));

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const std::string key = "conc:set:" + std::to_string(t) + ":" + std::to_string(i);
                if (!client_->hashSet(key, makeEntry("value-" + std::to_string(t) + "-" + std::to_string(i))))
                    failures++;
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(failures.load(), 0) << "some hashSet calls failed under contention";

    // Read back on the main thread: every key present with its own value.
    int missing = 0, wrong = 0;
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; ++i) {
            auto got = client_->hashGet("conc:set:" + std::to_string(t) + ":" + std::to_string(i));
            if (!got) { missing++; continue; }
            if (got->value != "value-" + std::to_string(t) + "-" + std::to_string(i)) wrong++;
        }
    }
    EXPECT_EQ(missing, 0) << "writes were lost under contention";
    EXPECT_EQ(wrong, 0)   << "values were interleaved between threads";
}

// Mixed read/write/search traffic must not crash or corrupt the context.
TEST_F(RedisConcurrencyTest, MixedReadWriteSearchIsStable) {
    for (int i = 0; i < kPerThread; ++i) {
        const std::string key = "conc:mixed:" + std::to_string(i);
        keys_.push_back(key);
        ASSERT_TRUE(client_->hashSet(key, makeEntry("seed-" + std::to_string(i))));
        ASSERT_TRUE(client_->vectorAdd(key, fakeEmbedding(i)));
    }

    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const std::string key = "conc:mixed:" + std::to_string(i);
                switch (t % 4) {
                    case 0: if (!client_->hashSet(key, makeEntry("rewrite"))) errors++; break;
                    case 1: if (!client_->hashGet(key).has_value())           errors++; break;
                    case 2: client_->vectorSearch(fakeEmbedding(i), 3);              break;
                    case 3: client_->scanKeys("conc:mixed:*", 100);                  break;
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(errors.load(), 0) << "mixed concurrent traffic produced failures";
    EXPECT_TRUE(client_->isConnected()) << "context died under mixed load";
}

// connect()/isConnected() are public and are also called internally by
// ensureConnected(). Racing them is what would expose a self-deadlock in the
// locking split, so this test would hang rather than fail if we got it wrong.
TEST_F(RedisConcurrencyTest, ConcurrentConnectDoesNotDeadlock) {
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 10; ++i) {
                client_->isConnected();
                if (client_->connect()) ok++;
                client_->hashGet("conc:does-not-exist");
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_GT(ok.load(), 0) << "no connect succeeded";
}

} // namespace
