#pragma once

#include <omni/event.h>
#include <omni/permission.h>
#include <omni/tool.h>
#include <omni/types.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace omni::engine {

class Engine;
class Session;
class PermissionDelegate;

enum class AgentType {
    GeneralPurpose,  // Full tool access
    Explore,         // Read-only tools only
    Feature,         // Direct implementation or spec/plan escalation
    Refactor,        // Behavior-preserving structural changes
    Audit,           // Findings-first read-only review
    Bugfix,          // Root-cause-first remediation
    Research,        // Read-only local + web research
    Spec,            // Spec writing + validation workflow
    Plan,            // Plan generation + validation workflow
};

struct AgentConfig {
    AgentType   type             = AgentType::GeneralPurpose;
    std::string task;             // the prompt/task for the agent
    bool        run_in_background = false;
    std::string name;             // optional name for SendMessage addressing
};

struct AgentResult {
    std::string agent_id;
    std::string response;         // final text output
    Usage       usage;
    bool        is_error      = false;
    std::string error_message;
};

using AgentCompletionCallback = std::function<void(const AgentResult& result)>;

class AgentManager {
public:
    AgentManager(Engine& engine, PermissionDelegate& delegate);
    AgentManager(Engine& engine, PermissionDelegate& delegate, EventObserver& observer);

    /// Spawn a child agent. Returns agent_id.
    /// If background, returns immediately and calls on_complete when done.
    /// If foreground, blocks until agent completes and calls on_complete.
    std::string spawn(
        const AgentConfig&      config,
        AgentCompletionCallback on_complete = nullptr);

    /// Send a follow-up message to a running/completed agent.
    /// If the agent currently has an in-flight turn, this waits for that turn
    /// to settle before submitting the follow-up.
    /// Returns false if agent not found.
    bool send_message(const std::string& agent_id, const std::string& message);

    /// Get result of a completed agent. Returns nullopt if still running or not found.
    std::optional<AgentResult> get_result(const std::string& agent_id) const;

    /// Check if an agent is still running.
    bool is_running(const std::string& agent_id) const;

    /// Number of active (running) agents.
    size_t active_count() const;

    /// Block until the given agent's current in-flight task completes.
    /// Returns false if agent not found.
    bool wait_for(const std::string& agent_id) const;

    /// Request stop on all currently running child agents.
    void stop_all_running();

    /// Request cancellation on all currently running child agents.
    void cancel_all_running();

    /// Copy the parent session's workspace/tool context into future child agents.
    void set_parent_tool_context(ToolContext context);

private:
    Engine&             engine_;
    PermissionDelegate& delegate_;
    EventObserver&      observer_;
    ToolContext         parent_tool_context_;

    struct AgentState {
        std::string                  id;
        AgentConfig                  config;
        std::shared_ptr<EventObserver> observer;
        std::shared_ptr<Session>     session;
        std::optional<AgentResult>   result;
        bool                         running = false;
    };

    mutable std::mutex                              mutex_;
    mutable std::condition_variable                 state_cv_;
    std::unordered_map<std::string, AgentState>     agents_;
    int                                             next_id_ = 0;

    std::string generate_id();
    std::vector<std::string> filter_tools_for_type(AgentType type) const;
    std::string profile_name_for_type(AgentType type) const;
    PermissionMode permission_mode_for_type(AgentType type) const;
    std::string system_prompt_for_type(AgentType type) const;
};

}  // namespace omni::engine
