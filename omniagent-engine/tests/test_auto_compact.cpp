#include <gtest/gtest.h>

#include <omni/provider.h>
#include <omni/types.h>
#include "core/auto_compact.h"

#include <atomic>
#include <string>
#include <vector>

using namespace omni::engine;

// ---------------------------------------------------------------------------
// Helper — build a simple text message
// ---------------------------------------------------------------------------

static Message make_text_message(Role role, const std::string& text) {
    Message m;
    m.role    = role;
    m.content = {ContentBlock{TextContent{text}}};
    return m;
}

// ---------------------------------------------------------------------------
// MockCompactionProvider — returns a fixed canned summary
// ---------------------------------------------------------------------------

class ACMockProvider : public LLMProvider {
public:
    std::string canned_summary = "Canned summary.";
    int         call_count     = 0;

    Usage complete(const CompletionRequest& /*req*/,
                   StreamCallback cb,
                   std::atomic<bool>& /*stop*/) override
    {
        ++call_count;

        { StreamEventData ev; ev.type = StreamEventType::MessageStart; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::ContentBlockStart;
          ev.index = 0; ev.delta_type = "text"; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::ContentBlockDelta;
          ev.index = 0; ev.delta_type = "text_delta";
          ev.delta_text = canned_summary; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::ContentBlockStop;
          ev.index = 0; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::MessageDelta;
          ev.stop_reason = "end_turn"; ev.usage = {5, 5, 0}; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::MessageStop; cb(ev); }

        return {5, 5, 0};
    }

    ModelCapabilities capabilities() const override { return {}; }
    std::string       name()         const override { return "mock-compact"; }
};

// ---------------------------------------------------------------------------
// estimate_tokens tests
// ---------------------------------------------------------------------------

TEST(AutoCompact, EstimateTokensEmpty) {
    EXPECT_EQ(estimate_tokens({}), 0);
}

TEST(AutoCompact, EstimateTokensRoughApprox) {
    // 400 chars of text → ~100 tokens
    std::string text(400, 'a');
    std::vector<Message> msgs = {make_text_message(Role::User, text)};
    EXPECT_EQ(estimate_tokens(msgs), 100);
}

TEST(AutoCompact, EstimateTokensMultipleMessages) {
    // 40 chars each, 4 messages → 160 chars → 40 tokens
    std::string text(40, 'x');
    std::vector<Message> msgs = {
        make_text_message(Role::User,      text),
        make_text_message(Role::Assistant, text),
        make_text_message(Role::User,      text),
        make_text_message(Role::Assistant, text),
    };
    EXPECT_EQ(estimate_tokens(msgs), 40);
}

TEST(AutoCompact, EstimateTokensIncludesToolResults) {
    Message tr_msg;
    tr_msg.role = Role::ToolResult;
    // "id-1" = 4 chars, content = 200 chars → 204 chars / 4 = 51 tokens
    tr_msg.tool_results = {ToolResult{"id-1", std::string(200, 'r'), false, {}}};

    EXPECT_EQ(estimate_tokens({tr_msg}), 51);
}

// ---------------------------------------------------------------------------
// auto_compact — below threshold → no compaction
// ---------------------------------------------------------------------------

TEST(AutoCompact, BelowThresholdNoCompaction) {
    ACMockProvider provider;

    // 4 short messages, each ~10 chars → ~10 tokens total
    std::vector<Message> msgs = {
        make_text_message(Role::User,      "Hello"),
        make_text_message(Role::Assistant, "Hi there"),
        make_text_message(Role::User,      "How are you?"),
        make_text_message(Role::Assistant, "Fine thanks"),
    };

    // max_context_tokens = 10000; threshold = 75% = 7500 → well above ~10 estimated
    CompactResult result = auto_compact(msgs, provider, 10000);

    EXPECT_FALSE(result.compacted);
    EXPECT_EQ(result.messages.size(), msgs.size());
    EXPECT_EQ(provider.call_count, 0);
}

// ---------------------------------------------------------------------------
// auto_compact — above threshold → compaction happens
// ---------------------------------------------------------------------------

TEST(AutoCompact, AboveThresholdCompacts) {
    ACMockProvider provider;
    provider.canned_summary = "Summary of the conversation.";

    // Create messages with enough text to exceed threshold.
    // Each message has 100 chars → 25 tokens. 10 messages → 250 tokens.
    // max_context=200, threshold=75%=150 → 250 > 150 → compact.
    const std::string long_text(100, 'z');
    std::vector<Message> msgs;
    for (int i = 0; i < 10; ++i) {
        msgs.push_back(make_text_message(
            (i % 2 == 0) ? Role::User : Role::Assistant, long_text));
    }

    CompactConfig cfg;
    cfg.soft_limit_pct = 0.75f;
    cfg.preserve_tail  = 3;

    CompactResult result = auto_compact(msgs, provider, 200, cfg);

    EXPECT_TRUE(result.compacted);
    EXPECT_EQ(provider.call_count, 1);
    EXPECT_EQ(result.summary, "Summary of the conversation.");

    // Compacted: 1 system summary + preserve_tail (3) = 4 messages
    ASSERT_EQ(result.messages.size(), 4u);
    EXPECT_EQ(result.messages[0].role, Role::System);

    // System message should contain the summary text
    ASSERT_FALSE(result.messages[0].content.empty());
    const auto* tc = std::get_if<TextContent>(&result.messages[0].content[0].data);
    ASSERT_NE(tc, nullptr);
    EXPECT_NE(tc->text.find("Summary of the conversation."), std::string::npos);
    EXPECT_NE(tc->text.find("[Context Summary]"), std::string::npos);

    // Preserved tail should be the last 3 original messages
    for (int i = 1; i < 4; ++i) {
        EXPECT_EQ(result.messages[i].role, msgs[msgs.size() - 3 + (i - 1)].role);
    }
}

// ---------------------------------------------------------------------------
// auto_compact — no context limit → no compaction
// ---------------------------------------------------------------------------

TEST(AutoCompact, ZeroMaxContextNoCompaction) {
    ACMockProvider provider;

    std::vector<Message> msgs = {make_text_message(Role::User, "test")};
    CompactResult result = auto_compact(msgs, provider, 0);

    EXPECT_FALSE(result.compacted);
    EXPECT_EQ(provider.call_count, 0);
}

// ---------------------------------------------------------------------------
// auto_compact — preserve_tail >= messages.size() → no compaction (nothing to summarize)
// ---------------------------------------------------------------------------

TEST(AutoCompact, PreserveTailLargerThanMessages) {
    ACMockProvider provider;

    std::vector<Message> msgs = {
        make_text_message(Role::User,      "hi"),
        make_text_message(Role::Assistant, "hello"),
    };

    CompactConfig cfg;
    cfg.soft_limit_pct = 0.0f;  // force threshold = 0 (always above)
    cfg.preserve_tail  = 10;    // larger than msgs.size()

    CompactResult result = auto_compact(msgs, provider, 100, cfg);

    EXPECT_FALSE(result.compacted);
    EXPECT_EQ(provider.call_count, 0);
}

// ---------------------------------------------------------------------------
// reactive_compact — always compacts regardless of size
// ---------------------------------------------------------------------------

TEST(AutoCompact, ReactiveCompactAlwaysCompacts) {
    ACMockProvider provider;
    provider.canned_summary = "Emergency summary.";

    // Small messages that wouldn't normally trigger auto-compact
    std::vector<Message> msgs = {
        make_text_message(Role::User,      "a"),
        make_text_message(Role::Assistant, "b"),
        make_text_message(Role::User,      "c"),
        make_text_message(Role::Assistant, "d"),
        make_text_message(Role::User,      "e"),
    };

    // max_context_tokens: even a non-zero value, reactive should always compact
    CompactResult result = reactive_compact(msgs, provider, 100000);

    EXPECT_TRUE(result.compacted);
    EXPECT_EQ(provider.call_count, 1);

    // reactive uses preserve_tail=2 → 1 summary + 2 preserved
    ASSERT_EQ(result.messages.size(), 3u);
    EXPECT_EQ(result.messages[0].role, Role::System);
}

// ---------------------------------------------------------------------------
// reactive_compact — preserve_tail=2 keeps exactly last 2
// ---------------------------------------------------------------------------

TEST(AutoCompact, ReactiveCompactPreservesTwoMessages) {
    ACMockProvider provider;

    const std::string text(50, 'q');
    std::vector<Message> msgs = {
        make_text_message(Role::User,      text),
        make_text_message(Role::Assistant, text),
        make_text_message(Role::User,      "penultimate"),
        make_text_message(Role::Assistant, "last"),
    };

    CompactResult result = reactive_compact(msgs, provider, 100000);

    ASSERT_EQ(result.messages.size(), 3u);
    EXPECT_EQ(result.messages[0].role, Role::System);

    // Last two original messages preserved
    const auto* tc2 = std::get_if<TextContent>(&result.messages[1].content[0].data);
    ASSERT_NE(tc2, nullptr);
    EXPECT_EQ(tc2->text, "penultimate");

    const auto* tc3 = std::get_if<TextContent>(&result.messages[2].content[0].data);
    ASSERT_NE(tc3, nullptr);
    EXPECT_EQ(tc3->text, "last");
}
