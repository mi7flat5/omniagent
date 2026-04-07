#include <gtest/gtest.h>
#include "core/stream_assembler.h"

using namespace omni::engine;

// ---------------------------------------------------------------------------
// Helpers to build StreamEventData more concisely
// ---------------------------------------------------------------------------

static StreamEventData make_message_start() {
    StreamEventData e;
    e.type = StreamEventType::MessageStart;
    return e;
}

static StreamEventData make_block_start_text(int index) {
    StreamEventData e;
    e.type       = StreamEventType::ContentBlockStart;
    e.index      = index;
    e.delta_type = "text";
    return e;
}

static StreamEventData make_block_start_thinking(int index) {
    StreamEventData e;
    e.type       = StreamEventType::ContentBlockStart;
    e.index      = index;
    e.delta_type = "thinking";
    return e;
}

static StreamEventData make_block_start_tool(int index,
                                              const std::string& id,
                                              const std::string& name) {
    StreamEventData e;
    e.type       = StreamEventType::ContentBlockStart;
    e.index      = index;
    e.delta_type = "tool_use";
    e.tool_id    = id;
    e.tool_name  = name;
    return e;
}

static StreamEventData make_text_delta(int index, const std::string& text) {
    StreamEventData e;
    e.type       = StreamEventType::ContentBlockDelta;
    e.index      = index;
    e.delta_type = "text_delta";
    e.delta_text = text;
    return e;
}

static StreamEventData make_tool_input_delta(int index, const nlohmann::json& delta) {
    StreamEventData e;
    e.type             = StreamEventType::ContentBlockDelta;
    e.index            = index;
    e.delta_type       = "input_json_delta";
    e.tool_input_delta = delta;
    return e;
}

static StreamEventData make_block_stop(int index,
                                        nlohmann::json complete_input = nullptr) {
    StreamEventData e;
    e.type       = StreamEventType::ContentBlockStop;
    e.index      = index;
    e.tool_input = std::move(complete_input);
    return e;
}

static StreamEventData make_message_delta(const std::string& stop_reason,
                                           int64_t input_tokens  = 10,
                                           int64_t output_tokens = 20) {
    StreamEventData e;
    e.type        = StreamEventType::MessageDelta;
    e.stop_reason = stop_reason;
    e.usage       = {input_tokens, output_tokens, 0};
    return e;
}

static StreamEventData make_message_stop() {
    StreamEventData e;
    e.type = StreamEventType::MessageStop;
    return e;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(StreamAssembler, AssemblesTextBlock) {
    StreamAssembler sa;

    sa.process(make_message_start());
    sa.process(make_block_start_text(0));
    sa.process(make_text_delta(0, "Hello, "));
    sa.process(make_text_delta(0, "world!"));
    sa.process(make_block_stop(0));
    sa.process(make_message_stop());

    auto blocks = sa.take_completed_blocks();
    ASSERT_EQ(blocks.size(), 1u);

    const auto* tc = std::get_if<TextContent>(&blocks[0].data);
    ASSERT_NE(tc, nullptr);
    EXPECT_EQ(tc->text, "Hello, world!");
}

TEST(StreamAssembler, AssemblesToolUseBlock) {
    StreamAssembler sa;

    sa.process(make_message_start());
    sa.process(make_block_start_tool(0, "tool-abc", "calculator"));
    // Provide complete input at the stop event (typical Messages API behaviour)
    nlohmann::json complete_input = {{"expression", "2+2"}};
    sa.process(make_block_stop(0, complete_input));
    sa.process(make_message_stop());

    auto blocks = sa.take_completed_blocks();
    ASSERT_EQ(blocks.size(), 1u);

    const auto* tuc = std::get_if<ToolUseContent>(&blocks[0].data);
    ASSERT_NE(tuc, nullptr);
    EXPECT_EQ(tuc->id,   "tool-abc");
    EXPECT_EQ(tuc->name, "calculator");
    EXPECT_EQ(tuc->input["expression"].get<std::string>(), "2+2");
}

TEST(StreamAssembler, ReconstructsStreamedToolJsonFragments) {
    StreamAssembler sa;

    sa.process(make_message_start());
    sa.process(make_block_start_tool(0, "tool-frag", "read_file"));
    sa.process(make_tool_input_delta(0, "{\"path\":\"README"));
    sa.process(make_tool_input_delta(0, ".md\",\"start\":1}"));
    sa.process(make_block_stop(0));
    sa.process(make_message_stop());

    auto blocks = sa.take_completed_blocks();
    ASSERT_EQ(blocks.size(), 1u);

    const auto* tuc = std::get_if<ToolUseContent>(&blocks[0].data);
    ASSERT_NE(tuc, nullptr);
    EXPECT_EQ(tuc->id, "tool-frag");
    EXPECT_EQ(tuc->name, "read_file");
    ASSERT_TRUE(tuc->input.is_object());
    EXPECT_EQ(tuc->input["path"], "README.md");
    EXPECT_EQ(tuc->input["start"], 1);
}

TEST(StreamAssembler, MixedBlocks) {
    StreamAssembler sa;

    sa.process(make_message_start());

    // Block 0 — text
    sa.process(make_block_start_text(0));
    sa.process(make_text_delta(0, "Let me calculate that."));
    sa.process(make_block_stop(0));

    // Block 1 — tool_use
    sa.process(make_block_start_tool(1, "tool-xyz", "add"));
    nlohmann::json tool_input = {{"a", 1}, {"b", 2}};
    sa.process(make_block_stop(1, tool_input));

    sa.process(make_message_stop());

    auto blocks = sa.take_completed_blocks();
    ASSERT_EQ(blocks.size(), 2u);

    // First block: text
    const auto* tc = std::get_if<TextContent>(&blocks[0].data);
    ASSERT_NE(tc, nullptr);
    EXPECT_EQ(tc->text, "Let me calculate that.");

    // Second block: tool_use
    const auto* tuc = std::get_if<ToolUseContent>(&blocks[1].data);
    ASSERT_NE(tuc, nullptr);
    EXPECT_EQ(tuc->name, "add");
}

TEST(StreamAssembler, ExtractsStopReason) {
    StreamAssembler sa;

    sa.process(make_message_start());
    sa.process(make_block_start_text(0));
    sa.process(make_text_delta(0, "Done."));
    sa.process(make_block_stop(0));
    sa.process(make_message_delta("end_turn", 100, 42));
    sa.process(make_message_stop());

    EXPECT_EQ(sa.stop_reason(), "end_turn");
    EXPECT_EQ(sa.final_usage().input_tokens,  100);
    EXPECT_EQ(sa.final_usage().output_tokens, 42);
}

TEST(StreamAssembler, TakeCompletedBlocksClearsState) {
    StreamAssembler sa;

    sa.process(make_block_start_text(0));
    sa.process(make_text_delta(0, "Hello"));
    sa.process(make_block_stop(0));

    auto first = sa.take_completed_blocks();
    ASSERT_EQ(first.size(), 1u);

    auto second = sa.take_completed_blocks();
    EXPECT_TRUE(second.empty());
}

TEST(StreamAssembler, ResetClearsAllState) {
    StreamAssembler sa;

    sa.process(make_block_start_text(0));
    sa.process(make_text_delta(0, "Hello"));
    sa.process(make_block_stop(0));
    sa.process(make_message_delta("end_turn", 5, 5));

    sa.reset();

    EXPECT_TRUE(sa.take_completed_blocks().empty());
    EXPECT_TRUE(sa.stop_reason().empty());
    EXPECT_EQ(sa.final_usage().input_tokens, 0);
}
