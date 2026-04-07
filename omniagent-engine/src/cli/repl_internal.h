#pragma once

#include <omni/project_session.h>

#include <algorithm>
#include <cctype>
#include <optional>
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

inline std::string normalize_approval_policy_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        if (ch == '_') {
            return '-';
        }
        return static_cast<char>(std::tolower(ch));
    });
    return text;
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