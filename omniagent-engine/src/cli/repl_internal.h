#pragma once

#include <omni/project_session.h>
#include <omni/run.h>

#include "tools/planner_clarification.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace omni::engine::cli::detail {

enum class CliApprovalPolicy {
    Prompt,
    Pause,
    AutoReadOnlyPrompt,
    AutoReadOnlyPause,
};

struct ClarificationQuestion {
    std::string id;
    std::string stage;
    std::string severity;
    std::string question;
    std::string recommended_default;
    bool pending = true;
};

struct PendingClarificationState {
    std::string run_id;
    std::string tool_name;
    std::string clarification_mode;
    std::string clarification_message;
    std::vector<ClarificationQuestion> questions;
    std::map<std::string, std::string> staged_answers;
    bool delegate_unanswered = false;
};

struct ReplInputHistory {
    std::vector<std::string> entries;
    std::optional<std::size_t> browse_index;
    std::string draft;
};

inline constexpr std::size_t kMaxPersistedReplHistoryEntries = 500;

inline std::filesystem::path repl_history_path(const std::filesystem::path& storage_dir) {
    return storage_dir / "repl-history.txt";
}

inline void clear_repl_history_navigation(ReplInputHistory& history) {
    history.browse_index.reset();
    history.draft.clear();
}

inline void remember_repl_history_entry(ReplInputHistory& history,
                                        const std::string& line) {
    if (line.empty()) {
        return;
    }
    history.entries.push_back(line);
    clear_repl_history_navigation(history);
}

inline std::optional<std::string> previous_repl_history_entry(ReplInputHistory& history,
                                                              const std::string& current_text) {
    if (history.entries.empty()) {
        return std::nullopt;
    }

    if (!history.browse_index.has_value()) {
        history.draft = current_text;
        history.browse_index = history.entries.size() - 1;
        return history.entries.back();
    }

    if (*history.browse_index == 0) {
        return history.entries.front();
    }

    --(*history.browse_index);
    return history.entries[*history.browse_index];
}

inline std::optional<std::string> next_repl_history_entry(ReplInputHistory& history) {
    if (!history.browse_index.has_value()) {
        return std::nullopt;
    }

    if (*history.browse_index + 1 >= history.entries.size()) {
        const std::string draft = history.draft;
        clear_repl_history_navigation(history);
        return draft;
    }

    ++(*history.browse_index);
    return history.entries[*history.browse_index];
}

inline void trim_repl_history_entries(ReplInputHistory& history,
                                      std::size_t max_entries = kMaxPersistedReplHistoryEntries) {
    if (history.entries.size() <= max_entries) {
        return;
    }
    history.entries.erase(history.entries.begin(),
                          history.entries.end() - static_cast<std::ptrdiff_t>(max_entries));
}

inline ReplInputHistory load_persisted_repl_history(
    const std::filesystem::path& storage_dir,
    std::size_t max_entries = kMaxPersistedReplHistoryEntries) {
    ReplInputHistory history;
    if (storage_dir.empty()) {
        return history;
    }

    std::ifstream input(repl_history_path(storage_dir));
    if (!input) {
        return history;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        history.entries.push_back(line);
    }
    trim_repl_history_entries(history, max_entries);
    clear_repl_history_navigation(history);
    return history;
}

inline void persist_repl_history(const std::filesystem::path& storage_dir,
                                 ReplInputHistory history,
                                 std::size_t max_entries = kMaxPersistedReplHistoryEntries) {
    if (storage_dir.empty()) {
        return;
    }

    std::filesystem::create_directories(storage_dir);
    trim_repl_history_entries(history, max_entries);
    clear_repl_history_navigation(history);

    std::ofstream output(repl_history_path(storage_dir), std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open REPL history file for writing: "
                                 + repl_history_path(storage_dir).string());
    }

    for (const auto& entry : history.entries) {
        if (entry.empty()) {
            continue;
        }
        output << entry << '\n';
    }

    if (!output) {
        throw std::runtime_error("failed to write REPL history file: "
                                 + repl_history_path(storage_dir).string());
    }
}

inline std::string normalize_approval_policy_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        if (ch == '_') {
            return '-';
        }
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

inline std::string trim_copy(std::string text) {
    const auto not_space = [](unsigned char ch) {
        return !std::isspace(ch);
    };

    text.erase(text.begin(),
               std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(),
               text.end());
    return text;
}

inline std::string format_project_prompt(const std::string& project_id) {
    return "[" + (project_id.empty() ? std::string{"project"} : project_id) + "> ";
}

inline std::string humanize_status_label_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        if (ch == '_' || ch == '-') {
            return ' ';
        }
        return static_cast<char>(std::tolower(ch));
    });
    return trim_copy(std::move(text));
}

inline std::string collapse_whitespace_copy(std::string text) {
    std::string output;
    output.reserve(text.size());

    bool previous_was_space = false;
    for (unsigned char ch : text) {
        if (std::isspace(ch)) {
            if (!previous_was_space) {
                output.push_back(' ');
                previous_was_space = true;
            }
            continue;
        }
        output.push_back(static_cast<char>(ch));
        previous_was_space = false;
    }
    return trim_copy(std::move(output));
}

inline std::string truncate_preview_copy(std::string text,
                                         std::size_t max_chars = 48) {
    text = collapse_whitespace_copy(std::move(text));
    if (text.size() <= max_chars) {
        return text;
    }
    if (max_chars <= 3) {
        return text.substr(0, max_chars);
    }
    return text.substr(0, max_chars - 3) + "...";
}

inline std::string shorten_path_copy(std::string path_text,
                                     std::size_t max_chars = 48) {
    std::replace(path_text.begin(), path_text.end(), '\\', '/');
    if (path_text.size() <= max_chars) {
        return path_text;
    }

    std::vector<std::string> segments;
    std::stringstream stream(path_text);
    std::string segment;
    while (std::getline(stream, segment, '/')) {
        if (!segment.empty()) {
            segments.push_back(segment);
        }
    }

    if (segments.empty()) {
        return truncate_preview_copy(std::move(path_text), max_chars);
    }

    const std::size_t keep_count = std::min<std::size_t>(3, segments.size());
    std::string shortened = segments.size() > keep_count ? ".../" : "";
    for (std::size_t index = segments.size() - keep_count; index < segments.size(); ++index) {
        if (!shortened.empty() && shortened.back() != '/') {
            shortened += '/';
        }
        shortened += segments[index];
    }
    return truncate_preview_copy(std::move(shortened), max_chars);
}

inline std::optional<int> parse_json_int(const nlohmann::json& value) {
    if (value.is_number_integer()) {
        return value.get<int>();
    }
    if (value.is_string()) {
        try {
            return std::stoi(value.get<std::string>());
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

inline std::string preview_json_value(const nlohmann::json& value,
                                      std::size_t max_chars = 48) {
    if (value.is_string()) {
        return truncate_preview_copy(value.get<std::string>(), max_chars);
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer() || value.is_number_unsigned() || value.is_number_float()) {
        return value.dump();
    }
    if (value.is_array()) {
        if (value.empty()) {
            return "[]";
        }
        if (value.size() == 1) {
            return preview_json_value(value.front(), max_chars);
        }
        return "[" + std::to_string(value.size()) + " items]";
    }
    if (value.is_object()) {
        return "{" + std::to_string(value.size()) + " keys}";
    }
    return truncate_preview_copy(value.dump(), max_chars);
}

inline std::string activity_indicator_text() {
    return "[working]";
}

inline std::string activity_tick_text() {
    return "[working]";
}

inline std::string tool_indicator_text(const std::string& tool_name,
                                       const nlohmann::json& input = nlohmann::json::object()) {
    const std::string label = humanize_status_label_copy(tool_name);
    if (!input.is_object() || input.empty()) {
        return label;
    }

    if (tool_name == "read_file") {
        std::string preview = shorten_path_copy(input.value("path", std::string{"?"}));
        const auto start_line = input.contains("start_line") ? parse_json_int(input.at("start_line")) : std::nullopt;
        const auto end_line = input.contains("end_line") ? parse_json_int(input.at("end_line")) : std::nullopt;
        if (start_line.has_value() && end_line.has_value()) {
            preview += ":" + std::to_string(*start_line) + "-" + std::to_string(*end_line);
        } else if (start_line.has_value()) {
            preview += ":" + std::to_string(*start_line);
        }
        return label + " " + preview;
    }

    if (tool_name == "list_dir"
        || tool_name == "write_file"
        || tool_name == "edit_file"
        || tool_name == "delete_file") {
        return label + " " + shorten_path_copy(input.value("path", std::string{"."}));
    }

    if (tool_name == "glob") {
        std::string preview = truncate_preview_copy(input.value("pattern", std::string{}));
        const std::string path = input.value("path", std::string{"."});
        if (!path.empty() && path != ".") {
            preview += " in " + shorten_path_copy(path);
        }
        return preview.empty() ? label : label + " " + preview;
    }

    if (tool_name == "grep") {
        std::string preview = truncate_preview_copy(input.value("pattern", std::string{}));
        const std::string path = input.value("path", std::string{"."});
        if (!path.empty() && path != ".") {
            preview += " in " + shorten_path_copy(path);
        }
        const std::string glob = input.value("glob", std::string{});
        if (!glob.empty()) {
            preview += " [" + truncate_preview_copy(glob, 24) + "]";
        }
        return preview.empty() ? label : label + " " + preview;
    }

    if (tool_name == "bash") {
        std::string preview = truncate_preview_copy(input.value("command", std::string{}));
        const std::string working_dir = input.value("working_dir", std::string{});
        if (!working_dir.empty()) {
            preview += " @" + shorten_path_copy(working_dir, 24);
        }
        return preview.empty() ? label : label + " " + preview;
    }

    if (tool_name == "web_search") {
        const std::string query = input.value("query", std::string{});
        return query.empty() ? label : label + " " + truncate_preview_copy(query);
    }

    if (tool_name == "web_fetch") {
        if (input.contains("url")) {
            return label + " " + truncate_preview_copy(preview_json_value(input.at("url")));
        }
        if (input.contains("urls")) {
            return label + " " + truncate_preview_copy(preview_json_value(input.at("urls")));
        }
        return label;
    }

    for (const char* key : {"path", "pattern", "query", "command", "url", "glob"}) {
        if (!input.contains(key) || input.at(key).is_null()) {
            continue;
        }
        std::string preview = preview_json_value(input.at(key));
        if (std::string_view(key) == "path") {
            preview = shorten_path_copy(preview);
        }
        if (!preview.empty()) {
            return label + " " + preview;
        }
    }

    return label;
}

inline std::string agent_indicator_text(const std::string& profile) {
    const std::string label = humanize_status_label_copy(
        profile.empty() ? std::string{"worker"} : profile);
    return label + " agent";
}

inline std::string agent_completion_indicator_text(bool success) {
    return success ? "agent done" : "agent failed";
}

inline std::string run_started_indicator_text() {
    return "[run]";
}

inline std::string run_paused_indicator_text() {
    return "[paused]";
}

inline std::string format_activity_status(const std::string& detail = {}) {
    if (detail.empty()) {
        return activity_indicator_text();
    }
    return activity_indicator_text() + " " + detail;
}

inline std::string normalize_clarification_text_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        if (ch == '_' || ch == '-') {
            return ' ';
        }
        return static_cast<char>(std::tolower(ch));
    });
    return trim_copy(std::move(text));
}

inline bool is_delegate_phrase(const std::string& text) {
    const std::string normalized = normalize_clarification_text_copy(text);
    return normalized == "you decide for me"
        || normalized == "decide for me"
        || normalized == "your call"
        || normalized == "use the recommended default"
        || normalized == "use recommended defaults"
        || normalized == "delegate";
}

inline std::optional<PendingClarificationState> parse_planner_clarification_state(
    const std::string& tool_name,
    const std::string& content) {
    const auto pending = planner::parse_pending_clarification(tool_name, content);
    if (!pending.has_value()) {
        return std::nullopt;
    }

    PendingClarificationState state;
    state.tool_name = pending->tool_name;
    state.clarification_mode = pending->clarification_mode;
    state.clarification_message = pending->clarification_message;

    std::unordered_set<std::string> pending_ids(
        pending->pending_question_ids.begin(),
        pending->pending_question_ids.end());

    for (const auto& item : pending->questions) {
        ClarificationQuestion question;
        question.id = item.id;
        question.stage = item.stage;
        question.severity = item.severity;
        question.question = item.question;
        question.recommended_default = item.recommended_default;
        question.pending = pending_ids.empty() || pending_ids.contains(question.id);
        state.questions.push_back(std::move(question));
    }

    if (state.questions.empty()) {
        return std::nullopt;
    }
    return state;
}

inline std::optional<PendingClarificationState> pending_clarification_state_from_run(
    const RunResult& result) {
    if (!result.pending_clarification.has_value()) {
        return std::nullopt;
    }

    PendingClarificationState state;
    state.run_id = result.run_id;
    state.tool_name = result.pending_clarification->tool_name;
    state.clarification_mode = result.pending_clarification->clarification_mode;
    state.clarification_message = result.pending_clarification->clarification_message;

    std::unordered_set<std::string> pending_ids(
        result.pending_clarification->pending_question_ids.begin(),
        result.pending_clarification->pending_question_ids.end());
    for (const auto& item : result.pending_clarification->questions) {
        ClarificationQuestion question;
        question.id = item.id;
        question.stage = item.stage;
        question.severity = item.severity;
        question.question = item.question;
        question.recommended_default = item.recommended_default;
        question.pending = pending_ids.empty() || pending_ids.contains(question.id);
        state.questions.push_back(std::move(question));
    }
    return state;
}

inline const ClarificationQuestion* find_question(
    const PendingClarificationState& state,
    const std::string& question_id) {
    const auto it = std::find_if(
        state.questions.begin(),
        state.questions.end(),
        [&](const ClarificationQuestion& question) {
            return question.id == question_id;
        });
    return it == state.questions.end() ? nullptr : &(*it);
}

inline std::string format_clarifications(const PendingClarificationState& state) {
    std::ostringstream stream;
    stream << "pending clarifications for " << state.tool_name;
    if (!state.run_id.empty()) {
        stream << " (run " << state.run_id << ")";
    }
    if (!state.clarification_mode.empty()) {
        stream << " [mode=" << state.clarification_mode << "]";
    }
    stream << '\n';
    if (!state.clarification_message.empty()) {
        stream << state.clarification_message << '\n';
    }

    for (const auto& question : state.questions) {
        stream << "- " << question.id;
        if (!question.pending) {
            stream << " [resolved]";
        }
        if (!question.stage.empty() || !question.severity.empty()) {
            stream << " (";
            bool needs_space = false;
            if (!question.stage.empty()) {
                stream << question.stage;
                needs_space = true;
            }
            if (!question.severity.empty()) {
                if (needs_space) {
                    stream << ' ';
                }
                stream << question.severity;
            }
            stream << ')';
        }
        stream << ": " << question.question << '\n';
        if (!question.recommended_default.empty()) {
            stream << "  default: " << question.recommended_default << '\n';
        }
    }
    return stream.str();
}

inline std::string format_staged_answers(const PendingClarificationState& state) {
    std::ostringstream stream;
    stream << "staged clarification answers";
    if (!state.run_id.empty()) {
        stream << " for run " << state.run_id;
    }
    stream << '\n';

    bool wrote_answer = false;
    for (const auto& question : state.questions) {
        const auto staged = state.staged_answers.find(question.id);
        if (staged == state.staged_answers.end()) {
            continue;
        }
        wrote_answer = true;
        stream << "- " << question.id << ": " << staged->second << '\n';
    }

    if (!wrote_answer) {
        stream << "(none)\n";
    }
    if (state.delegate_unanswered) {
        stream << "- unresolved questions: use recommended defaults\n";
    }
    return stream.str();
}

inline std::string build_clarification_resume_input(const PendingClarificationState& state) {
    if (state.staged_answers.empty() && !state.delegate_unanswered) {
        return {};
    }

    std::ostringstream stream;
    stream << "Continue the pending planner clarification flow for "
           << state.tool_name << ".\n";
    if (!state.staged_answers.empty()) {
        stream << "Use these answers exactly:\n";
        for (const auto& question : state.questions) {
            const auto staged = state.staged_answers.find(question.id);
            if (staged == state.staged_answers.end()) {
                continue;
            }
            stream << question.id << ": " << staged->second << '\n';
        }
    }
    if (state.delegate_unanswered) {
        stream << "For any remaining unanswered clarification questions, you decide for me.\n";
    }
    stream << "Then continue the same planner workflow.";
    return stream.str();
}

inline CliApprovalPolicy parse_approval_policy_value(const std::optional<std::string>& value,
                                                     CliApprovalPolicy default_policy) {
    if (!value.has_value() || value->empty()) {
        return default_policy;
    }

    const std::string normalized = normalize_approval_policy_copy(*value);
    if (normalized == "prompt") {
        return CliApprovalPolicy::Prompt;
    }
    if (normalized == "pause") {
        return CliApprovalPolicy::Pause;
    }
    if (normalized == "auto-read-only" || normalized == "auto-read-only-prompt") {
        return CliApprovalPolicy::AutoReadOnlyPrompt;
    }
    if (normalized == "auto-read-only-pause") {
        return CliApprovalPolicy::AutoReadOnlyPause;
    }

    throw std::invalid_argument(
        "unknown approval policy: " + *value
        + " (expected prompt, pause, auto-read-only, or auto-read-only-pause)");
}

inline bool should_auto_approve(const ToolSummary& tool) {
    return tool.read_only && !tool.destructive && !tool.sub_agent;
}

inline bool is_auto_read_only_policy(CliApprovalPolicy policy) {
    return policy == CliApprovalPolicy::AutoReadOnlyPrompt
        || policy == CliApprovalPolicy::AutoReadOnlyPause;
}

inline std::unordered_set<std::string> auto_approved_tool_names(
    const std::vector<ToolSummary>& tools,
    CliApprovalPolicy policy) {
    std::unordered_set<std::string> allowed;
    std::unordered_set<std::string> available_names;
    for (const auto& tool : tools) {
        available_names.insert(tool.name);
        if (should_auto_approve(tool)) {
            allowed.insert(tool.name);
        }
    }

    if (is_auto_read_only_policy(policy)) {
        for (const std::string& tool_name : {
                 std::string{"planner_validate_spec"},
                 std::string{"planner_validate_plan"},
                 std::string{"planner_repair_plan"},
                 std::string{"planner_build_plan"},
                 std::string{"planner_build_from_idea"},
             }) {
            if (available_names.count(tool_name) > 0) {
                allowed.insert(tool_name);
            }
        }
    }

    return allowed;
}

}  // namespace omni::engine::cli::detail