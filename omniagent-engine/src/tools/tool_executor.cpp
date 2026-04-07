#include "tool_executor.h"
#include "tool_registry.h"

#include <future>
#include <mutex>
#include <sstream>

namespace omni::engine {

// ---------------------------------------------------------------------------
// Lightweight JSON Schema validation (required-field presence check)
// Returns empty string on success, error message on failure.
// ---------------------------------------------------------------------------

static std::string validate_args(const nlohmann::json& args,
                                  const nlohmann::json& schema)
{
    const bool schema_has_required = schema.contains("required") &&
                                     schema["required"].is_array() &&
                                     !schema["required"].empty();
    const bool schema_has_properties = schema.contains("properties") &&
                                       schema["properties"].is_object() &&
                                       !schema["properties"].empty();

    if (!schema_has_required && !schema_has_properties) {
        return "";  // no schema constraints — accept anything
    }

    if (!args.is_object()) return "Arguments must be a JSON object";

    if (schema_has_required) {
        for (const auto& req : schema["required"]) {
            if (!args.contains(req.get<std::string>())) {
                return "Missing required field: " + req.get<std::string>();
            }
        }
    }
    return "";
}

// ---------------------------------------------------------------------------
// ToolExecutor
// ---------------------------------------------------------------------------

ToolExecutor::ToolExecutor(ToolRegistry&      registry,
                            PermissionChecker& checker,
                            EventObserver&     observer,
                            ToolExecutorConfig config,
                            HookEngine*        hooks,
                            ToolRegistry*      session_registry)
    : registry_(registry)
    , session_registry_(session_registry)
    , checker_(checker)
    , observer_(observer)
    , config_(config)
    , hooks_(hooks)
{}

// Look up a tool in session registry first, then engine registry.
static Tool* lookup_tool(ToolRegistry& engine_reg, ToolRegistry* session_reg,
                          const std::string& name)
{
    if (session_reg) {
        if (Tool* t = session_reg->get(name)) return t;
    }
    return engine_reg.get(name);
}

// ---------------------------------------------------------------------------
// truncate_result
// ---------------------------------------------------------------------------

std::string ToolExecutor::truncate_result(const std::string& content) const
{
    if (static_cast<int>(content.size()) <= config_.max_result_chars) {
        return content;
    }
    return content.substr(0, config_.max_result_chars)
        + "\n[truncated — " + std::to_string(content.size()) + " chars total]";
}

// ---------------------------------------------------------------------------
// record_call / check_doom_loop
// ---------------------------------------------------------------------------

void ToolExecutor::record_call(const std::string&    name,
                                const nlohmann::json& args,
                                bool                  success)
{
    CallRecord rec;
    rec.tool_name = name;
    const std::string dump = args.dump();
    rec.args_hash  = dump.substr(0, std::min<std::size_t>(64, dump.size()));
    rec.succeeded  = success;

    recent_calls_.push_back(std::move(rec));
    if (static_cast<int>(recent_calls_.size()) > 20) {
        recent_calls_.pop_front();
    }
}

bool ToolExecutor::check_doom_loop() const
{
    if (!config_.doom_loop_enabled) return false;
    if (recent_calls_.empty()) return false;

    const std::string& tail_name = recent_calls_.back().tool_name;
    int consecutive_failures = 0;
    for (auto it = recent_calls_.rbegin(); it != recent_calls_.rend(); ++it) {
        if (it->tool_name != tail_name || it->succeeded) break;
        ++consecutive_failures;
    }
    return consecutive_failures >= config_.doom_loop_threshold;
}

// ---------------------------------------------------------------------------
// execute_one — single tool call, full pipeline
// ---------------------------------------------------------------------------

ToolResult ToolExecutor::execute_one(const ToolUseContent& call,
                                      std::atomic<bool>&    cancelled)
{
    (void)cancelled;  // checked by caller between groups

    if (!config_.allowed_tools.empty()
        && std::find(config_.allowed_tools.begin(), config_.allowed_tools.end(), call.name)
            == config_.allowed_tools.end()) {
        ToolResult err;
        err.tool_use_id = call.id;
        err.content = "Tool '" + call.name + "' is not permitted by the active profile.";
        err.is_error = true;
        observer_.on_event(ToolResultEvent{call.id, call.name, err.content, true});
        record_call(call.name, call.input, false);
        return err;
    }

    Tool* tool = lookup_tool(registry_, session_registry_, call.name);
    if (!tool) {
        ToolResult err;
        err.tool_use_id = call.id;
        err.content     = "Tool '" + call.name + "' not found.";
        err.is_error    = true;
        // No start event — tool doesn't exist, nothing to start
        record_call(call.name, call.input, false);
        return err;
    }

    // ToolUseStart hook — may block execution of this tool.
    if (hooks_) {
        const HookResult hr = hooks_->fire(HookEvent::ToolUseStart,
                                           {{"tool", call.name}, {"args", call.input}});
        if (hr.should_block) {
            ToolResult blocked;
            blocked.tool_use_id = call.id;
            blocked.content     = "Blocked by hook: " + hr.message;
            blocked.is_error    = true;
            record_call(call.name, call.input, false);
            return blocked;
        }
    }

    observer_.on_event(ToolUseStartEvent{call.id, call.name, call.input});

    const std::string validation_error = validate_args(call.input, tool->input_schema());
    if (!validation_error.empty()) {
        ToolResult err;
        err.tool_use_id = call.id;
        err.content     = validation_error;
        err.is_error    = true;
        observer_.on_event(ToolResultEvent{call.id, call.name, err.content, true});
        record_call(call.name, call.input, false);
        return err;
    }

    const PermissionDecision decision =
        checker_.check(call.name, call.input, tool->description());

    if (decision == PermissionDecision::Deny) {
        ToolResult err;
        err.tool_use_id = call.id;
        err.content     = "Permission denied for tool '" + call.name + "'.";
        err.is_error    = true;
        observer_.on_event(ToolResultEvent{call.id, call.name, err.content, true});
        record_call(call.name, call.input, false);
        return err;
    }

    ToolCallResult call_result = tool->call(call.input, config_.tool_context);

    ToolResult tr;
    tr.tool_use_id = call.id;
    tr.content     = truncate_result(call_result.content);
    tr.is_error    = call_result.is_error;

    // ToolUseEnd hook — informational, result not blocking.
    if (hooks_) {
        hooks_->fire(HookEvent::ToolUseEnd,
                     {{"tool", call.name}, {"result", tr.content}});
    }

    observer_.on_event(ToolResultEvent{call.id, call.name, tr.content, tr.is_error});
    record_call(call.name, call.input, !tr.is_error);

    return tr;
}

// ---------------------------------------------------------------------------
// execute — partition into parallel/serial groups and run
// ---------------------------------------------------------------------------

ExecutorResult ToolExecutor::execute(const std::vector<ToolUseContent>& calls,
                                      std::atomic<bool>&                 cancelled)
{
    if (cancelled.load(std::memory_order_relaxed)) {
        return {};
    }

    // Partition calls into groups:
    //   - consecutive read-only tools → one parallel batch
    //   - any write/destructive tool  → its own serial item
    //
    // A tool that is NOT in the registry is treated as a serial item (safe default).

    struct Group {
        bool parallel;  // true = run concurrently; false = run serially (single call)
        std::vector<ToolUseContent> calls;
    };

    std::vector<Group> groups;

    for (const ToolUseContent& call : calls) {
        Tool* tool = lookup_tool(registry_, session_registry_, call.name);
        const bool read_only = tool && tool->is_read_only() && !tool->is_destructive();

        if (read_only) {
            // Append to existing parallel batch or start a new one
            if (!groups.empty() && groups.back().parallel) {
                groups.back().calls.push_back(call);
            } else {
                groups.push_back({true, {call}});
            }
        } else {
            // Serial item — always its own group
            groups.push_back({false, {call}});
        }
    }

    ExecutorResult exec_result;
    exec_result.results.reserve(calls.size());

    // EventObserver is not required to be thread-safe, so we serialize observer
    // calls from parallel workers under this mutex.
    std::mutex observer_mutex;

    for (const Group& group : groups) {
        if (cancelled.load(std::memory_order_relaxed)) break;

        if (!group.parallel || group.calls.size() <= 1) {
            // Serial execution
            for (const ToolUseContent& call : group.calls) {
                if (cancelled.load(std::memory_order_relaxed)) goto done;
                exec_result.results.push_back(execute_one(call, cancelled));
            }
        } else {
            // Parallel execution — cap at max_parallel
            const int n = static_cast<int>(group.calls.size());
            const int batch_size = std::min(n, config_.max_parallel);

            // We process in slices of batch_size to respect the cap, though in
            // practice a single LLM turn rarely sends > 10 read-only calls.
            int offset = 0;
            while (offset < n) {
                if (cancelled.load(std::memory_order_relaxed)) goto done;

                const int end = std::min(offset + batch_size, n);

                // Launch futures for this slice
                std::vector<std::future<ToolResult>> futures;
                futures.reserve(end - offset);

                for (int i = offset; i < end; ++i) {
                    const ToolUseContent call_copy = group.calls[i];
                    futures.push_back(std::async(std::launch::async,
                        [this, call_copy, &cancelled, &observer_mutex]() -> ToolResult {
                            // Wrap observer calls in a locking shim so parallel
                            // workers don't race on the observer.
                            struct LockingObserver : EventObserver {
                                EventObserver& inner;
                                std::mutex&    mtx;
                                LockingObserver(EventObserver& o, std::mutex& m)
                                    : inner(o), mtx(m) {}
                                void on_event(const Event& e) override {
                                    std::lock_guard<std::mutex> lk(mtx);
                                    inner.on_event(e);
                                }
                            } locking_observer(observer_, observer_mutex);

                            // Temporarily swap the observer for thread safety.
                            // We cannot mutate observer_ directly (it's a ref),
                            // so we build a private ToolExecutor with same deps.
                            ToolExecutor worker(registry_, checker_,
                                                locking_observer, config_, hooks_,
                                                session_registry_);
                            ToolResult result = worker.execute_one(call_copy, cancelled);

                            // Merge worker's call records back into our deque
                            // under the observer mutex (reuse it as a general lock).
                            {
                                std::lock_guard<std::mutex> lk(observer_mutex);
                                for (const auto& rec : worker.recent_calls_) {
                                    recent_calls_.push_back(rec);
                                    if (static_cast<int>(recent_calls_.size()) > 20) {
                                        recent_calls_.pop_front();
                                    }
                                }
                            }
                            return result;
                        }));
                }

                // Collect results in call order
                for (auto& fut : futures) {
                    exec_result.results.push_back(fut.get());
                }

                offset = end;
            }
        }
    }

done:
    if (check_doom_loop()) {
        exec_result.doom_loop_abort = true;
        exec_result.doom_loop_hint  =
            "Doom-loop detected: tool '" + recent_calls_.back().tool_name +
            "' has failed " + std::to_string(config_.doom_loop_threshold) +
            " consecutive times. Aborting to prevent infinite retry.";
    }

    return exec_result;
}

}  // namespace omni::engine
