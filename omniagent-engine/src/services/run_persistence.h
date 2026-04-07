#pragma once

#include <omni/run.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace omni::engine {

class RunPersistence {
public:
    explicit RunPersistence(std::filesystem::path storage_dir);

    void save(const RunResult& result);
    std::optional<RunResult> load(const std::string& run_id) const;
    std::vector<RunSummary> list(const std::string& project_id, int limit = 50) const;
    bool remove(const std::string& run_id);

    void save_pending(const RunResult& result);
    bool clear_pending(const std::string& run_id);

private:
    std::filesystem::path storage_dir_;

    std::filesystem::path runs_dir() const;
    std::filesystem::path pending_dir() const;
    std::filesystem::path run_path_for(const std::string& run_id) const;
    std::filesystem::path pending_path_for(const std::string& run_id) const;
};

}  // namespace omni::engine