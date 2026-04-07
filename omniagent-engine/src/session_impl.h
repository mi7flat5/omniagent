#pragma once

#include <omni/provider.h>
#include <omni/session.h>
#include "agents/agent_manager.h"
#include "core/query_engine.h"
#include "permissions/permission_checker.h"
#include "services/cost_tracker.h"
#include "services/session_persistence.h"
#include "tools/tool_registry.h"

#include <condition_variable>
#include <exception>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>

namespace omni::engine {

// Non-owning wrapper so sessions share the engine's provider
class ProviderRef : public LLMProvider {
public:
    explicit ProviderRef(LLMProvider& inner) : inner_(inner) {}

    Usage complete(const CompletionRequest& req, StreamCallback cb,
                   std::atomic<bool>& stop) override {
        return inner_.complete(req, std::move(cb), stop);
    }

    ModelCapabilities capabilities() const override { return inner_.capabilities(); }
    std::string name() const override { return inner_.name(); }

private:
    LLMProvider& inner_;
};

struct Session::Impl {
    // Permission checker owns the session-scoped checker (wraps host delegate).
    // Must be declared before query_engine so it outlives it.
    std::unique_ptr<PermissionChecker> checker;

    // Session-scoped tool registry for built-in tools (e.g. agent, send_message).
    // Declared before query_engine and agent_manager so it outlives both.
    ToolRegistry session_tools;

    std::unique_ptr<QueryEngine>  query_engine;
    std::unique_ptr<AgentManager> agent_manager;
    ToolRegistry*                 engine_tools  = nullptr;  // non-owning, points to Engine's registry
    CostTracker*                  cost_tracker  = nullptr;  // non-owning, points to Engine's tracker

    // Session identity and persistence (non-owning; may be null if no storage dir configured)
    std::string          session_id;
    SessionPersistence*  persistence = nullptr;

    // Thin enqueue callback — avoids exposing Engine::Impl type to session code
    std::function<void(std::function<void()>)> enqueue_work;

    // Synchronisation for non-blocking submit + wait()
    std::mutex              submit_mutex;
    std::condition_variable submit_cv;
    bool                    submit_done = true;  // true = no work in flight
    std::exception_ptr      submit_error;
    std::atomic<bool>       destroying{false};
};

}  // namespace omni::engine
