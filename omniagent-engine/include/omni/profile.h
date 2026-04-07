#pragma once

#include <omni/permission.h>

#include <string>
#include <vector>

namespace omni::engine {

struct ToolCapabilityPolicy {
    bool allow_read_only = true;
    bool allow_write = false;
    bool allow_destructive = false;
    bool allow_shell = false;
    bool allow_network = false;
    bool allow_mcp = false;
    bool allow_sub_agents = false;
    std::vector<std::string> explicit_allow;
    std::vector<std::string> explicit_deny;
};

struct AgentProfileManifest {
    std::string name;
    std::string system_prompt;
    ToolCapabilityPolicy tool_policy;
    PermissionMode default_permission_mode = PermissionMode::Default;
    bool sub_agents_allowed = false;
    int max_parallel_tools = 10;
};

}  // namespace omni::engine