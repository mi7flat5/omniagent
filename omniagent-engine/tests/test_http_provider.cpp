#include <gtest/gtest.h>

#define private public
#include <omni/provider.h>
#include "core/stream_assembler.h"
#include "providers/http_provider.h"
#undef private

using namespace omni::engine;

TEST(HttpProvider, SendsToolChoiceAndAssemblesStreamedToolCalls) {
    HttpProviderConfig config;
    config.base_url = "http://127.0.0.1:11434/v1";
    config.model = "test-model";

    HttpProvider provider(config);

    CompletionRequest request;
    request.system_prompt = "Use tools.";
    request.tool_choice = "required";
    request.tools = {
        {
            {"type", "function"},
            {"function", {
                {"name", "read_file"},
                {"description", "Read a file"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"path", {{"type", "string"}}},
                        {"start", {{"type", "integer"}}}
                    }},
                    {"required", nlohmann::json::array({"path"})}
                }}
            }}
        }
    };

    Message user_message;
    user_message.role = Role::User;
    user_message.content = {ContentBlock{TextContent{"Inspect the workspace."}}};
    request.messages = {user_message};

    const nlohmann::json body = provider.build_request_body(request);
    EXPECT_EQ(body["tool_choice"], "required");
    ASSERT_TRUE(body.contains("tools"));

    StreamAssembler assembler;
    StreamEventData message_start;
    message_start.type = StreamEventType::MessageStart;
    assembler.process(message_start);

    bool thinking_started = false;
    bool thinking_open = false;
    bool text_started = false;
    std::unordered_set<int> open_tool_blocks;
    Usage usage;

    const std::string chunk_one =
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"README"}}]}}]})";
    const std::string chunk_two =
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":".md\",\"start\":1}"}}]},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":11,"completion_tokens":7}})";

    StreamCallback stream_cb = [&](const StreamEventData& event) {
        assembler.process(event);
    };

    EXPECT_TRUE(provider.parse_sse_line(chunk_one, stream_cb, thinking_started, thinking_open,
                                        text_started, open_tool_blocks, usage));
    EXPECT_FALSE(provider.parse_sse_line(chunk_two, stream_cb, thinking_started, thinking_open,
                                         text_started, open_tool_blocks, usage));

    EXPECT_EQ(usage.input_tokens, 11);
    EXPECT_EQ(usage.output_tokens, 7);
    EXPECT_EQ(assembler.stop_reason(), "tool_use");

    auto blocks = assembler.take_completed_blocks();
    ASSERT_EQ(blocks.size(), 1u);
    const auto* tool = std::get_if<ToolUseContent>(&blocks[0].data);
    ASSERT_NE(tool, nullptr);
    EXPECT_EQ(tool->id, "call_1");
    EXPECT_EQ(tool->name, "read_file");
    ASSERT_TRUE(tool->input.is_object());
    EXPECT_EQ(tool->input["path"], "README.md");
    EXPECT_EQ(tool->input["start"], 1);
}