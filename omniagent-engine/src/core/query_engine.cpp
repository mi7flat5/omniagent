#include "query_engine.h"

#include "auto_compact.h"
#include "microcompact.h"
#include "slash_commands.h"
#include "stream_assembler.h"
#include "../tools/tool_executor.h"
#include "../tools/tool_registry.h"

#include <omni/event.h>
#include <omni/types.h>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace omni::engine {

QueryEngine::QueryEngine(std::unique_ptr<LLMProvider> provider,
                          ToolRegistry&                registry,
                          PermissionChecker&           checker,
                          EventObserver&               observer,
                          QueryEngineConfig            config,
                          LLMProvider*                 compaction_provider,
                          CostTracker*                 cost_tracker,
                          ToolRegistry*                session_registry)
    : provider_(std::move(provider))
    , compaction_provider_(compaction_provider)
    , cost_tracker_(cost_tracker)
    , persistence_(nullptr)
    , hooks_(nullptr)
    , memory_loader_(nullptr)
    , registry_(registry)
    , session_registry_(session_registry)
    , checker_(checker)
    , observer_(observer)
    , config_(std::move(config))
{}

QueryEngine::~QueryEngine() = default;

void QueryEngine::cancel() {
    cancelled_.store(true, std::memory_order_relaxed);
}

void QueryEngine::request_stop() {
    stop_requested_.store(true, std::memory_order_relaxed);
}

const std::vector<Message>& QueryEngine::messages() const {
    return messages_;
}

const Usage& QueryEngine::total_usage() const {
    return total_usage_;
}

void QueryEngine::set_messages(std::vector<Message> msgs) {
    messages_ = std::move(msgs);
}

void QueryEngine::set_persistence(SessionPersistence* persistence, std::string session_id) {
    persistence_ = persistence;
    session_id_  = std::move(session_id);
}

void QueryEngine::set_hooks(HookEngine* hooks) {
    hooks_ = hooks;
}

void QueryEngine::set_memory_loader(MemoryLoader* memory_loader) {
    memory_loader_ = memory_loader;
}

void QueryEngine::set_tool_filter(std::vector<std::string> allowed) {
    tool_filter_ = std::move(allowed);
}

void QueryEngine::set_tool_context(ToolContext context) {
    tool_context_ = std::move(context);
}

void QueryEngine::set_system_prompt(std::string system_prompt) {
    config_.system_prompt = std::move(system_prompt);
}

void QueryEngine::set_max_parallel_tools(int max_parallel_tools) {
    config_.max_parallel_tools = std::max(1, max_parallel_tools);
}

// ---------------------------------------------------------------------------
// build_request — apply microcompact and assemble a CompletionRequest
// ---------------------------------------------------------------------------

CompletionRequest QueryEngine::build_request() const {
    CompletionRequest req;

    // Memory context is prepended to the configured system prompt.
    if (memory_loader_) {
        const std::string ctx = memory_loader_->build_context();
        if (!ctx.empty()) {
            req.system_prompt = ctx + "\n" + config_.system_prompt;
        } else {
            req.system_prompt = config_.system_prompt;
        }
    } else {
        req.system_prompt = config_.system_prompt;
    }

    req.messages = microcompact(messages_,
                                config_.preserve_tail,
                                config_.max_result_chars);
    // Collect all tool definitions: engine-level + session-level.
    std::vector<nlohmann::json> all_defs = registry_.tool_definitions();
    if (session_registry_) {
        for (auto& d : session_registry_->tool_definitions()) {
            all_defs.push_back(std::move(d));
        }
    }

    if (tool_filter_.empty()) {
        req.tools = std::move(all_defs);
    } else {
        // Only include tools whose name is in the allowed list.
        for (const auto& def : all_defs) {
            const std::string n = def.at("function").at("name").get<std::string>();
            if (std::find(tool_filter_.begin(), tool_filter_.end(), n) != tool_filter_.end()) {
                req.tools.push_back(def);
            }
        }
    }

    const bool has_workspace = !tool_context_.workspace_root.empty();
    bool has_prior_tool_turn = false;
    for (const Message& message : messages_) {
        if (message.role == Role::ToolResult && !message.tool_results.empty()) {
            has_prior_tool_turn = true;
            break;
        }
        if (message.role != Role::Assistant) {
            continue;
        }
        for (const ContentBlock& block : message.content) {
            if (std::get_if<ToolUseContent>(&block.data)) {
                has_prior_tool_turn = true;
                break;
            }
        }
        if (has_prior_tool_turn) {
            break;
        }
    }

    if (!req.tools.empty()) {
        req.tool_choice = has_workspace && !has_prior_tool_turn ? "required" : "auto";
        if (req.tool_choice == "required") {
            if (!req.system_prompt.empty()) {
                req.system_prompt += "\n\n";
            }
            req.system_prompt +=
                "When tools are available in this turn, do not describe intended actions. "
                "Use structured tool calls to inspect or change the workspace before replying.";
        }
    }

    // Max output token escalation: if the last turn was truncated, double the
    // token limit (up to 8x initial) so incomplete tool calls can complete.
    const auto caps = provider_->capabilities();
    int target_tokens = config_.initial_max_tokens;
    if (config_.max_tokens_escalation && last_turn_truncated_) {
        target_tokens = config_.initial_max_tokens * 8;
    }
    if (caps.max_output_tokens > 0) {
        target_tokens = std::min(target_tokens, caps.max_output_tokens);
    }
    req.max_tokens = target_tokens;

    return req;
}

// ---------------------------------------------------------------------------
// run_turn — single LLM call + optional tool execution
// Returns true  → continue the loop (tool calls were made)
// Returns false → done (no tool calls, or cancelled)
// ---------------------------------------------------------------------------

static std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

// Returns true if the error message indicates a transient/retriable failure.
static bool is_transient_error(const std::string& msg) {
    const std::string lower = to_lower(msg);
    return lower.find("timeout")    != std::string::npos
        || lower.find("connection") != std::string::npos
        || lower.find("5xx")        != std::string::npos
        || lower.find("502")        != std::string::npos
        || lower.find("503")        != std::string::npos
        || lower.find("529")        != std::string::npos;
}

// Returns true if the error message indicates a context length overflow.
static bool is_context_overflow_error(const std::string& msg) {
    const std::string lower = to_lower(msg);
    return lower.find("context length")           != std::string::npos
        || lower.find("too many tokens")           != std::string::npos
        || lower.find("context_length_exceeded")   != std::string::npos
        || lower.find("prompt is too long")        != std::string::npos
        || lower.find("maximum context length")    != std::string::npos;
}

static std::string trim_copy(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

static void append_directory_snapshot(std::ostringstream& out,
                                      const std::string& label,
                                      const std::filesystem::path& dir,
                                      std::size_t max_entries) {
    std::error_code ec;
    std::vector<std::string> entries;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        std::error_code type_error;
        if (it->is_directory(type_error)) {
            name += "/";
        }
        entries.push_back(std::move(name));
    }

    out << label << ":\n";
    if (ec) {
        out << "- unavailable\n";
        return;
    }

    std::sort(entries.begin(), entries.end());
    if (entries.empty()) {
        out << "- (empty)\n";
        return;
    }

    const std::size_t limit = std::min(max_entries, entries.size());
    for (std::size_t index = 0; index < limit; ++index) {
        out << "- " << entries[index] << "\n";
    }
    if (entries.size() > limit) {
        out << "- ... (" << (entries.size() - limit) << " more)\n";
    }
}

static std::string read_file_prefix(const std::filesystem::path& path, std::size_t max_bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }

    std::string content;
    content.resize(max_bytes);
    input.read(content.data(), static_cast<std::streamsize>(max_bytes));
    content.resize(static_cast<std::size_t>(input.gcount()));
    return trim_copy(std::move(content));
}

static std::string build_workspace_snapshot(const ToolContext& context) {
    if (context.workspace_root.empty()) {
        return {};
    }

    std::error_code ec;
    std::filesystem::path workspace_root = std::filesystem::weakly_canonical(context.workspace_root, ec);
    if (ec) {
        workspace_root = context.workspace_root;
        ec.clear();
    }

    std::filesystem::path working_dir = context.working_dir.empty()
        ? workspace_root
        : context.working_dir;
    const std::filesystem::path canonical_working_dir =
        std::filesystem::weakly_canonical(working_dir, ec);
    if (!ec) {
        working_dir = canonical_working_dir;
    } else {
        ec.clear();
    }

    std::ostringstream out;
    out << "Workspace root: " << workspace_root.string() << "\n";
    out << "Working directory: " << working_dir.string() << "\n";
    append_directory_snapshot(out, "Top-level entries", workspace_root, 16);
    if (working_dir != workspace_root) {
        append_directory_snapshot(out, "Working directory entries", working_dir, 16);
    }

    for (const char* candidate : {"README.md", "README", "readme.md"}) {
        const std::filesystem::path readme_path = workspace_root / candidate;
        if (!std::filesystem::is_regular_file(readme_path, ec)) {
            ec.clear();
            continue;
        }

        const std::string excerpt = read_file_prefix(readme_path, 2048);
        out << "README excerpt:\n";
        if (excerpt.empty()) {
            out << "(empty)\n";
        } else {
            out << excerpt << "\n";
        }
        break;
    }

    return trim_copy(out.str());
}

static void replace_all(std::string& text,
                        const std::string& needle,
                        const std::string& replacement) {
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        text.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

static std::string sanitize_assistant_text(std::string text) {
    const bool has_markup = text.find("<|tool_call>") != std::string::npos
        || text.find("<|tool_response>") != std::string::npos
        || text.find("<|channel>thought") != std::string::npos
        || text.find("<channel|>") != std::string::npos
        || text.find("|thought") != std::string::npos;
    if (!has_markup) {
        return text;
    }

    const std::string channel_marker = "<channel|>";
    const std::size_t last_channel = text.rfind(channel_marker);
    if (last_channel != std::string::npos) {
        text = text.substr(last_channel + channel_marker.size());
    }

    replace_all(text, "<|channel>thought", "");
    replace_all(text, "<channel|>", "");
    replace_all(text, "|thought", "");
    replace_all(text, "<|tool_call>", "");
    replace_all(text, "<tool_call|>", "");
    replace_all(text, "<|tool_response>", "");
    return trim_copy(std::move(text));
}

static bool looks_like_malformed_tool_transcript_text(const std::string& text) {
    const std::string trimmed = trim_copy(text);
    if (trimmed.empty()) {
        return false;
    }

    return trimmed.find("<|tool_call>") != std::string::npos
        || trimmed.find("<|tool_response>") != std::string::npos
        || trimmed.find("<|channel>thought") != std::string::npos
        || trimmed.find("<channel|>") != std::string::npos
        || trimmed.find("|thought") != std::string::npos
        || trimmed.rfind("call:", 0) == 0
        || trimmed.find("\ncall:") != std::string::npos
        || trimmed.find("execute_command(") != std::string::npos
        || trimmed.find("ls_r(") != std::string::npos;
}

static bool has_malformed_tool_transcript(const std::vector<ContentBlock>& blocks) {
    std::string combined_text;
    for (const ContentBlock& block : blocks) {
        if (const auto* text = std::get_if<TextContent>(&block.data)) {
            combined_text += text->text;
        }
    }
    return looks_like_malformed_tool_transcript_text(combined_text);
}

static void sanitize_assistant_blocks(std::vector<ContentBlock>& blocks) {
    std::string combined_text;
    bool has_text = false;
    for (const ContentBlock& block : blocks) {
        if (const auto* text = std::get_if<TextContent>(&block.data)) {
            combined_text += text->text;
            has_text = true;
        }
    }

    if (!has_text) {
        return;
    }

    const std::string sanitized = sanitize_assistant_text(combined_text);
    if (sanitized == combined_text) {
        return;
    }

    std::vector<ContentBlock> rewritten;
    bool inserted_text = false;
    for (const ContentBlock& block : blocks) {
        if (std::get_if<TextContent>(&block.data)) {
            if (!inserted_text && !sanitized.empty()) {
                rewritten.push_back(ContentBlock{TextContent{sanitized}});
                inserted_text = true;
            }
            continue;
        }
        rewritten.push_back(block);
    }

    blocks = std::move(rewritten);
}

static void emit_assistant_text_blocks(const std::vector<ContentBlock>& blocks,
                                       EventObserver& observer) {
    for (const ContentBlock& block : blocks) {
        if (const auto* text = std::get_if<TextContent>(&block.data)) {
            if (!text->text.empty()) {
                observer.on_event(TextDeltaEvent{text->text, {}});
            }
        }
    }
}

static bool has_visible_assistant_content(const std::vector<ContentBlock>& blocks) {
    for (const ContentBlock& block : blocks) {
        if (const auto* text = std::get_if<TextContent>(&block.data)) {
            if (!text->text.empty()) {
                return true;
            }
        } else if (std::get_if<ToolUseContent>(&block.data)) {
            return true;
        }
    }
    return false;
}

static std::vector<ToolUseContent> extract_tool_calls(
    const std::vector<ContentBlock>& blocks) {
    std::vector<ToolUseContent> tool_calls;
    for (const ContentBlock& block : blocks) {
        if (const auto* tool = std::get_if<ToolUseContent>(&block.data)) {
            tool_calls.push_back(*tool);
        }
    }
    return tool_calls;
}

bool QueryEngine::run_turn() {
    reactive_compacted_this_turn_ = false;

    if (stop_requested_.load(std::memory_order_relaxed)) {
        return false;
    }

    // Auto-compact: if enabled and we know the context window, check whether we're
    // approaching the limit and summarize old messages before calling the LLM.
    if (config_.enable_auto_compact) {
        const int max_ctx = provider_->capabilities().max_context_tokens;
        if (max_ctx > 0) {
            LLMProvider& compact_prov =
                compaction_provider_ ? *compaction_provider_ : *provider_;

            CompactConfig ccfg;
            ccfg.soft_limit_pct = config_.compact_soft_limit_pct;
            ccfg.preserve_tail  = config_.compact_preserve_tail;

            CompactResult result = auto_compact(messages_, compact_prov, max_ctx, ccfg);
            if (result.compacted) {
                const int messages_before = static_cast<int>(messages_.size());
                const int messages_after = static_cast<int>(result.messages.size());
                messages_ = std::move(result.messages);
                observer_.on_event(CompactionEvent{messages_before, messages_after, {}});
                observer_.on_event(TextDeltaEvent{"[context compacted]", {}});
            }
        }
    }

    // PrePrompt hook — may block the LLM call entirely.
    if (hooks_) {
        const HookResult hr = hooks_->fire(HookEvent::PrePrompt, {});
        if (hr.should_block) {
            observer_.on_event(ErrorEvent{"Blocked by PrePrompt hook: " + hr.message, false, {}});
            return false;
        }
    }

    StreamAssembler assembler;

    // Track index → tool_id for ToolUseInputEvent forwarding
    std::unordered_map<int, std::string> block_tool_ids;

    auto stream_cb = [&](const StreamEventData& ev) {
        assembler.process(ev);

        if (ev.type == StreamEventType::ContentBlockStart) {
            if (ev.delta_type == "tool_use" && !ev.tool_id.empty()) {
                block_tool_ids[ev.index] = ev.tool_id;
            }
        } else if (ev.type == StreamEventType::ContentBlockDelta) {
            if (ev.delta_type == "thinking_delta" && !ev.delta_text.empty()) {
                observer_.on_event(ThinkingDeltaEvent{ev.delta_text, {}});
            } else if (ev.delta_type == "input_json_delta") {
                // Forward streaming tool input fragment to observer
                auto it = block_tool_ids.find(ev.index);
                const std::string& tid = (it != block_tool_ids.end()) ? it->second : "";
                // tool_input_delta is stored as a JSON string value wrapping the raw fragment
                std::string fragment;
                if (ev.tool_input_delta.is_string()) {
                    fragment = ev.tool_input_delta.get<std::string>();
                } else if (!ev.tool_input_delta.is_null()) {
                    fragment = ev.tool_input_delta.dump();
                }
                if (!fragment.empty()) {
                    observer_.on_event(ToolUseInputEvent{tid, fragment, {}});
                }
            }
        } else if (ev.type == StreamEventType::ContentBlockStop) {
            block_tool_ids.erase(ev.index);
        }
    };

    // Retry loop: up to max_retries attempts on transient errors.
    // Backoff: 500ms, 1000ms, 2000ms (doubling).
    CompletionRequest req = build_request();
    Usage turn_usage;
    const int max_attempts = 1 + config_.max_retries;
    int attempt = 0;
    while (true) {
        try {
            // Reset assembler state for each attempt (in case a previous attempt
            // partially streamed before throwing).
            assembler.reset();
            block_tool_ids.clear();
            turn_usage = provider_->complete(req, stream_cb, cancelled_);
            break;  // success
        } catch (const std::exception& e) {
            const std::string err_msg = e.what();

            // Context overflow: try reactive compact once, then rebuild and retry.
            if (is_context_overflow_error(err_msg) && !reactive_compacted_this_turn_) {
                LLMProvider& compact_prov =
                    compaction_provider_ ? *compaction_provider_ : *provider_;
                const int max_ctx = provider_->capabilities().max_context_tokens;
                CompactResult result = reactive_compact(messages_, compact_prov, max_ctx);
                if (result.compacted) {
                    const int messages_before = static_cast<int>(messages_.size());
                    const int messages_after = static_cast<int>(result.messages.size());
                    messages_  = std::move(result.messages);
                    reactive_compacted_this_turn_ = true;
                    observer_.on_event(CompactionEvent{messages_before, messages_after, {}});
                    observer_.on_event(TextDeltaEvent{"[context compacted]", {}});
                    req = build_request();   // rebuild with compacted messages
                    continue;               // retry immediately, don't count as attempt
                }
            }

            ++attempt;
            if (attempt >= max_attempts || !is_transient_error(err_msg)) {
                // Non-retriable or exhausted retries
                observer_.on_event(ErrorEvent{err_msg, false, {}});
                return false;
            }
            // Transient — emit recoverable error and back off
            observer_.on_event(ErrorEvent{err_msg, true, {}});
            const int backoff_ms = 500 * (1 << (attempt - 1));  // 500, 1000, 2000
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            // Reset assembler for retry
            assembler.reset();
            block_tool_ids.clear();
        }
    }
    std::vector<ContentBlock> blocks = assembler.take_completed_blocks();
    if (blocks.empty() && assembler.stop_reason().empty()) {
        // Nothing came back — treat as done
        return false;
    }

    std::vector<ToolUseContent> tool_calls = extract_tool_calls(blocks);
    bool malformed_tool_transcript = tool_calls.empty() && has_malformed_tool_transcript(blocks);
    if ((!has_visible_assistant_content(blocks) || malformed_tool_transcript)
        && tool_calls.empty()
        && !req.tools.empty()) {
        CompletionRequest fallback_req = req;
        fallback_req.tools.clear();
        if (!fallback_req.system_prompt.empty()) {
            fallback_req.system_prompt += "\n\n";
        }
        fallback_req.system_prompt +=
            "The previous attempt returned no usable answer. There are no tools available in this retry. "
            "Do not return thinking only. Do not simulate tool calls, tool responses, or channel tags such as "
            "<|tool_call>, <|tool_response>, <|channel>thought, <channel|>, call:, ls_r(...), or execute_command(...). "
            "Reply with only a concise final answer for the user in plain text.";
        const std::string workspace_snapshot = build_workspace_snapshot(tool_context_);
        if (!workspace_snapshot.empty()) {
            fallback_req.system_prompt += "\n\nUse this workspace snapshot as your source of truth for the answer:\n";
            fallback_req.system_prompt += workspace_snapshot;
        }
        try {
            assembler.reset();
            block_tool_ids.clear();
            turn_usage += provider_->complete(fallback_req, stream_cb, cancelled_);
            blocks = assembler.take_completed_blocks();
            tool_calls = extract_tool_calls(blocks);
            malformed_tool_transcript = tool_calls.empty() && has_malformed_tool_transcript(blocks);
        } catch (const std::exception& error) {
            observer_.on_event(ErrorEvent{error.what(), false, {}});
            return false;
        }
    }

    sanitize_assistant_blocks(blocks);
    malformed_tool_transcript = tool_calls.empty() && has_malformed_tool_transcript(blocks);
    total_usage_ += turn_usage;
    observer_.on_event(UsageUpdatedEvent{turn_usage, total_usage_, {}});
    if (cost_tracker_) {
        cost_tracker_->record(provider_->name(), turn_usage);
    }

    if (cancelled_.load(std::memory_order_relaxed)) return false;

    if (stop_requested_.load(std::memory_order_relaxed)) return false;

    // Track whether this turn was truncated so build_request() can escalate tokens.
    last_turn_truncated_ = (assembler.stop_reason() == "max_tokens");

    if (!has_visible_assistant_content(blocks) && tool_calls.empty()) {
        observer_.on_event(ErrorEvent{
            "Model returned no visible response. Try a more specific prompt or a model with better tool support.",
            false,
            {}
        });
        return false;
    }

    if (malformed_tool_transcript) {
        observer_.on_event(ErrorEvent{
            "Model emitted malformed tool text instead of a usable answer. Try again or switch models.",
            false,
            {}
        });
        return false;
    }

    Message assistant_msg;
    assistant_msg.role    = Role::Assistant;
    assistant_msg.content = std::move(blocks);
    messages_.push_back(assistant_msg);
    emit_assistant_text_blocks(messages_.back().content, observer_);

    if (tool_calls.empty()) return false;  // no tool calls → done

    // PostPrompt hook — fired after LLM response, before tool execution.
    if (hooks_) {
        hooks_->fire(HookEvent::PostPrompt, {});
    }

    // Execute tools (pass hooks_ so ToolUseStart/ToolUseEnd fire per-tool).
    ToolExecutorConfig exec_cfg;
    exec_cfg.max_parallel = config_.max_parallel_tools;
    exec_cfg.max_result_chars = config_.max_result_chars;
    exec_cfg.allowed_tools = tool_filter_;
    exec_cfg.tool_context = tool_context_;
    ToolExecutor executor(registry_, checker_, observer_, exec_cfg, hooks_, session_registry_);
    ExecutorResult exec_result = executor.execute(tool_calls, cancelled_);

    if (cancelled_.load(std::memory_order_relaxed)) return false;

    // Doom-loop: abort the turn and surface the hint to the observer
    if (exec_result.doom_loop_abort) {
        observer_.on_event(ErrorEvent{exec_result.doom_loop_hint, false, {}});
        return false;
    }

    // Push ToolResult message
    Message tr_msg;
    tr_msg.role         = Role::ToolResult;
    tr_msg.tool_results = std::move(exec_result.results);
    messages_.push_back(std::move(tr_msg));

    if (stop_requested_.load(std::memory_order_relaxed)) {
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// submit — entry point; runs the agent loop
// ---------------------------------------------------------------------------

void QueryEngine::submit(const std::string& text) {
    cancelled_.store(false, std::memory_order_relaxed);
    stop_requested_.store(false, std::memory_order_relaxed);

    // Handle slash commands before pushing the user message.
    auto cmd_result = handle_slash_command(text, messages_, total_usage_);
    if (cmd_result.handled) {
        if (cmd_result.clear_messages) messages_.clear();
        if (!cmd_result.response_text.empty()) {
            observer_.on_event(TextDeltaEvent{cmd_result.response_text, {}});
        }
        observer_.on_event(DoneEvent{total_usage_, {}});
        return;
    }

    Message user_msg;
    user_msg.role    = Role::User;
    user_msg.content = {ContentBlock{TextContent{text}}};
    messages_.push_back(std::move(user_msg));

    for (int turn = 0; turn < config_.max_turns; ++turn) {
        if (cancelled_.load(std::memory_order_relaxed)) break;
        const bool cont = run_turn();
        if (!cont) break;
    }

    observer_.on_event(DoneEvent{total_usage_, {}});

    // Auto-save: persist session transcript if persistence is configured.
    if (persistence_ && !session_id_.empty()) {
        const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string ts = std::to_string(now_us);

        SessionRecord rec;
        rec.id          = session_id_;
        rec.updated_at  = ts;
        rec.messages    = messages_;
        rec.total_usage = total_usage_;

        // Preserve created_at from an existing record if available.
        if (auto existing = persistence_->load(session_id_)) {
            rec.created_at = existing->created_at;
        } else {
            rec.created_at = ts;
        }

        persistence_->save(rec);
    }
}

}  // namespace omni::engine
