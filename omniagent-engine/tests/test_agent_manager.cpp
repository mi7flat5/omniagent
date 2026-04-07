#include <gtest/gtest.h>

#include <omni/engine.h>
#include <omni/event.h>
#include <omni/permission.h>
#include <omni/provider.h>
#include <omni/session.h>
#include <omni/tool.h>
#include <omni/types.h>
#include "agents/agent_manager.h"
#include "core/query_engine.h"
#include "permissions/permission_checker.h"
#include "tools/tool_registry.h"

#include <atomic>
#include <thread>
#include <string>
#include <vector>

using namespace omni::engine;

// ---------------------------------------------------------------------------
// Test doubles (prefixed AM_ to avoid ODR collisions)
// ---------------------------------------------------------------------------

class AMAllowAllDelegate : public PermissionDelegate {
public:
    PermissionDecision on_permission_request(const std::string&,
                                              const nlohmann::json&,
                                              const std::string&) override {
        return PermissionDecision::Allow;
    }
};

class AMCollectingObserver : public EventObserver {
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

    int count_done() const {
        int n = 0;
        for (const auto& e : events) {
            if (std::holds_alternative<DoneEvent>(e)) ++n;
        }
        return n;
    }
};

// MockProvider that returns a fixed response
class AMMockProvider : public LLMProvider {
public:
    std::string fixed_response = "agent done";
    int response_delay_ms = 0;

    Usage complete(const CompletionRequest& req,
                   StreamCallback cb,
                   std::atomic<bool>& /*stop*/) override
    {
        if (response_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(response_delay_ms));
        }
        { StreamEventData ev; ev.type = StreamEventType::MessageStart; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::ContentBlockStart;
          ev.index = 0; ev.delta_type = "text"; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::ContentBlockDelta;
          ev.index = 0; ev.delta_type = "text_delta";
          ev.delta_text = fixed_response; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::ContentBlockStop;
          ev.index = 0; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::MessageDelta;
          ev.stop_reason = "end_turn"; ev.usage = Usage{1, 1, 0}; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::MessageStop; cb(ev); }

        // Capture tool names for filter tests
        last_tool_names.clear();
        for (const auto& t : req.tools) {
            if (t.contains("function") && t["function"].contains("name")) {
                last_tool_names.push_back(t["function"]["name"].get<std::string>());
            }
        }

        return {1, 1, 0};
    }

    ModelCapabilities capabilities() const override { return {}; }
    std::string       name()         const override { return "am-mock"; }

    mutable std::vector<std::string> last_tool_names;
};

// A read-only test tool
class AMReadOnlyTool : public Tool {
public:
    std::string    name()         const override { return "am_read_tool"; }
    std::string    description()  const override { return "Read only"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()   const override { return true; }
    bool           is_destructive() const override { return false; }
    ToolCallResult call(const nlohmann::json&) override { return {"read result", false}; }
};

// A write test tool
class AMWriteTool : public Tool {
public:
    std::string    name()         const override { return "am_write_tool"; }
    std::string    description()  const override { return "Write tool"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()   const override { return false; }
    bool           is_destructive() const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"write result", false}; }
};

// An "edit" tool — should be excluded in Plan mode
class AMEditTool : public Tool {
public:
    std::string    name()         const override { return "edit_file"; }
    std::string    description()  const override { return "Edit a file"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()   const override { return false; }
    bool           is_destructive() const override { return false; }
    ToolCallResult call(const nlohmann::json&) override { return {"edited", false}; }
};

class AMNetworkTool : public Tool {
public:
    std::string    name()         const override { return "am_network_tool"; }
    std::string    description()  const override { return "Network tool"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()   const override { return true; }
    bool           is_destructive() const override { return false; }
    bool           is_network()     const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"network result", false}; }
};

class AMWriteFileTool : public Tool {
public:
    std::string    name()         const override { return "write_file"; }
    std::string    description()  const override { return "Write file"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()   const override { return false; }
    bool           is_destructive() const override { return false; }
    ToolCallResult call(const nlohmann::json&) override { return {"wrote file", false}; }
};

class AMBashTool : public Tool {
public:
    std::string    name()         const override { return "bash"; }
    std::string    description()  const override { return "Bash"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()   const override { return false; }
    bool           is_destructive() const override { return true; }
    bool           is_shell()       const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"shell", false}; }
};

class AMPlannerValidateSpecTool : public Tool {
public:
    std::string    name()         const override { return "planner_validate_spec"; }
    std::string    description()  const override { return "Validate spec"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()   const override { return true; }
    bool           is_destructive() const override { return false; }
    bool           is_network()     const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"validated spec", false}; }
};

class AMPlannerValidatePlanTool : public Tool {
public:
    std::string    name()         const override { return "planner_validate_plan"; }
    std::string    description()  const override { return "Validate plan"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()   const override { return true; }
    bool           is_destructive() const override { return false; }
    bool           is_network()     const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"validated plan", false}; }
};

class AMPlannerBuildPlanTool : public Tool {
public:
    std::string    name()         const override { return "planner_build_plan"; }
    std::string    description()  const override { return "Build plan"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()   const override { return false; }
    bool           is_destructive() const override { return false; }
    bool           is_network()     const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"built plan", false}; }
};

class AMPlannerRepairPlanTool : public Tool {
public:
    std::string    name()         const override { return "planner_repair_plan"; }
    std::string    description()  const override { return "Repair plan"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()   const override { return false; }
    bool           is_destructive() const override { return false; }
    bool           is_network()     const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"repaired plan", false}; }
};

class AMPlannerBuildFromIdeaTool : public Tool {
public:
    std::string    name()         const override { return "planner_build_from_idea"; }
    std::string    description()  const override { return "Build plan from idea"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()   const override { return false; }
    bool           is_destructive() const override { return false; }
    bool           is_network()     const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"built from idea", false}; }
};

class AMContextProbeTool : public Tool {
public:
    std::string    name()         const override { return "ctx_probe"; }
    std::string    description()  const override { return "Capture tool context"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()   const override { return true; }
    bool           is_destructive() const override { return false; }

    ToolCallResult call(const nlohmann::json&, const ToolContext& context) override {
        called = true;
        last_context = context;
        return {"context captured", false};
    }

    ToolCallResult call(const nlohmann::json&) override {
        return {"missing context", true};
    }

    bool called = false;
    ToolContext last_context;
};

class AMToolCallProvider : public LLMProvider {
public:
    explicit AMToolCallProvider(std::string tool_name)
        : tool_name_(std::move(tool_name)) {}

    Usage complete(const CompletionRequest&,
                   StreamCallback cb,
                   std::atomic<bool>& stop_flag) override {
        if (stop_flag.load(std::memory_order_relaxed)) {
            return {};
        }

        {
            StreamEventData event;
            event.type = StreamEventType::MessageStart;
            cb(event);
        }

        if (call_count_++ == 0) {
            {
                StreamEventData event;
                event.type = StreamEventType::ContentBlockStart;
                event.index = 0;
                event.delta_type = "tool_use";
                event.tool_id = "ctx-1";
                event.tool_name = tool_name_;
                cb(event);
            }
            {
                StreamEventData event;
                event.type = StreamEventType::ContentBlockDelta;
                event.index = 0;
                event.delta_type = "input_json_delta";
                event.tool_input_delta = nlohmann::json::object();
                cb(event);
            }
            {
                StreamEventData event;
                event.type = StreamEventType::ContentBlockStop;
                event.index = 0;
                event.tool_input = nlohmann::json::object();
                cb(event);
            }
            {
                StreamEventData event;
                event.type = StreamEventType::MessageDelta;
                event.stop_reason = "tool_use";
                event.usage = {1, 1, 0};
                cb(event);
            }
        } else {
            {
                StreamEventData event;
                event.type = StreamEventType::ContentBlockStart;
                event.index = 0;
                event.delta_type = "text";
                cb(event);
            }
            {
                StreamEventData event;
                event.type = StreamEventType::ContentBlockDelta;
                event.index = 0;
                event.delta_type = "text_delta";
                event.delta_text = "tool complete";
                cb(event);
            }
            {
                StreamEventData event;
                event.type = StreamEventType::ContentBlockStop;
                event.index = 0;
                cb(event);
            }
            {
                StreamEventData event;
                event.type = StreamEventType::MessageDelta;
                event.stop_reason = "end_turn";
                event.usage = {1, 1, 0};
                cb(event);
            }
        }

        {
            StreamEventData event;
            event.type = StreamEventType::MessageStop;
            cb(event);
        }
        return {1, 1, 0};
    }

    ModelCapabilities capabilities() const override { return {}; }
    std::string       name()         const override { return "am-tool-call"; }

private:
    std::string tool_name_;
    int call_count_ = 0;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Engine make_engine(AMMockProvider** out_provider = nullptr) {
    auto mock = std::make_unique<AMMockProvider>();
    if (out_provider) *out_provider = mock.get();

    Config cfg;
    cfg.max_turns = 3;
    return Engine(cfg, std::move(mock));
}

static Engine make_tool_call_engine(const std::string& tool_name) {
    auto mock = std::make_unique<AMToolCallProvider>(tool_name);

    Config cfg;
    cfg.max_turns = 3;
    return Engine(cfg, std::move(mock));
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// SpawnForegroundAgent: spawn general agent, verify it completes with a response.
TEST(AgentManager, SpawnForegroundAgent) {
    auto engine = make_engine();

    AMAllowAllDelegate   delegate;
    AMCollectingObserver observer;

    AgentManager mgr(engine, delegate);

    AgentResult captured;
    bool        got_result = false;

    const std::string agent_id = mgr.spawn(
        AgentConfig{AgentType::GeneralPurpose, "do something", false, ""},
        [&](const AgentResult& r) {
            captured   = r;
            got_result = true;
        });

    EXPECT_FALSE(agent_id.empty());
    EXPECT_TRUE(got_result);
    EXPECT_EQ(captured.agent_id, agent_id);
    EXPECT_EQ(captured.response, "agent done");
    EXPECT_FALSE(captured.is_error);

    // Agent should no longer be running after foreground spawn completes.
    EXPECT_FALSE(mgr.is_running(agent_id));

    // Result should be retrievable.
    auto result = mgr.get_result(agent_id);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->response, "agent done");
}

// SpawnBackgroundAgent: spawn background, verify returns immediately (agent still running or
// quickly completes), result available after waiting.
TEST(AgentManager, SpawnBackgroundAgent) {
    auto engine = make_engine();

    AMAllowAllDelegate delegate;

    AgentManager mgr(engine, delegate);

    std::atomic<bool> callback_fired{false};
    AgentResult       captured;

    AgentConfig cfg;
    cfg.type             = AgentType::GeneralPurpose;
    cfg.task             = "background task";
    cfg.run_in_background = true;

    const std::string agent_id = mgr.spawn(
        cfg,
        [&](const AgentResult& r) {
            captured       = r;
            callback_fired = true;
        });

    EXPECT_FALSE(agent_id.empty());

    // Wait for the background agent to finish (poll with timeout).
    for (int i = 0; i < 100 && !callback_fired.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_TRUE(callback_fired.load());
    EXPECT_EQ(captured.response, "agent done");
    EXPECT_FALSE(mgr.is_running(agent_id));
}

// SendMessageToAgent: spawn agent, send follow-up message, verify send_message returns true.
TEST(AgentManager, SendMessageToAgent) {
    auto engine = make_engine();

    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);

    // Foreground spawn so we know it's done before sending.
    const std::string agent_id = mgr.spawn(
        AgentConfig{AgentType::GeneralPurpose, "initial task", false, ""});

    // send_message should succeed (agent session exists).
    const bool ok = mgr.send_message(agent_id, "follow up");
    EXPECT_TRUE(ok);

    // Wait for the follow-up task to drain before the engine is torn down.
    mgr.wait_for(agent_id);

    // Invalid ID should return false.
    EXPECT_FALSE(mgr.send_message("nonexistent-id", "message"));
}

TEST(AgentManager, SendMessageWaitsForBackgroundAgentTurn) {
    AMMockProvider* raw_mock = nullptr;
    auto engine = make_engine(&raw_mock);
    raw_mock->response_delay_ms = 75;

    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);

    const std::string agent_id = mgr.spawn(
        AgentConfig{AgentType::GeneralPurpose, "initial task", true, ""});

    EXPECT_NO_THROW({
        const bool ok = mgr.send_message(agent_id, "follow up");
        EXPECT_TRUE(ok);
    });

    EXPECT_TRUE(mgr.wait_for(agent_id));
}

// SpawnExploreAgent: only read-only tools included in requests.
TEST(AgentManager, SpawnExploreAgentToolFilter) {
    AMMockProvider* raw_mock = nullptr;
    auto engine = make_engine(&raw_mock);

    // Register a read-only and a write tool on the engine.
    engine.register_tool(std::make_unique<AMReadOnlyTool>());
    engine.register_tool(std::make_unique<AMWriteTool>());

    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);

    mgr.spawn(AgentConfig{AgentType::Explore, "explore task", false, ""});

    // Only the read-only tool (and session tools) should have been sent.
    // am_write_tool should NOT be present.
    const auto& names = raw_mock->last_tool_names;
    EXPECT_NE(std::find(names.begin(), names.end(), "am_read_tool"),  names.end());
    EXPECT_EQ(std::find(names.begin(), names.end(), "am_write_tool"), names.end());
}

// SpawnPlanAgent: edit tools excluded.
TEST(AgentManager, SpawnPlanAgentToolFilter) {
    AMMockProvider* raw_mock = nullptr;
    auto engine = make_engine(&raw_mock);

    engine.register_tool(std::make_unique<AMReadOnlyTool>());
    engine.register_tool(std::make_unique<AMEditTool>());
    engine.register_tool(std::make_unique<AMPlannerValidatePlanTool>());
    engine.register_tool(std::make_unique<AMPlannerRepairPlanTool>());
    engine.register_tool(std::make_unique<AMPlannerBuildPlanTool>());
    engine.register_tool(std::make_unique<AMPlannerBuildFromIdeaTool>());

    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);

    mgr.spawn(AgentConfig{AgentType::Plan, "plan task", false, ""});

    const auto& names = raw_mock->last_tool_names;
    EXPECT_NE(std::find(names.begin(), names.end(), "am_read_tool"), names.end());
    EXPECT_EQ(std::find(names.begin(), names.end(), "edit_file"),    names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "planner_validate_plan"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "planner_repair_plan"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "planner_build_plan"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "planner_build_from_idea"), names.end());
}

TEST(AgentManager, SpawnResearchAgentIncludesNetworkTools) {
    AMMockProvider* raw_mock = nullptr;
    auto engine = make_engine(&raw_mock);

    engine.register_tool(std::make_unique<AMReadOnlyTool>());
    engine.register_tool(std::make_unique<AMNetworkTool>());
    engine.register_tool(std::make_unique<AMWriteTool>());

    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);

    mgr.spawn(AgentConfig{AgentType::Research, "research task", false, ""});

    const auto& names = raw_mock->last_tool_names;
    EXPECT_NE(std::find(names.begin(), names.end(), "am_read_tool"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "am_network_tool"), names.end());
    EXPECT_EQ(std::find(names.begin(), names.end(), "am_write_tool"), names.end());
}

TEST(AgentManager, SpawnSpecAgentAllowsWorkflowTools) {
    AMMockProvider* raw_mock = nullptr;
    auto engine = make_engine(&raw_mock);

    engine.register_tool(std::make_unique<AMReadOnlyTool>());
    engine.register_tool(std::make_unique<AMNetworkTool>());
    engine.register_tool(std::make_unique<AMWriteTool>());
    engine.register_tool(std::make_unique<AMEditTool>());
    engine.register_tool(std::make_unique<AMWriteFileTool>());
    engine.register_tool(std::make_unique<AMBashTool>());
    engine.register_tool(std::make_unique<AMPlannerValidateSpecTool>());

    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);

    mgr.spawn(AgentConfig{AgentType::Spec, "write a spec", false, ""});

    const auto& names = raw_mock->last_tool_names;
    EXPECT_NE(std::find(names.begin(), names.end(), "am_read_tool"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "am_network_tool"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "edit_file"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "write_file"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "bash"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "planner_validate_spec"), names.end());
    EXPECT_EQ(std::find(names.begin(), names.end(), "am_write_tool"), names.end());
}

TEST(AgentManager, ParentToolContextIsPropagatedToChildren) {
    auto engine = make_tool_call_engine("ctx_probe");

    auto probe = std::make_unique<AMContextProbeTool>();
    AMContextProbeTool* raw_probe = probe.get();
    engine.register_tool(std::move(probe));

    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);
    mgr.set_parent_tool_context(ToolContext{
        .project_id = "project-123",
        .session_id = "session-456",
        .run_id = "run-789",
        .profile = "coordinator",
        .workspace_root = "/tmp/omniagent-workspace",
        .working_dir = "/tmp/omniagent-workspace/src",
    });

    const auto agent_id = mgr.spawn(AgentConfig{AgentType::Explore, "inspect context", false, ""});

    ASSERT_FALSE(agent_id.empty());
    ASSERT_TRUE(raw_probe->called);
    EXPECT_EQ(raw_probe->last_context.project_id, "project-123");
    EXPECT_EQ(raw_probe->last_context.session_id, "session-456");
    EXPECT_EQ(raw_probe->last_context.run_id, "run-789");
    EXPECT_EQ(raw_probe->last_context.profile, "explore");
    EXPECT_EQ(raw_probe->last_context.workspace_root, "/tmp/omniagent-workspace");
    EXPECT_EQ(raw_probe->last_context.working_dir, "/tmp/omniagent-workspace/src");
}

// ToolFilter: set_tool_filter on QueryEngine directly, verify filtered tools in request.
TEST(AgentManager, SessionToolFilter) {
    AMMockProvider* raw_mock = nullptr;
    auto engine = make_engine(&raw_mock);

    engine.register_tool(std::make_unique<AMReadOnlyTool>());
    engine.register_tool(std::make_unique<AMWriteTool>());

    AMAllowAllDelegate   delegate;
    AMCollectingObserver observer;

    auto session = engine.create_session(observer, delegate);
    session->set_tool_filter({"am_read_tool"});
    session->submit("test");
    session->wait();

    // Only am_read_tool should appear in the request.
    const auto& names = raw_mock->last_tool_names;
    EXPECT_NE(std::find(names.begin(), names.end(), "am_read_tool"),  names.end());
    EXPECT_EQ(std::find(names.begin(), names.end(), "am_write_tool"), names.end());
}

// AgentCount: active_count reflects running agents.
TEST(AgentManager, ActiveCountDecreases) {
    auto engine = make_engine();
    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);

    // After foreground spawn, count should be 0 again.
    EXPECT_EQ(mgr.active_count(), 0u);
    mgr.spawn(AgentConfig{AgentType::GeneralPurpose, "task", false, ""});
    EXPECT_EQ(mgr.active_count(), 0u);  // done
}
