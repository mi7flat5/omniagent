#include "run_persistence.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace omni::engine {

namespace {

int64_t to_epoch_us(const std::chrono::system_clock::time_point& value) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        value.time_since_epoch()).count();
}

std::chrono::system_clock::time_point from_epoch_us(int64_t value) {
    return std::chrono::system_clock::time_point(std::chrono::microseconds(value));
}

std::string run_status_to_string(RunStatus status) {
    switch (status) {
        case RunStatus::Running:
            return "running";
        case RunStatus::Paused:
            return "paused";
        case RunStatus::Completed:
            return "completed";
        case RunStatus::Stopped:
            return "stopped";
        case RunStatus::Cancelled:
            return "cancelled";
        case RunStatus::Failed:
            return "failed";
    }
    return "failed";
}

RunStatus run_status_from_string(const std::string& value) {
    if (value == "running") {
        return RunStatus::Running;
    }
    if (value == "paused") {
        return RunStatus::Paused;
    }
    if (value == "completed") {
        return RunStatus::Completed;
    }
    if (value == "stopped") {
        return RunStatus::Stopped;
    }
    if (value == "cancelled") {
        return RunStatus::Cancelled;
    }
    return RunStatus::Failed;
}

nlohmann::json usage_to_json(const Usage& usage) {
    return {
        {"input_tokens", usage.input_tokens},
        {"output_tokens", usage.output_tokens},
        {"cache_read_tokens", usage.cache_read_tokens},
    };
}

Usage usage_from_json(const nlohmann::json& value) {
    Usage usage;
    usage.input_tokens = value.value("input_tokens", int64_t{0});
    usage.output_tokens = value.value("output_tokens", int64_t{0});
    usage.cache_read_tokens = value.value("cache_read_tokens", int64_t{0});
    return usage;
}

nlohmann::json pending_approval_to_json(const PendingApproval& value) {
    return {
        {"tool_name", value.tool_name},
        {"args", value.args},
        {"description", value.description},
        {"requested_at_us", to_epoch_us(value.requested_at)},
    };
}

PendingApproval pending_approval_from_json(const nlohmann::json& value) {
    PendingApproval pending;
    pending.tool_name = value.value("tool_name", std::string{});
    pending.args = value.value("args", nlohmann::json::object());
    pending.description = value.value("description", std::string{});
    pending.requested_at = from_epoch_us(value.value("requested_at_us", int64_t{0}));
    return pending;
}

nlohmann::json run_result_to_json(const RunResult& result) {
    nlohmann::json value = {
        {"run_id", result.run_id},
        {"session_id", result.session_id},
        {"project_id", result.project_id},
        {"profile", result.profile},
        {"status", run_status_to_string(result.status)},
        {"input", result.input},
        {"output", result.output},
        {"usage", usage_to_json(result.usage)},
        {"started_at_us", to_epoch_us(result.started_at)},
        {"finished_at_us", to_epoch_us(result.finished_at)},
    };

    if (result.error.has_value()) {
        value["error"] = *result.error;
    }
    if (result.pause_reason.has_value()) {
        value["pause_reason"] = *result.pause_reason;
    }
    if (result.pending_approval.has_value()) {
        value["pending_approval"] = pending_approval_to_json(*result.pending_approval);
    }

    return value;
}

RunResult run_result_from_json(const nlohmann::json& value) {
    RunResult result;
    result.run_id = value.value("run_id", std::string{});
    result.session_id = value.value("session_id", std::string{});
    result.project_id = value.value("project_id", std::string{});
    result.profile = value.value("profile", std::string{});
    result.status = run_status_from_string(value.value("status", std::string{"failed"}));
    result.input = value.value("input", std::string{});
    result.output = value.value("output", std::string{});
    if (value.contains("usage")) {
        result.usage = usage_from_json(value["usage"]);
    }
    if (value.contains("error") && value["error"].is_string()) {
        result.error = value["error"].get<std::string>();
    }
    if (value.contains("pause_reason") && value["pause_reason"].is_string()) {
        result.pause_reason = value["pause_reason"].get<std::string>();
    }
    if (value.contains("pending_approval") && value["pending_approval"].is_object()) {
        result.pending_approval = pending_approval_from_json(value["pending_approval"]);
    }
    result.started_at = from_epoch_us(value.value("started_at_us", int64_t{0}));
    result.finished_at = from_epoch_us(value.value("finished_at_us", int64_t{0}));
    return result;
}

RunSummary summary_from_result(const RunResult& result) {
    RunSummary summary;
    summary.run_id = result.run_id;
    summary.session_id = result.session_id;
    summary.project_id = result.project_id;
    summary.profile = result.profile;
    summary.status = result.status;
    summary.started_at = result.started_at;
    summary.finished_at = result.finished_at;
    return summary;
}

}  // namespace

RunPersistence::RunPersistence(std::filesystem::path storage_dir)
    : storage_dir_(std::move(storage_dir)) {}

std::filesystem::path RunPersistence::runs_dir() const {
    return storage_dir_ / "runs";
}

std::filesystem::path RunPersistence::pending_dir() const {
    return storage_dir_ / "pending";
}

std::filesystem::path RunPersistence::run_path_for(const std::string& run_id) const {
    return runs_dir() / (run_id + ".json");
}

std::filesystem::path RunPersistence::pending_path_for(const std::string& run_id) const {
    return pending_dir() / (run_id + ".json");
}

void RunPersistence::save(const RunResult& result) {
    std::filesystem::create_directories(runs_dir());

    const auto path = run_path_for(result.run_id);
    std::ofstream stream(path, std::ios::out | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("RunPersistence::save: cannot open " + path.string());
    }

    stream << run_result_to_json(result).dump(2);
    if (!stream) {
        throw std::runtime_error("RunPersistence::save: write error for " + path.string());
    }
}

std::optional<RunResult> RunPersistence::load(const std::string& run_id) const {
    const auto path = run_path_for(run_id);
    if (!std::filesystem::exists(path)) {
        return std::nullopt;
    }

    std::ifstream stream(path);
    if (!stream) {
        return std::nullopt;
    }

    nlohmann::json value;
    stream >> value;
    return run_result_from_json(value);
}

std::vector<RunSummary> RunPersistence::list(const std::string& project_id, int limit) const {
    if (!std::filesystem::exists(runs_dir())) {
        return {};
    }

    using Entry = std::pair<std::filesystem::file_time_type, RunSummary>;
    std::vector<Entry> entries;

    for (const auto& entry : std::filesystem::directory_iterator(runs_dir())) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }

        std::ifstream stream(entry.path());
        if (!stream) {
            continue;
        }

        nlohmann::json value;
        stream >> value;
        RunSummary summary = summary_from_result(run_result_from_json(value));
        if (!project_id.empty() && summary.project_id != project_id) {
            continue;
        }
        entries.emplace_back(entry.last_write_time(), std::move(summary));
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& left, const Entry& right) {
                  return left.first > right.first;
              });

    std::vector<RunSummary> result;
    const auto capped = std::min(static_cast<int>(entries.size()), limit);
    result.reserve(capped);
    for (int index = 0; index < capped; ++index) {
        result.push_back(std::move(entries[index].second));
    }
    return result;
}

bool RunPersistence::remove(const std::string& run_id) {
    const auto path = run_path_for(run_id);
    clear_pending(run_id);
    if (!std::filesystem::exists(path)) {
        return false;
    }
    std::filesystem::remove(path);
    return true;
}

void RunPersistence::save_pending(const RunResult& result) {
    if (!result.pending_approval.has_value()) {
        return;
    }

    std::filesystem::create_directories(pending_dir());
    const auto path = pending_path_for(result.run_id);
    std::ofstream stream(path, std::ios::out | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("RunPersistence::save_pending: cannot open " + path.string());
    }

    nlohmann::json value = run_result_to_json(result);
    stream << value.dump(2);
    if (!stream) {
        throw std::runtime_error("RunPersistence::save_pending: write error for " + path.string());
    }
}

bool RunPersistence::clear_pending(const std::string& run_id) {
    const auto path = pending_path_for(run_id);
    if (!std::filesystem::exists(path)) {
        return false;
    }
    std::filesystem::remove(path);
    return true;
}

}  // namespace omni::engine