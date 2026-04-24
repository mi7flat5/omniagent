#include "session_impl.h"

#include <omni/session.h>
#include <omni/tool.h>
#include "services/cost_tracker.h"

#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

namespace omni::engine {

// ---------------------------------------------------------------------------
// Internal: generate a session ID from timestamp + random suffix
// ---------------------------------------------------------------------------

namespace {

std::string generate_session_id() {
    using Clock = std::chrono::system_clock;
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            Clock::now().time_since_epoch()).count();

    // 6-char hex random suffix
    thread_local std::mt19937_64 rng{std::random_device{}()};
    const uint32_t rnd = static_cast<uint32_t>(rng() & 0xFFFFFF);

    std::ostringstream oss;
    oss << now_us << '-'
        << std::hex << std::setw(6) << std::setfill('0') << rnd;
    return oss.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// Session constructor / destructor / move
// ---------------------------------------------------------------------------

Session::Session(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
    // Generate a unique ID if none was assigned externally (normal path).
    if (impl_->session_id.empty()) {
        impl_->session_id = generate_session_id();
    }
}

Session::~Session() {
    if (!impl_) {
        return;
    }

    impl_->destroying.store(true, std::memory_order_relaxed);
    cancel();
    wait();
}

Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Session::submit(const std::string& text) {
    {
        std::unique_lock<std::mutex> lock(impl_->submit_mutex);
        if (impl_->destroying.load(std::memory_order_relaxed)) {
            throw std::runtime_error("session is shutting down");
        }
        if (!impl_->submit_done) {
            throw std::runtime_error("session already has an in-flight submit");
        }
        impl_->submit_done = false;
        impl_->submit_error = nullptr;
    }

    // Capture raw pointers: QueryEngine and Session::Impl are both owned by
    // Session which outlives any in-flight task (callers must call wait()
    // or let the Session destructor call wait() before destruction).
    QueryEngine*   qe = impl_->query_engine.get();
    Session::Impl* si = impl_.get();

    impl_->enqueue_work([qe, si, text]() {
        std::exception_ptr task_error;
        try {
            qe->submit(text);
        } catch (...) {
            task_error = std::current_exception();
        }
        {
            std::unique_lock<std::mutex> lock(si->submit_mutex);
            si->submit_error = task_error;
            si->submit_done = true;
        }
        si->submit_cv.notify_all();
    });
}

void Session::wait() {
    std::unique_lock<std::mutex> lock(impl_->submit_mutex);
    impl_->submit_cv.wait(lock, [this]() { return impl_->submit_done; });
    auto task_error = impl_->submit_error;
    impl_->submit_error = nullptr;
    lock.unlock();

    if (task_error) {
        std::rethrow_exception(task_error);
    }
}

void Session::cancel() {
    if (impl_->agent_manager) {
        impl_->agent_manager->cancel_all_running();
    }
    impl_->query_engine->cancel();
}

void Session::stop() {
    if (impl_->agent_manager) {
        impl_->agent_manager->stop_all_running();
    }
    impl_->query_engine->request_stop();
}

void Session::register_tool(std::unique_ptr<Tool> tool) {
    // Register into the session-local registry so each session has isolated tools.
    impl_->session_tools.register_tool(std::move(tool));
}

const std::vector<Message>& Session::messages() const {
    return impl_->query_engine->messages();
}

const Usage& Session::usage() const {
    return impl_->query_engine->total_usage();
}

CostSnapshot Session::cost() const {
    if (impl_->cost_tracker) {
        return impl_->cost_tracker->snapshot();
    }
    return CostSnapshot{};
}

const std::string& Session::id() const {
    return impl_->session_id;
}

void Session::resume(const std::vector<Message>& prior_messages) {
    impl_->query_engine->set_messages(prior_messages);
}

void Session::persist() {
    if (!impl_->persistence || impl_->session_id.empty()) {
        return;
    }

    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string ts = std::to_string(now_us);

    SessionRecord rec;
    rec.id = impl_->session_id;
    rec.updated_at = ts;
    rec.messages = impl_->query_engine->messages();
    rec.total_usage = impl_->query_engine->total_usage();

    if (auto existing = impl_->persistence->load(impl_->session_id)) {
        rec.created_at = existing->created_at;
    } else {
        rec.created_at = ts;
    }

    impl_->persistence->save(rec);
}

void Session::set_tool_filter(std::vector<std::string> allowed_tools) {
    impl_->query_engine->set_tool_filter(std::move(allowed_tools));
}

void Session::set_tool_context(ToolContext context) {
    impl_->query_engine->set_tool_context(context);
    if (impl_->agent_manager) {
        impl_->agent_manager->set_parent_tool_context(std::move(context));
    }
}

void Session::set_system_prompt(std::string system_prompt) {
    impl_->query_engine->set_system_prompt(std::move(system_prompt));
}

void Session::set_permission_mode(PermissionMode mode) {
    impl_->checker->set_mode(mode);
}

void Session::set_evidence_based_final_answer(bool enabled) {
    impl_->query_engine->set_evidence_based_final_answer(enabled);
}

void Session::set_max_parallel_tools(int max_parallel_tools) {
    impl_->query_engine->set_max_parallel_tools(max_parallel_tools);
}

std::vector<std::string> Session::tool_names() const {
    std::vector<std::string> names;
    if (impl_->engine_tools) {
        names = impl_->engine_tools->names();
    }
    auto session_names = impl_->session_tools.names();
    names.insert(names.end(), session_names.begin(), session_names.end());
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

Tool* Session::find_tool(const std::string& name) const {
    if (Tool* tool = impl_->session_tools.get(name)) {
        return tool;
    }
    if (!impl_->engine_tools) {
        return nullptr;
    }
    return impl_->engine_tools->get(name);
}

}  // namespace omni::engine
