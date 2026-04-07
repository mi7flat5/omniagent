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
        if (type == AgentType::Explore) {
            // Explore: read-only tools only
            const Tool* t = engine_.find_tool(n);
            if (t && t->is_read_only()) {
                filtered.push_back(n);
            }
        } else if (type == AgentType::Plan) {
            // Plan: exclude edit/write/bash/notebook tools
            const std::string lower = [&]() {
                std::string s = n;
                for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return s;
            }();
            const bool excluded =
                lower.find("edit")     != std::string::npos ||
                lower.find("write")    != std::string::npos ||
                lower.find("bash")     != std::string::npos ||
                lower.find("notebook") != std::string::npos;
            if (!excluded) {
                filtered.push_back(n);
            }
        }
    }
    return filtered;
}

std::string AgentManager::profile_name_for_type(AgentType type) const {
    switch (type) {
        case AgentType::Explore:
            return "explore";
        case AgentType::Plan:
            return "plan";
        case AgentType::GeneralPurpose:
            return "general";
    }
    return "general";
}

std::string AgentManager::spawn(const AgentConfig& config,
                                 AgentCompletionCallback on_complete)
{
    const std::string agent_id = [this]() {
        std::lock_guard<std::mutex> lock(mutex_);
        return generate_id();
    }();

    observer_.on_event(AgentSpawnedEvent{agent_id, config.task, profile_name_for_type(config.type)});

    // Build a shared observer so we can read results after completion.
    auto observer = std::make_shared<AgentObserver>();

    // Create the child session.
    auto session = engine_.create_session(*observer, delegate_);

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

            if (on_complete) {
                on_complete(result);
            }

            observer_.on_event(AgentCompletedEvent{agent_id, !result.is_error});
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

    if (on_complete) {
        on_complete(result);
    }

    observer_.on_event(AgentCompletedEvent{agent_id, !result.is_error});

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
    return true;
}

}  // namespace omni::engine
