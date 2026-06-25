// Local (llama.cpp) embedder tests. Skipped entirely in lean builds and when
// the GGUF model isn't present, so CI without the model still passes.
#ifdef HMS_WITH_LOCAL_EMBED

#include <gtest/gtest.h>
#include "local_embedder.h"

#include <cmath>
#include <memory>

namespace {
// Returns a loaded embedder, or nullptr (with a GTEST_SKIP reason) if the
// model can't be loaded in this environment.
std::unique_ptr<LocalEmbedder> tryLoad(std::string& skip_reason) {
    auto e = std::make_unique<LocalEmbedder>(); // LOCAL_EMBED_MODEL or default paths
    try {
        e->embed("warmup");
    } catch (const std::exception& ex) {
        skip_reason = ex.what();
        return nullptr;
    }
    return e;
}
} // namespace

TEST(LocalEmbedder, ProducesNormalized768Dim) {
    std::string skip;
    auto e = tryLoad(skip);
    if (!e) GTEST_SKIP() << "model unavailable: " << skip;

    auto v = e->embed("the cat sat on the mat");
    EXPECT_EQ(v.size(), 768u);

    double norm = 0;
    for (float x : v) norm += (double)x * x;
    norm = std::sqrt(norm);
    EXPECT_NEAR(norm, 1.0, 1e-3); // L2-normalized to match Ollama
}

TEST(LocalEmbedder, IsDeterministic) {
    std::string skip;
    auto e = tryLoad(skip);
    if (!e) GTEST_SKIP() << "model unavailable: " << skip;

    auto a = e->embed("deterministic check");
    auto b = e->embed("deterministic check");
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) EXPECT_FLOAT_EQ(a[i], b[i]);
}

TEST(LocalEmbedder, DistinctTextsDiffer) {
    std::string skip;
    auto e = tryLoad(skip);
    if (!e) GTEST_SKIP() << "model unavailable: " << skip;

    auto a = e->embed("apples and oranges");
    auto b = e->embed("quantum chromodynamics");
    double dot = 0;
    for (size_t i = 0; i < a.size(); ++i) dot += (double)a[i] * b[i];
    EXPECT_LT(dot, 0.99); // unrelated texts shouldn't be near-identical
}

#endif // HMS_WITH_LOCAL_EMBED
