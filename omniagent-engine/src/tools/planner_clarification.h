#pragma once

#include <omni/run.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace omni::engine::planner {

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

inline std::optional<nlohmann::json> parse_clarification_payload(const std::string& tool_name,
                                                                 const std::string& content) {
    if (tool_name.rfind("planner_", 0) != 0) {
        return std::nullopt;
    }
    if (content.find("STATUS: CLARIFICATION_REQUIRED") == std::string::npos) {
        return std::nullopt;
    }

    constexpr std::string_view marker = "raw_json:\n";
    const auto marker_pos = content.find(marker.data());
    if (marker_pos == std::string::npos) {
        return std::nullopt;
    }

    const std::string raw_json = trim_copy(content.substr(marker_pos + marker.size()));
    if (raw_json.empty()) {
        return std::nullopt;
    }

    try {
        return nlohmann::json::parse(raw_json);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

inline std::optional<PendingClarification> parse_pending_clarification(const std::string& tool_name,
                                                                       const std::string& content) {
    const auto payload = parse_clarification_payload(tool_name, content);
    if (!payload.has_value()) {
        return std::nullopt;
    }

    const auto& root = *payload;
    const auto clarification = root.contains("clarification") && root["clarification"].is_object()
        ? root["clarification"]
        : root;
    if (!clarification.value("clarification_required",
                             root.value("clarification_required", false))) {
        return std::nullopt;
    }

    PendingClarification pending;
    pending.tool_name = tool_name;
    pending.clarification_mode = clarification.value("clarification_mode", std::string{});
    pending.clarification_message = clarification.value("clarification_message", std::string{});
    pending.raw_payload = root;

    if (clarification.contains("pending_clarification_ids")
        && clarification["pending_clarification_ids"].is_array()) {
        for (const auto& item : clarification["pending_clarification_ids"]) {
            if (item.is_string()) {
                pending.pending_question_ids.push_back(item.get<std::string>());
            }
        }
    }

    const auto questions = clarification.contains("clarifications") && clarification["clarifications"].is_array()
        ? clarification["clarifications"]
        : nlohmann::json::array();
    for (const auto& item : questions) {
        if (!item.is_object()) {
            continue;
        }

        ClarificationQuestion question;
        question.id = item.value("id", std::string{});
        if (question.id.empty()) {
            continue;
        }
        question.stage = item.value("stage", std::string{});
        question.severity = item.value("severity", std::string{});
        question.quote = item.value("quote", std::string{});
        question.question = item.value("question", std::string{});
        question.recommended_default = item.value("recommended_default", std::string{});
        question.answer_type = item.value("answer_type", std::string{});
        question.options = item.value("options", nlohmann::json::array());
        pending.questions.push_back(std::move(question));
    }

    if (pending.questions.empty()) {
        return std::nullopt;
    }
    return pending;
}

}  // namespace omni::engine::planner