#pragma once

#include <omni/tool.h>
#include "mcp_client.h"

namespace omni::engine {

/// Wraps a single MCP server tool as a native engine Tool.
///
/// The tool name follows the convention "mcp__{server}__{tool}" so that
/// callers can identify the origin of the tool and MCP namespacing doesn't
/// collide with native engine tools.
///
/// Lifetime: MCPClient must outlive all MCPToolWrapper instances that
/// reference it.  The Engine::Impl map keeps both alive together.
class MCPToolWrapper : public Tool {
public:
    MCPToolWrapper(MCPClient& client, MCPToolInfo info);

    std::string    name()         const override;
    std::string    description()  const override;
    nlohmann::json input_schema() const override;
    bool           is_read_only()   const override;
    bool           is_destructive() const override;
    bool           is_mcp() const override { return true; }
    ToolCallResult call(const nlohmann::json& args) override;

private:
    MCPClient&  client_;
    MCPToolInfo info_;
    std::string wrapped_name_;  // "mcp__{server}__{tool}" — cached on construction
};

}  // namespace omni::engine
