#include <gtest/gtest.h>

#include "services/cost_tracker.h"

#include <thread>
#include <vector>

using namespace omni::engine;

// ---------------------------------------------------------------------------
// RecordAndSnapshot — two models, verify per-model breakdown
// ---------------------------------------------------------------------------

TEST(CostTracker, RecordAndSnapshot) {
    CostTracker tracker;

    Usage u1;
    u1.input_tokens      = 100;
    u1.output_tokens     = 50;
    u1.cache_read_tokens = 10;

    Usage u2;
    u2.input_tokens  = 200;
    u2.output_tokens = 80;

    tracker.record("model-a", u1);
    tracker.record("model-b", u2);

    CostSnapshot snap = tracker.snapshot();

    ASSERT_EQ(snap.per_model_usage.count("model-a"), 1u);
    ASSERT_EQ(snap.per_model_usage.count("model-b"), 1u);

    EXPECT_EQ(snap.per_model_usage.at("model-a").input_tokens,      100);
    EXPECT_EQ(snap.per_model_usage.at("model-a").output_tokens,     50);
    EXPECT_EQ(snap.per_model_usage.at("model-a").cache_read_tokens, 10);

    EXPECT_EQ(snap.per_model_usage.at("model-b").input_tokens,  200);
    EXPECT_EQ(snap.per_model_usage.at("model-b").output_tokens, 80);

    // Total usage
    EXPECT_EQ(snap.total_usage.input_tokens,      300);
    EXPECT_EQ(snap.total_usage.output_tokens,     130);
    EXPECT_EQ(snap.total_usage.cache_read_tokens, 10);
}

// ---------------------------------------------------------------------------
// CostCalculation — set model cost, record usage, verify USD
// ---------------------------------------------------------------------------

TEST(CostTracker, CostCalculation) {
    CostTracker tracker;

    // $1.00 / 1K input, $3.00 / 1K output, $0.50 / 1K cache
    ModelCost mc;
    mc.input_cost_per_1k  = 1.0;
    mc.output_cost_per_1k = 3.0;
    mc.cache_cost_per_1k  = 0.5;
    tracker.set_model_cost("test-model", mc);

    Usage u;
    u.input_tokens      = 2000;  // 2K → $2.00
    u.output_tokens     = 1000;  // 1K → $3.00
    u.cache_read_tokens = 4000;  // 4K → $2.00
    tracker.record("test-model", u);

    CostSnapshot snap = tracker.snapshot();

    EXPECT_DOUBLE_EQ(snap.per_model_cost.at("test-model"), 7.0);
    EXPECT_DOUBLE_EQ(snap.total_cost_usd, 7.0);
}

// ---------------------------------------------------------------------------
// LocalModelZeroCost — {0,0,0} cost: cost=0 but tokens tracked
// ---------------------------------------------------------------------------

TEST(CostTracker, LocalModelZeroCost) {
    CostTracker tracker;

    ModelCost zero{0.0, 0.0, 0.0};
    tracker.set_model_cost("ollama/llama3", zero);

    Usage u;
    u.input_tokens  = 500;
    u.output_tokens = 200;
    tracker.record("ollama/llama3", u);

    CostSnapshot snap = tracker.snapshot();

    EXPECT_EQ(snap.per_model_usage.at("ollama/llama3").input_tokens,  500);
    EXPECT_EQ(snap.per_model_usage.at("ollama/llama3").output_tokens, 200);
    EXPECT_DOUBLE_EQ(snap.per_model_cost.at("ollama/llama3"), 0.0);
    EXPECT_DOUBLE_EQ(snap.total_cost_usd, 0.0);
}

// ---------------------------------------------------------------------------
// Reset — record, reset, verify empty
// ---------------------------------------------------------------------------

TEST(CostTracker, Reset) {
    CostTracker tracker;

    Usage u;
    u.input_tokens  = 100;
    u.output_tokens = 50;
    tracker.record("model-x", u);

    tracker.reset();

    CostSnapshot snap = tracker.snapshot();

    EXPECT_TRUE(snap.per_model_usage.empty());
    EXPECT_TRUE(snap.per_model_cost.empty());
    EXPECT_EQ(snap.total_usage.input_tokens,  0);
    EXPECT_EQ(snap.total_usage.output_tokens, 0);
    EXPECT_DOUBLE_EQ(snap.total_cost_usd, 0.0);
}

// ---------------------------------------------------------------------------
// ThreadSafety — concurrent records from two threads
// ---------------------------------------------------------------------------

TEST(CostTracker, ThreadSafety) {
    CostTracker tracker;

    Usage u;
    u.input_tokens  = 10;
    u.output_tokens = 5;

    const int iterations = 500;

    auto worker = [&](const std::string& model) {
        for (int i = 0; i < iterations; ++i) {
            tracker.record(model, u);
        }
    };

    std::thread t1(worker, "model-p");
    std::thread t2(worker, "model-q");
    t1.join();
    t2.join();

    CostSnapshot snap = tracker.snapshot();

    EXPECT_EQ(snap.per_model_usage.at("model-p").input_tokens,
              static_cast<int64_t>(10 * iterations));
    EXPECT_EQ(snap.per_model_usage.at("model-q").input_tokens,
              static_cast<int64_t>(10 * iterations));
    EXPECT_EQ(snap.total_usage.input_tokens,
              static_cast<int64_t>(10 * iterations * 2));
}
