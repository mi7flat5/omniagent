#include <gtest/gtest.h>

#include "services/hooks.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace omni::engine;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// FireHook — register a hook, fire the event, verify handler was called
// ---------------------------------------------------------------------------

TEST(HookEngine, FireHook) {
    HookEngine engine;

    std::atomic<int> call_count{0};

    HookRegistration reg;
    reg.name    = "test-hook";
    reg.event   = HookEvent::PrePrompt;
    reg.handler = [&](HookEvent, const nlohmann::json&) -> HookResult {
        ++call_count;
        return {};
    };

    engine.add(std::move(reg));
    EXPECT_EQ(engine.count(), 1u);

    HookResult result = engine.fire(HookEvent::PrePrompt);
    EXPECT_FALSE(result.should_block);
    EXPECT_EQ(call_count.load(), 1);
}

// ---------------------------------------------------------------------------
// BlockingHook — hook returns should_block=true, verify combined result blocks
// ---------------------------------------------------------------------------

TEST(HookEngine, BlockingHook) {
    HookEngine engine;

    HookRegistration reg;
    reg.name    = "blocker";
    reg.event   = HookEvent::PrePrompt;
    reg.handler = [](HookEvent, const nlohmann::json&) -> HookResult {
        return {true, "not allowed"};
    };

    engine.add(std::move(reg));

    HookResult result = engine.fire(HookEvent::PrePrompt);
    EXPECT_TRUE(result.should_block);
    EXPECT_EQ(result.message, "not allowed");
}

// ---------------------------------------------------------------------------
// TimeoutHook — hook sleeps 10s, timeout=100ms → must return non-blocking
// ---------------------------------------------------------------------------

TEST(HookEngine, TimeoutHook) {
    HookEngine engine;

    HookRegistration reg;
    reg.name    = "slow-hook";
    reg.event   = HookEvent::PrePrompt;
    reg.timeout = 100ms;
    reg.handler = [](HookEvent, const nlohmann::json&) -> HookResult {
        // Sleep long enough to exceed the 100ms timeout but not so long
        // that the test runner stalls (detached thread, so it won't block us).
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        return {true, "should not reach here"};
    };

    engine.add(std::move(reg));

    const auto before = std::chrono::steady_clock::now();
    HookResult result = engine.fire(HookEvent::PrePrompt);
    const auto elapsed = std::chrono::steady_clock::now() - before;

    // Must NOT block — timed-out hook is treated as non-blocking.
    EXPECT_FALSE(result.should_block);

    // Must have returned within about 1 second (well before the 500ms sleep finishes).
    // Allow 2s as a generous bound to avoid flakiness under load.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 2000);
}

// ---------------------------------------------------------------------------
// MultipleHooks — two hooks for same event, one blocks → combined blocks
// ---------------------------------------------------------------------------

TEST(HookEngine, MultipleHooks) {
    HookEngine engine;

    std::atomic<int> call_count{0};

    HookRegistration allow_reg;
    allow_reg.name    = "allow-hook";
    allow_reg.event   = HookEvent::ToolUseStart;
    allow_reg.handler = [&](HookEvent, const nlohmann::json&) -> HookResult {
        ++call_count;
        return {};
    };

    HookRegistration block_reg;
    block_reg.name    = "block-hook";
    block_reg.event   = HookEvent::ToolUseStart;
    block_reg.handler = [&](HookEvent, const nlohmann::json&) -> HookResult {
        ++call_count;
        return {true, "blocked by policy"};
    };

    engine.add(std::move(allow_reg));
    engine.add(std::move(block_reg));

    EXPECT_EQ(engine.count(), 2u);

    HookResult result = engine.fire(HookEvent::ToolUseStart,
                                    {{"tool", "dangerous_tool"}});

    // Both hooks ran.
    EXPECT_EQ(call_count.load(), 2);
    // Combined result is blocking because one hook blocked.
    EXPECT_TRUE(result.should_block);
    EXPECT_EQ(result.message, "blocked by policy");
}

// ---------------------------------------------------------------------------
// RemoveHook — add then remove, verify handler is not called
// ---------------------------------------------------------------------------

TEST(HookEngine, RemoveHook) {
    HookEngine engine;

    std::atomic<int> call_count{0};

    HookRegistration reg;
    reg.name    = "removable";
    reg.event   = HookEvent::PostPrompt;
    reg.handler = [&](HookEvent, const nlohmann::json&) -> HookResult {
        ++call_count;
        return {};
    };

    engine.add(std::move(reg));
    EXPECT_EQ(engine.count(), 1u);

    engine.remove("removable");
    EXPECT_EQ(engine.count(), 0u);

    engine.fire(HookEvent::PostPrompt);
    EXPECT_EQ(call_count.load(), 0);
}

// ---------------------------------------------------------------------------
// EventFiltering — hooks only fire for their registered event
// ---------------------------------------------------------------------------

TEST(HookEngine, EventFiltering) {
    HookEngine engine;

    std::atomic<int> pre_count{0};
    std::atomic<int> post_count{0};

    HookRegistration pre_reg;
    pre_reg.name    = "pre";
    pre_reg.event   = HookEvent::PrePrompt;
    pre_reg.handler = [&](HookEvent, const nlohmann::json&) -> HookResult {
        ++pre_count;
        return {};
    };

    HookRegistration post_reg;
    post_reg.name    = "post";
    post_reg.event   = HookEvent::PostPrompt;
    post_reg.handler = [&](HookEvent, const nlohmann::json&) -> HookResult {
        ++post_count;
        return {};
    };

    engine.add(std::move(pre_reg));
    engine.add(std::move(post_reg));

    engine.fire(HookEvent::PrePrompt);
    EXPECT_EQ(pre_count.load(),  1);
    EXPECT_EQ(post_count.load(), 0);

    engine.fire(HookEvent::PostPrompt);
    EXPECT_EQ(pre_count.load(),  1);
    EXPECT_EQ(post_count.load(), 1);
}

// ---------------------------------------------------------------------------
// ThrowingHook — a hook that throws must not crash the engine
// ---------------------------------------------------------------------------

TEST(HookEngine, ThrowingHook) {
    HookEngine engine;

    HookRegistration reg;
    reg.name    = "thrower";
    reg.event   = HookEvent::PrePrompt;
    reg.handler = [](HookEvent, const nlohmann::json&) -> HookResult {
        throw std::runtime_error("hook crashed");
    };

    engine.add(std::move(reg));

    // Must not throw; result should be non-blocking (exception → treated as non-blocking).
    HookResult result;
    EXPECT_NO_THROW(result = engine.fire(HookEvent::PrePrompt));
    EXPECT_FALSE(result.should_block);
}
