#pragma once

#include <omni/project.h>
#include <omni/project_session.h>
#include <omni/run.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace omni::engine {

struct HostStatus {
    std::string project_id;
    std::filesystem::path workspace_root;
    std::filesystem::path working_dir;
    bool running = false;
    std::size_t open_sessions = 0;
    std::size_t active_runs = 0;
    std::size_t persisted_sessions = 0;
    std::vector<std::string> connected_mcp_servers;
};

class ProjectEngineHost {
public:
    static std::unique_ptr<ProjectEngineHost> create(ProjectRuntimeConfig config);

    ~ProjectEngineHost();
    ProjectEngineHost(ProjectEngineHost&&) noexcept;
    ProjectEngineHost& operator=(ProjectEngineHost&&) noexcept;
    ProjectEngineHost(const ProjectEngineHost&) = delete;
    ProjectEngineHost& operator=(const ProjectEngineHost&) = delete;

    const std::string& project_id() const;
    const WorkspaceContext& workspace() const;
    std::unique_ptr<ProjectSession> open_session(SessionOptions options = {});
    std::unique_ptr<ProjectSession> resume_session(const std::string& session_id);
    std::vector<SessionSummary> list_sessions() const;
    bool close_session(const std::string& session_id);
    std::vector<RunSummary> list_runs() const;
    std::optional<RunResult> get_run(const std::string& run_id) const;
    bool cancel_run(const std::string& run_id);
    bool stop_run(const std::string& run_id);
    bool resume_run(const std::string& run_id, const std::string& resume_input = "approve");
    bool delete_run(const std::string& run_id);
    void reload(ProjectRuntimeConfig config);
    HostStatus status() const;
    void shutdown();
    void register_tool(std::unique_ptr<Tool> tool);
    bool connect_mcp(const MCPServerConfig& config);
    void disconnect_mcp(const std::string& name);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;

    explicit ProjectEngineHost(std::shared_ptr<Impl> impl);
};

}  // namespace omni::engine