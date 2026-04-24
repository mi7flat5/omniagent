#include <gtest/gtest.h>

#include <omni/engine.h>
#include <omni/event.h>
#include <omni/permission.h>
#include <omni/provider.h>
#include <omni/session.h>
#include <omni/tool.h>
#include <omni/types.h>
#include "agents/agent_tool.h"
#include "agents/agent_manager.h"
#include "core/query_engine.h"
#include "permissions/permission_checker.h"
#include "project_runtime_internal.h"
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

class AMCountingDelegate : public PermissionDelegate {
public:
    PermissionDecision next_decision = PermissionDecision::Allow;
    int call_count = 0;

    PermissionDecision on_permission_request(const std::string&,
                                             const nlohmann::json&,
                                             const std::string&) override {
        ++call_count;
        return next_decision;
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
        last_system_prompt = req.system_prompt;
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
        tool_name_history.push_back(last_tool_names);

        return {1, 1, 0};
    }

    ModelCapabilities capabilities() const override { return {}; }
    std::string       name()         const override { return "am-mock"; }

    mutable std::string last_system_prompt;
    mutable std::vector<std::string> last_tool_names;
    mutable std::vector<std::vector<std::string>> tool_name_history;
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

class AMPlannerValidateReviewTool : public Tool {
public:
    std::string    name()         const override { return "planner_validate_review"; }
    std::string    description()  const override { return "Validate review"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()   const override { return true; }
    bool           is_destructive() const override { return false; }
    bool           is_network()     const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"validated review", false}; }
};

class AMPlannerValidateBugfixTool : public Tool {
public:
    std::string    name()         const override { return "planner_validate_bugfix"; }
    std::string    description()  const override { return "Validate bugfix"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()   const override { return true; }
    bool           is_destructive() const override { return false; }
    bool           is_network()     const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"validated bugfix", false}; }
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

class AMInterruptibleProvider : public LLMProvider {
public:
    Usage complete(const CompletionRequest&,
                   StreamCallback cb,
                   std::atomic<bool>& stop_flag) override {
        entered.store(true, std::memory_order_relaxed);

        StreamEventData start;
        start.type = StreamEventType::MessageStart;
        cb(start);

        for (int index = 0; index < 400; ++index) {
            if (stop_flag.load(std::memory_order_relaxed)) {
                saw_stop.store(true, std::memory_order_relaxed);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        StreamEventData text_start;
        text_start.type = StreamEventType::ContentBlockStart;
        text_start.index = 0;
        text_start.delta_type = "text";
        cb(text_start);

        StreamEventData text_stop;
        text_stop.type = StreamEventType::ContentBlockStop;
        text_stop.index = 0;
        cb(text_stop);

        StreamEventData delta;
        delta.type = StreamEventType::MessageDelta;
        delta.stop_reason = "end_turn";
        delta.usage = {1, 0, 0};
        cb(delta);

        StreamEventData stop;
        stop.type = StreamEventType::MessageStop;
        cb(stop);
        return {1, 0, 0};
    }

    ModelCapabilities capabilities() const override { return {}; }
    std::string name() const override { return "am-interruptible"; }

    std::atomic<bool> entered{false};
    std::atomic<bool> saw_stop{false};
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

static Engine make_interruptible_engine(AMInterruptibleProvider** out_provider = nullptr) {
    auto mock = std::make_unique<AMInterruptibleProvider>();
    if (out_provider) {
        *out_provider = mock.get();
    }

    Config cfg;
    cfg.max_turns = 3;
    return Engine(cfg, std::move(mock));
}

static bool has_name(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

static const AgentProfileManifest* lookup_profile(
    const std::vector<AgentProfileManifest>& profiles,
    const std::string& name) {
    const auto it = std::find_if(
        profiles.begin(),
        profiles.end(),
        [&](const AgentProfileManifest& profile) {
            return profile.name == name;
        });
    return it == profiles.end() ? nullptr : &(*it);
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

TEST(AgentManager, StopAllRunningRequestsStopOnBackgroundAgents) {
    AMInterruptibleProvider* raw_provider = nullptr;
    auto engine = make_interruptible_engine(&raw_provider);

    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);

    const std::string agent_id = mgr.spawn(
        AgentConfig{AgentType::GeneralPurpose, "interruptible task", true, ""});

    for (int index = 0; index < 100 && !raw_provider->entered.load(std::memory_order_relaxed); ++index) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ASSERT_TRUE(raw_provider->entered.load(std::memory_order_relaxed));
    EXPECT_TRUE(mgr.is_running(agent_id));

    mgr.stop_all_running();

    EXPECT_TRUE(mgr.wait_for(agent_id));
    EXPECT_TRUE(raw_provider->saw_stop.load(std::memory_order_relaxed));
    EXPECT_FALSE(mgr.is_running(agent_id));
}

TEST(AgentManager, CancelAllRunningRequestsStopOnBackgroundAgents) {
    AMInterruptibleProvider* raw_provider = nullptr;
    auto engine = make_interruptible_engine(&raw_provider);

    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);

    const std::string agent_id = mgr.spawn(
        AgentConfig{AgentType::GeneralPurpose, "interruptible task", true, ""});

    for (int index = 0; index < 100 && !raw_provider->entered.load(std::memory_order_relaxed); ++index) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ASSERT_TRUE(raw_provider->entered.load(std::memory_order_relaxed));
    EXPECT_TRUE(mgr.is_running(agent_id));

    mgr.cancel_all_running();

    EXPECT_TRUE(mgr.wait_for(agent_id));
    EXPECT_TRUE(raw_provider->saw_stop.load(std::memory_order_relaxed));
    EXPECT_FALSE(mgr.is_running(agent_id));
}

TEST(AgentManager, DefaultProfilesExposeWorkflowFamilies) {
    const auto profiles = default_profiles();

    const auto* coordinator = lookup_profile(profiles, "coordinator");
    const auto* explore = lookup_profile(profiles, "explore");
    const auto* feature = lookup_profile(profiles, "feature");
    const auto* refactor = lookup_profile(profiles, "refactor");
    const auto* audit = lookup_profile(profiles, "audit");
    const auto* research = lookup_profile(profiles, "research");
    const auto* bugfix = lookup_profile(profiles, "bugfix");

    ASSERT_NE(coordinator, nullptr);
    ASSERT_NE(explore, nullptr);
    ASSERT_NE(feature, nullptr);
    ASSERT_NE(refactor, nullptr);
    ASSERT_NE(audit, nullptr);
    ASSERT_NE(research, nullptr);
    ASSERT_NE(bugfix, nullptr);

    EXPECT_EQ(explore->default_permission_mode, PermissionMode::Plan);
    EXPECT_EQ(feature->default_permission_mode, PermissionMode::AcceptEdits);
    EXPECT_EQ(refactor->default_permission_mode, PermissionMode::AcceptEdits);
    EXPECT_EQ(audit->default_permission_mode, PermissionMode::Plan);
    EXPECT_EQ(research->default_permission_mode, PermissionMode::Plan);
    EXPECT_EQ(bugfix->default_permission_mode, PermissionMode::AcceptEdits);

    EXPECT_FALSE(feature->tool_policy.allow_network);
    EXPECT_FALSE(feature->tool_policy.allow_destructive);
    EXPECT_TRUE(has_name(feature->tool_policy.explicit_allow, "bash"));
    EXPECT_TRUE(has_name(feature->tool_policy.explicit_allow, "planner_build_plan"));
    EXPECT_TRUE(has_name(refactor->tool_policy.explicit_allow, "planner_build_from_idea"));
    EXPECT_FALSE(audit->tool_policy.allow_write);
    EXPECT_FALSE(audit->tool_policy.allow_network);
    EXPECT_TRUE(has_name(audit->tool_policy.explicit_allow, "planner_validate_review"));
    EXPECT_FALSE(bugfix->tool_policy.allow_destructive);
    EXPECT_TRUE(has_name(bugfix->tool_policy.explicit_allow, "bash"));
    EXPECT_TRUE(has_name(bugfix->tool_policy.explicit_allow, "planner_validate_bugfix"));

    EXPECT_NE(coordinator->system_prompt.find("audit agents"), std::string::npos);
    EXPECT_NE(coordinator->system_prompt.find("bugfix agents"), std::string::npos);
    EXPECT_NE(coordinator->system_prompt.find("refactor agents"), std::string::npos);
    EXPECT_NE(coordinator->system_prompt.find("feature agents"), std::string::npos);
    EXPECT_NE(coordinator->system_prompt.find("graph execution path"), std::string::npos);
    EXPECT_NE(coordinator->system_prompt.find("avoid open-ended sweeps"), std::string::npos);
    EXPECT_NE(explore->system_prompt.find("minimum tool calls needed to answer"), std::string::npos);
    EXPECT_NE(audit->system_prompt.find("map the relevant repository surface"), std::string::npos);
    EXPECT_NE(audit->system_prompt.find("answer as soon as the current evidence supports a conclusion"), std::string::npos);
    EXPECT_NE(audit->system_prompt.find("Do not pad the answer with generic sections"), std::string::npos);
    EXPECT_NE(audit->system_prompt.find("Only state counts or percentages when they come from explicit tool output"), std::string::npos);
    EXPECT_NE(research->system_prompt.find("focused source collection"), std::string::npos);
}

TEST(AgentManager, NewWorkflowTypesEmitExpectedSpawnProfiles) {
    auto engine = make_engine();

    AMAllowAllDelegate delegate;
    AMCollectingObserver observer;
    AgentManager mgr(engine, delegate, observer);

    mgr.spawn(AgentConfig{AgentType::Feature, "feature task", false, ""});
    mgr.spawn(AgentConfig{AgentType::Refactor, "refactor task", false, ""});
    mgr.spawn(AgentConfig{AgentType::Audit, "audit task", false, ""});
    mgr.spawn(AgentConfig{AgentType::Bugfix, "bugfix task", false, ""});

    const auto saw_profile = [&](const std::string& profile) {
        return std::any_of(observer.events.begin(), observer.events.end(),
                           [&](const Event& event) {
                               if (const auto* spawned = std::get_if<AgentSpawnedEvent>(&event)) {
                                   return spawned->profile == profile;
                               }
                               return false;
                           });
    };

    EXPECT_TRUE(saw_profile("feature"));
    EXPECT_TRUE(saw_profile("refactor"));
    EXPECT_TRUE(saw_profile("audit"));
    EXPECT_TRUE(saw_profile("bugfix"));
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
    engine.register_tool(std::make_unique<AMPlannerValidateReviewTool>());

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

TEST(AgentManager, SpawnFeatureAgentToolFilter) {
    AMMockProvider* raw_mock = nullptr;
    auto engine = make_engine(&raw_mock);

    engine.register_tool(std::make_unique<AMReadOnlyTool>());
    engine.register_tool(std::make_unique<AMNetworkTool>());
    engine.register_tool(std::make_unique<AMWriteTool>());
    engine.register_tool(std::make_unique<AMEditTool>());
    engine.register_tool(std::make_unique<AMWriteFileTool>());
    engine.register_tool(std::make_unique<AMBashTool>());
    engine.register_tool(std::make_unique<AMPlannerValidateSpecTool>());
    engine.register_tool(std::make_unique<AMPlannerValidatePlanTool>());
    engine.register_tool(std::make_unique<AMPlannerRepairPlanTool>());
    engine.register_tool(std::make_unique<AMPlannerBuildPlanTool>());
    engine.register_tool(std::make_unique<AMPlannerBuildFromIdeaTool>());

    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);

    mgr.spawn(AgentConfig{AgentType::Feature, "feature task", false, ""});

    const auto& names = raw_mock->last_tool_names;
    EXPECT_TRUE(has_name(names, "am_read_tool"));
    EXPECT_FALSE(has_name(names, "am_network_tool"));
    EXPECT_FALSE(has_name(names, "am_write_tool"));
    EXPECT_TRUE(has_name(names, "edit_file"));
    EXPECT_TRUE(has_name(names, "write_file"));
    EXPECT_TRUE(has_name(names, "bash"));
    EXPECT_TRUE(has_name(names, "planner_validate_spec"));
    EXPECT_TRUE(has_name(names, "planner_validate_plan"));
    EXPECT_TRUE(has_name(names, "planner_repair_plan"));
    EXPECT_TRUE(has_name(names, "planner_build_plan"));
    EXPECT_TRUE(has_name(names, "planner_build_from_idea"));
}

TEST(AgentManager, SpawnRefactorAgentToolFilter) {
    AMMockProvider* raw_mock = nullptr;
    auto engine = make_engine(&raw_mock);

    engine.register_tool(std::make_unique<AMReadOnlyTool>());
    engine.register_tool(std::make_unique<AMNetworkTool>());
    engine.register_tool(std::make_unique<AMEditTool>());
    engine.register_tool(std::make_unique<AMWriteFileTool>());
    engine.register_tool(std::make_unique<AMBashTool>());
    engine.register_tool(std::make_unique<AMPlannerValidateSpecTool>());
    engine.register_tool(std::make_unique<AMPlannerBuildPlanTool>());

    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);

    mgr.spawn(AgentConfig{AgentType::Refactor, "refactor task", false, ""});

    const auto& names = raw_mock->last_tool_names;
    EXPECT_TRUE(has_name(names, "am_read_tool"));
    EXPECT_FALSE(has_name(names, "am_network_tool"));
    EXPECT_TRUE(has_name(names, "edit_file"));
    EXPECT_TRUE(has_name(names, "write_file"));
    EXPECT_TRUE(has_name(names, "bash"));
    EXPECT_TRUE(has_name(names, "planner_validate_spec"));
    EXPECT_TRUE(has_name(names, "planner_build_plan"));
}

TEST(AgentManager, SpawnAuditAgentToolFilter) {
    AMMockProvider* raw_mock = nullptr;
    auto engine = make_engine(&raw_mock);

    engine.register_tool(std::make_unique<AMReadOnlyTool>());
    engine.register_tool(std::make_unique<AMNetworkTool>());
    engine.register_tool(std::make_unique<AMWriteTool>());
    engine.register_tool(std::make_unique<AMEditTool>());
    engine.register_tool(std::make_unique<AMWriteFileTool>());
    engine.register_tool(std::make_unique<AMBashTool>());
    engine.register_tool(std::make_unique<AMPlannerValidateSpecTool>());
    engine.register_tool(std::make_unique<AMPlannerValidateReviewTool>());

    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);

    mgr.spawn(AgentConfig{AgentType::Audit, "audit task", false, ""});

    const auto history_it = std::find_if(
        raw_mock->tool_name_history.begin(),
        raw_mock->tool_name_history.end(),
        [](const std::vector<std::string>& names) {
            return has_name(names, "am_read_tool");
        });
    ASSERT_NE(history_it, raw_mock->tool_name_history.end());

    const auto& names = *history_it;
    EXPECT_TRUE(has_name(names, "am_read_tool"));
    EXPECT_FALSE(has_name(names, "am_network_tool"));
    EXPECT_FALSE(has_name(names, "am_write_tool"));
    EXPECT_FALSE(has_name(names, "edit_file"));
    EXPECT_FALSE(has_name(names, "write_file"));
    EXPECT_FALSE(has_name(names, "bash"));
    EXPECT_FALSE(has_name(names, "planner_validate_spec"));
    EXPECT_TRUE(has_name(names, "planner_validate_review"));
}

TEST(AgentManager, SpawnBugfixAgentToolFilter) {
    AMMockProvider* raw_mock = nullptr;
    auto engine = make_engine(&raw_mock);

    engine.register_tool(std::make_unique<AMReadOnlyTool>());
    engine.register_tool(std::make_unique<AMNetworkTool>());
    engine.register_tool(std::make_unique<AMWriteTool>());
    engine.register_tool(std::make_unique<AMEditTool>());
    engine.register_tool(std::make_unique<AMWriteFileTool>());
    engine.register_tool(std::make_unique<AMBashTool>());
    engine.register_tool(std::make_unique<AMPlannerValidateBugfixTool>());
    engine.register_tool(std::make_unique<AMPlannerBuildPlanTool>());

    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);

    mgr.spawn(AgentConfig{AgentType::Bugfix, "bugfix task", false, ""});

    const auto& names = raw_mock->last_tool_names;
    EXPECT_TRUE(has_name(names, "am_read_tool"));
    EXPECT_FALSE(has_name(names, "am_network_tool"));
    EXPECT_FALSE(has_name(names, "am_write_tool"));
    EXPECT_TRUE(has_name(names, "edit_file"));
    EXPECT_TRUE(has_name(names, "write_file"));
    EXPECT_TRUE(has_name(names, "bash"));
    EXPECT_TRUE(has_name(names, "planner_validate_bugfix"));
    EXPECT_FALSE(has_name(names, "planner_build_plan"));
}

TEST(AgentManager, ExploreAgentUsesPlanPermissionModeForReadOnlyTools) {
    auto engine = make_tool_call_engine("am_read_tool");
    engine.register_tool(std::make_unique<AMReadOnlyTool>());

    AMCountingDelegate delegate;
    AgentManager mgr(engine, delegate);

    mgr.spawn(AgentConfig{AgentType::Explore, "explore", false, ""});

    EXPECT_EQ(delegate.call_count, 0);
}

TEST(AgentManager, AuditAgentUsesPlanPermissionModeForReadOnlyTools) {
    auto engine = make_tool_call_engine("am_read_tool");
    engine.register_tool(std::make_unique<AMReadOnlyTool>());

    AMCountingDelegate delegate;
    AgentManager mgr(engine, delegate);

    mgr.spawn(AgentConfig{AgentType::Audit, "audit", false, ""});

    EXPECT_EQ(delegate.call_count, 0);
}

TEST(AgentManager, FeatureAgentUsesAcceptEditsPermissionModeForEditTools) {
    auto engine = make_tool_call_engine("edit_file");
    engine.register_tool(std::make_unique<AMEditTool>());

    AMCountingDelegate delegate;
    AgentManager mgr(engine, delegate);

    mgr.spawn(AgentConfig{AgentType::Feature, "feature", false, ""});

    EXPECT_EQ(delegate.call_count, 0);
}

TEST(AgentManager, BugfixAgentUsesAcceptEditsPermissionModeForEditTools) {
    auto engine = make_tool_call_engine("edit_file");
    engine.register_tool(std::make_unique<AMEditTool>());

    AMCountingDelegate delegate;
    AgentManager mgr(engine, delegate);

    mgr.spawn(AgentConfig{AgentType::Bugfix, "bugfix", false, ""});

    EXPECT_EQ(delegate.call_count, 0);
}

TEST(AgentManager, WorkflowPromptsDescribeExpectedBehavior) {
    AMMockProvider* raw_mock = nullptr;
    auto engine = make_engine(&raw_mock);

    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);

    mgr.spawn(AgentConfig{AgentType::Explore, "explore", false, ""});
    EXPECT_NE(raw_mock->last_system_prompt.find("existing-codebase exploration worker"), std::string::npos);

    mgr.spawn(AgentConfig{AgentType::Feature, "feature", false, ""});
    EXPECT_NE(raw_mock->last_system_prompt.find("direct implementation path"), std::string::npos);
    EXPECT_NE(raw_mock->last_system_prompt.find("graph setup"), std::string::npos);

    mgr.spawn(AgentConfig{AgentType::Refactor, "refactor", false, ""});
    EXPECT_NE(raw_mock->last_system_prompt.find("behavior-preserving"), std::string::npos);
    EXPECT_NE(raw_mock->last_system_prompt.find("invariants"), std::string::npos);

    mgr.spawn(AgentConfig{AgentType::Audit, "audit", false, ""});
    EXPECT_NE(raw_mock->last_system_prompt.find("findings first"), std::string::npos);
    EXPECT_NE(raw_mock->last_system_prompt.find("map the relevant repository surface first"), std::string::npos);
    EXPECT_NE(raw_mock->last_system_prompt.find("Do not modify files"), std::string::npos);
    EXPECT_NE(raw_mock->last_system_prompt.find("Do not pad the answer with generic sections"), std::string::npos);
    EXPECT_NE(raw_mock->last_system_prompt.find("Only state counts or percentages when they come from explicit tool output"), std::string::npos);

    mgr.spawn(AgentConfig{AgentType::Bugfix, "bugfix", false, ""});
    EXPECT_NE(raw_mock->last_system_prompt.find("root cause"), std::string::npos);
    EXPECT_NE(raw_mock->last_system_prompt.find("smallest defensible fix"), std::string::npos);
}

TEST(AgentManager, AgentToolRecognizesNewWorkflowTypeStrings) {
    AMMockProvider* raw_mock = nullptr;
    auto engine = make_engine(&raw_mock);

    engine.register_tool(std::make_unique<AMReadOnlyTool>());
    engine.register_tool(std::make_unique<AMNetworkTool>());
    engine.register_tool(std::make_unique<AMEditTool>());
    engine.register_tool(std::make_unique<AMWriteFileTool>());
    engine.register_tool(std::make_unique<AMBashTool>());
    engine.register_tool(std::make_unique<AMPlannerValidateSpecTool>());
    engine.register_tool(std::make_unique<AMPlannerBuildPlanTool>());

    AMAllowAllDelegate delegate;
    AgentManager mgr(engine, delegate);
    AgentTool agent_tool(mgr);

    auto result = agent_tool.call(nlohmann::json{{"task", "audit task"}, {"type", "audit"}});
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(raw_mock->last_system_prompt.find("audit worker"), std::string::npos);
    EXPECT_NE(raw_mock->last_system_prompt.find("map the relevant repository surface first"), std::string::npos);
    EXPECT_NE(raw_mock->last_system_prompt.find("Do not pad the answer with generic sections"), std::string::npos);
    EXPECT_FALSE(has_name(raw_mock->last_tool_names, "edit_file"));

    result = agent_tool.call(nlohmann::json{{"task", "explore task"}, {"type", "explore"}});
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(raw_mock->last_system_prompt.find("existing-codebase exploration worker"), std::string::npos);
    EXPECT_NE(raw_mock->last_system_prompt.find("minimum read-only inspection needed to answer"), std::string::npos);
    EXPECT_FALSE(has_name(raw_mock->last_tool_names, "edit_file"));
    EXPECT_FALSE(has_name(raw_mock->last_tool_names, "write_file"));

    result = agent_tool.call(nlohmann::json{{"task", "feature task"}, {"type", "feature"}});
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(raw_mock->last_system_prompt.find("feature implementation worker"), std::string::npos);
    EXPECT_TRUE(has_name(raw_mock->last_tool_names, "edit_file"));
    EXPECT_TRUE(has_name(raw_mock->last_tool_names, "planner_build_plan"));

    result = agent_tool.call(nlohmann::json{{"task", "refactor task"}, {"type", "refactor"}});
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(raw_mock->last_system_prompt.find("refactor worker"), std::string::npos);
    EXPECT_TRUE(has_name(raw_mock->last_tool_names, "planner_build_plan"));

    result = agent_tool.call(nlohmann::json{{"task", "bugfix task"}, {"type", "bugfix"}});
    EXPECT_FALSE(result.is_error);
    EXPECT_NE(raw_mock->last_system_prompt.find("bugfix worker"), std::string::npos);
    EXPECT_TRUE(has_name(raw_mock->last_tool_names, "edit_file"));
    EXPECT_FALSE(has_name(raw_mock->last_tool_names, "planner_build_plan"));
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
