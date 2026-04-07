#pragma once

#include <omni/event.h>
#include <omni/permission.h>
#include <omni/tool.h>
#include <omni/types.h>
#include "../permissions/permission_checker.h"
#include "../services/hooks.h"
#include <atomic>
#include <deque>
#include <string>
#include <vector>

namespace omni::engine {

class ToolRegistry;

struct ToolExecutorConfig {
    int  max_parallel       = 10;    // max concurrent read-only tools
    int  max_result_chars   = 50000; // truncate results larger than this
    bool doom_loop_enabled  = true;
    int  doom_loop_threshold = 6;    // consecutive same-tool failures before abort
    std::vector<std::string> allowed_tools;
    ToolContext tool_context;
};

struct ExecutorResult {
    std::vector<ToolResult> results;
    bool        doom_loop_abort = false;
    std::string doom_loop_hint;
};

class ToolExecutor {
public:
    ToolExecutor(ToolRegistry& registry, PermissionChecker& checker,
                 EventObserver& observer, ToolExecutorConfig config = {},
                 HookEngine* hooks = nullptr,
                 ToolRegistry* session_registry = nullptr);

    /// Execute a batch of tool calls with concurrency partitioning.
    ExecutorResult execute(const std::vector<ToolUseContent>& calls,
                           std::atomic<bool>& cancelled);

private:
    ToolRegistry&      registry_;
    ToolRegistry*      session_registry_;  // non-owning; may be null
    PermissionChecker& checker_;
    EventObserver&     observer_;
    ToolExecutorConfig config_;
    HookEngine*        hooks_;   // non-owning; may be null

    // Doom-loop tracking
    struct CallRecord {
        std::string tool_name;
        std::string args_hash;
        bool succeeded;
    };
    std::deque<CallRecord> recent_calls_;

    ToolResult  execute_one(const ToolUseContent& call, std::atomic<bool>& cancelled);
    void        record_call(const std::string& name, const nlohmann::json& args, bool success);
    bool        check_doom_loop() const;
    std::string truncate_result(const std::string& content) const;
};

}  // namespace omni::engine
