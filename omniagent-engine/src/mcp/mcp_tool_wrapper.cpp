#include "mcp_tool_wrapper.h"

#include <sstream>

namespace omni::engine {

MCPToolWrapper::MCPToolWrapper(MCPClient& client, MCPToolInfo info)
    : client_(client)
    , info_(std::move(info))
{
    // Build the canonical wrapped name once.
    wrapped_name_ = "mcp__" + client_.server_name() + "__" + info_.name;
}

std::string MCPToolWrapper::name() const {
    return wrapped_name_;
}

std::string MCPToolWrapper::description() const {
    return info_.description;
}

nlohmann::json MCPToolWrapper::input_schema() const {
    return info_.input_schema;
}

bool MCPToolWrapper::is_read_only() const {
    return info_.read_only_hint;
}

bool MCPToolWrapper::is_destructive() const {
    return info_.destructive_hint;
}

ToolCallResult MCPToolWrapper::call(const nlohmann::json& args) {
    nlohmann::json result = client_.call_tool(info_.name, args);

    // MCP tool call result format:
    // { "content": [ { "type": "text", "text": "..." }, ... ], "isError": false }
    bool is_error = result.value("isError", false);

    std::string combined;
    if (result.contains("content") && result["content"].is_array()) {
        for (const auto& part : result["content"]) {
            const std::string type = part.value("type", "");
            if (type == "text") {
                if (!combined.empty()) combined += "\n";
                combined += part.value("text", std::string{});
            }
        }
    }

    if (combined.empty() && is_error) {
        combined = "MCP tool call failed";
    }

    return {combined, is_error};
}

}  // namespace omni::engine
