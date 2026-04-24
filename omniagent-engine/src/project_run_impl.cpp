#include "project_runtime_internal.h"

namespace omni::engine {

ProjectRun::ProjectRun(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ProjectRun::~ProjectRun() = default;
ProjectRun::ProjectRun(ProjectRun&&) noexcept = default;
ProjectRun& ProjectRun::operator=(ProjectRun&&) noexcept = default;

const std::string& ProjectRun::run_id() const {
    return impl_->state->result.run_id;
}

RunStatus ProjectRun::status() const {
    std::lock_guard<std::mutex> lock(impl_->state->mutex);
    return impl_->state->result.status;
}

void ProjectRun::cancel() {
    auto run_state = impl_->state;
    {
        std::lock_guard<std::mutex> lock(run_state->mutex);
        if (run_state->finalised) {
            return;
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
}

void ProjectRun::stop() {
    auto run_state = impl_->state;
    {
        std::lock_guard<std::mutex> lock(run_state->mutex);
        if (run_state->finalised) {
            return;
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
}

void ProjectRun::resume(const std::string& resume_input) {
    auto run_state = impl_->state;
    if (!resume_live_run(run_state, resume_input)) {
        throw std::runtime_error("run is not paused");
    }
}

void ProjectRun::wait() {
    auto run_state = impl_->state;
    std::unique_lock<std::mutex> lock(run_state->mutex);
    run_state->approval_cv.wait(lock, [&]() { return run_state->settled; });
}

RunResult ProjectRun::result() const {
    std::lock_guard<std::mutex> lock(impl_->state->mutex);
    return impl_->state->result;
}

}  // namespace omni::engine