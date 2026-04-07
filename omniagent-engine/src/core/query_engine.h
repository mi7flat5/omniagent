#pragma once

#include <omni/event.h>
#include <omni/permission.h>
#include <omni/provider.h>
#include <omni/tool.h>
#include <omni/types.h>
#include "../permissions/permission_checker.h"
#include "../services/cost_tracker.h"
#include "../services/hooks.h"
#include "../services/memory_loader.h"
#include "../services/session_persistence.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace omni::engine {

class ToolRegistry;

struct QueryEngineConfig {
    int         max_turns             = 50;
    int         max_retries           = 3;
    int         preserve_tail         = 4;
    int         max_result_chars      = 500;
    int         initial_max_tokens    = 8192;
    bool        max_tokens_escalation = true;
    std::string system_prompt;
    int         max_parallel_tools    = 10;

    // Auto-compact settings
    float compact_soft_limit_pct = 0.75f;
    int   compact_preserve_tail  = 6;
    bool  enable_auto_compact    = true;
};

class QueryEngine {
public:
    QueryEngine(std::unique_ptr<LLMProvider> provider,
                ToolRegistry&                registry,
                PermissionChecker&           checker,
                EventObserver&               observer,
                QueryEngineConfig            config = {},
                LLMProvider*                 compaction_provider = nullptr,
                CostTracker*                 cost_tracker = nullptr,
                ToolRegistry*                session_registry = nullptr);
    ~QueryEngine();

    void submit(const std::string& text);
    void cancel();
    void request_stop();
    const std::vector<Message>& messages() const;
    const Usage& total_usage() const;

    /// Replace the current message history (for session resume).
    void set_messages(std::vector<Message> msgs);

    /// Attach persistence + session ID so submit() auto-saves after each call.
    void set_persistence(SessionPersistence* persistence, std::string session_id);

    /// Attach a hook engine (non-owning). Hooks fire at PrePrompt/PostPrompt/ToolUseStart/ToolUseEnd.
    void set_hooks(HookEngine* hooks);

    /// Attach a memory loader (non-owning). Loaded context is prepended to system prompt.
    void set_memory_loader(MemoryLoader* memory_loader);

    /// Restrict which tools are included in requests. Empty = all tools.
    void set_tool_filter(std::vector<std::string> allowed);
    void set_tool_context(ToolContext context);
    void set_system_prompt(std::string system_prompt);
    void set_max_parallel_tools(int max_parallel_tools);

private:
    bool run_turn();
    CompletionRequest build_request() const;

    std::unique_ptr<LLMProvider> provider_;
    LLMProvider*                 compaction_provider_;  // non-owning; may be null
    CostTracker*                 cost_tracker_;         // non-owning; may be null
    SessionPersistence*          persistence_;          // non-owning; may be null
    HookEngine*                  hooks_;                // non-owning; may be null
    MemoryLoader*                memory_loader_;        // non-owning; may be null
    ToolRegistry&                registry_;
    ToolRegistry*                session_registry_;     // non-owning; may be null (session-scoped tools)
    PermissionChecker&           checker_;
    EventObserver&               observer_;
    QueryEngineConfig            config_;
    std::vector<Message>         messages_;
    Usage                        total_usage_;
    std::string                  session_id_;
    std::atomic<bool>            cancelled_{false};
    std::atomic<bool>            stop_requested_{false};
    bool                         last_turn_truncated_{false};
    bool                         reactive_compacted_this_turn_{false};
    std::vector<std::string>     tool_filter_;  // empty = all tools
    ToolContext                  tool_context_;
};

}  // namespace omni::engine
