#include <gtest/gtest.h>

#include <omni/event.h>
#include <omni/permission.h>
#include <omni/provider.h>
#include <omni/tool.h>
#include <omni/types.h>
#include "core/query_engine.h"
#include "permissions/permission_checker.h"
#include "tools/tool_registry.h"

#include <atomic>
#include <string>
#include <vector>

using namespace omni::engine;

// ---------------------------------------------------------------------------
// Test doubles — all prefixed QE to avoid ODR collisions with other test TUs
// ---------------------------------------------------------------------------

class QEAllowAllDelegate : public PermissionDelegate {
public:
    PermissionDecision on_permission_request(const std::string&,
                                              const nlohmann::json&,
                                              const std::string&) override {
        return PermissionDecision::Allow;
    }
};

class QECollectingObserver : public EventObserver {
public:
    std::vector<Event> events;

    void on_event(const Event& e) override {
        events.push_back(e);
    }

    std::string all_text() const {
        std::string out;
        for (const auto& e : events) {
            if (const auto* td = std::get_if<TextDeltaEvent>(&e)) {
                out += td->text;
            }
        }
        return out;
    }

    int count_tool_use_start() const {
        int n = 0;
        for (const auto& e : events) {
            if (std::holds_alternative<ToolUseStartEvent>(e)) ++n;
        }
        return n;
    }

    int count_done() const {
        int n = 0;
        for (const auto& e : events) {
            if (std::holds_alternative<DoneEvent>(e)) ++n;
        }
        return n;
    }
};

// ---------------------------------------------------------------------------
// MockProvider
// ---------------------------------------------------------------------------

struct QECannedResponse {
    std::string text;
    std::vector<ToolUseContent> tool_calls;
    Usage usage{1, 1, 0};
};

class QEMockProvider : public LLMProvider {
public:
    struct RequestSnapshot {
        size_t tool_count = 0;
        std::string system_prompt;
    };

    std::vector<QECannedResponse> responses;
    std::vector<RequestSnapshot>   requests;
    size_t                        call_count = 0;

    Usage complete(const CompletionRequest& request,
                   StreamCallback cb,
                   std::atomic<bool>& stop_flag) override
    {
        if (stop_flag.load()) return {};

        requests.push_back(RequestSnapshot{request.tools.size(), request.system_prompt});

        const QECannedResponse& resp =
            (call_count < responses.size())
                ? responses[call_count]
                : responses.back();
        ++call_count;

        // MessageStart
        { StreamEventData ev; ev.type = StreamEventType::MessageStart; cb(ev); }

        // Text block
        { StreamEventData ev; ev.type = StreamEventType::ContentBlockStart; ev.index = 0; ev.delta_type = "text"; cb(ev); }
        if (!resp.text.empty()) {
            StreamEventData ev; ev.type = StreamEventType::ContentBlockDelta;
            ev.index = 0; ev.delta_type = "text_delta"; ev.delta_text = resp.text; cb(ev);
        }
        { StreamEventData ev; ev.type = StreamEventType::ContentBlockStop; ev.index = 0; cb(ev); }

        // Tool call blocks
        for (size_t i = 0; i < resp.tool_calls.size(); ++i) {
            const ToolUseContent& tc = resp.tool_calls[i];
            const int idx = static_cast<int>(i) + 1;

            { StreamEventData ev; ev.type = StreamEventType::ContentBlockStart;
              ev.index = idx; ev.delta_type = "tool_use";
              ev.tool_id = tc.id; ev.tool_name = tc.name; cb(ev); }

            { StreamEventData ev; ev.type = StreamEventType::ContentBlockDelta;
              ev.index = idx; ev.delta_type = "input_json_delta";
              ev.tool_input_delta = tc.input; cb(ev); }

            { StreamEventData ev; ev.type = StreamEventType::ContentBlockStop;
              ev.index = idx; ev.tool_input = tc.input; cb(ev); }
        }

        const std::string sr = resp.tool_calls.empty() ? "end_turn" : "tool_use";
        { StreamEventData ev; ev.type = StreamEventType::MessageDelta;
          ev.stop_reason = sr; ev.usage = resp.usage; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::MessageStop; cb(ev); }

        return resp.usage;
    }

    ModelCapabilities capabilities() const override { return {}; }
    std::string       name()         const override { return "mock"; }
};

// A simple tool that records calls and returns a fixed string
class QEEchoTool : public Tool {
public:
    int call_count = 0;

    std::string    name()         const override { return "echo"; }
    std::string    description()  const override { return "Echo"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()    const override { return true; }
    bool           is_destructive()  const override { return false; }

    ToolCallResult call(const nlohmann::json&) override {
        ++call_count;
        return {"echo_result", false};
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// SimpleTextResponse: one response with text, no tools → observer gets text + DoneEvent
TEST(QueryEngine, SimpleTextResponse) {
    auto mock = std::make_unique<QEMockProvider>();
    mock->responses = {QECannedResponse{"Hello, world!", {}, {2, 3, 0}}};

    ToolRegistry          registry;
    QEAllowAllDelegate    delegate;
    QECollectingObserver  observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 10;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock), registry, checker, observer, cfg);
    engine.submit("hi");

    EXPECT_EQ(observer.all_text(), "Hello, world!");
    EXPECT_EQ(observer.count_done(), 1);

    ASSERT_EQ(engine.messages().size(), 2u);
    EXPECT_EQ(engine.messages()[0].role, Role::User);
    EXPECT_EQ(engine.messages()[1].role, Role::Assistant);

    EXPECT_EQ(engine.total_usage().input_tokens,  2);
    EXPECT_EQ(engine.total_usage().output_tokens, 3);
}

// ToolCallLoop: first response has tool call, second is text → tool fires, final text arrives
TEST(QueryEngine, ToolCallLoop) {
    auto echo_owned = std::make_unique<QEEchoTool>();
    QEEchoTool* raw_echo = echo_owned.get();

    ToolRegistry registry;
    registry.register_tool(std::move(echo_owned));

    auto mock = std::make_unique<QEMockProvider>();
    mock->responses = {
        QECannedResponse{"", {ToolUseContent{"call-1", "echo", nlohmann::json::object()}}, {1,1,0}},
        QECannedResponse{"Done!", {}, {1,1,0}}
    };

    QEAllowAllDelegate   delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 10;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock), registry, checker, observer, cfg);
    engine.submit("use echo");

    EXPECT_EQ(observer.count_tool_use_start(), 1);
    EXPECT_EQ(observer.all_text(), "Done!");
    EXPECT_EQ(observer.count_done(), 1);
    EXPECT_EQ(raw_echo->call_count, 1);

    // Messages: user, assistant(tool_call), tool_result, assistant(text)
    ASSERT_EQ(engine.messages().size(), 4u);
    EXPECT_EQ(engine.messages()[0].role, Role::User);
    EXPECT_EQ(engine.messages()[1].role, Role::Assistant);
    EXPECT_EQ(engine.messages()[2].role, Role::ToolResult);
    EXPECT_EQ(engine.messages()[3].role, Role::Assistant);
}

// MaxTurnsLimit: every response calls a tool → stops after max_turns
TEST(QueryEngine, MaxTurnsLimit) {
    ToolRegistry registry;
    registry.register_tool(std::make_unique<QEEchoTool>());

    auto mock = std::make_unique<QEMockProvider>();
    mock->responses = {
        QECannedResponse{"", {ToolUseContent{"c1", "echo", nlohmann::json::object()}}, {1,1,0}}
    };

    QEAllowAllDelegate   delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 3;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock), registry, checker, observer, cfg);
    engine.submit("loop");

    EXPECT_EQ(observer.count_done(), 1);

    // max_turns = 3 → 3 assistant messages (each turn calls the tool then loops back)
    size_t assistant_count = 0;
    for (const auto& m : engine.messages()) {
        if (m.role == Role::Assistant) ++assistant_count;
    }
    EXPECT_EQ(assistant_count, 3u);
}

// RetryOnTransientError: provider throws on first call with a transient error,
// succeeds on second. Observer gets ErrorEvent(recoverable=true) then normal response.
class QEThrowOnceProvider : public LLMProvider {
public:
    int call_count = 0;
    std::string throw_message = "connection refused";

    Usage complete(const CompletionRequest& /*request*/,
                   StreamCallback cb,
                   std::atomic<bool>& stop_flag) override
    {
        if (stop_flag.load()) return {};
        ++call_count;
        if (call_count == 1) {
            throw std::runtime_error(throw_message);
        }
        // Second call succeeds with a simple text response.
        { StreamEventData ev; ev.type = StreamEventType::MessageStart; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::ContentBlockStart; ev.index = 0; ev.delta_type = "text"; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::ContentBlockDelta; ev.index = 0; ev.delta_type = "text_delta"; ev.delta_text = "Recovered!"; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::ContentBlockStop; ev.index = 0; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::MessageDelta; ev.stop_reason = "end_turn"; ev.usage = {1,1,0}; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::MessageStop; cb(ev); }
        return {1, 1, 0};
    }

    ModelCapabilities capabilities() const override { return {}; }
    std::string       name()         const override { return "throw-once"; }
};

TEST(QueryEngine, RetryOnTransientError) {
    auto mock = std::make_unique<QEThrowOnceProvider>();
    QEThrowOnceProvider* raw = mock.get();

    ToolRegistry          registry;
    QEAllowAllDelegate    delegate;
    QECollectingObserver  observer;

    QueryEngineConfig cfg;
    cfg.max_turns  = 10;
    cfg.max_retries = 3;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock), registry, checker, observer, cfg);
    engine.submit("hi");

    // Provider was called twice (first threw, second succeeded)
    EXPECT_EQ(raw->call_count, 2);

    // Observer should have received one recoverable ErrorEvent
    int error_count = 0;
    bool had_recoverable = false;
    for (const auto& e : observer.events) {
        if (const auto* err = std::get_if<ErrorEvent>(&e)) {
            ++error_count;
            if (err->recoverable) had_recoverable = true;
        }
    }
    EXPECT_EQ(error_count, 1);
    EXPECT_TRUE(had_recoverable);

    // Normal text response arrived after retry
    EXPECT_EQ(observer.all_text(), "Recovered!");
    EXPECT_EQ(observer.count_done(), 1);
}

// SlashCommandClear: submit "/clear" after some messages → messages cleared, DoneEvent emitted.
TEST(QueryEngine, SlashCommandClear) {
    auto mock_owned = std::make_unique<QEMockProvider>();
    mock_owned->responses = {
        QECannedResponse{"Hello!", {}, {1,1,0}},
    };
    QEMockProvider* mock_raw = mock_owned.get();

    ToolRegistry          registry;
    QEAllowAllDelegate    delegate;
    QECollectingObserver  observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 5;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);

    // Populate some messages (uses one provider call)
    engine.submit("first message");
    ASSERT_EQ(engine.messages().size(), 2u);  // user + assistant
    const size_t calls_before_clear = mock_raw->call_count;

    // Now clear — slash command, no LLM call
    observer.events.clear();
    engine.submit("/clear");

    // Messages should be empty after /clear
    EXPECT_TRUE(engine.messages().empty());
    // Observer should get a DoneEvent
    EXPECT_EQ(observer.count_done(), 1);
    // Provider should NOT have been called again for the /clear command
    EXPECT_EQ(mock_raw->call_count, calls_before_clear);
    // The response text should mention "cleared"
    EXPECT_NE(observer.all_text().find("cleared"), std::string::npos);
}

// MessagesAccumulate: two submits → messages grow across calls
TEST(QueryEngine, MessagesAccumulate) {
    auto mock = std::make_unique<QEMockProvider>();
    mock->responses = {
        QECannedResponse{"First reply",  {}, {1,1,0}},
        QECannedResponse{"Second reply", {}, {1,1,0}}
    };

    ToolRegistry          registry;
    QEAllowAllDelegate    delegate;
    QECollectingObserver  observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 5;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock), registry, checker, observer, cfg);

    engine.submit("first");
    engine.submit("second");

    // user1, assistant1, user2, assistant2
    ASSERT_EQ(engine.messages().size(), 4u);
    EXPECT_EQ(engine.messages()[0].role, Role::User);
    EXPECT_EQ(engine.messages()[1].role, Role::Assistant);
    EXPECT_EQ(engine.messages()[2].role, Role::User);
    EXPECT_EQ(engine.messages()[3].role, Role::Assistant);

    EXPECT_EQ(observer.count_done(), 2);
}

TEST(QueryEngine, RetriesWithoutToolsAfterEmptyAssistantTurn) {
    auto mock_owned = std::make_unique<QEMockProvider>();
    mock_owned->responses = {
        QECannedResponse{"", {}, {1, 2, 0}},
        QECannedResponse{"Project summary", {}, {3, 4, 0}}
    };
    QEMockProvider* mock_raw = mock_owned.get();

    ToolRegistry registry;
    registry.register_tool(std::make_unique<QEEchoTool>());

    QEAllowAllDelegate   delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 5;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);
    engine.submit("summarize the project");

    ASSERT_EQ(mock_raw->call_count, 2u);
    ASSERT_EQ(mock_raw->requests.size(), 2u);
    EXPECT_GT(mock_raw->requests[0].tool_count, 0u);
    EXPECT_EQ(mock_raw->requests[1].tool_count, 0u);
    EXPECT_NE(
        mock_raw->requests[1].system_prompt.find("The previous attempt returned no usable answer."),
        std::string::npos);

    EXPECT_EQ(observer.all_text(), "Project summary");
    EXPECT_EQ(observer.count_done(), 1);

    int error_count = 0;
    for (const auto& event : observer.events) {
        if (std::holds_alternative<ErrorEvent>(event)) {
            ++error_count;
        }
    }
    EXPECT_EQ(error_count, 0);

    ASSERT_EQ(engine.messages().size(), 2u);
    EXPECT_EQ(engine.messages()[1].role, Role::Assistant);
    EXPECT_EQ(engine.total_usage().input_tokens, 4);
    EXPECT_EQ(engine.total_usage().output_tokens, 6);
}

TEST(QueryEngine, SanitizesPseudoToolTranscriptText) {
    auto mock = std::make_unique<QEMockProvider>();
    mock->responses = {
        QECannedResponse{
            "<|tool_call>call:ls_r(path=\".\")<tool_call|><|tool_response>[{\"output\":\".omniagent/\\n\"}]|thought\n"
            "I inspected the workspace.<channel|>This workspace only contains a .omniagent directory.",
            {},
            {1, 1, 0}
        }
    };

    ToolRegistry registry;
    QEAllowAllDelegate   delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 5;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock), registry, checker, observer, cfg);
    engine.submit("tell me about the project");

    EXPECT_EQ(observer.all_text(), "This workspace only contains a .omniagent directory.");
    ASSERT_EQ(engine.messages().size(), 2u);
    ASSERT_EQ(engine.messages()[1].content.size(), 1u);
    const auto* text = std::get_if<TextContent>(&engine.messages()[1].content[0].data);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "This workspace only contains a .omniagent directory.");
}

TEST(QueryEngine, RetriesWithoutToolsAfterMalformedPlainToolTranscript) {
    auto mock_owned = std::make_unique<QEMockProvider>();
    mock_owned->responses = {
        QECannedResponse{
            "call:ls -R /home/mi7fl/projects/engine-repl-project/.omniagent\n"
            "call:execute_command(command=\"ls -a\")",
            {},
            {1, 2, 0}
        },
        QECannedResponse{"The workspace looks empty apart from .omniagent.", {}, {3, 4, 0}}
    };
    QEMockProvider* mock_raw = mock_owned.get();

    ToolRegistry registry;
    registry.register_tool(std::make_unique<QEEchoTool>());

    QEAllowAllDelegate   delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 5;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);
    engine.submit("what can you tell me about this project?");

    ASSERT_EQ(mock_raw->call_count, 2u);
    ASSERT_EQ(mock_raw->requests.size(), 2u);
    EXPECT_GT(mock_raw->requests[0].tool_count, 0u);
    EXPECT_EQ(mock_raw->requests[1].tool_count, 0u);
    EXPECT_NE(
        mock_raw->requests[1].system_prompt.find("The previous attempt returned no usable answer."),
        std::string::npos);

    EXPECT_EQ(observer.all_text(), "The workspace looks empty apart from .omniagent.");

    int error_count = 0;
    for (const auto& event : observer.events) {
        if (std::holds_alternative<ErrorEvent>(event)) {
            ++error_count;
        }
    }
    EXPECT_EQ(error_count, 0);
}
