#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace omni::engine {

enum class HookEvent {
    PrePrompt,      // before LLM call
    PostPrompt,     // after LLM call, before tool execution
    ToolUseStart,   // before a tool executes
    ToolUseEnd,     // after a tool executes
};

struct HookResult {
    bool        should_block = false;  // if true, abort the action
    std::string message;               // optional message (e.g., why blocked)
};

/// Function hook type: host registers a callable.
using HookFunction = std::function<HookResult(HookEvent event, const nlohmann::json& context)>;

struct HookRegistration {
    std::string                   name;
    HookEvent                     event;
    HookFunction                  handler;
    std::chrono::milliseconds     timeout{5000};  // max time before timeout
};

class HookEngine {
public:
    /// Register a hook.
    void add(HookRegistration hook);

    /// Remove a hook by name.
    void remove(const std::string& name);

    /// Fire all hooks for an event. Returns the combined result.
    /// If any hook blocks, the result is blocking.
    /// Respects timeouts — a hung hook is treated as non-blocking.
    HookResult fire(HookEvent event, const nlohmann::json& context = {});

    /// Number of registered hooks.
    size_t count() const;

private:
    mutable std::mutex            mutex_;
    std::vector<HookRegistration> hooks_;
};

}  // namespace omni::engine
