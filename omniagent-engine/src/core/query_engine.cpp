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

namespace {

Tool* lookup_tool(ToolRegistry& registry,
                  ToolRegistry* session_registry,
                  const std::string& name) {
    if (session_registry) {
        if (Tool* tool = session_registry->get(name)) {
            return tool;
        }
    }
    return registry.get(name);
}

std::string tool_call_signature(const ToolUseContent& call) {
    return call.name + "\n" + call.input.dump();
}

std::string focus_value_signature(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number() || value.is_boolean()) {
        return value.dump();
    }
    if (value.is_array()) {
        std::string rendered;
        std::size_t count = 0;
        for (const auto& item : value) {
            if (count >= 3) {
                rendered += "|...";
                break;
            }
            if (!rendered.empty()) {
                rendered += '|';
            }
            rendered += focus_value_signature(item);
            ++count;
        }
        return rendered.empty() ? value.dump() : rendered;
    }
    return value.dump();
}

std::string tool_focus_signature(const ToolUseContent& call) {
    if (!call.input.is_object()) {
        return tool_call_signature(call);
    }

    static const std::vector<std::string> kFocusKeys = {
        "path",
        "filePath",
        "dirPath",
        "uri",
        "query",
        "pattern",
        "includePattern",
        "symbol",
        "repo",
        "url",
        "urls",
        "commandId"
    };

    std::string signature = call.name;
    bool found_focus = false;
    for (const auto& key : kFocusKeys) {
        auto it = call.input.find(key);
        if (it == call.input.end()) {
            continue;
        }
        signature += "\n" + key + '=' + focus_value_signature(*it);
        found_focus = true;
    }

    if (!found_focus) {
        return tool_call_signature(call);
    }
    return signature;
}

std::string tool_turn_signature(const std::vector<ToolUseContent>& calls) {
    std::string signature;
    for (const auto& call : calls) {
        if (!signature.empty()) {
            signature += "\n--\n";
        }
        signature += tool_call_signature(call);
    }
    return signature;
}

std::vector<std::string> tool_turn_focus_signatures(const std::vector<ToolUseContent>& calls) {
    std::vector<std::string> focuses;
    focuses.reserve(calls.size());
    for (const auto& call : calls) {
        const std::string focus = tool_focus_signature(call);
        if (std::find(focuses.begin(), focuses.end(), focus) == focuses.end()) {
            focuses.push_back(focus);
        }
    }
    return focuses;
}

bool are_read_only_tool_calls(const std::vector<ToolUseContent>& calls,
                              ToolRegistry& registry,
                              ToolRegistry* session_registry) {
    if (calls.empty()) {
        return false;
    }

    for (const auto& call : calls) {
        Tool* tool = lookup_tool(registry, session_registry, call.name);
        if (!tool || !tool->is_read_only() || tool->is_destructive()) {
            return false;
        }
    }
    return true;
}

bool assistant_tool_turn_signature(const Message& message,
                                   ToolRegistry& registry,
                                   ToolRegistry* session_registry,
                                   std::string& signature,
                                   std::vector<std::string>* focus_signatures = nullptr) {
    if (message.role != Role::Assistant || message.content.empty()) {
        return false;
    }

    std::vector<ToolUseContent> calls;
    calls.reserve(message.content.size());
    for (const auto& block : message.content) {
        const auto* tool = std::get_if<ToolUseContent>(&block.data);
        if (!tool) {
            return false;
        }
        Tool* resolved = lookup_tool(registry, session_registry, tool->name);
        if (!resolved || !resolved->is_read_only() || resolved->is_destructive()) {
            return false;
        }
        calls.push_back(*tool);
    }

    signature = tool_turn_signature(calls);
    if (focus_signatures) {
        *focus_signatures = tool_turn_focus_signatures(calls);
    }
    return !signature.empty();
}

std::vector<std::string> visible_tool_names(const std::optional<std::vector<std::string>>& tool_filter,
                                            ToolRegistry& registry,
                                            ToolRegistry* session_registry) {
    if (tool_filter.has_value()) {
        return *tool_filter;
    }

    std::vector<std::string> names = registry.names();
    if (session_registry) {
        for (const auto& name : session_registry->names()) {
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
            }
        }
    }
    return names;
}

std::vector<std::string> visible_read_only_tool_names(const std::optional<std::vector<std::string>>& tool_filter,
                                                      ToolRegistry& registry,
                                                      ToolRegistry* session_registry) {
    std::vector<std::string> read_only_names;
    for (const auto& name : visible_tool_names(tool_filter, registry, session_registry)) {
        Tool* tool = lookup_tool(registry, session_registry, name);
        if (!tool || !tool->is_read_only() || tool->is_destructive()) {
            continue;
        }
        read_only_names.push_back(name);
    }
    return read_only_names;
}

bool visible_tools_are_read_only(const std::optional<std::vector<std::string>>& tool_filter,
                                 ToolRegistry& registry,
                                 ToolRegistry* session_registry) {
    const std::vector<std::string> names = visible_tool_names(tool_filter, registry, session_registry);
    if (names.empty()) {
        return false;
    }

    for (const auto& name : names) {
        Tool* tool = lookup_tool(registry, session_registry, name);
        if (!tool || !tool->is_read_only() || tool->is_destructive()) {
            return false;
        }
    }
    return true;
}

bool has_recent_read_only_exploration_churn(const std::vector<Message>& messages,
                                            ToolRegistry& registry,
                                            ToolRegistry* session_registry,
                                            const std::optional<std::vector<std::string>>& tool_filter,
                                            const std::vector<ToolUseContent>& current_calls) {
    if (!visible_tools_are_read_only(tool_filter, registry, session_registry)) {
        return false;
    }

    if (!are_read_only_tool_calls(current_calls, registry, session_registry)) {
        return false;
    }

    constexpr std::size_t kRepeatedFocusWindow = 6;
    constexpr std::size_t kExactLoopWindow = 8;
    constexpr std::size_t kHardReadOnlyTurnBudget = 12;
    constexpr std::size_t kMaxUniqueTurns = 5;
    constexpr std::size_t kMaxUniqueFocuses = 6;

    std::vector<std::string> recent_turns;
    std::vector<std::vector<std::string>> recent_focuses;
    recent_turns.reserve(kHardReadOnlyTurnBudget);
    recent_focuses.reserve(kHardReadOnlyTurnBudget);
    recent_turns.push_back(tool_turn_signature(current_calls));
    recent_focuses.push_back(tool_turn_focus_signatures(current_calls));

    for (auto it = messages.rbegin(); it != messages.rend() && recent_turns.size() < kHardReadOnlyTurnBudget; ++it) {
        if (it->role == Role::ToolResult) {
            continue;
        }
        if (it->role != Role::Assistant) {
            break;
        }

        std::string signature;
        std::vector<std::string> focus_signatures;
        if (!assistant_tool_turn_signature(*it,
                                           registry,
                                           session_registry,
                                           signature,
                                           &focus_signatures)) {
            break;
        }
        recent_turns.push_back(std::move(signature));
        recent_focuses.push_back(std::move(focus_signatures));
    }

    if (recent_turns.size() >= kHardReadOnlyTurnBudget) {
        return true;
    }

    if (recent_turns.size() >= kExactLoopWindow) {
        const std::string& current_signature = recent_turns.front();
        if (std::find(recent_turns.begin() + 1, recent_turns.end(), current_signature)
            != recent_turns.end()) {
            std::vector<std::string> unique_turns;
            unique_turns.reserve(recent_turns.size());
            for (const auto& turn : recent_turns) {
                if (std::find(unique_turns.begin(), unique_turns.end(), turn) == unique_turns.end()) {
                    unique_turns.push_back(turn);
                }
            }
            if (unique_turns.size() <= kMaxUniqueTurns) {
                return true;
            }
        }
    }

    if (recent_focuses.size() < kRepeatedFocusWindow || recent_focuses.front().empty()) {
        return false;
    }

    std::unordered_map<std::string, std::size_t> prior_focus_counts;
    std::vector<std::string> unique_focuses;
    for (const auto& turn_focuses : recent_focuses) {
        for (const auto& focus : turn_focuses) {
            if (std::find(unique_focuses.begin(), unique_focuses.end(), focus) == unique_focuses.end()) {
                unique_focuses.push_back(focus);
            }
        }
    }

    for (std::size_t index = 1; index < recent_focuses.size(); ++index) {
        for (const auto& focus : recent_focuses[index]) {
            ++prior_focus_counts[focus];
        }
    }

    const bool current_focuses_seen_before = std::all_of(
        recent_focuses.front().begin(),
        recent_focuses.front().end(),
        [&](const std::string& focus) {
            return prior_focus_counts.contains(focus);
        });

    return current_focuses_seen_before && unique_focuses.size() <= kMaxUniqueFocuses;
}

}  // namespace

static std::string build_plain_text_retry_user_instruction(const std::string& prefix);
static std::vector<Message> build_plain_text_retry_messages(
    const std::vector<Message>& messages,
    int preserve_tail,
    int compact_max_result_chars,
    const ToolContext& tool_context,
    const std::string& final_user_instruction);

constexpr int kPlainTextRetryMaxTokens = 4096;

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
    cancelled_.store(true, std::memory_order_relaxed);
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

void QueryEngine::set_evidence_based_final_answer(bool enabled) {
    config_.enforce_evidence_based_final_answer = enabled;
}

void QueryEngine::set_max_parallel_tools(int max_parallel_tools) {
    config_.max_parallel_tools = std::max(1, max_parallel_tools);
}

bool QueryEngine::run_review_validation_recovery_turn(const std::string& draft_answer,
                                                      const std::string& validation_feedback) {
    constexpr int kReviewValidationRecoveryMaxTurns = 4;

    const std::string saved_system_prompt = config_.system_prompt;
    const bool saved_plain_text_finalization_mode = plain_text_finalization_mode_;
    const bool saved_require_tool_call = require_tool_call_on_next_turn_;
    const auto saved_tool_filter = tool_filter_;
    const std::size_t saved_message_count = messages_.size();

    if (tool_filter_.has_value()
        && tool_filter_->empty()
        && !forced_finalization_recovery_tools_.empty()) {
        tool_filter_ = forced_finalization_recovery_tools_;
    }

    plain_text_finalization_mode_ = false;
    require_tool_call_on_next_turn_ = true;
    if (!config_.system_prompt.empty()) {
        config_.system_prompt += "\n\n";
    }
    config_.system_prompt +=
        "You are continuing a systematic code audit after deterministic validation failed. "
        "Do not finalize yet. Use the validation feedback to identify which repository surfaces still need evidence. "
        "Audit in this order when relevant: repository map and entrypoints, validation or contract surfaces such as tests, CLI behavior, schemas, config loaders, API contracts, or explicit command output, then the implementation behind the mismatch. "
        "Gather only the missing evidence needed to confirm or reject the missing findings, prefer targeted reads and searches over broad sweeps, and stop once you can produce a concise findings-first report.\n\n"
        "Rejected draft:\n\n" + draft_answer + "\n\nValidation feedback:\n\n" + validation_feedback;

    bool exhausted_tool_turn_budget = false;
    for (int turn = 0; turn < kReviewValidationRecoveryMaxTurns; ++turn) {
        if (cancelled_.load(std::memory_order_relaxed) || stop_requested_.load(std::memory_order_relaxed)) {
            break;
        }
        const bool cont = run_turn();
        if (!cont) {
            break;
        }
        if (turn == kReviewValidationRecoveryMaxTurns - 1) {
            exhausted_tool_turn_budget = true;
        }
    }

    if (exhausted_tool_turn_budget
        && !cancelled_.load(std::memory_order_relaxed)
        && !stop_requested_.load(std::memory_order_relaxed)) {
        (void)run_forced_final_answer_turn(
            "You have gathered additional audit evidence after deterministic review validation feedback. "
            "Do not call more tools. Based only on the evidence already gathered, return the corrected findings-first audit report now.");
    }

    tool_filter_ = saved_tool_filter;
    config_.system_prompt = saved_system_prompt;
    plain_text_finalization_mode_ = saved_plain_text_finalization_mode;
    require_tool_call_on_next_turn_ = saved_require_tool_call;
    return messages_.size() > saved_message_count;
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

    if (plain_text_finalization_mode_) {
        req.messages = build_plain_text_retry_messages(
            messages_,
            config_.preserve_tail,
            config_.compact_max_result_chars,
            tool_context_,
            build_plain_text_retry_user_instruction({}));
    } else {
        req.messages = microcompact(messages_,
                                    config_.preserve_tail,
                                    config_.compact_max_result_chars);
    }
    // Collect all tool definitions: engine-level + session-level.
    std::vector<nlohmann::json> all_defs = registry_.tool_definitions();
    if (session_registry_) {
        for (auto& d : session_registry_->tool_definitions()) {
            all_defs.push_back(std::move(d));
        }
    }

    if (!tool_filter_.has_value()) {
        req.tools = std::move(all_defs);
    } else {
        // Only include tools whose name is in the allowed list.
        for (const auto& def : all_defs) {
            const std::string n = def.at("function").at("name").get<std::string>();
            if (std::find(tool_filter_->begin(), tool_filter_->end(), n) != tool_filter_->end()) {
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
        req.tool_choice = (require_tool_call_on_next_turn_ || (has_workspace && !has_prior_tool_turn))
            ? "required"
            : "auto";
        if (req.tool_choice == "required") {
            if (!req.system_prompt.empty()) {
                req.system_prompt += "\n\n";
            }
            if (require_tool_call_on_next_turn_) {
                req.system_prompt +=
                    "Deterministic review validation found missing evidence. In this turn you must use at least one available tool to gather that missing evidence before replying.";
            } else {
                req.system_prompt +=
                    "When tools are available in this turn, do not describe intended actions. "
                    "Use structured tool calls to inspect or change the workspace before replying.";
            }
        }
    }

    // Max output token escalation: if the last turn was truncated, double the
    // token limit (up to 8x initial) so incomplete tool calls can complete.
    const auto caps = provider_->capabilities();
    req.temperature = config_.temperature;
    req.top_p = config_.top_p;
    req.top_k = config_.top_k;
    req.min_p = config_.min_p;
    req.presence_penalty = config_.presence_penalty;
    req.frequency_penalty = config_.frequency_penalty;
    int target_tokens = config_.initial_max_tokens;
    if (config_.max_tokens_escalation && last_turn_truncated_) {
        target_tokens = config_.initial_max_tokens * 8;
    }
    if (caps.max_output_tokens > 0) {
        target_tokens = std::min(target_tokens, caps.max_output_tokens);
    }
    req.max_tokens = target_tokens;
    if (plain_text_finalization_mode_ && *req.max_tokens > kPlainTextRetryMaxTokens) {
        req.max_tokens = kPlainTextRetryMaxTokens;
    }

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

static void erase_tagged_sections(std::string& text,
                                  const std::string& open_tag,
                                  const std::string& close_tag) {
    std::size_t search_from = 0;
    while (true) {
        const std::size_t open = text.find(open_tag, search_from);
        if (open == std::string::npos) {
            return;
        }

        const std::size_t close = text.find(close_tag, open + open_tag.size());
        if (close == std::string::npos) {
            return;
        }

        text.erase(open, close + close_tag.size() - open);
        search_from = open;
    }
}

static nlohmann::json parse_text_tool_parameter_value(std::string text) {
    text = trim_copy(std::move(text));
    if (text.empty()) {
        return "";
    }

    const unsigned char first = static_cast<unsigned char>(text.front());
    const bool looks_like_json = text == "true"
        || text == "false"
        || text == "null"
        || text.front() == '{'
        || text.front() == '['
        || text.front() == '"'
        || std::isdigit(first)
        || text.front() == '-'
        || text.front() == '+';
    if (looks_like_json) {
        try {
            return nlohmann::json::parse(text);
        } catch (...) {
        }
    }

    return text;
}

static std::optional<ToolUseContent> parse_xmlish_tool_call(std::string text,
                                                            std::size_t index) {
    constexpr std::string_view function_open = "<function=";
    constexpr std::string_view function_close = "</function>";
    constexpr std::string_view parameter_open = "<parameter=";
    constexpr std::string_view parameter_close = "</parameter>";

    const std::size_t function_start = text.find(function_open);
    if (function_start == std::string::npos) {
        return std::nullopt;
    }

    const std::size_t function_name_end = text.find('>', function_start + function_open.size());
    if (function_name_end == std::string::npos) {
        return std::nullopt;
    }

    const std::string function_name = trim_copy(
        text.substr(function_start + function_open.size(),
                    function_name_end - function_start - function_open.size()));
    if (function_name.empty()) {
        return std::nullopt;
    }

    const std::size_t function_end = text.find(function_close, function_name_end + 1);
    if (function_end == std::string::npos) {
        return std::nullopt;
    }

    const std::string body = text.substr(function_name_end + 1,
                                         function_end - function_name_end - 1);
    nlohmann::json input = nlohmann::json::object();
    std::size_t search_from = 0;
    while (true) {
        const std::size_t parameter_start = body.find(parameter_open, search_from);
        if (parameter_start == std::string::npos) {
            break;
        }

        const std::size_t parameter_name_end = body.find('>', parameter_start + parameter_open.size());
        if (parameter_name_end == std::string::npos) {
            return std::nullopt;
        }

        const std::size_t parameter_end = body.find(parameter_close,
                                                    parameter_name_end + 1);
        if (parameter_end == std::string::npos) {
            return std::nullopt;
        }

        const std::string parameter_name = trim_copy(
            body.substr(parameter_start + parameter_open.size(),
                        parameter_name_end - parameter_start - parameter_open.size()));
        if (parameter_name.empty()) {
            return std::nullopt;
        }

        input[parameter_name] = parse_text_tool_parameter_value(
            body.substr(parameter_name_end + 1,
                        parameter_end - parameter_name_end - 1));
        search_from = parameter_end + parameter_close.size();
    }

    return ToolUseContent{
        "recovered-tool-call-" + std::to_string(index + 1),
        function_name,
        std::move(input),
    };
}

static std::optional<std::pair<nlohmann::json, std::size_t>> parse_embedded_json_object(
    const std::string& text,
    std::size_t start) {
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    if (start >= text.size() || text[start] != '{') {
        return std::nullopt;
    }

    bool in_string = false;
    bool escape = false;
    int depth = 0;
    for (std::size_t index = start; index < text.size(); ++index) {
        const char ch = text[index];
        if (in_string) {
            if (escape) {
                escape = false;
            } else if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == '{') {
            ++depth;
            continue;
        }
        if (ch != '}') {
            continue;
        }

        --depth;
        if (depth != 0) {
            continue;
        }

        try {
            return std::make_pair(
                nlohmann::json::parse(text.substr(start, index - start + 1)),
                index + 1);
        } catch (...) {
            return std::nullopt;
        }
    }

    return std::nullopt;
}

static std::vector<ToolUseContent> recover_named_tool_use_calls(std::vector<ContentBlock>& blocks) {
    std::string combined_text;
    for (const ContentBlock& block : blocks) {
        const auto* text = std::get_if<TextContent>(&block.data);
        if (text == nullptr) {
            return {};
        }
        combined_text += text->text;
    }

    constexpr std::string_view tool_use_open = "<tool_use";

    std::vector<ToolUseContent> recovered;
    std::size_t search_from = 0;
    while (true) {
        const std::size_t tool_start = combined_text.find(tool_use_open, search_from);
        if (tool_start == std::string::npos) {
            break;
        }

        const std::size_t name_attr = combined_text.find("name=", tool_start + tool_use_open.size());
        if (name_attr == std::string::npos) {
            return {};
        }

        const std::size_t quote_pos = name_attr + 5;
        if (quote_pos >= combined_text.size()) {
            return {};
        }

        const char quote = combined_text[quote_pos];
        if (quote != '"' && quote != '\'') {
            return {};
        }

        const std::size_t name_end = combined_text.find(quote, quote_pos + 1);
        if (name_end == std::string::npos) {
            return {};
        }

        const std::string tool_name = trim_copy(
            combined_text.substr(quote_pos + 1, name_end - quote_pos - 1));
        if (tool_name.empty()) {
            return {};
        }

        const std::size_t tag_end = combined_text.find('>', name_end + 1);
        if (tag_end == std::string::npos) {
            return {};
        }

        const auto parsed_json = parse_embedded_json_object(combined_text, tag_end + 1);
        if (!parsed_json.has_value() || !parsed_json->first.is_object()) {
            return {};
        }

        recovered.push_back(ToolUseContent{
            "recovered-tool-use-" + std::to_string(recovered.size() + 1),
            tool_name,
            parsed_json->first,
        });
        search_from = parsed_json->second;
    }

    if (recovered.empty()) {
        return {};
    }

    std::vector<ContentBlock> rewritten;
    rewritten.reserve(recovered.size());
    for (const auto& tool_call : recovered) {
        rewritten.push_back(ContentBlock{tool_call});
    }
    blocks = std::move(rewritten);
    return recovered;
}

static std::vector<ToolUseContent> recover_xmlish_tool_calls(std::vector<ContentBlock>& blocks) {
    std::string combined_text;
    for (const ContentBlock& block : blocks) {
        const auto* text = std::get_if<TextContent>(&block.data);
        if (text == nullptr) {
            return {};
        }
        combined_text += text->text;
    }

    constexpr std::string_view tool_open = "<tool_call>";
    constexpr std::string_view tool_close = "</tool_call>";

    std::vector<ToolUseContent> recovered;
    std::size_t search_from = 0;
    while (true) {
        const std::size_t tool_start = combined_text.find(tool_open, search_from);
        if (tool_start == std::string::npos) {
            break;
        }

        const std::size_t tool_end = combined_text.find(tool_close,
                                                        tool_start + tool_open.size());
        if (tool_end == std::string::npos) {
            return {};
        }

        auto parsed = parse_xmlish_tool_call(
            combined_text.substr(tool_start + tool_open.size(),
                                 tool_end - tool_start - tool_open.size()),
            recovered.size());
        if (!parsed.has_value()) {
            return {};
        }

        recovered.push_back(std::move(*parsed));
        search_from = tool_end + tool_close.size();
    }

    if (recovered.empty()) {
        return {};
    }

    std::vector<ContentBlock> rewritten;
    rewritten.reserve(recovered.size());
    for (const auto& tool_call : recovered) {
        rewritten.push_back(ContentBlock{tool_call});
    }
    blocks = std::move(rewritten);
    return recovered;
}

static std::string sanitize_assistant_text(std::string text) {
    const bool has_markup = text.find("<|tool_call>") != std::string::npos
        || text.find("<|tool_response>") != std::string::npos
        || text.find("<|channel>thought") != std::string::npos
        || text.find("<channel|>") != std::string::npos
        || text.find("|thought") != std::string::npos
        || text.find("<tool_use name=") != std::string::npos
        || text.find("<tool_call>") != std::string::npos
        || text.find("<function=") != std::string::npos
        || text.find("<parameter=") != std::string::npos;
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
    erase_tagged_sections(text, "<tool_call>", "</tool_call>");
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
        || trimmed.find("<tool_use name=") != std::string::npos
        || trimmed.find("<tool_call>") != std::string::npos
        || trimmed.find("</tool_call>") != std::string::npos
        || trimmed.find("<function=") != std::string::npos
        || trimmed.find("<parameter=") != std::string::npos
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

static void strip_text_blocks_from_tool_turn(std::vector<ContentBlock>& blocks) {
    const bool has_tool_calls = std::any_of(blocks.begin(), blocks.end(), [](const ContentBlock& block) {
        return std::get_if<ToolUseContent>(&block.data) != nullptr;
    });
    if (!has_tool_calls) {
        return;
    }

    blocks.erase(
        std::remove_if(blocks.begin(), blocks.end(), [](const ContentBlock& block) {
            return std::get_if<TextContent>(&block.data) != nullptr;
        }),
        blocks.end());
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

static Message make_text_message(Role role, const std::string& text) {
    Message message;
    message.role = role;
    message.content = {ContentBlock{TextContent{text}}};
    return message;
}

static std::string assistant_text_from_blocks(const std::vector<ContentBlock>& blocks);

static std::string build_plain_text_retry_user_instruction(const std::string& prefix) {
    std::string instruction = trim_copy(prefix);
    if (!instruction.empty()) {
        instruction += "\n\n";
    }
    instruction +=
        "Using only the evidence already gathered above, produce the final user-facing answer now. "
        "Do not ask to inspect more files. Do not call or simulate tools. Do not emit thinking-only text, "
        "tool markup, channel tags, or pseudo-tool syntax. Reply in plain text only.";
    return instruction;
}

static std::vector<Message> build_plain_text_retry_messages(
    const std::vector<Message>& messages,
    int preserve_tail,
    int compact_max_result_chars,
    const ToolContext& tool_context,
    const std::string& final_user_instruction)
{
    const std::vector<Message> compacted =
        microcompact(messages, preserve_tail, compact_max_result_chars);

    std::vector<Message> rewritten;
    rewritten.reserve(compacted.size() + 2);

    for (const Message& message : compacted) {
        if (message.role == Role::User || message.role == Role::System) {
            rewritten.push_back(message);
            continue;
        }

        if (message.role == Role::Assistant) {
            const std::string assistant_text = assistant_text_from_blocks(message.content);
            if (!assistant_text.empty()) {
                rewritten.push_back(make_text_message(Role::Assistant, assistant_text));
            }
            continue;
        }

        if (message.role != Role::ToolResult || message.tool_results.empty()) {
            continue;
        }

        std::ostringstream evidence;
        evidence << "Evidence gathered from previous workspace inspection:\n\n";
        for (std::size_t index = 0; index < message.tool_results.size(); ++index) {
            const ToolResult& result = message.tool_results[index];
            evidence << "Tool result";
            if (!result.tool_use_id.empty()) {
                evidence << " [" << result.tool_use_id << "]";
            }
            if (result.is_error) {
                evidence << " (error)";
            }
            evidence << ":\n" << result.content;
            if (result.content.empty() || result.content.back() != '\n') {
                evidence << '\n';
            }
            if (index + 1 < message.tool_results.size()) {
                evidence << '\n';
            }
        }
        rewritten.push_back(make_text_message(Role::User, trim_copy(evidence.str())));
    }

    const std::string workspace_snapshot = build_workspace_snapshot(tool_context);
    if (!workspace_snapshot.empty()) {
        rewritten.push_back(make_text_message(
            Role::User,
            "Workspace snapshot for final answer synthesis:\n\n" + workspace_snapshot));
    }

    rewritten.push_back(make_text_message(Role::User, final_user_instruction));
    return rewritten;
}

static std::string assistant_text_from_blocks(const std::vector<ContentBlock>& blocks) {
    std::string combined;
    for (const ContentBlock& block : blocks) {
        if (const auto* text = std::get_if<TextContent>(&block.data)) {
            combined += text->text;
        }
    }
    return trim_copy(std::move(combined));
}

static std::vector<ContentBlock> evidence_verification_failure_blocks() {
    return {ContentBlock{TextContent{
        "Unable to produce a trustworthy audit summary from the gathered evidence."
    }}};
}

static std::vector<ContentBlock> final_report_validation_failure_blocks() {
    return {ContentBlock{TextContent{
        "Unable to produce a trustworthy final report from the gathered evidence."
    }}};
}

static bool report_validation_unavailable(const std::string& content) {
    const std::string lower = to_lower(content);
    return lower.find("no tracked review case matched") != std::string::npos
        || lower.find("multiple tracked review cases matched") != std::string::npos
        || lower.find("could not infer a tracked review case") != std::string::npos
        || lower.find("either 'case_id' or 'case_path' is required") != std::string::npos
        || lower.find("requires a project workspace context") != std::string::npos
        || lower.find("could not locate planner-harness") != std::string::npos;
}

static bool has_non_validator_recovery_tool(const CompletionRequest& request) {
    for (const auto& tool : request.tools) {
        const auto function_it = tool.find("function");
        if (function_it == tool.end() || !function_it->is_object()) {
            continue;
        }
        const auto name_it = function_it->find("name");
        if (name_it == function_it->end() || !name_it->is_string()) {
            continue;
        }
        if (name_it->get<std::string>() != "planner_validate_review") {
            return true;
        }
    }
    return false;
}

static bool has_non_validator_recovery_tool_names(const std::vector<std::string>& tool_names) {
    return std::any_of(tool_names.begin(), tool_names.end(), [](const std::string& name) {
        return name != "planner_validate_review";
    });
}

static std::optional<std::vector<ContentBlock>> rewrite_final_report_from_validation_feedback(
    const CompletionRequest& base_request,
    const std::string& draft_answer,
    const std::string& validation_feedback,
    LLMProvider& provider,
    std::atomic<bool>& cancelled,
    Usage& turn_usage,
    Usage& total_usage,
    EventObserver& observer,
    CostTracker* cost_tracker) {
    CompletionRequest rewrite_req = base_request;
    rewrite_req.tools.clear();
    rewrite_req.tool_choice.reset();
    if (!rewrite_req.system_prompt.empty()) {
        rewrite_req.system_prompt += "\n\n";
    }
    rewrite_req.system_prompt +=
        "You are correcting a final code-review answer after deterministic validation failed. "
        "Rewrite it into a concise findings-first report that addresses the validation feedback only when "
        "those corrections are directly supported by evidence already gathered in the conversation. Add "
        "missing findings only if the exact code or tool output is already present in the conversation. "
        "Remove unsupported claims, test-blame, invented defects, and generic filler sections. Return only "
        "the corrected final answer.";
    rewrite_req.messages.push_back(make_text_message(
        Role::User,
        "Draft final answer:\n\n" + draft_answer + "\n\nValidation feedback:\n\n" + validation_feedback));

    StreamAssembler rewrite_assembler;
    auto rewrite_cb = [&](const StreamEventData& ev) {
        rewrite_assembler.process(ev);
    };

    try {
        const Usage rewrite_usage = provider.complete(rewrite_req, rewrite_cb, cancelled);
        turn_usage += rewrite_usage;
        total_usage += rewrite_usage;
        observer.on_event(UsageUpdatedEvent{rewrite_usage, total_usage, {}});
        if (cost_tracker) {
            cost_tracker->record(provider.name(), rewrite_usage);
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }

    std::vector<ContentBlock> rewritten_blocks = rewrite_assembler.take_completed_blocks();
    sanitize_assistant_blocks(rewritten_blocks);
    auto rewritten_tool_calls = extract_tool_calls(rewritten_blocks);
    if (rewritten_tool_calls.empty()) {
        rewritten_tool_calls = recover_named_tool_use_calls(rewritten_blocks);
    }
    if (rewritten_tool_calls.empty()) {
        rewritten_tool_calls = recover_xmlish_tool_calls(rewritten_blocks);
    }
    strip_text_blocks_from_tool_turn(rewritten_blocks);
    const bool rewritten_malformed = rewritten_tool_calls.empty()
        && has_malformed_tool_transcript(rewritten_blocks);
    if (rewritten_malformed
        || !rewritten_tool_calls.empty()
        || !has_visible_assistant_content(rewritten_blocks)) {
        return std::nullopt;
    }

    return rewritten_blocks;
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

            if (cancelled_.load(std::memory_order_relaxed)
                || stop_requested_.load(std::memory_order_relaxed)) {
                return false;
            }

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
        if (cancelled_.load(std::memory_order_relaxed)
            || stop_requested_.load(std::memory_order_relaxed)) {
            return false;
        }

        observer_.on_event(ErrorEvent{
            "Model returned an empty response. Try again or switch models.",
            false,
            {}
        });
        return false;
    }

    std::vector<ToolUseContent> tool_calls = extract_tool_calls(blocks);
    if (tool_calls.empty()) {
        tool_calls = recover_named_tool_use_calls(blocks);
    }
    if (tool_calls.empty()) {
        tool_calls = recover_xmlish_tool_calls(blocks);
    } else {
        require_tool_call_on_next_turn_ = false;
    }
    if (!tool_calls.empty()) {
        require_tool_call_on_next_turn_ = false;
    }
    bool malformed_tool_transcript = tool_calls.empty() && has_malformed_tool_transcript(blocks);
    bool tool_calls_when_unavailable = req.tools.empty() && !tool_calls.empty();
    if (!has_visible_assistant_content(blocks)
        || malformed_tool_transcript
        || tool_calls_when_unavailable) {
        CompletionRequest fallback_req = req;
        fallback_req.tools.clear();
        fallback_req.tool_choice.reset();
        if (!fallback_req.system_prompt.empty()) {
            fallback_req.system_prompt += "\n\n";
        }
        std::string retry_prefix;
        if (tool_calls_when_unavailable) {
            retry_prefix =
                "The previous attempt tried to call tools even though no tools were available for this turn.";
            fallback_req.system_prompt += retry_prefix + " ";
        }
        fallback_req.system_prompt +=
            "The previous attempt returned no usable answer. There are no tools available in this retry. "
            "Do not return thinking only. Do not simulate tool calls, tool responses, or channel tags such as "
            "<|tool_call>, <|tool_response>, <|channel>thought, <channel|>, <tool_use name=...>, call:, ls_r(...), or execute_command(...). "
            "Reply with only a concise final answer for the user in plain text.";
        fallback_req.messages = build_plain_text_retry_messages(
            messages_,
            config_.preserve_tail,
            config_.compact_max_result_chars,
            tool_context_,
            build_plain_text_retry_user_instruction(retry_prefix));
        if (!fallback_req.max_tokens.has_value()
            || *fallback_req.max_tokens > kPlainTextRetryMaxTokens) {
            fallback_req.max_tokens = kPlainTextRetryMaxTokens;
        }
        try {
            assembler.reset();
            block_tool_ids.clear();
            turn_usage += provider_->complete(fallback_req, stream_cb, cancelled_);
            blocks = assembler.take_completed_blocks();
            tool_calls = extract_tool_calls(blocks);
            if (tool_calls.empty()) {
                tool_calls = recover_named_tool_use_calls(blocks);
            }
            if (tool_calls.empty()) {
                tool_calls = recover_xmlish_tool_calls(blocks);
            }
            malformed_tool_transcript = tool_calls.empty() && has_malformed_tool_transcript(blocks);
            tool_calls_when_unavailable = fallback_req.tools.empty() && !tool_calls.empty();
        } catch (const std::exception& error) {
            observer_.on_event(ErrorEvent{error.what(), false, {}});
            return false;
        }
    }

    sanitize_assistant_blocks(blocks);
    strip_text_blocks_from_tool_turn(blocks);
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

    if (tool_calls_when_unavailable) {
        observer_.on_event(ErrorEvent{
            "Model attempted to call tools even though no tools were available for this turn.",
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

    if (!tool_calls.empty()
        && !cancelled_.load(std::memory_order_relaxed)
        && !stop_requested_.load(std::memory_order_relaxed)
        && has_recent_read_only_exploration_churn(messages_,
                              registry_,
                              session_registry_,
                              tool_filter_,
                              tool_calls)) {
        return run_forced_final_answer_turn(
            "Your read-only exploration is looping or yielding only marginally new information. Do not call more tools. "
            "Based only on the evidence already gathered in the conversation, answer the user's request directly now. "
            "Summarize the most relevant findings, call out what remains uncertain or unverified, and stop exploring instead of rereading more files or rerunning similar searches.");
    }

    if (tool_calls.empty()
        && config_.enforce_evidence_based_final_answer
        && !cancelled_.load(std::memory_order_relaxed)
        && !stop_requested_.load(std::memory_order_relaxed)) {
        const std::string draft_answer = assistant_text_from_blocks(blocks);
        if (!draft_answer.empty()) {
            CompletionRequest verify_req = req;
            verify_req.tools.clear();
            verify_req.tool_choice.reset();
            if (!verify_req.system_prompt.empty()) {
                verify_req.system_prompt += "\n\n";
            }
            verify_req.system_prompt +=
                "You are verifying a final code-review answer. Rewrite it into a short findings-first answer where "
                "every finding is directly supported by evidence already gathered in the conversation. You may add "
                "concise findings that are directly and unambiguously supported by explicit code or tool output already "
                "present in the conversation when the draft omitted them, but do not invent beyond that evidence. "
                "Remove any finding whose evidence is missing, weak, or contradicted. For syntax errors, invalid "
                "identifiers, missing attributes, or line-specific claims, keep them only if the exact offending code "
                "or tool output is present in the conversation. Do not label something a test bug or mock issue unless "
                "the evidence explicitly shows the test expectation or patch target is wrong. Do not keep generic filler "
                "sections such as broad summaries, recommendations, security considerations, documentation commentary, "
                "production-readiness verdicts, missing-feature wish lists, or percentages/counts unless the user "
                "requested them or the evidence explicitly supports them. When explicit failing test or command output is "
                "present, preserve materially distinct failure clusters shown there instead of collapsing them into a "
                "single guessed root cause or omitting them. If no evidence-backed findings remain, say that explicitly. "
                "Return only the corrected final answer.";
            verify_req.messages.push_back(make_text_message(
                Role::User,
                "Draft final answer to verify and rewrite:\n\n" + draft_answer));

            StreamAssembler verify_assembler;
            auto verify_cb = [&](const StreamEventData& ev) {
                verify_assembler.process(ev);
            };

            bool verification_succeeded = false;
            try {
                const Usage verification_usage = provider_->complete(verify_req, verify_cb, cancelled_);
                turn_usage += verification_usage;
                total_usage_ += verification_usage;
                observer_.on_event(UsageUpdatedEvent{verification_usage, total_usage_, {}});
                if (cost_tracker_) {
                    cost_tracker_->record(provider_->name(), verification_usage);
                }
                std::vector<ContentBlock> verified_blocks = verify_assembler.take_completed_blocks();
                sanitize_assistant_blocks(verified_blocks);
                const auto verified_tool_calls = extract_tool_calls(verified_blocks);
                strip_text_blocks_from_tool_turn(verified_blocks);
                const bool verified_malformed = verified_tool_calls.empty()
                    && has_malformed_tool_transcript(verified_blocks);
                if (!verified_malformed
                    && verified_tool_calls.empty()
                    && has_visible_assistant_content(verified_blocks)) {
                    blocks = std::move(verified_blocks);
                    verification_succeeded = true;
                }
            } catch (const std::exception&) {
                verification_succeeded = false;
            }

            if (!verification_succeeded) {
                blocks = evidence_verification_failure_blocks();
            }
        }
    }

    if (tool_calls.empty()
        && config_.enforce_evidence_based_final_answer
        && !cancelled_.load(std::memory_order_relaxed)
        && !stop_requested_.load(std::memory_order_relaxed)) {
        const std::string draft_answer = assistant_text_from_blocks(blocks);
        Tool* review_validator = lookup_tool(registry_, session_registry_, "planner_validate_review");
        if (!draft_answer.empty() && review_validator != nullptr) {
            const ToolCallResult validation_result = review_validator->call(
                nlohmann::json{{"report_text", draft_answer}},
                tool_context_);
            if (validation_result.is_error && !report_validation_unavailable(validation_result.content)) {
                const bool can_attempt_tool_recovery = review_validation_recovery_attempts_ == 0
                    && (has_non_validator_recovery_tool(req)
                        || has_non_validator_recovery_tool_names(forced_finalization_recovery_tools_));
                if (can_attempt_tool_recovery) {
                    ++review_validation_recovery_attempts_;
                    if (run_review_validation_recovery_turn(draft_answer, validation_result.content)) {
                        return false;
                    }
                }

                const auto rewritten_blocks = rewrite_final_report_from_validation_feedback(
                    req,
                    draft_answer,
                    validation_result.content,
                    *provider_,
                    cancelled_,
                    turn_usage,
                    total_usage_,
                    observer_,
                    cost_tracker_);
                if (!rewritten_blocks.has_value()) {
                    blocks = final_report_validation_failure_blocks();
                } else {
                    const std::string corrected_answer = assistant_text_from_blocks(*rewritten_blocks);
                    const ToolCallResult corrected_validation = review_validator->call(
                        nlohmann::json{{"report_text", corrected_answer}},
                        tool_context_);
                    if (corrected_validation.is_error
                        && !report_validation_unavailable(corrected_validation.content)) {
                        blocks = final_report_validation_failure_blocks();
                    } else {
                        blocks = *rewritten_blocks;
                    }
                }
            }
        }
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
    exec_cfg.allowed_tools = tool_filter_.value_or(std::vector<std::string>{});
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

bool QueryEngine::run_forced_final_answer_turn(const std::string& extra_instruction) {
    const auto saved_tool_filter = tool_filter_;
    const std::string saved_system_prompt = config_.system_prompt;
    const bool saved_plain_text_finalization_mode = plain_text_finalization_mode_;

    forced_finalization_recovery_tools_ = visible_read_only_tool_names(
        tool_filter_,
        registry_,
        session_registry_);
    tool_filter_ = std::vector<std::string>{};
    plain_text_finalization_mode_ = true;
    if (!config_.system_prompt.empty()) {
        config_.system_prompt += "\n\n";
    }
    if (!extra_instruction.empty()) {
        config_.system_prompt += extra_instruction;
        config_.system_prompt += "\n\n";
    }
    config_.system_prompt +=
        "You have finished using tools for this request. Do not ask to use more tools. "
        "Based only on the evidence already gathered in the conversation, provide the final answer now.";

    const bool cont = run_turn();

    tool_filter_ = saved_tool_filter;
    config_.system_prompt = saved_system_prompt;
    plain_text_finalization_mode_ = saved_plain_text_finalization_mode;
    return cont;
}

// ---------------------------------------------------------------------------
// submit — entry point; runs the agent loop
// ---------------------------------------------------------------------------

void QueryEngine::submit(const std::string& text) {
    cancelled_.store(false, std::memory_order_relaxed);
    stop_requested_.store(false, std::memory_order_relaxed);
    require_tool_call_on_next_turn_ = false;
    review_validation_recovery_attempts_ = 0;
    forced_finalization_recovery_tools_.clear();

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

    bool exhausted_tool_turn_budget = false;
    for (int turn = 0; turn < config_.max_turns; ++turn) {
        if (cancelled_.load(std::memory_order_relaxed)) break;
        const bool cont = run_turn();
        if (!cont) break;
        if (turn == config_.max_turns - 1) {
            exhausted_tool_turn_budget = true;
        }
    }

    if (exhausted_tool_turn_budget
        && !cancelled_.load(std::memory_order_relaxed)
        && !stop_requested_.load(std::memory_order_relaxed)) {
        (void)run_forced_final_answer_turn();
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
