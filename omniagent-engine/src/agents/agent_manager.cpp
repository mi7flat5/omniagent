#include "agent_manager.h"

#include <omni/engine.h>
#include <omni/event.h>
#include <omni/permission.h>
#include <omni/session.h>

#include <sstream>
#include <thread>

namespace omni::engine {

// ---------------------------------------------------------------------------
// Collecting observer — gathers text and usage from an agent session
// ---------------------------------------------------------------------------

namespace {

class AgentObserver : public EventObserver {
public:
    void on_event(const Event& e) override {
        if (const auto* td = std::get_if<TextDeltaEvent>(&e)) {
            response_ += td->text;
        } else if (const auto* err = std::get_if<ErrorEvent>(&e)) {
            if (!err->recoverable) {
                is_error_       = true;
                error_message_ += err->message;
            }
        } else if (const auto* done = std::get_if<DoneEvent>(&e)) {
            usage_ = done->usage;
        }
    }

    std::string response()       const { return response_; }
    Usage       usage()          const { return usage_; }
    bool        is_error()       const { return is_error_; }
    std::string error_message()  const { return error_message_; }

private:
    std::string response_;
    Usage       usage_;
    bool        is_error_     = false;
    std::string error_message_;
};

class NullEventObserver : public EventObserver {
public:
    void on_event(const Event&) override {}
};

NullEventObserver& null_event_observer() {
    static NullEventObserver observer;
    return observer;
}

bool is_local_read_only_tool(const Tool* tool) {
    return tool && tool->is_read_only() && !tool->is_network() && !tool->is_mcp();
}

bool is_non_mcp_read_only_tool(const Tool* tool) {
    return tool && tool->is_read_only() && !tool->is_mcp();
}

bool is_planner_tool_name(const std::string& name) {
    return name == "planner_validate_spec"
        || name == "planner_validate_plan"
        || name == "planner_repair_plan"
        || name == "planner_build_plan"
        || name == "planner_build_from_idea";
}

bool is_review_validation_tool_name(const std::string& name) {
    return name == "planner_validate_review";
}

bool is_bugfix_validation_tool_name(const std::string& name) {
    return name == "planner_validate_bugfix";
}

}  // namespace

// ---------------------------------------------------------------------------
// AgentManager
// ---------------------------------------------------------------------------

AgentManager::AgentManager(Engine& engine, PermissionDelegate& delegate)
    : AgentManager(engine, delegate, null_event_observer())
{}

AgentManager::AgentManager(Engine& engine, PermissionDelegate& delegate,
                           EventObserver& observer)
    : engine_(engine), delegate_(delegate)
    , observer_(observer)
{}

void AgentManager::set_parent_tool_context(ToolContext context) {
    parent_tool_context_ = std::move(context);
}

std::string AgentManager::generate_id() {
    std::ostringstream oss;
    oss << "agent-" << next_id_++;
    return oss.str();
}

std::vector<std::string> AgentManager::filter_tools_for_type(AgentType type) const {
    const std::vector<std::string> all_names = engine_.tool_names();

    if (type == AgentType::GeneralPurpose) {
        return {};  // empty = all tools
    }

    std::vector<std::string> filtered;
    for (const auto& n : all_names) {
        const Tool* t = engine_.find_tool(n);
        if (!t) {
            continue;
        }

        bool allowed = false;
        switch (type) {
            case AgentType::Explore:
                allowed = is_local_read_only_tool(t);
                break;
            case AgentType::Feature:
            case AgentType::Refactor:
                allowed = is_local_read_only_tool(t)
                    || n == "write_file"
                    || n == "edit_file"
                    || n == "bash"
                    || is_planner_tool_name(n);
                break;
            case AgentType::Audit:
                allowed = is_local_read_only_tool(t)
                    || is_review_validation_tool_name(n);
                break;
            case AgentType::Bugfix:
                allowed = is_local_read_only_tool(t)
                    || n == "write_file"
                    || n == "edit_file"
                    || n == "bash"
                    || is_bugfix_validation_tool_name(n);
                break;
            case AgentType::Research:
                allowed = is_non_mcp_read_only_tool(t);
                break;
            case AgentType::Spec:
                allowed = is_non_mcp_read_only_tool(t)
                    || n == "write_file"
                    || n == "edit_file"
                    || n == "bash"
                    || n == "planner_validate_spec";
                break;
            case AgentType::Plan:
                allowed = is_local_read_only_tool(t)
                    || n == "write_file"
                    || n == "bash"
                    || n == "planner_validate_plan"
                    || n == "planner_repair_plan"
                    || n == "planner_build_plan"
                    || n == "planner_build_from_idea";
                break;
            case AgentType::GeneralPurpose:
                break;
        }

        if (allowed) {
            filtered.push_back(n);
        }
    }
    return filtered;
}

std::string AgentManager::profile_name_for_type(AgentType type) const {
    switch (type) {
        case AgentType::Explore:
            return "explore";
        case AgentType::Feature:
            return "feature";
        case AgentType::Refactor:
            return "refactor";
        case AgentType::Audit:
            return "audit";
        case AgentType::Bugfix:
            return "bugfix";
        case AgentType::Research:
            return "research";
        case AgentType::Spec:
            return "spec";
        case AgentType::Plan:
            return "plan";
        case AgentType::GeneralPurpose:
            return "general";
    }
    return "general";
}

PermissionMode AgentManager::permission_mode_for_type(AgentType type) const {
    switch (type) {
        case AgentType::GeneralPurpose:
            return PermissionMode::Default;
        case AgentType::Explore:
        case AgentType::Research:
        case AgentType::Audit:
            return PermissionMode::Plan;
        case AgentType::Feature:
        case AgentType::Refactor:
        case AgentType::Bugfix:
            return PermissionMode::AcceptEdits;
        case AgentType::Spec:
        case AgentType::Plan:
            return PermissionMode::Bypass;
    }
    return PermissionMode::Default;
}

std::string AgentManager::system_prompt_for_type(AgentType type) const {
    std::ostringstream prompt;
    prompt << "You are a delegated subagent working on behalf of a coordinator.\n";
    if (!parent_tool_context_.project_id.empty()) {
        prompt << "Project id: " << parent_tool_context_.project_id << "\n";
    }
    if (!parent_tool_context_.workspace_root.empty()) {
        prompt << "Workspace root: " << parent_tool_context_.workspace_root.string() << "\n";
    }
    if (!parent_tool_context_.working_dir.empty()) {
        prompt << "Current working directory: " << parent_tool_context_.working_dir.string() << "\n";
    }
    prompt << "Use only the tools available in this subagent session.\n";
    prompt << "Return focused results for the coordinator instead of a long conversational transcript.\n\n";

    switch (type) {
        case AgentType::Explore:
             prompt << "Role: existing-codebase exploration worker.\n"
                 << "Inspect files, trace execution flow, identify ownership boundaries, and summarize only the findings needed for the task.\n"
                                 << "Use the minimum read-only inspection needed to answer, avoid rereading the same files or rerunning the same searches unless confirming a concrete point, and stop once you have enough evidence.\n"
                   << "Do not modify files and do not attempt web research.\n";
            break;
         case AgentType::Feature:
             prompt << "Role: feature implementation worker.\n"
                 << "Inspect existing code paths, abstractions, and test style before editing.\n"
                 << "Use a direct implementation path when the request is contained, and escalate to spec/plan plus graph setup when scope or ambiguity warrants it.\n"
                 << "Run targeted verification before finishing, or state exactly why it could not be run.\n";
             break;
         case AgentType::Refactor:
             prompt << "Role: refactor worker.\n"
                 << "Treat the task as behavior-preserving unless the user explicitly asks for semantic change.\n"
                 << "Name the invariants, compatibility assumptions, or regression checks that must hold, prefer reviewable edits, and run targeted regression validation before finishing.\n"
                 << "If the work actually adds new behavior or needs broader planning, say so instead of silently changing semantics.\n";
             break;
         case AgentType::Audit:
             prompt << "Role: audit worker.\n"
                 << "Review the requested code or changes without modifying the workspace.\n"
                 << "Run the audit systematically: map the relevant repository surface first, identify entrypoints and ownership boundaries, inspect at least one validation or contract surface when present, then pair each finding with both the implementation evidence and the violated contract or observed behavior.\n"
                 << "Report findings first, ordered by severity, with concrete evidence and affected files.\n"
                 << "Keep only findings that are directly supported by gathered evidence; if the exact code, symbol, or tool output is missing or contradictory, drop the finding.\n"
                 << "Do not label something a test bug or mock issue unless the evidence explicitly shows the test expectation or patch target is wrong; otherwise describe the code/test contract mismatch neutrally.\n"
                 << "Do not pad the answer with generic sections such as broad summaries, missing features, documentation advice, security considerations, or production-readiness claims unless the user explicitly asked for them or the evidence directly supports them.\n"
                 << "Only state counts or percentages when they come from explicit tool output, and if tests or command output show multiple distinct failure clusters, reflect those actual clusters instead of collapsing them into a single guessed root cause.\n"
                 << "When planner_validate_review is available and a tracked review case exists, validate your draft report text before you finish, and if validation says required coverage is missing, gather the missing evidence with targeted reads or searches before you finalize.\n"
                 << "Avoid repeated rereads once the evidence is already sufficient, prefer targeted inspection over broad sweeps, and stop exploring as soon as you can support a conclusion.\n"
                 << "State explicitly when no findings are present. Do not modify files or claim fixes were applied.\n";
             break;
         case AgentType::Bugfix:
             prompt << "Role: bugfix worker.\n"
                 << "Capture the failing behavior or repro first, isolate the root cause, and make the smallest defensible fix.\n"
                 << "Prefer direct repository work for contained issues. If the problem expands into broader planning or feature work, say so explicitly rather than widening scope silently.\n"
                 << "When planner_validate_bugfix is available and you have a tracked case id or case path, validate the final writeup before you finish.\n"
                 << "Run the repro or targeted validation after the fix, or state exactly why it could not be run.\n";
             break;
        case AgentType::Research:
            prompt << "Role: research worker.\n"
                   << "Use read-only local tools plus web_search/web_fetch to gather evidence and summarize it clearly.\n"
                 << "Prefer focused source collection over open-ended browsing, avoid repeating the same searches unless confirming a concrete point, and stop once you have enough evidence to answer.\n"
                   << "Do not modify files. If web_search reports that BRAVE_SEARCH_KEY is missing, say that briefly and continue with any specific URLs via web_fetch when useful.\n";
            break;
        case AgentType::Spec:
            prompt << "Role: specification writer.\n"
                   << "Create or update SPEC.md in the workspace root unless the task names a different target file.\n"
                   << "The spec must be concrete, implementation-oriented, and suitable for the planner-harness rubric.\n"
                   << "Use local inspection and web research when necessary.\n"
                 << "Use planner_validate_spec for validation instead of scripting the harness manually whenever possible.\n"
                   << "Iterate on SPEC.md when validation reveals clear rubric failures. Return the artifact path and the key validation outcome.\n";
            break;
        case AgentType::Plan:
            prompt << "Role: implementation planner.\n"
                   << "Read SPEC.md, generate a planner prompt, generate PLAN.json, and validate the result for graph execution compatibility.\n"
               << "Use planner_build_from_idea when the user provides an idea and needs SPEC.md plus PLAN.json in one run.\n"
               << "Use planner_build_plan when SPEC.md already exists, planner_repair_plan to patch an existing PLAN.json after failures, and planner_validate_plan for the final check on any candidate plan.\n"
               << "If a planner tool result starts with 'PLANNER_* STATUS: FAILED', treat that as a failed validation even if the raw JSON contains command-level ok fields.\n"
                << "If a planner tool result starts with 'PLANNER_* STATUS: CLARIFICATION_REQUIRED', ask the user the listed questions conversationally, accept one-by-one or batched answers, and then rerun the same planner tool with clarification answers included.\n"
                << "If the user says 'you decide for me', pass delegate_unanswered=true so recommended defaults are applied for unresolved clarification questions.\n"
               << "If you edit or repair PLAN.json in any way, run planner_validate_plan again before you finish.\n"
               << "Do not report success unless the latest planner_validate_plan result passes; otherwise report the remaining blocking checks explicitly.\n"
               << "For Python package __init__.py files, include matching tests named tests/test_<parent>_init.py instead of treating package markers as exempt.\n"
                   << "The final plan must use the phases/tasks/file schema compatible with the existing PLAN.json graph parser. Return artifact paths and the validation summary.\n";
            break;
        case AgentType::GeneralPurpose:
            prompt << "Role: general-purpose worker. Use the available tools to complete the assigned task directly.\n";
            break;
    }

    return prompt.str();
}

std::string AgentManager::spawn(const AgentConfig& config,
                                 AgentCompletionCallback on_complete)
{
    const std::string agent_id = [this]() {
        std::lock_guard<std::mutex> lock(mutex_);
        return generate_id();
    }();

    observer_.on_event(AgentSpawnedEvent{agent_id, config.task, profile_name_for_type(config.type), {}});

    // Build a shared observer so we can read results after completion.
    auto observer = std::make_shared<AgentObserver>();

    // Create the child session.
    auto session = engine_.create_session(*observer, delegate_);

    ToolContext child_context = parent_tool_context_;
    child_context.profile = profile_name_for_type(config.type);
    if (!child_context.workspace_root.empty()) {
        session->set_tool_context(child_context);
    }
    session->set_system_prompt(system_prompt_for_type(config.type));
    session->set_permission_mode(permission_mode_for_type(config.type));
    session->set_evidence_based_final_answer(config.type == AgentType::Audit);

    // Apply tool filter for typed agents.
    const std::vector<std::string> allowed = filter_tools_for_type(config.type);
    if (!allowed.empty()) {
        session->set_tool_filter(allowed);
    }

    // Register state.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        AgentState state;
        state.id      = agent_id;
        state.config  = config;
        state.observer = observer;
        state.session = std::move(session);
        state.running = true;
        agents_.emplace(agent_id, std::move(state));
    }

    // Helper to build result from observer after completion.
    auto build_result = [agent_id, obs = observer]() -> AgentResult {
        AgentResult r;
        r.agent_id      = agent_id;
        r.response      = obs->response();
        r.usage         = obs->usage();
        r.is_error      = obs->is_error();
        r.error_message = obs->error_message();
        return r;
    };

    if (config.run_in_background) {
        // Submit is non-blocking — engine pool runs the work.
        // We need to watch for completion in a separate thread that owns
        // a shared_ptr to the session (prevents UAF if AgentManager is
        // destroyed before the agent finishes).
        std::shared_ptr<Session> session_ptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session_ptr = agents_.at(agent_id).session;
        }

        session_ptr->submit(config.task);

        // Launch watcher thread — captures session by shared_ptr (safe lifetime),
        // and everything else by value.
        std::thread([this, agent_id, session_ptr, build_result,
                     on_complete = std::move(on_complete)]() mutable {
            session_ptr->wait();

            AgentResult result = build_result();

            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = agents_.find(agent_id);
                if (it != agents_.end()) {
                    it->second.result  = result;
                    it->second.running = false;
                }
            }
            state_cv_.notify_all();

            if (on_complete) {
                on_complete(result);
            }

            observer_.on_event(AgentCompletedEvent{agent_id, !result.is_error, {}});
        }).detach();

        return agent_id;
    }

    // Foreground: submit then block until done.
    std::shared_ptr<Session> session_ptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_ptr = agents_.at(agent_id).session;
    }

    session_ptr->submit(config.task);
    session_ptr->wait();

    AgentResult result = build_result();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = agents_.find(agent_id);
        if (it != agents_.end()) {
            it->second.result  = result;
            it->second.running = false;
        }
    }
    state_cv_.notify_all();

    if (on_complete) {
        on_complete(result);
    }

    observer_.on_event(AgentCompletedEvent{agent_id, !result.is_error, {}});

    return agent_id;
}

bool AgentManager::send_message(const std::string& agent_id,
                                 const std::string& message)
{
    std::shared_ptr<Session> session_ptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = agents_.find(agent_id);
        if (it == agents_.end()) return false;
        session_ptr = it->second.session;
    }

    if (!session_ptr) return false;

    session_ptr->wait();
    session_ptr->submit(message);
    return true;
}

std::optional<AgentResult> AgentManager::get_result(
    const std::string& agent_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = agents_.find(agent_id);
    if (it == agents_.end()) return std::nullopt;
    return it->second.result;
}

bool AgentManager::is_running(const std::string& agent_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = agents_.find(agent_id);
    if (it == agents_.end()) return false;
    return it->second.running;
}

size_t AgentManager::active_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& [_, state] : agents_) {
        if (state.running) ++n;
    }
    return n;
}

bool AgentManager::wait_for(const std::string& agent_id) const {
    std::shared_ptr<Session> session_ptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = agents_.find(agent_id);
        if (it == agents_.end()) return false;
        session_ptr = it->second.session;
    }
    if (!session_ptr) return false;
    session_ptr->wait();

    std::unique_lock<std::mutex> lock(mutex_);
    state_cv_.wait(lock, [&]() {
        const auto it = agents_.find(agent_id);
        return it == agents_.end() || !it->second.running;
    });
    return true;
}

void AgentManager::stop_all_running() {
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [_, state] : agents_) {
            if (state.running && state.session) {
                sessions.push_back(state.session);
            }
        }
    }

    for (const auto& session : sessions) {
        session->stop();
    }
}

void AgentManager::cancel_all_running() {
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [_, state] : agents_) {
            if (state.running && state.session) {
                sessions.push_back(state.session);
            }
        }
    }

    for (const auto& session : sessions) {
        session->cancel();
    }
}

}  // namespace omni::engine
