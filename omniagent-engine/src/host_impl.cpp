#include "project_runtime_internal.h"

#include <map>
#include <random>
#include <sstream>

namespace omni::engine {

namespace {

void validate_workspace(WorkspaceContext& workspace) {
    if (workspace.project_id.empty()) {
        throw std::invalid_argument("workspace project_id must not be empty");
    }
    if (workspace.workspace_root.empty()) {
        throw std::invalid_argument("workspace_root must not be empty");
    }
    if (!std::filesystem::exists(workspace.workspace_root)) {
        throw std::invalid_argument("workspace_root does not exist: " + workspace.workspace_root.string());
    }

    workspace.workspace_root = std::filesystem::weakly_canonical(workspace.workspace_root);
    if (workspace.working_dir.empty()) {
        workspace.working_dir = workspace.workspace_root;
    }
    if (!std::filesystem::exists(workspace.working_dir)) {
        throw std::invalid_argument("working_dir does not exist: " + workspace.working_dir.string());
    }
    workspace.working_dir = std::filesystem::weakly_canonical(workspace.working_dir);

    if (!is_within_workspace(workspace, workspace.working_dir)) {
        throw std::invalid_argument("working_dir must remain within workspace_root");
    }
}

std::shared_ptr<HostState> make_host_state(ProjectRuntimeConfig config) {
    validate_workspace(config.workspace);
    if (!config.provider_factory) {
        throw std::invalid_argument("provider_factory must not be empty");
    }

    auto provider = config.provider_factory();
    if (!provider) {
        throw std::invalid_argument("provider_factory returned a null provider");
    }

    auto host_state = std::make_shared<HostState>();
    host_state->config = std::move(config);
    host_state->profiles = merge_profiles(host_state->config.profiles);
    host_state->engine = std::make_unique<Engine>(host_state->config.engine, std::move(provider));

    for (auto& tool : host_state->config.project_tools) {
        if (tool) {
            host_state->engine->register_tool(std::move(tool));
        }
    }
    host_state->config.project_tools.clear();

    for (const auto& mcp_config : host_state->config.mcp_servers) {
        if (host_state->engine->connect_mcp_server(mcp_config)) {
            host_state->connected_mcp_servers.push_back(mcp_config.name);
        }
    }

    return host_state;
}

}  // namespace

std::string generate_run_id() {
    using clock = std::chrono::system_clock;
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        clock::now().time_since_epoch()).count();

    thread_local std::mt19937_64 generator{std::random_device{}()};
    const auto suffix = static_cast<unsigned int>(generator() & 0xFFFFFFu);

    std::ostringstream stream;
    stream << "run-" << now_us << '-';
    stream << std::hex << suffix;
    return stream.str();
}

bool is_within_workspace(const WorkspaceContext& workspace,
                         const std::filesystem::path& candidate) {
    if (workspace.allow_workspace_escape) {
        return true;
    }

    const auto root = std::filesystem::weakly_canonical(workspace.workspace_root).lexically_normal();
    const auto path = std::filesystem::weakly_canonical(candidate).lexically_normal();
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

void ForwardingObserver::on_event(const Event& event) {
    auto session_state = session_state_.lock();
    if (!session_state) {
        return;
    }

    auto run_state = current_run_for_session(session_state);
    if (!run_state) {
        return;
    }

    bool clear_active_run = false;
    Event enriched = event;

    {
        std::lock_guard<std::mutex> run_lock(run_state->mutex);
        enriched = enrich_event(event,
                                session_state->project_id,
                                session_state->session_id,
                                run_state->result.run_id);

        if (const auto* text_delta = std::get_if<TextDeltaEvent>(&enriched)) {
            run_state->result.output += text_delta->text;
        } else if (const auto* error = std::get_if<ErrorEvent>(&enriched)) {
            if (!error->recoverable) {
                run_state->result.error = error->message;
            }
        } else if (const auto* usage = std::get_if<UsageUpdatedEvent>(&enriched)) {
            run_state->result.usage = usage->cumulative;
        } else if (const auto* done = std::get_if<DoneEvent>(&enriched)) {
            run_state->result.usage = done->usage;
            clear_active_run = true;
        }
    }

    dispatch_run_event(run_state, enriched);

    if (clear_active_run) {
        finalize_run_state(run_state, RunStatus::Completed);
        persist_run_state(run_state);
        RunStatus final_status = RunStatus::Completed;
        {
            std::lock_guard<std::mutex> run_lock(run_state->mutex);
            final_status = run_state->result.status;
        }
        if (final_status == RunStatus::Stopped) {
            dispatch_run_event(run_state, RunStoppedEvent{run_state->result.run_id, {}});
        } else if (final_status == RunStatus::Cancelled) {
            dispatch_run_event(run_state, RunCancelledEvent{run_state->result.run_id, {}});
        }
    }
}

PermissionDecision ForwardingPermissionDelegate::on_permission_request(
    const std::string& tool_name,
    const nlohmann::json& args,
    const std::string& description) {
    auto session_state = session_state_.lock();
    if (!session_state) {
        return PermissionDecision::Deny;
    }

    auto run_state = current_run_for_session(session_state);
    if (!run_state) {
        return PermissionDecision::Deny;
    }

    ApprovalDelegate* approvals = nullptr;
    PendingApproval pending_approval;
    pending_approval.tool_name = tool_name;
    pending_approval.args = args;
    pending_approval.description = description;
    pending_approval.requested_at = std::chrono::system_clock::now();
    {
        std::lock_guard<std::mutex> run_lock(run_state->mutex);
        approvals = run_state->approvals;
    }

    dispatch_run_event(run_state, ApprovalRequestedEvent{tool_name, args, description, {}});

    if (!approvals) {
        dispatch_run_event(run_state, ApprovalResolvedEvent{tool_name, ApprovalDecision::Deny, {}});
        return PermissionDecision::Deny;
    }

    ApprovalDecision decision = approvals->on_approval_requested(tool_name, args, description);

    if (decision == ApprovalDecision::Pause) {
        std::string run_id;
        {
            std::lock_guard<std::mutex> run_lock(run_state->mutex);
            if (run_state->finalised) {
                return PermissionDecision::Deny;
            }
            run_state->result.status = RunStatus::Paused;
            run_state->result.pause_reason = "approval_required";
            run_state->result.pending_approval = pending_approval;
            run_id = run_state->result.run_id;
        }

        persist_run_state(run_state);
        dispatch_run_event(run_state, RunPausedEvent{run_id, "approval_required", {}});

        bool cancelled_or_stopped = false;
        {
            std::unique_lock<std::mutex> run_lock(run_state->mutex);
            run_state->approval_cv.wait(run_lock, [&]() {
                return run_state->finalised || run_state->pending_resolution.has_value();
            });
            if (run_state->finalised) {
                return PermissionDecision::Deny;
            }

            decision = *run_state->pending_resolution;
            run_state->pending_resolution.reset();
            cancelled_or_stopped = run_state->cancel_requested || run_state->stop_requested;
            run_state->result.status = RunStatus::Running;
            run_state->result.pause_reason.reset();
            run_state->result.pending_approval.reset();
        }

        persist_run_state(run_state);
        if (!cancelled_or_stopped) {
            dispatch_run_event(run_state, RunResumedEvent{run_id, {}});
        }
    }

    dispatch_run_event(run_state, ApprovalResolvedEvent{tool_name, decision, {}});

    switch (decision) {
        case ApprovalDecision::Approve:
            return PermissionDecision::Allow;
        case ApprovalDecision::ApproveAlways:
            return PermissionDecision::AlwaysAllow;
        case ApprovalDecision::Deny:
            return PermissionDecision::Deny;
        case ApprovalDecision::Pause:
            return PermissionDecision::Deny;
    }

    return PermissionDecision::Deny;
}

ProjectEngineHost::ProjectEngineHost(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ProjectEngineHost::~ProjectEngineHost() {
    if (!impl_) {
        return;
    }
    try {
        shutdown();
    } catch (...) {
    }
}
ProjectEngineHost::ProjectEngineHost(ProjectEngineHost&&) noexcept = default;
ProjectEngineHost& ProjectEngineHost::operator=(ProjectEngineHost&&) noexcept = default;

std::unique_ptr<ProjectEngineHost> ProjectEngineHost::create(ProjectRuntimeConfig config) {
    return std::unique_ptr<ProjectEngineHost>(
        new ProjectEngineHost(std::make_shared<Impl>(make_host_state(std::move(config)))));
}

const std::string& ProjectEngineHost::project_id() const {
    return impl_->state->config.workspace.project_id;
}

const WorkspaceContext& ProjectEngineHost::workspace() const {
    return impl_->state->config.workspace;
}

std::unique_ptr<ProjectSession> ProjectEngineHost::open_session(SessionOptions options) {
    auto host_state = impl_->state;
    {
        std::lock_guard<std::mutex> host_lock(host_state->mutex);
        if (!host_state->running) {
            throw std::runtime_error("project host is shut down");
        }
    }

    auto session_state = std::make_shared<SessionState>();
    session_state->host = host_state;
    session_state->project_id = host_state->config.workspace.project_id;
    session_state->active_profile = options.profile.empty() ? "explore" : options.profile;
    session_state->working_dir = options.working_dir_override.value_or(default_working_dir(host_state->config.workspace));

    if (!std::filesystem::exists(session_state->working_dir)) {
        throw std::invalid_argument("working_dir does not exist: " + session_state->working_dir.string());
    }
    if (!is_within_workspace(host_state->config.workspace, session_state->working_dir)) {
        throw std::invalid_argument("working_dir must remain within workspace_root");
    }

    session_state->observer_bridge = std::make_shared<ForwardingObserver>(session_state);
    session_state->approval_bridge = std::make_shared<ForwardingPermissionDelegate>(session_state);
    session_state->session.reset(host_state->engine->create_session(
        *session_state->observer_bridge,
        *session_state->approval_bridge,
        options.session_id).release());
    session_state->session_id = session_state->session->id();

    {
        std::lock_guard<std::mutex> host_lock(host_state->mutex);
        std::lock_guard<std::mutex> session_lock(session_state->mutex);
        apply_profile_filter_locked(session_state, *host_state);
        prune_closed_sessions_locked(*host_state);
        host_state->live_sessions.push_back(session_state);
    }

    return std::unique_ptr<ProjectSession>(
        new ProjectSession(std::make_shared<ProjectSession::Impl>(session_state)));
}

std::unique_ptr<ProjectSession> ProjectEngineHost::resume_session(const std::string& session_id) {
    if (auto live_session = find_live_session(impl_->state, session_id)) {
        return std::unique_ptr<ProjectSession>(
            new ProjectSession(std::make_shared<ProjectSession::Impl>(live_session)));
    }

    if (!has_persistence(*impl_->state)) {
        throw std::runtime_error("session not found: " + session_id);
    }

    SessionPersistence persistence = make_session_persistence(*impl_->state);
    auto record = persistence.load(session_id);
    if (!record) {
        throw std::runtime_error("session not found: " + session_id);
    }

    SessionOptions options;
    options.profile = "explore";
    options.session_id = session_id;

    auto session = open_session(options);
    session->impl_->state->session->resume(record->messages);
    return session;
}

std::unique_ptr<ProjectSession> ProjectEngineHost::fork_session(const std::string& session_id,
                                                                SessionOptions options) {
    if (session_id.empty()) {
        throw std::invalid_argument("source session id must not be empty");
    }

    auto host_state = impl_->state;
    std::string source_profile = "explore";
    std::filesystem::path source_working_dir = default_working_dir(host_state->config.workspace);
    std::vector<Message> source_messages;

    if (auto source_session = find_live_session(host_state, session_id)) {
        std::lock_guard<std::mutex> session_lock(source_session->mutex);
        if (source_session->closed) {
            throw std::runtime_error("session is closed: " + session_id);
        }
        source_profile = source_session->active_profile;
        source_working_dir = source_session->working_dir;
        source_messages = source_session->session->messages();
    } else {
        if (!has_persistence(*host_state)) {
            throw std::runtime_error("session not found: " + session_id);
        }
        SessionPersistence persistence = make_session_persistence(*host_state);
        auto record = persistence.load(session_id);
        if (!record) {
            throw std::runtime_error("session not found: " + session_id);
        }
        source_messages = std::move(record->messages);
    }

    if (options.session_id.has_value()) {
        const std::string& requested_id = *options.session_id;
        if (requested_id.empty()) {
            throw std::invalid_argument("fork session id must not be empty");
        }
        if (find_live_session(host_state, requested_id)) {
            throw std::runtime_error("session id already exists: " + requested_id);
        }
        if (has_persistence(*host_state)) {
            SessionPersistence persistence = make_session_persistence(*host_state);
            if (persistence.load(requested_id).has_value()) {
                throw std::runtime_error("session id already exists: " + requested_id);
            }
        }
    }

    options.profile = source_profile;
    if (!options.working_dir_override.has_value()) {
        options.working_dir_override = source_working_dir;
    }

    auto forked_session = open_session(std::move(options));
    forked_session->impl_->state->session->resume(source_messages);
    forked_session->impl_->state->session->persist();
    return forked_session;
}

std::vector<SessionSummary> ProjectEngineHost::list_sessions() const {
    auto host_state = impl_->state;
    std::map<std::string, SessionSummary> summaries;

    if (has_persistence(*host_state)) {
        SessionPersistence persistence = make_session_persistence(*host_state);
        for (const auto& session_id : persistence.list()) {
            auto record = persistence.load(session_id);
            if (record) {
                summaries[session_id] = build_persisted_session_summary(host_state->config.workspace, *record);
            }
        }
    }

    std::lock_guard<std::mutex> host_lock(host_state->mutex);
    for (const auto& session_state : host_state->live_sessions) {
        if (!session_state) {
            continue;
        }
        std::lock_guard<std::mutex> session_lock(session_state->mutex);
        if (session_state->closed) {
            continue;
        }
        summaries[session_state->session_id] = build_live_session_summary(*session_state);
    }

    std::vector<SessionSummary> result;
    result.reserve(summaries.size());
    for (auto& [_, summary] : summaries) {
        result.push_back(std::move(summary));
    }
    return result;
}

bool ProjectEngineHost::close_session(const std::string& session_id) {
    auto host_state = impl_->state;
    auto session_state = find_live_session(host_state, session_id);
    if (!session_state) {
        return false;
    }

    ProjectSession session(std::make_shared<ProjectSession::Impl>(session_state));
    session.close();
    return true;
}

std::vector<RunSummary> ProjectEngineHost::list_runs() const {
    auto host_state = impl_->state;
    std::map<std::string, RunSummary> summaries;

    if (has_persistence(*host_state)) {
        RunPersistence persistence = make_run_persistence(*host_state);
        for (const auto& summary : persistence.list(host_state->config.workspace.project_id)) {
            summaries[summary.run_id] = summary;
        }
    }

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
        summaries[session_state->active_run->result.run_id] =
            build_run_summary(session_state->active_run->result);
    }

    std::vector<RunSummary> result;
    result.reserve(summaries.size());
    for (auto& [_, summary] : summaries) {
        result.push_back(std::move(summary));
    }
    std::sort(result.begin(), result.end(),
              [](const RunSummary& left, const RunSummary& right) {
                  return left.started_at > right.started_at;
              });
    return result;
}

std::optional<RunResult> ProjectEngineHost::get_run(const std::string& run_id) const {
    auto host_state = impl_->state;
    if (auto run_state = find_live_run(host_state, run_id)) {
        std::lock_guard<std::mutex> run_lock(run_state->mutex);
        return run_state->result;
    }

    if (!has_persistence(*host_state)) {
        return std::nullopt;
    }

    RunPersistence persistence = make_run_persistence(*host_state);
    return persistence.load(run_id);
}

bool ProjectEngineHost::cancel_run(const std::string& run_id) {
    auto run_state = find_live_run(impl_->state, run_id);
    if (!run_state) {
        return false;
    }

    {
        std::lock_guard<std::mutex> run_lock(run_state->mutex);
        if (run_state->finalised) {
            return false;
        }
        run_state->cancel_requested = true;
        if (run_state->result.pending_approval.has_value()) {
            run_state->pending_resolution = ApprovalDecision::Deny;
        }
    }
    run_state->approval_cv.notify_all();

    if (auto session_state = run_state->session.lock()) {
        session_state->session->cancel();
    }
    return true;
}

bool ProjectEngineHost::stop_run(const std::string& run_id) {
    auto run_state = find_live_run(impl_->state, run_id);
    if (!run_state) {
        return false;
    }

    {
        std::lock_guard<std::mutex> run_lock(run_state->mutex);
        if (run_state->finalised) {
            return false;
        }
        run_state->stop_requested = true;
        if (run_state->result.pending_approval.has_value()) {
            run_state->pending_resolution = ApprovalDecision::Deny;
        }
    }
    run_state->approval_cv.notify_all();

    if (auto session_state = run_state->session.lock()) {
        session_state->session->stop();
    }
    return true;
}

bool ProjectEngineHost::resume_run(const std::string& run_id, const std::string& resume_input) {
    auto run_state = find_live_run(impl_->state, run_id);
    if (!run_state) {
        return false;
    }

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

bool ProjectEngineHost::delete_run(const std::string& run_id) {
    if (find_live_run(impl_->state, run_id)) {
        return false;
    }
    if (!has_persistence(*impl_->state)) {
        return false;
    }
    RunPersistence persistence = make_run_persistence(*impl_->state);
    return persistence.remove(run_id);
}

void ProjectEngineHost::reload(ProjectRuntimeConfig config) {
    auto current_state = impl_->state;
    {
        std::lock_guard<std::mutex> host_lock(current_state->mutex);
        prune_closed_sessions_locked(*current_state);
        if (!current_state->live_sessions.empty()) {
            throw std::runtime_error("cannot reload host while sessions are still open");
        }
    }

    impl_->state = make_host_state(std::move(config));
}

HostStatus ProjectEngineHost::status() const {
    auto host_state = impl_->state;
    HostStatus host_status;
    host_status.project_id = host_state->config.workspace.project_id;
    host_status.workspace_root = host_state->config.workspace.workspace_root;
    host_status.working_dir = default_working_dir(host_state->config.workspace);
    host_status.running = host_state->running;
    host_status.connected_mcp_servers = host_state->connected_mcp_servers;
    if (has_persistence(*host_state)) {
        SessionPersistence persistence = make_session_persistence(*host_state);
        host_status.persisted_sessions = persistence.list().size();
    }

    std::lock_guard<std::mutex> host_lock(host_state->mutex);
    prune_closed_sessions_locked(*host_state);
    host_status.open_sessions = host_state->live_sessions.size();
    for (const auto& session_state : host_state->live_sessions) {
        if (!session_state) {
            continue;
        }
        std::lock_guard<std::mutex> session_lock(session_state->mutex);
        if (session_state->active_run) {
            ++host_status.active_runs;
        }
    }

    return host_status;
}

void ProjectEngineHost::shutdown() {
    auto host_state = impl_->state;
    std::vector<std::shared_ptr<SessionState>> live_sessions;
    {
        std::lock_guard<std::mutex> host_lock(host_state->mutex);
        if (!host_state->running) {
            return;
        }
        host_state->running = false;
        live_sessions = host_state->live_sessions;
    }

    for (const auto& session_state : live_sessions) {
        if (!session_state) {
            continue;
        }

        std::shared_ptr<RunState> active_run;
        {
            std::lock_guard<std::mutex> session_lock(session_state->mutex);
            active_run = session_state->active_run;
        }

        try {
            session_state->session->cancel();
            session_state->session->wait();
        } catch (...) {
        }

        if (active_run) {
            finalize_run_state(active_run, RunStatus::Cancelled);
            persist_run_state(active_run);
            mark_run_settled(active_run);
        }

        std::lock_guard<std::mutex> session_lock(session_state->mutex);
        session_state->closed = true;
        session_state->active_run.reset();
    }
}

void ProjectEngineHost::register_tool(std::unique_ptr<Tool> tool) {
    if (!tool) {
        return;
    }
    auto host_state = impl_->state;
    host_state->engine->register_tool(std::move(tool));

    std::vector<std::shared_ptr<SessionState>> sessions;
    {
        std::lock_guard<std::mutex> host_lock(host_state->mutex);
        sessions = host_state->live_sessions;
    }

    for (const auto& session_state : sessions) {
        if (!session_state) {
            continue;
        }
        std::lock_guard<std::mutex> session_lock(session_state->mutex);
        if (!session_state->closed && !session_state->active_run) {
            apply_profile_filter_locked(session_state, *host_state);
        }
    }
}

bool ProjectEngineHost::connect_mcp(const MCPServerConfig& config) {
    auto host_state = impl_->state;
    const bool connected = host_state->engine->connect_mcp_server(config);
    if (connected) {
        std::vector<std::shared_ptr<SessionState>> sessions;
        {
            std::lock_guard<std::mutex> lock(host_state->mutex);
            host_state->connected_mcp_servers.push_back(config.name);
            sessions = host_state->live_sessions;
        }
        for (const auto& session_state : sessions) {
            if (!session_state) {
                continue;
            }
            std::lock_guard<std::mutex> session_lock(session_state->mutex);
            if (!session_state->closed && !session_state->active_run) {
                apply_profile_filter_locked(session_state, *host_state);
            }
        }
    }
    return connected;
}

void ProjectEngineHost::disconnect_mcp(const std::string& name) {
    auto host_state = impl_->state;
    host_state->engine->disconnect_mcp_server(name);

    std::vector<std::shared_ptr<SessionState>> sessions;
    {
        std::lock_guard<std::mutex> lock(host_state->mutex);
        host_state->connected_mcp_servers.erase(
            std::remove(host_state->connected_mcp_servers.begin(),
                        host_state->connected_mcp_servers.end(),
                        name),
            host_state->connected_mcp_servers.end());
        sessions = host_state->live_sessions;
    }

    for (const auto& session_state : sessions) {
        if (!session_state) {
            continue;
        }
        std::lock_guard<std::mutex> session_lock(session_state->mutex);
        if (!session_state->closed && !session_state->active_run) {
            apply_profile_filter_locked(session_state, *host_state);
        }
    }
}

}  // namespace omni::engine