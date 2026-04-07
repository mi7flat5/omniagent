#pragma once

#include <omni/types.h>

#include <chrono>
#include <nlohmann/json.hpp>
#include <memory>
#include <optional>
#include <string>

namespace omni::engine {

enum class RunStatus {
    Running,
    Paused,
    Completed,
    Stopped,
    Cancelled,
    Failed,
};

struct PendingApproval {
    std::string tool_name;
    nlohmann::json args;
    std::string description;
    std::chrono::system_clock::time_point requested_at{};
};

struct RunSummary {
    std::string run_id;
    std::string session_id;
    std::string project_id;
    std::string profile;
    RunStatus status = RunStatus::Running;
    std::chrono::system_clock::time_point started_at{};
    std::chrono::system_clock::time_point finished_at{};
};

struct RunResult {
    std::string run_id;
    std::string session_id;
    std::string project_id;
    std::string profile;
    RunStatus status = RunStatus::Running;
    std::string input;
    std::string output;
    Usage usage;
    std::optional<std::string> error;
    std::optional<std::string> pause_reason;
    std::optional<PendingApproval> pending_approval;
    std::chrono::system_clock::time_point started_at{};
    std::chrono::system_clock::time_point finished_at{};
};

class ProjectRun {
public:
    ~ProjectRun();
    ProjectRun(ProjectRun&&) noexcept;
    ProjectRun& operator=(ProjectRun&&) noexcept;
    ProjectRun(const ProjectRun&) = delete;
    ProjectRun& operator=(const ProjectRun&) = delete;

    const std::string& run_id() const;
    RunStatus status() const;
    void cancel();
    void stop();
    void resume(const std::string& resume_input = "");
    void wait();
    RunResult result() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;

    explicit ProjectRun(std::shared_ptr<Impl> impl);

    friend class ProjectSession;
};

}  // namespace omni::engine