#pragma once

#include <omni/approval.h>
#include <omni/engine.h>
#include <omni/event.h>
#include <omni/host.h>
#include <omni/observer.h>
#include <omni/project.h>
#include <omni/project_session.h>
#include <omni/run.h>
#include <omni/session.h>

#include "services/session_persistence.h"
#include "services/run_persistence.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace omni::engine {

struct HostState;
struct SessionState;
struct RunState;

struct ProjectEngineHost::Impl {
    explicit Impl(std::shared_ptr<HostState> value)
        : state(std::move(value)) {}

    std::shared_ptr<HostState> state;
};

struct ProjectSession::Impl {
    explicit Impl(std::shared_ptr<SessionState> value)
        : state(std::move(value)) {}

    std::shared_ptr<SessionState> state;
};

struct ProjectRun::Impl {
    explicit Impl(std::shared_ptr<RunState> value)
        : state(std::move(value)) {}

    std::shared_ptr<RunState> state;
};

std::string generate_run_id();

inline std::string to_lower_copy(const std::string& value) {
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return lowered;
}

inline Event enrich_event(const Event& event,
                          const std::string& project_id,
                          const std::string& session_id,
                          const std::string& run_id) {
    Event enriched = event;
    std::visit([&](auto& entry) {
        entry.context.project_id = project_id;
        entry.context.session_id = session_id;
        entry.context.run_id = run_id;
    }, enriched);
    return enriched;
}

inline std::vector<AgentProfileManifest> default_profiles() {
    std::vector<AgentProfileManifest> profiles;

    ToolCapabilityPolicy coordinator_policy;
    coordinator_policy.allow_read_only = false;
    coordinator_policy.explicit_allow = {"agent", "send_message"};
    profiles.push_back(AgentProfileManifest{
        .name = "coordinator",
        .system_prompt = "Act as a coordinator-only agent. Do not inspect the local filesystem directly, do not browse the web directly, and do not edit files directly. Delegate local investigation to explore agents, web-backed investigation to research agents, spec creation and validation to spec agents, and implementation planning to plan agents. When the user asks to build an application, drive a spec-first workflow that produces a validated SPEC.md before a plan worker generates and validates PLAN.json for graph execution.",
        .tool_policy = coordinator_policy,
        .default_permission_mode = PermissionMode::Bypass,
        .sub_agents_allowed = true,
        .max_parallel_tools = 4,
    });

    profiles.push_back(AgentProfileManifest{
        .name = "explore",
        .system_prompt = "Focus on inspection, explanation, and safe read-only investigation.",
        .tool_policy = ToolCapabilityPolicy{},
        .default_permission_mode = PermissionMode::Default,
        .sub_agents_allowed = false,
        .max_parallel_tools = 10,
    });

    ToolCapabilityPolicy spec_policy;
    spec_policy.allow_write = true;
    spec_policy.allow_shell = true;
    spec_policy.allow_network = true;
    spec_policy.allow_mcp = true;
    profiles.push_back(AgentProfileManifest{
        .name = "spec",
        .system_prompt = "Focus on requirements, prior art, and precise specification work.",
        .tool_policy = spec_policy,
        .default_permission_mode = PermissionMode::Default,
        .sub_agents_allowed = false,
        .max_parallel_tools = 10,
    });

    ToolCapabilityPolicy plan_policy;
    plan_policy.allow_write = true;
    plan_policy.allow_shell = true;
    plan_policy.explicit_allow = {
        "planner_validate_plan",
        "planner_repair_plan",
        "planner_build_plan",
        "planner_build_from_idea",
    };
    profiles.push_back(AgentProfileManifest{
        .name = "plan",
        .system_prompt = "Produce concrete execution plans and avoid mutating the workspace.",
        .tool_policy = plan_policy,
        .default_permission_mode = PermissionMode::Default,
        .sub_agents_allowed = false,
        .max_parallel_tools = 10,
    });

    ToolCapabilityPolicy research_policy;
    research_policy.allow_network = true;
    research_policy.allow_mcp = true;
    profiles.push_back(AgentProfileManifest{
        .name = "research",
        .system_prompt = "Gather evidence, compare sources, and summarize findings clearly.",
        .tool_policy = research_policy,
        .default_permission_mode = PermissionMode::Default,
        .sub_agents_allowed = false,
        .max_parallel_tools = 10,
    });

    ToolCapabilityPolicy bugfix_policy;
    bugfix_policy.allow_write = true;
    bugfix_policy.allow_destructive = true;
    bugfix_policy.allow_shell = true;
    profiles.push_back(AgentProfileManifest{
        .name = "bugfix",
        .system_prompt = "Fix the problem directly, using edits and commands when necessary.",
        .tool_policy = bugfix_policy,
        .default_permission_mode = PermissionMode::AcceptEdits,
        .sub_agents_allowed = false,
        .max_parallel_tools = 10,
    });

    ToolCapabilityPolicy general_policy;
    general_policy.allow_write = true;
    general_policy.allow_destructive = true;
    general_policy.allow_shell = true;
    general_policy.allow_network = true;
    general_policy.allow_mcp = true;
    general_policy.allow_sub_agents = true;
    profiles.push_back(AgentProfileManifest{
        .name = "general",
        .system_prompt = "Use the full runtime when the task requires it, while staying deliberate.",
        .tool_policy = general_policy,
        .default_permission_mode = PermissionMode::Default,
        .sub_agents_allowed = true,
        .max_parallel_tools = 10,
    });

    return profiles;
}

inline std::vector<AgentProfileManifest> merge_profiles(
    const std::vector<AgentProfileManifest>& overrides) {
    std::map<std::string, AgentProfileManifest> merged;
    for (auto& profile : default_profiles()) {
        merged.emplace(to_lower_copy(profile.name), std::move(profile));
    }
    for (const auto& profile : overrides) {
        merged[to_lower_copy(profile.name)] = profile;
    }

    std::vector<AgentProfileManifest> result;
    result.reserve(merged.size());
    for (auto& [_, profile] : merged) {
        result.push_back(std::move(profile));
    }
    return result;
}

inline const AgentProfileManifest& resolve_profile(const HostState& host_state,
                                                   const std::string& requested_name);

inline bool contains_name(const std::vector<std::string>& names,
                          const std::string& value) {
    return std::find(names.begin(), names.end(), value) != names.end();
}

inline ApprovalDecision parse_resume_decision(const std::string& value) {
    const std::string normalized = to_lower_copy(value);
    if (normalized.empty() || normalized == "approve" || normalized == "allow"
        || normalized == "y" || normalized == "yes") {
        return ApprovalDecision::Approve;
    }
    if (normalized == "always" || normalized == "approve_always") {
        return ApprovalDecision::ApproveAlways;
    }
    if (normalized == "deny" || normalized == "reject"
        || normalized == "n" || normalized == "no") {
        return ApprovalDecision::Deny;
    }
    throw std::invalid_argument("unknown approval resolution: " + value);
}

inline bool tool_allowed_by_policy(const Tool& tool,
                                   const ToolCapabilityPolicy& policy) {
    if (contains_name(policy.explicit_deny, tool.name())) {
        return false;
    }
    if (contains_name(policy.explicit_allow, tool.name())) {
        return true;
    }
    if (tool.is_sub_agent_tool() && !policy.allow_sub_agents) {
        return false;
    }
    if (tool.is_mcp() && !policy.allow_mcp) {
        return false;
    }
    if (tool.is_shell() && !policy.allow_shell) {
        return false;
    }
    if (tool.is_network() && !policy.allow_network) {
        return false;
    }
    if (tool.is_destructive() && !policy.allow_destructive) {
        return false;
    }
    if (tool.is_read_only() && !policy.allow_read_only) {
        return false;
    }
    if (!tool.is_read_only() && !policy.allow_write) {
        return false;
    }
    return true;
}

inline void append_prompt_section(std::string& prompt,
                                  const std::string& section);

inline std::string runtime_environment_prompt(
    const std::shared_ptr<SessionState>& session_state,
    const HostState& host_state,
    const AgentProfileManifest& profile);

inline std::string composed_system_prompt(
    const std::shared_ptr<SessionState>& session_state,
    const HostState& host_state,
                                          const AgentProfileManifest& profile);

inline std::vector<std::string> allowed_tools_for_profile(
    const std::shared_ptr<SessionState>& session_state,
    const HostState& host_state,
    const AgentProfileManifest& profile);

class ForwardingObserver : public EventObserver {
public:
    explicit ForwardingObserver(std::weak_ptr<SessionState> session_state)
        : session_state_(std::move(session_state)) {}

    void on_event(const Event& event) override;

private:
    std::weak_ptr<SessionState> session_state_;
};

class ForwardingPermissionDelegate : public PermissionDelegate {
public:
    explicit ForwardingPermissionDelegate(std::weak_ptr<SessionState> session_state)
        : session_state_(std::move(session_state)) {}

    PermissionDecision on_permission_request(const std::string& tool_name,
                                             const nlohmann::json& args,
                                             const std::string& description) override;

private:
    std::weak_ptr<SessionState> session_state_;
};

struct RunState {
    mutable std::mutex mutex;
    std::condition_variable approval_cv;
    std::weak_ptr<SessionState> session;
    RunObserver* observer = nullptr;
    ApprovalDelegate* approvals = nullptr;
    RunResult result;
    bool cancel_requested = false;
    bool stop_requested = false;
    bool finalised = false;
    bool settled = false;
    std::optional<ApprovalDecision> pending_resolution;
};

struct SessionState : std::enable_shared_from_this<SessionState> {
    mutable std::mutex mutex;
    std::weak_ptr<HostState> host;
    std::shared_ptr<Session> session;
    std::string session_id;
    std::string project_id;
    std::string active_profile = "explore";
    std::filesystem::path working_dir;
    bool closed = false;
    std::shared_ptr<RunState> active_run;
    std::shared_ptr<ForwardingObserver> observer_bridge;
    std::shared_ptr<ForwardingPermissionDelegate> approval_bridge;
};

struct HostState : std::enable_shared_from_this<HostState> {
    mutable std::mutex mutex;
    ProjectRuntimeConfig config;
    std::unique_ptr<Engine> engine;
    bool running = true;
    std::vector<std::shared_ptr<SessionState>> live_sessions;
    std::vector<std::string> connected_mcp_servers;
    std::vector<AgentProfileManifest> profiles;
};

inline std::shared_ptr<HostState> require_host_state(
    const std::shared_ptr<SessionState>& session_state);

inline void prune_closed_sessions_locked(HostState& host_state) {
    host_state.live_sessions.erase(
        std::remove_if(host_state.live_sessions.begin(), host_state.live_sessions.end(),
                       [](const std::shared_ptr<SessionState>& session_state) {
                           if (!session_state) {
                               return true;
                           }

                           std::lock_guard<std::mutex> lock(session_state->mutex);
                           return session_state->closed;
                       }),
        host_state.live_sessions.end());
}

inline std::filesystem::path default_working_dir(const WorkspaceContext& workspace) {
    if (workspace.working_dir.empty()) {
        return workspace.workspace_root;
    }
    return workspace.working_dir;
}

inline SessionPersistence make_session_persistence(const HostState& host_state) {
    return SessionPersistence(host_state.config.engine.session_storage_dir);
}

inline RunPersistence make_run_persistence(const HostState& host_state) {
    return RunPersistence(host_state.config.engine.session_storage_dir);
}

inline bool has_persistence(const HostState& host_state) {
    return !host_state.config.engine.session_storage_dir.empty();
}

inline const AgentProfileManifest& resolve_profile(const HostState& host_state,
                                                   const std::string& requested_name) {
    const std::string requested = to_lower_copy(requested_name.empty() ? "explore" : requested_name);
    auto it = std::find_if(host_state.profiles.begin(), host_state.profiles.end(),
                           [&requested](const AgentProfileManifest& profile) {
                               return to_lower_copy(profile.name) == requested;
                           });
    if (it == host_state.profiles.end()) {
        throw std::runtime_error("unknown profile: " + requested_name);
    }
    return *it;
}

inline void append_prompt_section(std::string& prompt,
                                  const std::string& section) {
    if (section.empty()) {
        return;
    }
    if (!prompt.empty()) {
        prompt += "\n\n";
    }
    prompt += section;
}

inline std::string runtime_environment_prompt(
    const std::shared_ptr<SessionState>& session_state,
    const HostState& host_state,
    const AgentProfileManifest& profile) {
    std::string prompt;
    prompt += "You are a project-owned software engineering agent.\n";
    prompt += "Operate on the selected project workspace instead of behaving like a generic hosted chatbot.\n";
    prompt += "Project id: " + session_state->project_id + "\n";
    prompt += "Workspace root: " + host_state.config.workspace.workspace_root.string() + "\n";
    prompt += "Current working directory: " + session_state->working_dir.string() + "\n";
    prompt += "Active profile: " + profile.name + "\n";
    if (host_state.config.workspace.user_id.has_value()) {
        prompt += "Owning user id: " + *host_state.config.workspace.user_id + "\n";
    }
    if (host_state.config.workspace.allow_workspace_escape) {
        prompt += "Workspace boundary: workspace escape is allowed when necessary.\n";
    } else {
        prompt += "Workspace boundary: stay within the workspace root unless the runtime explicitly directs otherwise.\n";
    }
    prompt += "Treat the selected workspace as the source tree you should inspect and modify.\n";
    prompt += "Do not claim that you lack a filesystem, a working directory, or access to project files.\n";
    prompt += "If asked about the workspace or working directory, answer from the paths above and use available tools to verify details when needed.\n";
    prompt += "Do not end a turn with thinking only; either answer directly or call an available workspace tool to gather the missing information.\n";
    prompt += "Do not assume the omniagent repository is relevant unless it is the selected workspace.\n";
    prompt += "Use the tools available in this session whenever inspection, commands, or edits are required.";
    return prompt;
}

inline std::string composed_system_prompt(
    const std::shared_ptr<SessionState>& session_state,
    const HostState& host_state,
                                          const AgentProfileManifest& profile) {
    std::string prompt;
    append_prompt_section(prompt, host_state.config.engine.system_prompt);
    append_prompt_section(prompt,
                          runtime_environment_prompt(session_state,
                                                     host_state,
                                                     profile));
    append_prompt_section(prompt, profile.system_prompt);
    return prompt;
}

inline std::vector<std::string> allowed_tools_for_profile(
    const std::shared_ptr<SessionState>& session_state,
    const HostState& host_state,
    const AgentProfileManifest& profile) {
    (void)host_state;
    ToolCapabilityPolicy effective_policy = profile.tool_policy;
    effective_policy.allow_sub_agents = effective_policy.allow_sub_agents || profile.sub_agents_allowed;
    std::vector<std::string> allowed;
    for (const auto& tool_name : session_state->session->tool_names()) {
        Tool* tool = session_state->session->find_tool(tool_name);
        if (!tool) {
            continue;
        }
        if (tool_allowed_by_policy(*tool, effective_policy)) {
            allowed.push_back(tool_name);
        }
    }
    std::sort(allowed.begin(), allowed.end());
    return allowed;
}

inline void apply_profile_filter_locked(const std::shared_ptr<SessionState>& session_state,
                                        const HostState& host_state) {
    const auto& profile = resolve_profile(host_state, session_state->active_profile);
    session_state->active_profile = profile.name;
    session_state->session->set_system_prompt(
        composed_system_prompt(session_state, host_state, profile));
    session_state->session->set_permission_mode(profile.default_permission_mode);
    session_state->session->set_max_parallel_tools(profile.max_parallel_tools);
    session_state->session->set_tool_filter(
        allowed_tools_for_profile(session_state, host_state, profile));
}

inline void clear_active_run_if_matches(const std::shared_ptr<SessionState>& session_state,
                                        const std::shared_ptr<RunState>& run_state) {
    std::lock_guard<std::mutex> lock(session_state->mutex);
    if (session_state->active_run == run_state) {
        session_state->active_run.reset();
    }
}

inline void finalize_run_state(const std::shared_ptr<RunState>& run_state,
                               RunStatus success_status) {
    RunStatus final_status = success_status;
    {
        std::lock_guard<std::mutex> lock(run_state->mutex);
        if (run_state->finalised) {
            return;
        }

        run_state->result.finished_at = std::chrono::system_clock::now();
        if (run_state->cancel_requested) {
            final_status = RunStatus::Cancelled;
        } else if (run_state->stop_requested) {
            final_status = RunStatus::Stopped;
        } else if (run_state->result.error.has_value()) {
            final_status = RunStatus::Failed;
        }

        run_state->result.status = final_status;
        run_state->result.pause_reason.reset();
        run_state->result.pending_approval.reset();
        run_state->pending_resolution.reset();
        run_state->finalised = true;
    }

    run_state->approval_cv.notify_all();
}

inline void mark_run_settled(const std::shared_ptr<RunState>& run_state) {
    {
        std::lock_guard<std::mutex> lock(run_state->mutex);
        run_state->settled = true;
    }
    run_state->approval_cv.notify_all();
}

inline SessionSummary build_live_session_summary(const SessionState& session_state) {
    SessionSummary summary;
    summary.session_id = session_state.session_id;
    summary.project_id = session_state.project_id;
    summary.active_profile = session_state.active_profile;
    summary.working_dir = session_state.working_dir;
    summary.message_count = session_state.session ? session_state.session->messages().size() : 0;
    return summary;
}

inline SessionSummary build_persisted_session_summary(const WorkspaceContext& workspace,
                                                      const SessionRecord& record) {
    SessionSummary summary;
    summary.session_id = record.id;
    summary.project_id = workspace.project_id;
    summary.active_profile = "explore";
    summary.working_dir = default_working_dir(workspace);
    summary.updated_at = record.updated_at;
    summary.message_count = record.messages.size();
    return summary;
}

inline std::shared_ptr<RunState> current_run_for_session(const std::shared_ptr<SessionState>& session_state) {
    std::lock_guard<std::mutex> lock(session_state->mutex);
    return session_state->active_run;
}

inline void dispatch_run_event(const std::shared_ptr<RunState>& run_state,
                               const Event& event) {
    auto session_state = run_state->session.lock();
    if (!session_state) {
        return;
    }

    RunObserver* observer = nullptr;
    std::string run_id;
    {
        std::lock_guard<std::mutex> run_lock(run_state->mutex);
        observer = run_state->observer;
        run_id = run_state->result.run_id;
    }

    if (!observer) {
        return;
    }

    const Event enriched = enrich_event(event,
                                        session_state->project_id,
                                        session_state->session_id,
                                        run_id);
    observer->on_event(enriched,
                       session_state->project_id,
                       session_state->session_id,
                       run_id);
}

inline void persist_run_state(const std::shared_ptr<RunState>& run_state) {
    auto session_state = run_state->session.lock();
    if (!session_state) {
        return;
    }

    auto host_state = session_state->host.lock();
    if (!host_state) {
        return;
    }
    if (!has_persistence(*host_state)) {
        return;
    }

    RunResult result;
    {
        std::lock_guard<std::mutex> lock(run_state->mutex);
        result = run_state->result;
    }

    RunPersistence persistence = make_run_persistence(*host_state);
    persistence.save(result);
    if (result.status == RunStatus::Paused && result.pending_approval.has_value()) {
        persistence.save_pending(result);
    } else {
        persistence.clear_pending(result.run_id);
    }
}

inline RunSummary build_run_summary(const RunResult& result) {
    RunSummary summary;
    summary.run_id = result.run_id;
    summary.session_id = result.session_id;
    summary.project_id = result.project_id;
    summary.profile = result.profile;
    summary.status = result.status;
    summary.started_at = result.started_at;
    summary.finished_at = result.finished_at;
    return summary;
}

inline std::shared_ptr<SessionState> find_live_session(
    const std::shared_ptr<HostState>& host_state,
    const std::string& session_id) {
    std::lock_guard<std::mutex> host_lock(host_state->mutex);
    for (const auto& session_state : host_state->live_sessions) {
        if (!session_state) {
            continue;
        }
        std::lock_guard<std::mutex> session_lock(session_state->mutex);
        if (!session_state->closed && session_state->session_id == session_id) {
            return session_state;
        }
    }
    return nullptr;
}

inline std::shared_ptr<RunState> find_live_run(const std::shared_ptr<HostState>& host_state,
                                               const std::string& run_id) {
    std::lock_guard<std::mutex> host_lock(host_state->mutex);
    for (const auto& session_state : host_state->live_sessions) {
        if (!session_state) {
            continue;
        }

        std::lock_guard<std::mutex> session_lock(session_state->mutex);
        if (session_state->closed || !session_state->active_run) {
            continue;
        }

        std::lock_guard<std::mutex> run_lock(session_state->active_run->mutex);
        if (session_state->active_run->result.run_id == run_id) {
            return session_state->active_run;
        }
    }
    return nullptr;
}

inline ToolSummary build_tool_summary(const Tool& tool) {
    ToolSummary summary;
    summary.name = tool.name();
    summary.read_only = tool.is_read_only();
    summary.destructive = tool.is_destructive();
    summary.shell = tool.is_shell();
    summary.network = tool.is_network();
    summary.mcp = tool.is_mcp();
    summary.sub_agent = tool.is_sub_agent_tool();
    return summary;
}

inline std::shared_ptr<HostState> require_host_state(const std::shared_ptr<SessionState>& session_state) {
    auto host_state = session_state->host.lock();
    if (!host_state) {
        throw std::runtime_error("project host is no longer available");
    }
    return host_state;
}

}  // namespace omni::engine