#pragma once

#include <omni/fwd.h>
#include <omni/mcp.h>
#include <omni/permission.h>
#include <omni/provider.h>
#include <omni/tool.h>
#include <omni/types.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace omni::engine {

struct Config {
    std::string system_prompt;
    int max_turns        = 50;
    int preserve_tail    = 4;
    int max_result_chars = 50000;
    int compact_max_result_chars = 500;
    std::optional<double> temperature;
    std::optional<double> top_p;
    std::optional<int> top_k;
    std::optional<double> min_p;
    std::optional<double> presence_penalty;
    std::optional<double> frequency_penalty;
    int initial_max_tokens = 8192;

    // Auto-compact settings
    float compact_soft_limit_pct = 0.75f;
    int   compact_preserve_tail  = 6;
    bool  enable_auto_compact    = true;

    /// Optional separate provider for context compaction.
    /// If null, uses the main provider.
    LLMProvider* compaction_provider = nullptr;

    // Permission settings
    PermissionMode              permission_mode  = PermissionMode::Default;
    std::vector<PermissionRule> permission_rules;  // programmatic rules (highest priority)

    /// Session persistence: directory for JSONL transcript storage.
    /// Empty path = no persistence.
    std::filesystem::path session_storage_dir;
};

class Engine {
public:
    Engine(Config config, std::unique_ptr<LLMProvider> provider);
    ~Engine();
    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    std::unique_ptr<Session> create_session(
        EventObserver& observer,
        PermissionDelegate& delegate,
        std::optional<std::string> session_id = std::nullopt);
    void register_tool(std::unique_ptr<Tool> tool);

    /// Connect to an MCP server and register its tools.
    /// Tools are wrapped and appear under the name "mcp__{server}__{tool}".
    /// Returns true if the server connected successfully.
    bool connect_mcp_server(const MCPServerConfig& config);

    /// Disconnect an MCP server and unregister its tools.
    void disconnect_mcp_server(const std::string& name);

    /// Returns the names of all registered engine-level tools.
    std::vector<std::string> tool_names() const;

    /// Look up an engine-level tool by name. Returns null if not found.
    Tool* find_tool(const std::string& name) const;

    LLMProvider& provider();
    const LLMProvider& provider() const;
    const Config& config() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace omni::engine
