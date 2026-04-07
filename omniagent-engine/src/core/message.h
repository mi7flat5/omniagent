#pragma once

#include <omni/types.h>

namespace omni::engine {

constexpr const char* kRole      = "role";
constexpr const char* kContent   = "content";
constexpr const char* kType      = "type";
constexpr const char* kText      = "text";
constexpr const char* kId        = "id";
constexpr const char* kName      = "name";
constexpr const char* kInput     = "input";
constexpr const char* kToolUseId = "tool_use_id";
constexpr const char* kIsError   = "is_error";

std::string    role_to_string(Role role);
Role           role_from_string(const std::string& s);
nlohmann::json content_block_to_json(const ContentBlock& block);
ContentBlock   content_block_from_json(const nlohmann::json& j);

}  // namespace omni::engine
