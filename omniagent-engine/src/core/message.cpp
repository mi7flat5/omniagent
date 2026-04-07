#include "message.h"

#include <stdexcept>

namespace omni::engine {

// ---------------------------------------------------------------------------
// Role helpers
// ---------------------------------------------------------------------------

std::string role_to_string(Role role) {
    switch (role) {
        case Role::User:       return "user";
        case Role::Assistant:  return "assistant";
        case Role::System:     return "system";
        case Role::ToolResult: return "tool";
    }
    throw std::invalid_argument("Unknown Role enum value");
}

Role role_from_string(const std::string& s) {
    if (s == "user")       return Role::User;
    if (s == "assistant")  return Role::Assistant;
    if (s == "system")     return Role::System;
    if (s == "tool")       return Role::ToolResult;
    throw std::invalid_argument("Unknown role string: " + s);
}

// ---------------------------------------------------------------------------
// ContentBlock serialization
// ---------------------------------------------------------------------------

nlohmann::json content_block_to_json(const ContentBlock& block) {
    return std::visit([](const auto& v) -> nlohmann::json {
        using T = std::decay_t<decltype(v)>;

        if constexpr (std::is_same_v<T, TextContent>) {
            return {{kType, "text"}, {kText, v.text}};
        } else if constexpr (std::is_same_v<T, ThinkingContent>) {
            return {{kType, "thinking"}, {kText, v.text}};
        } else if constexpr (std::is_same_v<T, ToolUseContent>) {
            return {{kType, "tool_use"}, {kId, v.id}, {kName, v.name}, {kInput, v.input}};
        } else if constexpr (std::is_same_v<T, ImageContent>) {
            return {{kType, "image"},
                    {"source", {{"type", "base64"},
                                {"media_type", v.media_type},
                                {"data", v.data}}}};
        }
    }, block.data);
}

ContentBlock content_block_from_json(const nlohmann::json& j) {
    const std::string type = j.at(kType).get<std::string>();

    if (type == "text") {
        return ContentBlock{TextContent{j.at(kText).get<std::string>()}};
    }
    if (type == "thinking") {
        return ContentBlock{ThinkingContent{j.at(kText).get<std::string>()}};
    }
    if (type == "tool_use") {
        ToolUseContent tu;
        tu.id    = j.at(kId).get<std::string>();
        tu.name  = j.at(kName).get<std::string>();
        tu.input = j.at(kInput);
        return ContentBlock{std::move(tu)};
    }
    if (type == "image") {
        ImageContent img;
        const auto& src = j.at("source");
        img.media_type  = src.at("media_type").get<std::string>();
        img.data        = src.at("data").get<std::string>();
        return ContentBlock{std::move(img)};
    }
    throw std::invalid_argument("Unknown content block type: " + type);
}

// ---------------------------------------------------------------------------
// Message serialization
// ---------------------------------------------------------------------------

nlohmann::json Message::to_json() const {
    nlohmann::json j;
    j[kRole] = role_to_string(role);

    if (!id.empty()) {
        j[kId] = id;
    }

    // ToolResult role: encode tool_results array instead of content blocks
    if (role == Role::ToolResult) {
        nlohmann::json results = nlohmann::json::array();
        for (const auto& tr : tool_results) {
            nlohmann::json r;
            r[kToolUseId] = tr.tool_use_id;
            r[kContent]   = tr.content;
            r[kIsError]   = tr.is_error;
            if (!tr.metadata.is_null()) {
                r["metadata"] = tr.metadata;
            }
            results.push_back(std::move(r));
        }
        j["tool_results"] = std::move(results);
    } else {
        nlohmann::json blocks = nlohmann::json::array();
        for (const auto& cb : content) {
            blocks.push_back(content_block_to_json(cb));
        }
        j[kContent] = std::move(blocks);
    }

    return j;
}

Message Message::from_json(const nlohmann::json& j) {
    Message msg;
    msg.role = role_from_string(j.at(kRole).get<std::string>());

    if (j.contains(kId) && j[kId].is_string()) {
        msg.id = j[kId].get<std::string>();
    }

    if (msg.role == Role::ToolResult) {
        if (j.contains("tool_results") && j["tool_results"].is_array()) {
            for (const auto& r : j["tool_results"]) {
                ToolResult tr;
                tr.tool_use_id = r.at(kToolUseId).get<std::string>();
                tr.content     = r.at(kContent).get<std::string>();
                tr.is_error    = r.value(kIsError, false);
                if (r.contains("metadata")) {
                    tr.metadata = r["metadata"];
                }
                msg.tool_results.push_back(std::move(tr));
            }
        }
    } else {
        if (j.contains(kContent) && j[kContent].is_array()) {
            for (const auto& block : j[kContent]) {
                msg.content.push_back(content_block_from_json(block));
            }
        }
    }

    return msg;
}

}  // namespace omni::engine
