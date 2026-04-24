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

inline std::string trim_copy(std::string value) {
    const auto not_space = [](unsigned char ch) {
        return !std::isspace(ch);
    };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

inline std::string normalize_brief_turn_copy(std::string value) {
    value = to_lower_copy(trim_copy(std::move(value)));
    while (!value.empty()) {
        const char ch = value.back();
        if (ch == '!' || ch == '?' || ch == '.' || ch == ',') {
            value.pop_back();
            continue;
        }
        break;
    }
    return trim_copy(std::move(value));
}

inline bool is_social_or_meta_turn(const std::string& input) {
    const std::string normalized = normalize_brief_turn_copy(input);
    return normalized == "hi"
        || normalized == "hello"
        || normalized == "hey"
        || normalized == "hi there"
        || normalized == "hello there"
        || normalized == "hey there"
        || normalized == "thanks"
        || normalized == "thank you"
        || normalized == "who are you"
        || normalized == "what are you"
        || normalized == "what can you do"
        || normalized == "help";
}

inline bool contains_any_phrase(const std::string& text,
                                const std::vector<std::string>& phrases) {
    return std::any_of(phrases.begin(), phrases.end(), [&](const std::string& phrase) {
        return text.find(phrase) != std::string::npos;
    });
}

enum class CoordinatorTurnStrategy {
    Conversational,
    DirectAudit,
    Delegate,
};

inline CoordinatorTurnStrategy coordinator_turn_strategy(const std::string& input) {
    const std::string normalized = normalize_brief_turn_copy(input);
    if (normalized.empty()) {
        return CoordinatorTurnStrategy::Conversational;
    }

    if (is_social_or_meta_turn(normalized)) {
        return CoordinatorTurnStrategy::Conversational;
    }

    const std::vector<std::string> audit_phrases = {
        "audit", "review", "full report", "findings", "security review",
        "code quality", "review this code", "code audit"
    };
    if (contains_any_phrase(normalized, audit_phrases)) {
        return CoordinatorTurnStrategy::DirectAudit;
    }

    const std::vector<std::string> work_phrases = {
        "fix", "debug", "investigate", "implement", "add ", "change", "edit",
        "refactor", "rename", "review", "audit", "inspect", "explore", "search",
        "find", "open", "read", "list", "show", "run", "test", "build",
        "compile", "plan", "spec", "workspace", "repo", "repository", "code",
        "file", "files", "directory", "cwd", "project", "bug", "failure", "error"
    };

    if (contains_any_phrase(normalized, work_phrases)) {
        return CoordinatorTurnStrategy::Delegate;
    }

    const std::vector<std::string> self_phrases = {
        "your name", "you name", "rname", "who are", "what are you", "what can you"
    };
    if (contains_any_phrase(normalized, self_phrases)) {
        return CoordinatorTurnStrategy::Conversational;
    }

    if ((normalized.find("you") != std::string::npos || normalized.find("your") != std::string::npos)
        && normalized.size() <= 80) {
        return CoordinatorTurnStrategy::Conversational;
    }

    return CoordinatorTurnStrategy::Delegate;
}

inline Message make_text_message(Role role, const std::string& text) {
    Message message;
    message.role = role;
    message.content = {ContentBlock{TextContent{text}}};
    return message;
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
        .system_prompt = "Act as a coordinator-only agent. Do not inspect the local filesystem directly, do not browse the web directly, and do not edit files directly. Delegate repository understanding to explore agents, findings-first review work to audit agents, failing behavior to bugfix agents, behavior-preserving cleanup to refactor agents, feature delivery to feature agents, web-backed investigation to research agents, spec creation and validation to spec agents, and implementation planning to plan agents. When feature scope or ambiguity is large, escalate into spec and plan artifacts that can hand off to the separate graph execution path instead of treating this loop as the graph executor. For exploratory or review tasks, prefer the minimum delegation needed to answer, avoid open-ended sweeps, and stop once gathered evidence is sufficient to respond. Answer tersely by default. Do not introduce yourself, announce your role, narrate routine delegation or exploration, or promise future help unless the user explicitly asked for that context. For greetings or short conversational turns, reply briefly and wait for the actual task.",
        .tool_policy = coordinator_policy,
        .default_permission_mode = PermissionMode::Bypass,
        .sub_agents_allowed = true,
        .max_parallel_tools = 4,
    });

    profiles.push_back(AgentProfileManifest{
        .name = "explore",
        .system_prompt = "Focus on existing-codebase inspection, explanation, and safe read-only investigation. Use the minimum tool calls needed to answer, avoid rereading the same files or rerunning the same searches unless confirming a concrete point, and once you have enough evidence, stop exploring and answer directly while calling out any remaining uncertainty.",
        .tool_policy = ToolCapabilityPolicy{},
        .default_permission_mode = PermissionMode::Plan,
        .sub_agents_allowed = false,
        .max_parallel_tools = 10,
    });

    ToolCapabilityPolicy feature_policy;
    feature_policy.allow_write = true;
    feature_policy.allow_shell = true;
    feature_policy.explicit_allow = {
        "bash",
        "planner_validate_spec",
        "planner_validate_plan",
        "planner_repair_plan",
        "planner_build_plan",
        "planner_build_from_idea",
    };
    profiles.push_back(AgentProfileManifest{
        .name = "feature",
        .system_prompt = "Add or extend repository behavior. Inspect existing patterns first, implement directly when the change is contained, escalate into spec and plan artifacts when scope or ambiguity warrants it, and finish with targeted verification or an explicit explanation of what could not be run.",
        .tool_policy = feature_policy,
        .default_permission_mode = PermissionMode::AcceptEdits,
        .sub_agents_allowed = false,
        .max_parallel_tools = 10,
    });

    ToolCapabilityPolicy refactor_policy = feature_policy;
    profiles.push_back(AgentProfileManifest{
        .name = "refactor",
        .system_prompt = "Improve repository structure while preserving behavior unless the user explicitly requests a semantic change. State the invariants or regression checks that must hold, keep edits reviewable, and finish with targeted verification.",
        .tool_policy = refactor_policy,
        .default_permission_mode = PermissionMode::AcceptEdits,
        .sub_agents_allowed = false,
        .max_parallel_tools = 10,
    });

    ToolCapabilityPolicy audit_policy;
    audit_policy.explicit_allow = {"planner_validate_review"};
    profiles.push_back(AgentProfileManifest{
        .name = "audit",
        .system_prompt = "Review code or recent changes without modifying the workspace. Run the audit systematically: first map the relevant repository surface, entrypoints, and ownership boundaries; then inspect at least one validation or contract surface when present, such as tests, CLI behavior, schemas, config loaders, API contracts, or explicit command output; then pair each finding with both the implementation evidence and the violated contract or observed behavior. Report findings first, order them by severity, include evidence, and state explicitly when no findings are present. Every finding must be directly supported by gathered evidence from the conversation. If the exact code, symbol, or tool output is not present or is contradicted by the evidence, omit the finding instead of speculating. Do not label something a test bug or mock issue unless the evidence explicitly shows the test expectation or patch target is wrong; otherwise describe the code/test contract mismatch neutrally. Do not pad the answer with generic sections such as broad summaries, missing features, documentation advice, security considerations, or production-readiness claims unless the user explicitly asked for them or the evidence directly supports them. Only state counts or percentages when they come from explicit tool output. If tests or command output show multiple distinct failure clusters, reflect those actual clusters instead of collapsing them into a single guessed root cause. When planner_validate_review is available and a tracked review case exists, validate your draft report text before finishing, and if validation says required coverage is missing, gather the missing evidence with targeted reads or searches before you finalize. Prefer targeted inspection over broad sweeps, avoid repeated rereads once the evidence is already sufficient, and answer as soon as the current evidence supports a conclusion.",
        .tool_policy = audit_policy,
        .default_permission_mode = PermissionMode::Plan,
        .sub_agents_allowed = false,
        .enforce_evidence_based_final_answer = true,
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
        .system_prompt = "Gather evidence, compare sources, and summarize findings clearly. Prefer focused source collection over open-ended browsing, avoid repeating the same searches or sources unless confirming a concrete point, and stop once the available evidence is enough to answer.",
        .tool_policy = research_policy,
        .default_permission_mode = PermissionMode::Plan,
        .sub_agents_allowed = false,
        .max_parallel_tools = 10,
    });

    ToolCapabilityPolicy bugfix_policy;
    bugfix_policy.allow_write = true;
    bugfix_policy.allow_shell = true;
    bugfix_policy.explicit_allow = {"bash", "planner_validate_bugfix"};
    profiles.push_back(AgentProfileManifest{
        .name = "bugfix",
        .system_prompt = "Fix failing behavior by capturing the repro, isolating the root cause, applying the smallest defensible fix, and rerunning targeted verification or explaining what blocked validation. When planner_validate_bugfix is available and you have a tracked case id or case path, validate the final writeup before finishing.",
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
    bool restore_profile_runtime = false;
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
    prompt += "Default to concise, task-focused responses. Do not introduce yourself, restate your role, or narrate routine background work unless the user asked for that detail.\n";
    prompt += "If you need to inspect before answering, do the inspection and then report the result instead of announcing that you are about to inspect.\n";
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

inline void apply_runtime_profile_locked(const std::shared_ptr<SessionState>& session_state,
                                         const HostState& host_state,
                                         const AgentProfileManifest& profile) {
    session_state->session->set_system_prompt(
        composed_system_prompt(session_state, host_state, profile));
    session_state->session->set_permission_mode(profile.default_permission_mode);
    session_state->session->set_evidence_based_final_answer(
        profile.enforce_evidence_based_final_answer);
    session_state->session->set_max_parallel_tools(profile.max_parallel_tools);
    session_state->session->set_tool_filter(
        allowed_tools_for_profile(session_state, host_state, profile));
}

inline void apply_profile_filter_locked(const std::shared_ptr<SessionState>& session_state,
                                        const HostState& host_state) {
    const auto& profile = resolve_profile(host_state, session_state->active_profile);
    session_state->active_profile = profile.name;
    apply_runtime_profile_locked(session_state, host_state, profile);
}

inline void clear_active_run_if_matches(const std::shared_ptr<SessionState>& session_state,
                                        const std::shared_ptr<RunState>& run_state) {
    std::lock_guard<std::mutex> lock(session_state->mutex);
    if (session_state->active_run == run_state) {
        session_state->active_run.reset();
    }
}

inline void bind_run_tool_context_locked(const std::shared_ptr<SessionState>& session_state,
                                         const std::shared_ptr<HostState>& host_state,
                                         const std::shared_ptr<RunState>& run_state) {
    session_state->session->set_tool_context(ToolContext{
        .project_id = session_state->project_id,
        .session_id = session_state->session_id,
        .run_id = run_state->result.run_id,
        .profile = session_state->active_profile,
        .workspace_root = host_state->config.workspace.workspace_root,
        .working_dir = session_state->working_dir,
    });
}

inline void restore_profile_runtime_if_needed(const std::shared_ptr<SessionState>& session_state,
                                              const std::shared_ptr<RunState>& run_state) {
    bool should_restore = false;
    {
        std::lock_guard<std::mutex> run_lock(run_state->mutex);
        should_restore = run_state->restore_profile_runtime;
        if (should_restore) {
            run_state->restore_profile_runtime = false;
        }
    }
    if (!should_restore) {
        return;
    }

    auto host_state = session_state->host.lock();
    if (!host_state) {
        return;
    }
    std::lock_guard<std::mutex> session_lock(session_state->mutex);
    if (session_state->closed) {
        return;
    }
    const auto& profile = resolve_profile(*host_state, session_state->active_profile);
    apply_runtime_profile_locked(session_state, *host_state, profile);
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
        } else if (run_state->result.pending_clarification.has_value()) {
            final_status = RunStatus::Paused;
            run_state->result.pause_reason = "clarification_required";
        }

        run_state->result.status = final_status;
        if (!run_state->result.pending_clarification.has_value()) {
            run_state->result.pause_reason.reset();
        }
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
    if (result.status == RunStatus::Paused
        && (result.pending_approval.has_value() || result.pending_clarification.has_value())) {
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

inline void launch_run_execution(const std::shared_ptr<SessionState>& session_state,
                                 const std::shared_ptr<RunState>& run_state,
                                 const std::string& input) {
    if (session_state->active_profile == "coordinator") {
        auto host_state = require_host_state(session_state);
        const auto strategy = coordinator_turn_strategy(input);
        std::lock_guard<std::mutex> session_lock(session_state->mutex);
        if (strategy == CoordinatorTurnStrategy::Conversational) {
            session_state->session->set_tool_filter({});
            std::lock_guard<std::mutex> run_lock(run_state->mutex);
            run_state->restore_profile_runtime = true;
        } else if (strategy == CoordinatorTurnStrategy::DirectAudit) {
            const auto audit_profile = resolve_profile(*host_state, "audit");
            apply_runtime_profile_locked(session_state, *host_state, audit_profile);
            std::lock_guard<std::mutex> run_lock(run_state->mutex);
            run_state->result.profile = audit_profile.name;
            run_state->restore_profile_runtime = true;
        } else {
            const auto& coordinator_profile = resolve_profile(*host_state, session_state->active_profile);
            apply_runtime_profile_locked(session_state, *host_state, coordinator_profile);
        }
    }

    try {
        session_state->session->submit(input);

        std::thread([session_state, run_state]() {
            try {
                session_state->session->wait();
            } catch (const std::exception& error) {
                {
                    std::lock_guard<std::mutex> run_lock(run_state->mutex);
                    if (!run_state->finalised) {
                        run_state->result.error = error.what();
                    }
                }
                finalize_run_state(run_state, RunStatus::Failed);
                persist_run_state(run_state);
                restore_profile_runtime_if_needed(session_state, run_state);
                clear_active_run_if_matches(session_state, run_state);
                mark_run_settled(run_state);
                return;
            }

            bool should_emit_terminal = false;
            RunStatus final_status = RunStatus::Completed;
            std::optional<std::string> pause_reason;
            {
                std::lock_guard<std::mutex> run_lock(run_state->mutex);
                should_emit_terminal = !run_state->finalised
                    && (run_state->cancel_requested || run_state->stop_requested);
            }

            finalize_run_state(run_state, RunStatus::Completed);
            persist_run_state(run_state);
            {
                std::lock_guard<std::mutex> run_lock(run_state->mutex);
                final_status = run_state->result.status;
                pause_reason = run_state->result.pause_reason;
            }
            if (should_emit_terminal) {
                if (final_status == RunStatus::Stopped) {
                    dispatch_run_event(run_state, RunStoppedEvent{run_state->result.run_id, {}});
                } else if (final_status == RunStatus::Cancelled) {
                    dispatch_run_event(run_state, RunCancelledEvent{run_state->result.run_id, {}});
                }
            } else if (final_status == RunStatus::Paused) {
                dispatch_run_event(run_state,
                                   RunPausedEvent{run_state->result.run_id,
                                                  pause_reason.value_or("paused"),
                                                  {}});
            }
            if (final_status != RunStatus::Paused) {
                restore_profile_runtime_if_needed(session_state, run_state);
            }
            if (final_status != RunStatus::Paused) {
                clear_active_run_if_matches(session_state, run_state);
            }
            mark_run_settled(run_state);
        }).detach();
    } catch (const std::exception& error) {
        run_state->result.error = error.what();
        finalize_run_state(run_state, RunStatus::Failed);
        persist_run_state(run_state);
        restore_profile_runtime_if_needed(session_state, run_state);
        clear_active_run_if_matches(session_state, run_state);
        mark_run_settled(run_state);
    }
}

inline bool resume_live_run(const std::shared_ptr<RunState>& run_state,
                            const std::string& resume_input) {
    const auto session_state = run_state->session.lock();
    if (!session_state) {
        return false;
    }

    bool has_pending_approval = false;
    bool has_pending_clarification = false;
    {
        std::lock_guard<std::mutex> run_lock(run_state->mutex);
        if (run_state->result.status != RunStatus::Paused) {
            return false;
        }
        has_pending_approval = run_state->result.pending_approval.has_value();
        has_pending_clarification = run_state->result.pending_clarification.has_value();
    }

    if (has_pending_approval) {
        const auto decision = parse_resume_decision(resume_input);
        {
            std::lock_guard<std::mutex> run_lock(run_state->mutex);
            if (run_state->result.status != RunStatus::Paused
                || !run_state->result.pending_approval.has_value()) {
                return false;
            }
            run_state->pending_resolution = decision;
            run_state->result.status = RunStatus::Running;
            run_state->result.pause_reason.reset();
            run_state->result.pending_approval.reset();
        }
        persist_run_state(run_state);
        run_state->approval_cv.notify_all();
        return true;
    }

    if (!has_pending_clarification) {
        return false;
    }

    auto host_state = session_state->host.lock();
    if (!host_state) {
        return false;
    }

    {
        std::lock_guard<std::mutex> session_lock(session_state->mutex);
        if (session_state->closed) {
            return false;
        }
        if (session_state->active_run && session_state->active_run != run_state) {
            return false;
        }
        session_state->active_run = run_state;
        bind_run_tool_context_locked(session_state, host_state, run_state);
    }

    std::string run_id;
    {
        std::lock_guard<std::mutex> run_lock(run_state->mutex);
        if (run_state->result.status != RunStatus::Paused
            || !run_state->result.pending_clarification.has_value()) {
            return false;
        }
        run_id = run_state->result.run_id;
        run_state->result.status = RunStatus::Running;
        run_state->result.pause_reason.reset();
        run_state->result.pending_clarification.reset();
        run_state->result.finished_at = {};
        run_state->pending_resolution.reset();
        run_state->finalised = false;
        run_state->settled = false;
    }

    persist_run_state(run_state);
    dispatch_run_event(run_state, RunResumedEvent{run_id, {}});
    launch_run_execution(session_state, run_state, resume_input);
    return true;
}

}  // namespace omni::engine