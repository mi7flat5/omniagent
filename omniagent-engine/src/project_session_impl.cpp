#include "project_runtime_internal.h"

#include <thread>

namespace omni::engine {

ProjectSession::ProjectSession(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ProjectSession::~ProjectSession() {
    try {
        close();
    } catch (...) {
    }
}

ProjectSession::ProjectSession(ProjectSession&&) noexcept = default;
ProjectSession& ProjectSession::operator=(ProjectSession&&) noexcept = default;

const std::string& ProjectSession::session_id() const {
    return impl_->state->session_id;
}

const std::string& ProjectSession::project_id() const {
    return impl_->state->project_id;
}

const std::string& ProjectSession::active_profile() const {
    return impl_->state->active_profile;
}

std::unique_ptr<ProjectRun> ProjectSession::submit_turn(const std::string& input,
                                                        RunObserver& observer,
                                                        ApprovalDelegate& approvals) {
    auto session_state = impl_->state;
    auto host_state = require_host_state(session_state);

    auto run_state = std::make_shared<RunState>();
    run_state->session = session_state;
    run_state->observer = &observer;
    run_state->approvals = &approvals;
    run_state->result.run_id = generate_run_id();
    run_state->result.session_id = session_state->session_id;
    run_state->result.project_id = session_state->project_id;
    run_state->result.profile = session_state->active_profile;
    run_state->result.input = input;
    run_state->result.started_at = std::chrono::system_clock::now();
    run_state->result.status = RunStatus::Running;

    {
        std::lock_guard<std::mutex> session_lock(session_state->mutex);
        if (session_state->closed) {
            throw std::runtime_error("session is closed");
        }
        if (session_state->active_run) {
            throw std::runtime_error("session already has an active run");
        }
        session_state->active_run = run_state;
        bind_run_tool_context_locked(session_state, host_state, run_state);
    }

    dispatch_run_event(run_state, RunStartedEvent{run_state->result.run_id, session_state->active_profile, {}});
    persist_run_state(run_state);
    launch_run_execution(session_state, run_state, input);

    return std::unique_ptr<ProjectRun>(
        new ProjectRun(std::make_shared<ProjectRun::Impl>(run_state)));
}

void ProjectSession::set_profile(const std::string& profile_name) {
    auto session_state = impl_->state;
    auto host_state = require_host_state(session_state);

    std::lock_guard<std::mutex> session_lock(session_state->mutex);
    if (session_state->active_run) {
        throw std::runtime_error("cannot change profile while a run is active");
    }
    session_state->active_profile = profile_name.empty() ? "explore" : profile_name;
    apply_profile_filter_locked(session_state, *host_state);
}

SessionSnapshot ProjectSession::snapshot() const {
    SessionSnapshot snapshot;
    auto session_state = impl_->state;
    std::lock_guard<std::mutex> session_lock(session_state->mutex);

    snapshot.session_id = session_state->session_id;
    snapshot.project_id = session_state->project_id;
    snapshot.active_profile = session_state->active_profile;
    snapshot.working_dir = session_state->working_dir;
    snapshot.messages = session_state->session->messages();
    snapshot.usage = session_state->session->usage();
    if (session_state->active_run) {
        std::lock_guard<std::mutex> run_lock(session_state->active_run->mutex);
        snapshot.active_run_id = session_state->active_run->result.run_id;
    }

    return snapshot;
}

std::vector<ToolSummary> ProjectSession::tools() const {
    auto session_state = impl_->state;
    std::vector<ToolSummary> tools;

    std::lock_guard<std::mutex> session_lock(session_state->mutex);
    for (const auto& tool_name : session_state->session->tool_names()) {
        if (Tool* tool = session_state->session->find_tool(tool_name)) {
            tools.push_back(build_tool_summary(*tool));
        }
    }

    std::sort(tools.begin(), tools.end(),
              [](const ToolSummary& left, const ToolSummary& right) {
                  return left.name < right.name;
              });
    return tools;
}

void ProjectSession::reset() {
    auto session_state = impl_->state;
    std::lock_guard<std::mutex> session_lock(session_state->mutex);
    if (session_state->active_run) {
        throw std::runtime_error("cannot reset while a run is active");
    }
    session_state->session->resume({});
    session_state->session->persist();
}

std::size_t ProjectSession::rewind_messages(std::size_t count) {
    if (count == 0) {
        return 0;
    }

    auto session_state = impl_->state;
    std::lock_guard<std::mutex> session_lock(session_state->mutex);
    if (session_state->active_run) {
        throw std::runtime_error("cannot rewind while a run is active");
    }

    auto messages = session_state->session->messages();
    const std::size_t removable = std::min(count, messages.size());
    if (removable == 0) {
        return 0;
    }

    messages.resize(messages.size() - removable);
    session_state->session->resume(messages);
    session_state->session->persist();
    return removable;
}

void ProjectSession::close() {
    auto session_state = impl_->state;

    std::shared_ptr<RunState> active_run;
    {
        std::lock_guard<std::mutex> session_lock(session_state->mutex);
        if (session_state->closed) {
            return;
        }
        active_run = session_state->active_run;
        session_state->closed = true;
    }

    if (active_run) {
        session_state->session->cancel();
    }

    try {
        session_state->session->wait();
    } catch (...) {
    }

    if (active_run) {
        finalize_run_state(active_run, RunStatus::Cancelled);
        persist_run_state(active_run);
        clear_active_run_if_matches(session_state, active_run);
        mark_run_settled(active_run);
    }
}

}  // namespace omni::engine