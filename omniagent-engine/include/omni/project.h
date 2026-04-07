#pragma once

#include <omni/engine.h>
#include <omni/mcp.h>
#include <omni/profile.h>
#include <omni/tool.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace omni::engine {

struct WorkspaceContext {
    std::string project_id;
    std::filesystem::path workspace_root;
    std::filesystem::path working_dir;
    std::optional<std::string> user_id;
    bool allow_workspace_escape = false;
};

using ProviderFactory = std::function<std::unique_ptr<LLMProvider>()>;

struct ProjectRuntimeConfig {
    WorkspaceContext workspace;
    Config engine;
    ProviderFactory provider_factory;
    std::vector<MCPServerConfig> mcp_servers;
    std::vector<std::unique_ptr<Tool>> project_tools;
    std::vector<AgentProfileManifest> profiles;
};

bool is_within_workspace(const WorkspaceContext& workspace,
                         const std::filesystem::path& candidate);

}  // namespace omni::engine