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

    Usage complete(const CompletionRequest& req,
                   StreamCallback cb,
                   std::atomic<bool>& /*stop*/) override
    {
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

    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);

    mgr.spawn(AgentConfig{AgentType::Plan, "plan task", false, ""});

    const auto& names = raw_mock->last_tool_names;
    EXPECT_NE(std::find(names.begin(), names.end(), "am_read_tool"), names.end());
    EXPECT_EQ(std::find(names.begin(), names.end(), "edit_file"),    names.end());
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
