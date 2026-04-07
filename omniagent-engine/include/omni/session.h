#pragma once

#include <omni/permission.h>
#include <omni/tool.h>
#include <omni/types.h>
#include <memory>
#include <string>
#include <vector>

namespace omni::engine {

struct CostSnapshot;  // forward declaration — full type in services/cost_tracker.h

class Tool;

class Session {
public:
    ~Session();
    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void submit(const std::string& text);
    void wait();    // blocks until the in-flight submit completes
    void cancel();
    void stop();
    void register_tool(std::unique_ptr<Tool> tool);
    const std::vector<Message>& messages() const;
    const Usage& usage() const;
    CostSnapshot cost() const;

    /// Get the session ID (stable for the lifetime of this Session).
    const std::string& id() const;

    /// Resume from a prior session: pre-populate the message history.
    void resume(const std::vector<Message>& prior_messages);

    /// Persist the current transcript immediately when persistence is configured.
    void persist();

    /// Restrict which tools are sent to the LLM. Empty = all tools.
    void set_tool_filter(std::vector<std::string> allowed_tools);
    void set_tool_context(ToolContext context);
    void set_system_prompt(std::string system_prompt);
    void set_permission_mode(PermissionMode mode);
    void set_max_parallel_tools(int max_parallel_tools);
    std::vector<std::string> tool_names() const;
    Tool* find_tool(const std::string& name) const;

private:
    friend class Engine;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit Session(std::unique_ptr<Impl> impl);
};

}  // namespace omni::engine
