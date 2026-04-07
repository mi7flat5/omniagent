#pragma once

#include <omni/approval.h>
#include <omni/observer.h>
#include <omni/run.h>
#include <omni/types.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace omni::engine {

struct SessionOptions {
    std::string profile = "explore";
    std::optional<std::string> session_id;
    std::optional<std::filesystem::path> working_dir_override;
};

struct SessionSummary {
    std::string session_id;
    std::string project_id;
    std::size_t message_count = 0;
    std::string active_profile;
    std::filesystem::path working_dir;
    std::string updated_at;
};

struct SessionSnapshot {
    std::string session_id;
    std::string project_id;
    std::string active_profile;
    std::filesystem::path working_dir;
    std::vector<Message> messages;
    Usage usage;
    std::optional<std::string> active_run_id;
};

struct ToolSummary {
    std::string name;
    bool read_only = false;
    bool destructive = false;
    bool shell = false;
    bool network = false;
    bool mcp = false;
    bool sub_agent = false;
};

class ProjectSession {
public:
    ~ProjectSession();
    ProjectSession(ProjectSession&&) noexcept;
    ProjectSession& operator=(ProjectSession&&) noexcept;
    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;

    const std::string& session_id() const;
    const std::string& project_id() const;
    const std::string& active_profile() const;
    std::unique_ptr<ProjectRun> submit_turn(const std::string& input,
                                            RunObserver& observer,
                                            ApprovalDelegate& approvals);
    void set_profile(const std::string& profile_name);
    SessionSnapshot snapshot() const;
    std::vector<ToolSummary> tools() const;
    void reset();
    std::size_t rewind_messages(std::size_t count = 1);
    void close();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;

    explicit ProjectSession(std::shared_ptr<Impl> impl);

    friend class ProjectEngineHost;
};

}  // namespace omni::engine