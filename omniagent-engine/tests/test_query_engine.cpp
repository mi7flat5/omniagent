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
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
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
        std::optional<std::string> tool_choice;
        std::optional<double> temperature;
        std::optional<double> top_p;
        std::optional<int> top_k;
        std::optional<double> min_p;
        std::optional<double> presence_penalty;
        std::optional<double> frequency_penalty;
        std::optional<int> max_tokens;
        std::vector<Message> messages;
    };

    std::vector<QECannedResponse> responses;
    std::vector<RequestSnapshot>   requests;
    size_t                        call_count = 0;

    Usage complete(const CompletionRequest& request,
                   StreamCallback cb,
                   std::atomic<bool>& stop_flag) override
    {
        if (stop_flag.load()) return {};

        requests.push_back(RequestSnapshot{request.tools.size(),
                                          request.system_prompt,
                                          request.tool_choice,
                                          request.temperature,
                                          request.top_p,
                                          request.top_k,
                                          request.min_p,
                                          request.presence_penalty,
                                          request.frequency_penalty,
                                          request.max_tokens,
                                          request.messages});

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

class QEWriteTool : public Tool {
public:
    std::string    name()         const override { return "write"; }
    std::string    description()  const override { return "Write"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()   const override { return false; }
    bool           is_destructive() const override { return false; }

    ToolCallResult call(const nlohmann::json&) override {
        return {"write_result", false};
    }
};

class QEReviewValidatorTool : public Tool {
public:
    int call_count = 0;
    std::vector<std::string> report_texts;

    std::string    name()         const override { return "planner_validate_review"; }
    std::string    description()  const override { return "Validate review report text"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool           is_read_only()    const override { return true; }
    bool           is_destructive()  const override { return false; }

    ToolCallResult call(const nlohmann::json& args) override {
        ++call_count;
        const std::string report_text = args.value("report_text", std::string{});
        report_texts.push_back(report_text);
        if (report_text.find("confctl/schema.py loads the wrong YAML node") != std::string::npos) {
            return {"validated", false};
        }
        return {"tracked review validation failed: missing the loader finding from the benchmark case", true};
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

TEST(QueryEngine, ForcesFinalAnswerAfterToolTurnBudgetExhausted) {
    auto echo_owned = std::make_unique<QEEchoTool>();
    QEEchoTool* raw_echo = echo_owned.get();

    ToolRegistry registry;
    registry.register_tool(std::move(echo_owned));

    auto mock_owned = std::make_unique<QEMockProvider>();
    mock_owned->responses = {
        QECannedResponse{"", {ToolUseContent{"call-1", "echo", nlohmann::json::object()}}, {1, 1, 0}},
        QECannedResponse{"Audit summary", {}, {2, 3, 0}}
    };
    QEMockProvider* mock_raw = mock_owned.get();

    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 1;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);
    engine.submit("do a code audit");

    ASSERT_EQ(mock_raw->call_count, 2u);
    ASSERT_EQ(mock_raw->requests.size(), 2u);
    EXPECT_GT(mock_raw->requests[0].tool_count, 0u);
    EXPECT_EQ(mock_raw->requests[1].tool_count, 0u);
    EXPECT_NE(mock_raw->requests[1].system_prompt.find("You have finished using tools for this request."),
              std::string::npos);

    EXPECT_EQ(observer.all_text(), "Audit summary");
    EXPECT_EQ(observer.count_done(), 1);
    EXPECT_EQ(raw_echo->call_count, 1);

    int error_count = 0;
    for (const auto& event : observer.events) {
        if (std::holds_alternative<ErrorEvent>(event)) {
            ++error_count;
        }
    }
    EXPECT_EQ(error_count, 0);

    ASSERT_EQ(engine.messages().size(), 4u);
    EXPECT_EQ(engine.messages()[3].role, Role::Assistant);
}

TEST(QueryEngine, ForcesFinalAnswerAfterRepeatedReadOnlyToolLoop) {
    auto echo_owned = std::make_unique<QEEchoTool>();
    QEEchoTool* raw_echo = echo_owned.get();

    ToolRegistry registry;
    registry.register_tool(std::move(echo_owned));

    auto mock_owned = std::make_unique<QEMockProvider>();
    QEMockProvider* mock_raw = mock_owned.get();
    mock_owned->responses = {
        QECannedResponse{"", {ToolUseContent{"call-1", "echo", nlohmann::json{{"path", "builder.py"}}}}, {1, 1, 0}},
        QECannedResponse{"", {ToolUseContent{"call-2", "echo", nlohmann::json{{"path", "store.py"}}}}, {1, 1, 0}},
        QECannedResponse{"", {ToolUseContent{"call-3", "echo", nlohmann::json{{"path", "models.py"}}}}, {1, 1, 0}},
        QECannedResponse{"", {ToolUseContent{"call-4", "echo", nlohmann::json{{"path", "schema.py"}}}}, {1, 1, 0}},
        QECannedResponse{"", {ToolUseContent{"call-5", "echo", nlohmann::json{{"path", "builder.py"}}}}, {1, 1, 0}},
        QECannedResponse{"", {ToolUseContent{"call-6", "echo", nlohmann::json{{"path", "store.py"}}}}, {1, 1, 0}},
        QECannedResponse{"No findings.", {}, {2, 3, 0}},
    };

    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 20;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);
    engine.submit("do a code audit");

    ASSERT_EQ(mock_raw->requests.size(), 7u);
    EXPECT_GT(mock_raw->requests[0].tool_count, 0u);
    EXPECT_GT(mock_raw->requests[5].tool_count, 0u);
    EXPECT_EQ(mock_raw->requests[6].tool_count, 0u);
    EXPECT_NE(mock_raw->requests[6].system_prompt.find("Your read-only exploration is looping or yielding only marginally new information."),
              std::string::npos);

    EXPECT_EQ(raw_echo->call_count, 5);
    EXPECT_EQ(observer.all_text(), "No findings.");
    EXPECT_EQ(observer.count_done(), 1);

    int error_count = 0;
    for (const auto& event : observer.events) {
        if (std::holds_alternative<ErrorEvent>(event)) {
            ++error_count;
        }
    }
    EXPECT_EQ(error_count, 0);
}

TEST(QueryEngine, ForcesFinalAnswerAfterRepeatedReadOnlyFocusChurn) {
    auto echo_owned = std::make_unique<QEEchoTool>();
    QEEchoTool* raw_echo = echo_owned.get();

    ToolRegistry registry;
    registry.register_tool(std::move(echo_owned));

    auto mock_owned = std::make_unique<QEMockProvider>();
    QEMockProvider* mock_raw = mock_owned.get();
    mock_owned->responses = {
        QECannedResponse{"", {ToolUseContent{"call-1", "echo", nlohmann::json{{"path", "builder.py"}, {"startLine", 1}}}}, {1, 1, 0}},
        QECannedResponse{"", {ToolUseContent{"call-2", "echo", nlohmann::json{{"path", "builder.py"}, {"startLine", 101}}}}, {1, 1, 0}},
        QECannedResponse{"", {ToolUseContent{"call-3", "echo", nlohmann::json{{"path", "builder.py"}, {"startLine", 201}}}}, {1, 1, 0}},
        QECannedResponse{"", {ToolUseContent{"call-4", "echo", nlohmann::json{{"path", "builder.py"}, {"startLine", 301}}}}, {1, 1, 0}},
        QECannedResponse{"", {ToolUseContent{"call-5", "echo", nlohmann::json{{"path", "builder.py"}, {"startLine", 401}}}}, {1, 1, 0}},
        QECannedResponse{"", {ToolUseContent{"call-6", "echo", nlohmann::json{{"path", "builder.py"}, {"startLine", 501}}}}, {1, 1, 0}},
        QECannedResponse{"Builder summary.", {}, {2, 3, 0}},
    };

    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 20;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);
    engine.submit("explain builder.py");

    ASSERT_EQ(mock_raw->requests.size(), 7u);
    EXPECT_EQ(mock_raw->requests[6].tool_count, 0u);
    EXPECT_NE(mock_raw->requests[6].system_prompt.find("stop exploring instead of rereading more files"),
              std::string::npos);
    EXPECT_EQ(raw_echo->call_count, 5);
    EXPECT_EQ(observer.all_text(), "Builder summary.");
}

TEST(QueryEngine, DoesNotForceFinalAnswerForReadOnlyChurnWhenWriteToolsAreVisible) {
    auto echo_owned = std::make_unique<QEEchoTool>();
    QEEchoTool* raw_echo = echo_owned.get();

    ToolRegistry registry;
    registry.register_tool(std::move(echo_owned));
    registry.register_tool(std::make_unique<QEWriteTool>());

    auto mock_owned = std::make_unique<QEMockProvider>();
    QEMockProvider* mock_raw = mock_owned.get();
    mock_owned->responses = {
        QECannedResponse{"", {ToolUseContent{"call-1", "echo", nlohmann::json{{"path", "builder.py"}}}}, {1, 1, 0}},
        QECannedResponse{"", {ToolUseContent{"call-2", "echo", nlohmann::json{{"path", "store.py"}}}}, {1, 1, 0}},
        QECannedResponse{"", {ToolUseContent{"call-3", "echo", nlohmann::json{{"path", "models.py"}}}}, {1, 1, 0}},
        QECannedResponse{"", {ToolUseContent{"call-4", "echo", nlohmann::json{{"path", "schema.py"}}}}, {1, 1, 0}},
        QECannedResponse{"", {ToolUseContent{"call-5", "echo", nlohmann::json{{"path", "builder.py"}}}}, {1, 1, 0}},
        QECannedResponse{"", {ToolUseContent{"call-6", "echo", nlohmann::json{{"path", "store.py"}}}}, {1, 1, 0}},
        QECannedResponse{"Need to edit next.", {}, {2, 3, 0}},
    };

    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 20;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);
    engine.submit("debug this");

    ASSERT_EQ(mock_raw->requests.size(), 7u);
    EXPECT_GT(mock_raw->requests[6].tool_count, 0u);
    EXPECT_EQ(mock_raw->requests[6].system_prompt.find("Your read-only exploration is looping or yielding only marginally new information."),
              std::string::npos);
    EXPECT_EQ(raw_echo->call_count, 6);
    EXPECT_EQ(observer.all_text(), "Need to edit next.");
}

TEST(QueryEngine, AppliesSamplingParametersToRequests) {
    auto mock_owned = std::make_unique<QEMockProvider>();
    QEMockProvider* mock_raw = mock_owned.get();
    mock_owned->responses = {QECannedResponse{"Audit summary", {}, {1, 1, 0}}};

    ToolRegistry registry;
    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.temperature = 0.6;
    cfg.top_p = 0.92;
    cfg.top_k = 40;
    cfg.min_p = 0.05;
    cfg.presence_penalty = 0.3;
    cfg.frequency_penalty = -0.1;
    cfg.initial_max_tokens = 32768;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);
    engine.submit("audit this repo");

    ASSERT_EQ(mock_raw->requests.size(), 1u);
    ASSERT_TRUE(mock_raw->requests[0].temperature.has_value());
    EXPECT_DOUBLE_EQ(*mock_raw->requests[0].temperature, 0.6);
    ASSERT_TRUE(mock_raw->requests[0].top_p.has_value());
    EXPECT_DOUBLE_EQ(*mock_raw->requests[0].top_p, 0.92);
    ASSERT_TRUE(mock_raw->requests[0].top_k.has_value());
    EXPECT_EQ(*mock_raw->requests[0].top_k, 40);
    ASSERT_TRUE(mock_raw->requests[0].min_p.has_value());
    EXPECT_DOUBLE_EQ(*mock_raw->requests[0].min_p, 0.05);
    ASSERT_TRUE(mock_raw->requests[0].presence_penalty.has_value());
    EXPECT_DOUBLE_EQ(*mock_raw->requests[0].presence_penalty, 0.3);
    ASSERT_TRUE(mock_raw->requests[0].frequency_penalty.has_value());
    EXPECT_DOUBLE_EQ(*mock_raw->requests[0].frequency_penalty, -0.1);
    ASSERT_TRUE(mock_raw->requests[0].max_tokens.has_value());
    EXPECT_EQ(*mock_raw->requests[0].max_tokens, 32768);
}

TEST(QueryEngine, EvidenceVerificationRewritesFinalAnswerBeforeEmission) {
    auto mock_owned = std::make_unique<QEMockProvider>();
    mock_owned->responses = {
        QECannedResponse{"High: Broken load_schema function in confctl/schema.py causes a SyntaxError.",
                         {},
                         {2, 3, 0}},
        QECannedResponse{"Medium: ConfigStore._cast_value accesses field.type even though SchemaField defines value_type.",
                         {},
                         {1, 2, 0}},
    };
    QEMockProvider* mock_raw = mock_owned.get();

    ToolRegistry registry;
    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.enforce_evidence_based_final_answer = true;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);
    engine.submit("do a code audit");

    ASSERT_EQ(mock_raw->call_count, 2u);
    ASSERT_EQ(mock_raw->requests.size(), 2u);
    EXPECT_EQ(mock_raw->requests[1].tool_count, 0u);
    EXPECT_NE(mock_raw->requests[1].system_prompt.find("You are verifying a final code-review answer."),
              std::string::npos);
    EXPECT_NE(mock_raw->requests[1].system_prompt.find("Do not keep generic filler sections"),
              std::string::npos);
    EXPECT_NE(mock_raw->requests[1].system_prompt.find("When explicit failing test or command output is present"),
              std::string::npos);
    ASSERT_FALSE(mock_raw->requests[1].messages.empty());
    EXPECT_EQ(mock_raw->requests[1].messages.back().role, Role::User);

    EXPECT_EQ(observer.all_text(),
              "Medium: ConfigStore._cast_value accesses field.type even though SchemaField defines value_type.");
    ASSERT_EQ(engine.messages().size(), 2u);
    EXPECT_EQ(engine.messages()[1].role, Role::Assistant);
    ASSERT_EQ(engine.messages()[1].content.size(), 1u);
    const auto* text = std::get_if<TextContent>(&engine.messages()[1].content[0].data);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text,
              "Medium: ConfigStore._cast_value accesses field.type even though SchemaField defines value_type.");
    EXPECT_EQ(engine.total_usage().input_tokens, 3);
    EXPECT_EQ(engine.total_usage().output_tokens, 5);
}

TEST(QueryEngine, EvidenceVerificationPromptRejectsFillerAndAllowsExplicitTestClusters) {
    auto mock_owned = std::make_unique<QEMockProvider>();
    mock_owned->responses = {
        QECannedResponse{
            "Summary: confctl has several issues.\n\n"
            "High: Schema loading is mismatched.\n\n"
            "Security Considerations: env vars may be manipulated.\n\n"
            "Production readiness: not ready.",
            {},
            {2, 3, 0}
        },
        QECannedResponse{
            "High: confctl/schema.py reads the top-level YAML mapping directly instead of descending into the fields mapping expected by the schema tests.\n"
            "High: confctl/cli.py rejects tested CLI forms such as placing --schema after the get subcommand and calling diff with only one file argument.\n"
            "Medium: confctl/builder.py failures expose a separate builder contract mismatch beyond the schema/store defects.",
            {},
            {1, 2, 0}
        },
    };
    QEMockProvider* mock_raw = mock_owned.get();

    ToolRegistry registry;
    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.enforce_evidence_based_final_answer = true;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);

    Message tool_result_message;
    tool_result_message.role = Role::ToolResult;
    tool_result_message.tool_results.push_back(ToolResult{
        .tool_use_id = "tool-1",
        .content =
            "FAILED tests/test_schema.py::TestLoadSchema::test_load_schema_valid_yaml\n"
            "FAILED tests/test_schema.py::TestValidateConfig::test_validate_config_valid\n"
            "FAILED tests/test_cli.py::TestCLIGetSubcommand::test_get_with_schema\n"
            "FAILED tests/test_cli.py::TestCLIDiffSubcommand::test_diff_with_changes\n"
            "FAILED tests/test_builder.py::TestConfigBuilder::test_build_with_only_env\n",
        .is_error = false,
        .metadata = {},
    });
    engine.set_messages({tool_result_message});

    engine.submit("do a code audit");

    ASSERT_EQ(mock_raw->call_count, 2u);
    ASSERT_EQ(mock_raw->requests.size(), 2u);
    EXPECT_EQ(mock_raw->requests[1].tool_count, 0u);
    EXPECT_NE(mock_raw->requests[1].system_prompt.find("Do not keep generic filler sections"),
              std::string::npos);
    EXPECT_NE(mock_raw->requests[1].system_prompt.find("When explicit failing test or command output is present"),
              std::string::npos);
    EXPECT_NE(mock_raw->requests[1].system_prompt.find("You may add concise findings"),
              std::string::npos);

    EXPECT_EQ(observer.all_text(),
              "High: confctl/schema.py reads the top-level YAML mapping directly instead of descending into the fields mapping expected by the schema tests.\n"
              "High: confctl/cli.py rejects tested CLI forms such as placing --schema after the get subcommand and calling diff with only one file argument.\n"
              "Medium: confctl/builder.py failures expose a separate builder contract mismatch beyond the schema/store defects.");
    ASSERT_EQ(engine.messages().size(), 3u);
    EXPECT_EQ(engine.messages()[2].role, Role::Assistant);
}

TEST(QueryEngine, SuppressesAssistantPreambleWhenToolCallsPresent) {
    auto echo_owned = std::make_unique<QEEchoTool>();
    QEEchoTool* raw_echo = echo_owned.get();

    ToolRegistry registry;
    registry.register_tool(std::move(echo_owned));

    auto mock = std::make_unique<QEMockProvider>();
    mock->responses = {
        QECannedResponse{"Let me inspect the workspace before I audit it.",
                         {ToolUseContent{"call-1", "echo", nlohmann::json::object()}},
                         {1, 1, 0}},
        QECannedResponse{"Audit complete.", {}, {1, 1, 0}}
    };

    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 10;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock), registry, checker, observer, cfg);
    engine.submit("audit this codebase");

    EXPECT_EQ(observer.count_tool_use_start(), 1);
    EXPECT_EQ(observer.all_text(), "Audit complete.");
    EXPECT_EQ(raw_echo->call_count, 1);

    ASSERT_EQ(engine.messages().size(), 4u);
    ASSERT_EQ(engine.messages()[1].content.size(), 1u);
    EXPECT_NE(std::get_if<ToolUseContent>(&engine.messages()[1].content[0].data), nullptr);
}

TEST(QueryEngine, RecoversXmlishToolTranscriptIntoToolCalls) {
    auto echo_owned = std::make_unique<QEEchoTool>();
    QEEchoTool* raw_echo = echo_owned.get();

    ToolRegistry registry;
    registry.register_tool(std::move(echo_owned));

    auto mock = std::make_unique<QEMockProvider>();
    mock->responses = {
        QECannedResponse{
            "<tool_call>\n"
            "<function=echo>\n"
            "<parameter=path>\n"
            "confctl/builder.py\n"
            "</parameter>\n"
            "</function>\n"
            "</tool_call>",
            {},
            {1, 1, 0}
        },
        QECannedResponse{"Audit complete.", {}, {1, 1, 0}}
    };

    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 10;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock), registry, checker, observer, cfg);
    engine.submit("do a code audit");

    EXPECT_EQ(observer.count_tool_use_start(), 1);
    EXPECT_EQ(observer.all_text(), "Audit complete.");
    EXPECT_EQ(raw_echo->call_count, 1);

    ASSERT_EQ(engine.messages().size(), 4u);
    ASSERT_EQ(engine.messages()[1].content.size(), 1u);
    const auto* tool_call = std::get_if<ToolUseContent>(&engine.messages()[1].content[0].data);
    ASSERT_NE(tool_call, nullptr);
    EXPECT_EQ(tool_call->name, "echo");
    ASSERT_TRUE(tool_call->input.is_object());
    EXPECT_EQ(tool_call->input.value("path", std::string{}), "confctl/builder.py");
}

TEST(QueryEngine, RecoversNamedToolUseTranscriptIntoToolCalls) {
    auto echo_owned = std::make_unique<QEEchoTool>();
    QEEchoTool* raw_echo = echo_owned.get();

    ToolRegistry registry;
    registry.register_tool(std::move(echo_owned));

    auto mock = std::make_unique<QEMockProvider>();
    mock->responses = {
        QECannedResponse{
            "<tool_use name=\"echo\">\n"
            "{\n"
            "  \"path\": \"confctl/schema.py\"\n"
            "}",
            {},
            {1, 1, 0}
        },
        QECannedResponse{"Audit complete.", {}, {1, 1, 0}}
    };

    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 10;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock), registry, checker, observer, cfg);
    engine.submit("do a code audit");

    EXPECT_EQ(observer.count_tool_use_start(), 1);
    EXPECT_EQ(observer.all_text(), "Audit complete.");
    EXPECT_EQ(raw_echo->call_count, 1);

    ASSERT_EQ(engine.messages().size(), 4u);
    ASSERT_EQ(engine.messages()[1].content.size(), 1u);
    const auto* tool_call = std::get_if<ToolUseContent>(&engine.messages()[1].content[0].data);
    ASSERT_NE(tool_call, nullptr);
    EXPECT_EQ(tool_call->name, "echo");
    ASSERT_TRUE(tool_call->input.is_object());
    EXPECT_EQ(tool_call->input.value("path", std::string{}), "confctl/schema.py");
}

TEST(QueryEngine, RequiresToolUseOnInitialProjectTurn) {
    ToolRegistry registry;
    registry.register_tool(std::make_unique<QEEchoTool>());

    auto mock_owned = std::make_unique<QEMockProvider>();
    mock_owned->responses = {QECannedResponse{"Done", {}, {1, 1, 0}}};
    QEMockProvider* mock_raw = mock_owned.get();

    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 5;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);

    ToolContext context;
    context.workspace_root = "/tmp/project";
    context.working_dir = "/tmp/project";
    engine.set_tool_context(context);
    engine.submit("inspect the project");

    ASSERT_EQ(mock_raw->requests.size(), 1u);
    ASSERT_TRUE(mock_raw->requests[0].tool_choice.has_value());
    EXPECT_EQ(*mock_raw->requests[0].tool_choice, "required");
}

TEST(QueryEngine, RelaxesToolRequirementAfterToolResultsExist) {
    auto echo_owned = std::make_unique<QEEchoTool>();

    ToolRegistry registry;
    registry.register_tool(std::move(echo_owned));

    auto mock_owned = std::make_unique<QEMockProvider>();
    mock_owned->responses = {
        QECannedResponse{"", {ToolUseContent{"call-1", "echo", nlohmann::json::object()}}, {1, 1, 0}},
        QECannedResponse{"Done", {}, {1, 1, 0}}
    };
    QEMockProvider* mock_raw = mock_owned.get();

    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 5;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);

    ToolContext context;
    context.workspace_root = "/tmp/project";
    context.working_dir = "/tmp/project";
    engine.set_tool_context(context);
    engine.submit("inspect the project");

    ASSERT_EQ(mock_raw->requests.size(), 2u);
    ASSERT_TRUE(mock_raw->requests[0].tool_choice.has_value());
    EXPECT_EQ(*mock_raw->requests[0].tool_choice, "required");
    ASSERT_TRUE(mock_raw->requests[1].tool_choice.has_value());
    EXPECT_EQ(*mock_raw->requests[1].tool_choice, "auto");
}

TEST(QueryEngine, UsesSeparateCompactionLimitForOlderToolResults) {
    ToolRegistry registry;

    auto mock_owned = std::make_unique<QEMockProvider>();
    mock_owned->responses = {QECannedResponse{"Done", {}, {1, 1, 0}}};
    QEMockProvider* mock_raw = mock_owned.get();

    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 5;
    cfg.preserve_tail = 1;
    cfg.max_result_chars = 4096;
    cfg.compact_max_result_chars = 64;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);

    Message tool_result_message;
    tool_result_message.role = Role::ToolResult;
    tool_result_message.tool_results.push_back(ToolResult{
        .tool_use_id = "tool-1",
        .content = std::string(256, 'x'),
        .is_error = false,
        .metadata = {},
    });
    engine.set_messages({tool_result_message});

    engine.submit("continue");

    ASSERT_EQ(mock_raw->requests.size(), 1u);
    ASSERT_EQ(mock_raw->requests[0].messages.size(), 2u);
    ASSERT_EQ(mock_raw->requests[0].messages[0].tool_results.size(), 1u);

    const auto& compacted = mock_raw->requests[0].messages[0].tool_results[0].content;
    EXPECT_NE(compacted.find("[truncated]"), std::string::npos);
    EXPECT_LT(compacted.size(), 128u);

    ASSERT_EQ(engine.messages().size(), 3u);
    EXPECT_EQ(engine.messages()[0].tool_results[0].content.size(), 256u);
    EXPECT_EQ(observer.all_text(), "Done");
}

// MaxTurnsLimit: every response calls a tool → stops after max_turns and rejects
// forced-final-answer retries that still try to call tools with no tools available.
TEST(QueryEngine, MaxTurnsLimit) {
    ToolRegistry registry;
    auto echo_owned = std::make_unique<QEEchoTool>();
    QEEchoTool* raw_echo = echo_owned.get();
    registry.register_tool(std::move(echo_owned));

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

    // max_turns = 3 → 3 tool turns. The forced final-answer retry does not append
    // another assistant message when the model still attempts tool calls.
    size_t assistant_count = 0;
    for (const auto& m : engine.messages()) {
        if (m.role == Role::Assistant) ++assistant_count;
    }
    EXPECT_EQ(assistant_count, 3u);
    EXPECT_EQ(raw_echo->call_count, 3);

    int error_count = 0;
    bool saw_no_tools_error = false;
    for (const auto& event : observer.events) {
        if (const auto* error = std::get_if<ErrorEvent>(&event)) {
            ++error_count;
            if (!error->recoverable
                && error->message.find("no tools were available") != std::string::npos) {
                saw_no_tools_error = true;
            }
        }
    }
    EXPECT_EQ(error_count, 1);
    EXPECT_TRUE(saw_no_tools_error);
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

class QEStopAwareProvider : public LLMProvider {
public:
    Usage complete(const CompletionRequest&,
                   StreamCallback,
                   std::atomic<bool>& stop_flag) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            started = true;
        }
        started_cv.notify_all();

        while (!stop_flag.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        throw std::runtime_error(
            "HTTP transport failure for '/v1/chat/completions': Connection handling canceled");
    }

    ModelCapabilities capabilities() const override { return {}; }
    std::string name() const override { return "stop-aware"; }

    std::mutex mutex;
    std::condition_variable started_cv;
    bool started = false;
};

class QEEmptyResponseProvider : public LLMProvider {
public:
    int call_count = 0;

    Usage complete(const CompletionRequest&,
                   StreamCallback cb,
                   std::atomic<bool>& stop_flag) override {
        if (stop_flag.load(std::memory_order_relaxed)) {
            return {};
        }

        ++call_count;
        StreamEventData ev;
        ev.type = StreamEventType::MessageStart;
        cb(ev);
        return {};
    }

    ModelCapabilities capabilities() const override { return {}; }
    std::string name() const override { return "empty-response"; }
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

TEST(QueryEngine, ReportsErrorForEmptyProviderResponse) {
    auto mock = std::make_unique<QEEmptyResponseProvider>();
    QEEmptyResponseProvider* raw = mock.get();

    ToolRegistry registry;
    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 5;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock), registry, checker, observer, cfg);
    engine.submit("do a code audit");

    EXPECT_EQ(raw->call_count, 1);
    EXPECT_EQ(observer.count_done(), 1);
    EXPECT_TRUE(observer.all_text().empty());

    int error_count = 0;
    bool saw_empty_response_error = false;
    for (const auto& event : observer.events) {
        if (const auto* error = std::get_if<ErrorEvent>(&event)) {
            ++error_count;
            if (!error->recoverable
                && error->message.find("empty response") != std::string::npos) {
                saw_empty_response_error = true;
            }
        }
    }

    EXPECT_EQ(error_count, 1);
    EXPECT_TRUE(saw_empty_response_error);
    ASSERT_EQ(engine.messages().size(), 1u);
    EXPECT_EQ(engine.messages()[0].role, Role::User);
}

TEST(QueryEngine, RequestStopSuppressesExpectedCancellationError) {
    auto mock = std::make_unique<QEStopAwareProvider>();
    QEStopAwareProvider* raw = mock.get();

    ToolRegistry registry;
    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 10;
    cfg.max_retries = 3;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock), registry, checker, observer, cfg);

    std::thread submit_thread([&engine]() {
        engine.submit("interrupt me");
    });

    {
        std::unique_lock<std::mutex> lock(raw->mutex);
        raw->started_cv.wait(lock, [&raw]() { return raw->started; });
    }

    engine.request_stop();
    submit_thread.join();

    int error_count = 0;
    for (const auto& event : observer.events) {
        if (std::holds_alternative<ErrorEvent>(event)) {
            ++error_count;
        }
    }

    EXPECT_EQ(error_count, 0);
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

TEST(QueryEngine, RewritesFinalAnswerWhenReviewValidationFails) {
    auto validator_owned = std::make_unique<QEReviewValidatorTool>();
    QEReviewValidatorTool* raw_validator = validator_owned.get();

    ToolRegistry registry;
    registry.register_tool(std::move(validator_owned));

    auto mock_owned = std::make_unique<QEMockProvider>();
    mock_owned->responses = {
        QECannedResponse{
            "High: confctl has loader problems.",
            {},
            {2, 3, 0}
        },
        QECannedResponse{
            "High: confctl has loader problems.",
            {},
            {1, 2, 0}
        },
        QECannedResponse{
            "High: confctl/schema.py loads the wrong YAML node, so schema-backed commands fail on valid configs.",
            {},
            {1, 2, 0}
        },
    };
    QEMockProvider* mock_raw = mock_owned.get();

    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.enforce_evidence_based_final_answer = true;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);
    engine.submit("do a code audit");

    ASSERT_EQ(mock_raw->call_count, 3u);
    ASSERT_EQ(mock_raw->requests.size(), 3u);
    EXPECT_EQ(mock_raw->requests[1].tool_count, 0u);
    EXPECT_EQ(mock_raw->requests[2].tool_count, 0u);
    EXPECT_NE(mock_raw->requests[1].system_prompt.find("You are verifying a final code-review answer."),
              std::string::npos);
    EXPECT_NE(mock_raw->requests[2].system_prompt.find("deterministic validation failed"),
              std::string::npos);
    ASSERT_FALSE(mock_raw->requests[2].messages.empty());
    EXPECT_EQ(mock_raw->requests[2].messages.back().role, Role::User);

    EXPECT_EQ(raw_validator->call_count, 2);
    ASSERT_EQ(raw_validator->report_texts.size(), 2u);
    EXPECT_EQ(raw_validator->report_texts[0], "High: confctl has loader problems.");
    EXPECT_EQ(raw_validator->report_texts[1],
              "High: confctl/schema.py loads the wrong YAML node, so schema-backed commands fail on valid configs.");

    EXPECT_EQ(observer.all_text(),
              "High: confctl/schema.py loads the wrong YAML node, so schema-backed commands fail on valid configs.");
    ASSERT_EQ(engine.messages().size(), 2u);
    EXPECT_EQ(engine.messages()[1].role, Role::Assistant);
    ASSERT_EQ(engine.messages()[1].content.size(), 1u);
    const auto* text = std::get_if<TextContent>(&engine.messages()[1].content[0].data);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text,
              "High: confctl/schema.py loads the wrong YAML node, so schema-backed commands fail on valid configs.");
}

TEST(QueryEngine, RecoversWithToolsWhenReviewValidationFails) {
    auto echo_owned = std::make_unique<QEEchoTool>();
    QEEchoTool* raw_echo = echo_owned.get();
    auto validator_owned = std::make_unique<QEReviewValidatorTool>();
    QEReviewValidatorTool* raw_validator = validator_owned.get();

    ToolRegistry registry;
    registry.register_tool(std::move(echo_owned));
    registry.register_tool(std::move(validator_owned));

    auto mock_owned = std::make_unique<QEMockProvider>();
    mock_owned->responses = {
        QECannedResponse{
            "High: confctl has loader problems.",
            {},
            {2, 3, 0}
        },
        QECannedResponse{
            "High: confctl has loader problems.",
            {},
            {1, 2, 0}
        },
        QECannedResponse{
            "",
            {ToolUseContent{"call-1", "echo", nlohmann::json{{"path", "confctl/cli.py"}}}},
            {1, 1, 0}
        },
        QECannedResponse{
            "High: confctl/schema.py loads the wrong YAML node, so schema-backed commands fail on valid configs.",
            {},
            {1, 2, 0}
        },
        QECannedResponse{
            "High: confctl/schema.py loads the wrong YAML node, so schema-backed commands fail on valid configs.",
            {},
            {1, 2, 0}
        },
    };
    QEMockProvider* mock_raw = mock_owned.get();

    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.enforce_evidence_based_final_answer = true;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);
    engine.submit("do a code audit");

    ASSERT_EQ(mock_raw->call_count, 5u);
    ASSERT_EQ(mock_raw->requests.size(), 5u);
    EXPECT_GT(mock_raw->requests[0].tool_count, 0u);
    EXPECT_EQ(mock_raw->requests[1].tool_count, 0u);
    EXPECT_GT(mock_raw->requests[2].tool_count, 0u);
    EXPECT_GT(mock_raw->requests[3].tool_count, 0u);
    EXPECT_EQ(mock_raw->requests[4].tool_count, 0u);
    EXPECT_NE(mock_raw->requests[2].system_prompt.find("continuing a systematic code audit after deterministic validation failed"),
              std::string::npos);
    ASSERT_TRUE(mock_raw->requests[2].tool_choice.has_value());
    EXPECT_EQ(*mock_raw->requests[2].tool_choice, "required");
    ASSERT_TRUE(mock_raw->requests[3].tool_choice.has_value());
    EXPECT_EQ(*mock_raw->requests[3].tool_choice, "auto");

    EXPECT_EQ(raw_echo->call_count, 1);
    EXPECT_EQ(raw_validator->call_count, 2);
    ASSERT_EQ(raw_validator->report_texts.size(), 2u);
    EXPECT_EQ(raw_validator->report_texts[0], "High: confctl has loader problems.");
    EXPECT_EQ(raw_validator->report_texts[1],
              "High: confctl/schema.py loads the wrong YAML node, so schema-backed commands fail on valid configs.");

    EXPECT_EQ(observer.all_text(),
              "High: confctl/schema.py loads the wrong YAML node, so schema-backed commands fail on valid configs.");
    ASSERT_EQ(engine.messages().size(), 4u);
    EXPECT_EQ(engine.messages()[1].role, Role::Assistant);
    EXPECT_EQ(engine.messages()[2].role, Role::ToolResult);
    EXPECT_EQ(engine.messages()[3].role, Role::Assistant);
}

TEST(QueryEngine, RecoversWithToolsAfterForcedFinalAnswerValidationFails) {
    auto echo_owned = std::make_unique<QEEchoTool>();
    QEEchoTool* raw_echo = echo_owned.get();
    auto validator_owned = std::make_unique<QEReviewValidatorTool>();
    QEReviewValidatorTool* raw_validator = validator_owned.get();

    ToolRegistry registry;
    registry.register_tool(std::move(echo_owned));
    registry.register_tool(std::move(validator_owned));

    auto mock_owned = std::make_unique<QEMockProvider>();
    mock_owned->responses = {
        QECannedResponse{
            "",
            {ToolUseContent{"call-1", "echo", nlohmann::json{{"path", "confctl/cli.py"}}}},
            {1, 1, 0}
        },
        QECannedResponse{
            "High: confctl has loader problems.",
            {},
            {2, 3, 0}
        },
        QECannedResponse{
            "High: confctl has loader problems.",
            {},
            {1, 2, 0}
        },
        QECannedResponse{
            "",
            {ToolUseContent{"call-2", "echo", nlohmann::json{{"path", "confctl/loader.py"}}}},
            {1, 1, 0}
        },
        QECannedResponse{
            "High: confctl/schema.py loads the wrong YAML node, so schema-backed commands fail on valid configs.",
            {},
            {1, 2, 0}
        },
        QECannedResponse{
            "High: confctl/schema.py loads the wrong YAML node, so schema-backed commands fail on valid configs.",
            {},
            {1, 2, 0}
        },
    };
    QEMockProvider* mock_raw = mock_owned.get();

    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 1;
    cfg.enforce_evidence_based_final_answer = true;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);
    engine.submit("do a code audit");

    ASSERT_EQ(mock_raw->call_count, 6u);
    ASSERT_EQ(mock_raw->requests.size(), 6u);
    EXPECT_GT(mock_raw->requests[0].tool_count, 0u);
    EXPECT_EQ(mock_raw->requests[1].tool_count, 0u);
    EXPECT_EQ(mock_raw->requests[2].tool_count, 0u);
    EXPECT_GT(mock_raw->requests[3].tool_count, 0u);
    EXPECT_GT(mock_raw->requests[4].tool_count, 0u);
    EXPECT_EQ(mock_raw->requests[5].tool_count, 0u);
    EXPECT_NE(mock_raw->requests[1].system_prompt.find("You have finished using tools for this request."),
              std::string::npos);
    EXPECT_NE(mock_raw->requests[3].system_prompt.find("continuing a systematic code audit after deterministic validation failed"),
              std::string::npos);
    ASSERT_TRUE(mock_raw->requests[3].tool_choice.has_value());
    EXPECT_EQ(*mock_raw->requests[3].tool_choice, "required");
    ASSERT_TRUE(mock_raw->requests[4].tool_choice.has_value());
    EXPECT_EQ(*mock_raw->requests[4].tool_choice, "auto");

    EXPECT_EQ(raw_echo->call_count, 2);
    EXPECT_EQ(raw_validator->call_count, 2);
    ASSERT_EQ(raw_validator->report_texts.size(), 2u);
    EXPECT_EQ(raw_validator->report_texts[0], "High: confctl has loader problems.");
    EXPECT_EQ(raw_validator->report_texts[1],
              "High: confctl/schema.py loads the wrong YAML node, so schema-backed commands fail on valid configs.");

    EXPECT_EQ(observer.all_text(),
              "High: confctl/schema.py loads the wrong YAML node, so schema-backed commands fail on valid configs.");
    ASSERT_EQ(engine.messages().size(), 6u);
    EXPECT_EQ(engine.messages()[1].role, Role::Assistant);
    EXPECT_EQ(engine.messages()[2].role, Role::ToolResult);
    EXPECT_EQ(engine.messages()[3].role, Role::Assistant);
    EXPECT_EQ(engine.messages()[4].role, Role::ToolResult);
    EXPECT_EQ(engine.messages()[5].role, Role::Assistant);
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

TEST(QueryEngine, RetriesWithoutToolsAfterForcedFinalAnswerEmptyTurn) {
    auto echo_owned = std::make_unique<QEEchoTool>();
    QEEchoTool* raw_echo = echo_owned.get();

    ToolRegistry registry;
    registry.register_tool(std::move(echo_owned));

    auto mock_owned = std::make_unique<QEMockProvider>();
    mock_owned->responses = {
        QECannedResponse{"", {ToolUseContent{"call-1", "echo", nlohmann::json::object()}}, {1, 2, 0}},
        QECannedResponse{"", {}, {3, 4, 0}},
        QECannedResponse{"Final audit summary", {}, {5, 6, 0}}
    };
    QEMockProvider* mock_raw = mock_owned.get();

    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 1;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);
    engine.submit("do a code audit");

    ASSERT_EQ(mock_raw->call_count, 3u);
    ASSERT_EQ(mock_raw->requests.size(), 3u);
    EXPECT_GT(mock_raw->requests[0].tool_count, 0u);
    EXPECT_EQ(mock_raw->requests[1].tool_count, 0u);
    EXPECT_NE(
        mock_raw->requests[1].system_prompt.find("You have finished using tools for this request."),
        std::string::npos);
    EXPECT_EQ(mock_raw->requests[2].tool_count, 0u);
    EXPECT_NE(
        mock_raw->requests[2].system_prompt.find("The previous attempt returned no usable answer."),
        std::string::npos);

    EXPECT_EQ(observer.all_text(), "Final audit summary");
    EXPECT_EQ(observer.count_done(), 1);
    EXPECT_EQ(raw_echo->call_count, 1);

    int error_count = 0;
    for (const auto& event : observer.events) {
        if (std::holds_alternative<ErrorEvent>(event)) {
            ++error_count;
        }
    }
    EXPECT_EQ(error_count, 0);

    ASSERT_EQ(engine.messages().size(), 4u);
    EXPECT_EQ(engine.messages()[3].role, Role::Assistant);
}

TEST(QueryEngine, RetriesWithoutToolsAfterForcedFinalAnswerToolCallTurn) {
    auto echo_owned = std::make_unique<QEEchoTool>();
    QEEchoTool* raw_echo = echo_owned.get();

    ToolRegistry registry;
    registry.register_tool(std::move(echo_owned));

    auto mock_owned = std::make_unique<QEMockProvider>();
    mock_owned->responses = {
        QECannedResponse{"", {ToolUseContent{"call-1", "echo", nlohmann::json::object()}}, {1, 2, 0}},
        QECannedResponse{"", {ToolUseContent{"call-2", "echo", nlohmann::json::object()}}, {3, 4, 0}},
        QECannedResponse{"Final audit summary", {}, {5, 6, 0}}
    };
    QEMockProvider* mock_raw = mock_owned.get();

    QEAllowAllDelegate delegate;
    QECollectingObserver observer;

    QueryEngineConfig cfg;
    cfg.max_turns = 1;

    PermissionChecker checker(delegate);
    QueryEngine engine(std::move(mock_owned), registry, checker, observer, cfg);
    engine.submit("do a code audit");

    ASSERT_EQ(mock_raw->call_count, 3u);
    ASSERT_EQ(mock_raw->requests.size(), 3u);
    EXPECT_GT(mock_raw->requests[0].tool_count, 0u);
    EXPECT_EQ(mock_raw->requests[1].tool_count, 0u);
    EXPECT_EQ(mock_raw->requests[2].tool_count, 0u);
    ASSERT_TRUE(mock_raw->requests[1].max_tokens.has_value());
    EXPECT_EQ(*mock_raw->requests[1].max_tokens, 4096);
    ASSERT_TRUE(mock_raw->requests[2].max_tokens.has_value());
    EXPECT_EQ(*mock_raw->requests[2].max_tokens, 4096);

    ASSERT_FALSE(mock_raw->requests[1].messages.empty());
    EXPECT_EQ(mock_raw->requests[1].messages.back().role, Role::User);
    const auto* forced_final_text =
        std::get_if<TextContent>(&mock_raw->requests[1].messages.back().content[0].data);
    ASSERT_NE(forced_final_text, nullptr);
    EXPECT_NE(forced_final_text->text.find("produce the final user-facing answer now"),
              std::string::npos);

    bool saw_rewritten_tool_evidence = false;
    bool saw_tool_result_role = false;
    bool saw_assistant_tool_call = false;
    for (const auto& message : mock_raw->requests[1].messages) {
        if (message.role == Role::ToolResult) {
            saw_tool_result_role = true;
        }
        if (message.role == Role::User && !message.content.empty()) {
            if (const auto* text = std::get_if<TextContent>(&message.content[0].data)) {
                if (text->text.find("Evidence gathered from previous workspace inspection")
                    != std::string::npos) {
                    saw_rewritten_tool_evidence = true;
                }
            }
        }
        if (message.role != Role::Assistant) {
            continue;
        }
        for (const auto& block : message.content) {
            if (std::get_if<ToolUseContent>(&block.data) != nullptr) {
                saw_assistant_tool_call = true;
                break;
            }
        }
    }
    EXPECT_TRUE(saw_rewritten_tool_evidence);
    EXPECT_FALSE(saw_tool_result_role);
    EXPECT_FALSE(saw_assistant_tool_call);

    ASSERT_FALSE(mock_raw->requests[2].messages.empty());
    EXPECT_EQ(mock_raw->requests[2].messages.back().role, Role::User);
    const auto* retry_text =
        std::get_if<TextContent>(&mock_raw->requests[2].messages.back().content[0].data);
    ASSERT_NE(retry_text, nullptr);
    EXPECT_NE(retry_text->text.find("The previous attempt tried to call tools"),
              std::string::npos);

    EXPECT_EQ(observer.all_text(), "Final audit summary");
    EXPECT_EQ(observer.count_done(), 1);
    EXPECT_EQ(raw_echo->call_count, 1);

    int error_count = 0;
    for (const auto& event : observer.events) {
        if (std::holds_alternative<ErrorEvent>(event)) {
            ++error_count;
        }
    }
    EXPECT_EQ(error_count, 0);

    ASSERT_EQ(engine.messages().size(), 4u);
    EXPECT_EQ(engine.messages()[3].role, Role::Assistant);
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
