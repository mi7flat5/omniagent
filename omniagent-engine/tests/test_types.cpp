#include <gtest/gtest.h>
#include <omni/types.h>

using namespace omni::engine;

// ---------------------------------------------------------------------------
// MessageRoundTrip — user message with text content
// ---------------------------------------------------------------------------
TEST(Types, MessageRoundTrip) {
    Message msg;
    msg.role = Role::User;
    msg.content.push_back(ContentBlock{TextContent{"Hello, world!"}});

    const nlohmann::json j = msg.to_json();
    EXPECT_EQ(j["role"], "user");
    ASSERT_TRUE(j["content"].is_array());
    ASSERT_EQ(j["content"].size(), 1u);
    EXPECT_EQ(j["content"][0]["type"], "text");
    EXPECT_EQ(j["content"][0]["text"], "Hello, world!");

    const Message roundtripped = Message::from_json(j);
    EXPECT_EQ(roundtripped.role, Role::User);
    ASSERT_EQ(roundtripped.content.size(), 1u);
    const auto& cb = std::get<TextContent>(roundtripped.content[0].data);
    EXPECT_EQ(cb.text, "Hello, world!");
}

// ---------------------------------------------------------------------------
// ToolUseContentBlock
// ---------------------------------------------------------------------------
TEST(Types, ToolUseContentBlock) {
    Message msg;
    msg.role = Role::Assistant;
    ToolUseContent tu;
    tu.id    = "call_abc";
    tu.name  = "read_file";
    tu.input = {{"path", "/tmp/foo.txt"}};
    msg.content.push_back(ContentBlock{std::move(tu)});

    const nlohmann::json j = msg.to_json();
    ASSERT_EQ(j["content"].size(), 1u);
    const auto& block = j["content"][0];
    EXPECT_EQ(block["type"],  "tool_use");
    EXPECT_EQ(block["id"],    "call_abc");
    EXPECT_EQ(block["name"],  "read_file");
    EXPECT_EQ(block["input"]["path"], "/tmp/foo.txt");

    const Message rt = Message::from_json(j);
    const auto& tu2 = std::get<ToolUseContent>(rt.content[0].data);
    EXPECT_EQ(tu2.id,   "call_abc");
    EXPECT_EQ(tu2.name, "read_file");
    EXPECT_EQ(tu2.input["path"], "/tmp/foo.txt");
}

// ---------------------------------------------------------------------------
// ToolResultContentBlock — role=ToolResult with tool_results array
// ---------------------------------------------------------------------------
TEST(Types, ToolResultMessage) {
    Message msg;
    msg.role = Role::ToolResult;

    ToolResult tr;
    tr.tool_use_id = "call_abc";
    tr.content     = "file contents here";
    tr.is_error    = false;
    msg.tool_results.push_back(std::move(tr));

    const nlohmann::json j = msg.to_json();
    EXPECT_EQ(j["role"], "tool");
    ASSERT_TRUE(j["tool_results"].is_array());
    ASSERT_EQ(j["tool_results"].size(), 1u);
    EXPECT_EQ(j["tool_results"][0]["tool_use_id"], "call_abc");
    EXPECT_EQ(j["tool_results"][0]["content"],     "file contents here");
    EXPECT_EQ(j["tool_results"][0]["is_error"],    false);

    const Message rt = Message::from_json(j);
    EXPECT_EQ(rt.role, Role::ToolResult);
    ASSERT_EQ(rt.tool_results.size(), 1u);
    EXPECT_EQ(rt.tool_results[0].tool_use_id, "call_abc");
    EXPECT_EQ(rt.tool_results[0].content,     "file contents here");
    EXPECT_EQ(rt.tool_results[0].is_error,    false);
}

// ---------------------------------------------------------------------------
// UsageAccumulation
// ---------------------------------------------------------------------------
TEST(Types, UsageAccumulation) {
    Usage a{100, 200, 50};
    Usage b{10,  20,  5};

    Usage sum = a + b;
    EXPECT_EQ(sum.input_tokens,      110);
    EXPECT_EQ(sum.output_tokens,     220);
    EXPECT_EQ(sum.cache_read_tokens,  55);

    a += b;
    EXPECT_EQ(a.input_tokens,      110);
    EXPECT_EQ(a.output_tokens,     220);
    EXPECT_EQ(a.cache_read_tokens,  55);
}

// ---------------------------------------------------------------------------
// ModelCapabilitiesDefaults
// ---------------------------------------------------------------------------
TEST(Types, ModelCapabilitiesDefaults) {
    ModelCapabilities caps;
    EXPECT_EQ(caps.max_context_tokens,     0);
    EXPECT_EQ(caps.max_output_tokens,      0);
    EXPECT_EQ(caps.supports_tool_use,      false);
    EXPECT_EQ(caps.supports_thinking,      false);
    EXPECT_EQ(caps.supports_images,        false);
    EXPECT_EQ(caps.supports_cache_control, false);
}

// ---------------------------------------------------------------------------
// AssistantMessageWithToolUse — text block + tool_use block
// ---------------------------------------------------------------------------
TEST(Types, AssistantMessageWithToolUse) {
    Message msg;
    msg.role = Role::Assistant;
    msg.content.push_back(ContentBlock{TextContent{"I will read the file."}});

    ToolUseContent tu;
    tu.id    = "call_xyz";
    tu.name  = "bash";
    tu.input = {{"command", "ls /"}};
    msg.content.push_back(ContentBlock{std::move(tu)});

    const nlohmann::json j = msg.to_json();
    EXPECT_EQ(j["role"], "assistant");
    ASSERT_EQ(j["content"].size(), 2u);
    EXPECT_EQ(j["content"][0]["type"], "text");
    EXPECT_EQ(j["content"][1]["type"], "tool_use");
    EXPECT_EQ(j["content"][1]["name"], "bash");

    const Message rt = Message::from_json(j);
    ASSERT_EQ(rt.content.size(), 2u);
    const auto& text = std::get<TextContent>(rt.content[0].data);
    EXPECT_EQ(text.text, "I will read the file.");
    const auto& tool = std::get<ToolUseContent>(rt.content[1].data);
    EXPECT_EQ(tool.name, "bash");
    EXPECT_EQ(tool.input["command"], "ls /");
}
