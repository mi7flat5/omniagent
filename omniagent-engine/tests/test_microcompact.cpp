#include <gtest/gtest.h>
#include "core/microcompact.h"

using namespace omni::engine;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Message make_tool_result_msg(const std::string& content) {
    Message msg;
    msg.role = Role::ToolResult;
    ToolResult tr;
    tr.tool_use_id = "tr-1";
    tr.content     = content;
    msg.tool_results.push_back(tr);
    return msg;
}

static Message make_user_msg(const std::string& text) {
    Message msg;
    msg.role = Role::User;
    msg.content.push_back(ContentBlock{TextContent{text}});
    return msg;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(Microcompact, TruncatesOldToolResults) {
    // Build a 5-message history: 1 old tool result + 4 preserved tail messages
    std::vector<Message> msgs;
    msgs.push_back(make_tool_result_msg(std::string(10000, 'x')));  // old, should truncate
    for (int i = 0; i < 4; ++i) {
        msgs.push_back(make_user_msg("tail message " + std::to_string(i)));
    }

    auto result = microcompact(msgs, /*preserve_tail=*/4, /*max_result_chars=*/500);

    ASSERT_EQ(result.size(), 5u);

    // First message should be truncated
    const ToolResult& tr = result[0].tool_results[0];
    EXPECT_LE(static_cast<int>(tr.content.size()), 500 + 12);  // 12 = len("\n[truncated]")
    EXPECT_TRUE(tr.content.find("[truncated]") != std::string::npos);

    // Tail messages are untouched
    for (size_t i = 1; i < result.size(); ++i) {
        EXPECT_EQ(result[i].role, Role::User);
    }
}

TEST(Microcompact, PreservesTailMessages) {
    // 5 user messages, preserve_tail=3 → last 3 untouched, first 2 also untouched
    // (they're not ToolResult so nothing to truncate, but they must all be present)
    std::vector<Message> msgs;
    for (int i = 0; i < 5; ++i) {
        msgs.push_back(make_user_msg("msg " + std::to_string(i)));
    }

    auto result = microcompact(msgs, /*preserve_tail=*/3);

    ASSERT_EQ(result.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(result[i].role, Role::User);
    }
}

TEST(Microcompact, EmptyMessages) {
    std::vector<Message> empty;
    auto result = microcompact(empty);
    EXPECT_TRUE(result.empty());
}

TEST(Microcompact, ShortToolResultNotTruncated) {
    std::vector<Message> msgs;
    msgs.push_back(make_tool_result_msg("short result"));
    msgs.push_back(make_user_msg("tail"));
    msgs.push_back(make_user_msg("tail"));
    msgs.push_back(make_user_msg("tail"));
    msgs.push_back(make_user_msg("tail"));

    auto result = microcompact(msgs, /*preserve_tail=*/4, /*max_result_chars=*/500);

    EXPECT_EQ(result[0].tool_results[0].content, "short result");
}

TEST(Microcompact, TailToolResultNotTruncated) {
    // If the long tool result is inside the preserved tail, it should NOT be truncated
    std::vector<Message> msgs;
    for (int i = 0; i < 3; ++i) {
        msgs.push_back(make_user_msg("old msg " + std::to_string(i)));
    }
    msgs.push_back(make_tool_result_msg(std::string(10000, 'y')));  // in tail, preserve

    auto result = microcompact(msgs, /*preserve_tail=*/4, /*max_result_chars=*/500);

    // The tool result is in the last 4 messages → not truncated
    EXPECT_EQ(result[3].tool_results[0].content.size(), 10000u);
}
