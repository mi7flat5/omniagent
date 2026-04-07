#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace omni::engine {

struct TextContent {
    std::string text;
};

struct ThinkingContent {
    std::string text;
};

struct ToolUseContent {
    std::string    id;
    std::string    name;
    nlohmann::json input;
};

struct ImageContent {
    std::string media_type;
    std::string data;
};

struct ContentBlock {
    std::variant<TextContent, ThinkingContent, ToolUseContent, ImageContent> data;
};

struct ToolResult {
    std::string    tool_use_id;
    std::string    content;
    bool           is_error = false;
    nlohmann::json metadata;
};

enum class Role { User, Assistant, System, ToolResult };

struct Message {
    Role                      role;
    std::vector<ContentBlock> content;
    std::string               id;
    std::vector<ToolResult>   tool_results;

    nlohmann::json to_json() const;
    static Message from_json(const nlohmann::json& j);
};

struct Usage {
    int64_t input_tokens      = 0;
    int64_t output_tokens     = 0;
    int64_t cache_read_tokens = 0;

    Usage operator+(const Usage& other) const {
        return {input_tokens + other.input_tokens,
                output_tokens + other.output_tokens,
                cache_read_tokens + other.cache_read_tokens};
    }
    Usage& operator+=(const Usage& other) {
        input_tokens      += other.input_tokens;
        output_tokens     += other.output_tokens;
        cache_read_tokens += other.cache_read_tokens;
        return *this;
    }
};

struct ModelCapabilities {
    int  max_context_tokens     = 0;
    int  max_output_tokens      = 0;
    bool supports_tool_use      = false;
    bool supports_thinking      = false;
    bool supports_images        = false;
    bool supports_cache_control = false;
};

}  // namespace omni::engine
