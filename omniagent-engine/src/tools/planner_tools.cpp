#include "planner_tools.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace omni::engine {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::size_t kMaxOutputBytes = 128 * 1024;

ToolCallResult missing_context(const char* tool_name) {
    return {std::string(tool_name) + " requires a project workspace context.", true};
}

bool path_within_root(const fs::path& root, const fs::path& candidate) {
    const fs::path normalized_root = root.lexically_normal();
    const fs::path normalized_candidate = candidate.lexically_normal();
    const fs::path relative = normalized_candidate.lexically_relative(normalized_root);
    if (relative.empty()) {
        return normalized_candidate == normalized_root;
    }
    auto it = relative.begin();
    return it == relative.end() || *it != "..";
}

std::optional<fs::path> canonical_workspace_root(const ToolContext& context,
                                                 std::string& error) {
    if (context.workspace_root.empty()) {
        error = "workspace_root is empty";
        return std::nullopt;
    }

    std::error_code ec;
    const fs::path root = fs::weakly_canonical(context.workspace_root, ec);
    if (ec) {
        error = "failed to resolve workspace root '" + context.workspace_root.string()
            + "': " + ec.message();
        return std::nullopt;
    }
    return root;
}

std::optional<fs::path> canonical_working_dir(const ToolContext& context,
                                              const fs::path& root,
                                              std::string& error) {
    const fs::path base = context.working_dir.empty() ? root : context.working_dir;
    std::error_code ec;
    const fs::path cwd = fs::weakly_canonical(base, ec);
    if (ec) {
        error = "failed to resolve working directory '" + base.string()
            + "': " + ec.message();
        return std::nullopt;
    }
    if (!path_within_root(root, cwd)) {
        error = "working directory escapes workspace root";
        return std::nullopt;
    }
    return cwd;
}

std::optional<fs::path> resolve_workspace_path(const fs::path& requested,
                                               const ToolContext& context,
                                               std::string& error,
                                               bool allow_missing_leaf = false) {
    const auto root = canonical_workspace_root(context, error);
    if (!root) {
        return std::nullopt;
    }
    const auto cwd = canonical_working_dir(context, *root, error);
    if (!cwd) {
        return std::nullopt;
    }

    fs::path candidate = requested.empty() ? *cwd : requested;
    if (candidate.is_relative()) {
        candidate = *cwd / candidate;
    }

    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(candidate, ec);
    if (ec) {
        error = "failed to resolve path '" + candidate.string() + "': " + ec.message();
        return std::nullopt;
    }
    if (!path_within_root(*root, resolved)) {
        error = "path escapes workspace root: " + requested.string();
        return std::nullopt;
    }
    if (!allow_missing_leaf && !fs::exists(resolved)) {
        error = "path does not exist: " + requested.string();
        return std::nullopt;
    }
    return resolved;
}

std::string relative_to_root(const fs::path& absolute_path,
                             const ToolContext& context) {
    std::string error;
    const auto root = canonical_workspace_root(context, error);
    if (!root) {
        return absolute_path.string();
    }

    const fs::path relative = absolute_path.lexically_relative(*root);
    if (relative.empty()) {
        return ".";
    }
    return relative.generic_string();
}

std::optional<int> parse_optional_int(const json& value) {
    if (value.is_number_integer()) {
        return value.get<int>();
    }
    if (value.is_string()) {
        try {
            return std::stoi(value.get<std::string>());
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

struct ProcessResult {
    std::string output;
    int exit_code = -1;
    bool timed_out = false;
    std::string error;
    bool exec_missing = false;
};

#ifndef _WIN32
ProcessResult run_process(const std::vector<std::string>& argv,
                          int timeout_ms,
                          const fs::path& working_dir) {
    ProcessResult result;
    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        result.error = "failed to create pipe: " + std::string(std::strerror(errno));
        return result;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        result.error = "failed to fork planner process: " + std::string(std::strerror(errno));
        return result;
    }

    if (pid == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);

        if (chdir(working_dir.c_str()) != 0) {
            std::fprintf(stderr, "failed to change directory to %s: %s\n",
                         working_dir.c_str(), std::strerror(errno));
            _exit(127);
        }

        std::vector<char*> exec_args;
        exec_args.reserve(argv.size() + 1);
        for (const auto& arg : argv) {
            exec_args.push_back(const_cast<char*>(arg.c_str()));
        }
        exec_args.push_back(nullptr);

        execvp(exec_args[0], exec_args.data());
        std::fprintf(stderr, "failed to exec %s: %s\n", exec_args[0], std::strerror(errno));
        _exit(127);
    }

    close(pipe_fds[1]);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buffer[4096];
    bool truncated = false;

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            result.timed_out = true;
            break;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        struct pollfd pfd {
            pipe_fds[0],
            POLLIN,
            0,
        };
        const int poll_result = poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(pipe_fds[0]);
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            result.error = "poll failed: " + std::string(std::strerror(errno));
            return result;
        }
        if (poll_result == 0) {
            result.timed_out = true;
            break;
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            const ssize_t read_count = read(pipe_fds[0], buffer, sizeof(buffer));
            if (read_count > 0) {
                if (result.output.size() < kMaxOutputBytes) {
                    const std::size_t remaining_capacity = kMaxOutputBytes - result.output.size();
                    result.output.append(buffer, buffer + std::min<std::size_t>(remaining_capacity,
                                                                                 static_cast<std::size_t>(read_count)));
                    truncated = truncated || static_cast<std::size_t>(read_count) > remaining_capacity;
                } else {
                    truncated = true;
                }
                continue;
            }
            break;
        }
        if ((pfd.revents & POLLIN) == 0) {
            continue;
        }

        const ssize_t read_count = read(pipe_fds[0], buffer, sizeof(buffer));
        if (read_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(pipe_fds[0]);
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            result.error = "read failed: " + std::string(std::strerror(errno));
            return result;
        }
        if (read_count == 0) {
            break;
        }

        if (result.output.size() < kMaxOutputBytes) {
            const std::size_t remaining_capacity = kMaxOutputBytes - result.output.size();
            result.output.append(buffer, buffer + std::min<std::size_t>(remaining_capacity,
                                                                         static_cast<std::size_t>(read_count)));
            truncated = truncated || static_cast<std::size_t>(read_count) > remaining_capacity;
        } else {
            truncated = true;
        }
    }

    close(pipe_fds[0]);

    int status = 0;
    if (result.timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    } else {
        waitpid(pid, &status, 0);
    }

    if (truncated) {
        result.output += "\n[truncated]";
    }
    if (result.timed_out) {
        return result;
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }

    result.exec_missing = result.exit_code == 127
        && result.output.find("failed to exec") != std::string::npos;
    return result;
}
#else
ProcessResult run_process(const std::vector<std::string>&,
                          int,
                          const fs::path&) {
    ProcessResult result;
    result.error = "planner tools are not implemented on Windows in omniagent-engine.";
    return result;
}
#endif

std::optional<fs::path> locate_planner_bridge(const ToolContext& context,
                                              std::string& error) {
    const auto root = canonical_workspace_root(context, error);
    if (!root) {
        return std::nullopt;
    }

    const fs::path direct = *root / "planner-harness" / "bridge.py";
    if (fs::is_regular_file(direct)) {
        return direct;
    }

    std::error_code ec;
    for (fs::recursive_directory_iterator it(*root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            ec.clear();
            continue;
        }
        const fs::path candidate = it->path();
        if (candidate.filename() == "bridge.py" && candidate.parent_path().filename() == "planner-harness") {
            return candidate;
        }
    }

    error = "could not locate planner-harness/bridge.py under the workspace root";
    return std::nullopt;
}

std::vector<std::string> python_candidates() {
    std::vector<std::string> candidates;
    if (const char* configured = std::getenv("OMNIAGENT_PYTHON")) {
        if (*configured != '\0') {
            candidates.push_back(configured);
        }
    }
    candidates.push_back("python3");
    candidates.push_back("python");
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    std::stable_sort(candidates.begin(), candidates.end(), [](const std::string& a, const std::string& b) {
        if (a == "python3") {
            return true;
        }
        if (b == "python3") {
            return false;
        }
        if (a.find("OMNIAGENT") != std::string::npos) {
            return true;
        }
        return a < b;
    });
    return candidates;
}

ToolCallResult invoke_bridge(const ToolContext& context,
                             const std::optional<fs::path>& config_path,
                             const std::vector<std::string>& bridge_args,
                             int timeout_ms,
                             json& payload) {
    std::string error;
    const auto root = canonical_workspace_root(context, error);
    if (!root) {
        return {error, true};
    }
    const auto bridge = locate_planner_bridge(context, error);
    if (!bridge) {
        return {error, true};
    }

    std::string last_exec_error;
    for (const auto& python : python_candidates()) {
        std::vector<std::string> command;
        command.push_back(python);
        command.push_back(bridge->string());
        if (config_path.has_value()) {
            command.push_back("--config");
            command.push_back(config_path->string());
        }
        command.insert(command.end(), bridge_args.begin(), bridge_args.end());

        ProcessResult result = run_process(command, timeout_ms, *root);
        if (!result.error.empty()) {
            return {result.error, true};
        }
        if (result.exec_missing) {
            last_exec_error = result.output;
            continue;
        }
        if (result.timed_out) {
            return {"planner bridge timed out after " + std::to_string(timeout_ms) + "ms", true};
        }

        try {
            payload = json::parse(result.output);
        } catch (const std::exception& ex) {
            return {std::string{"planner bridge returned invalid JSON: "} + ex.what()
                + "\n" + result.output, true};
        }

        if (!payload.is_object()) {
            return {"planner bridge returned a non-object JSON payload", true};
        }
        if (!payload.value("ok", false)) {
            return {payload.dump(2), true};
        }
        return {"", false};
    }

    if (last_exec_error.empty()) {
        last_exec_error = "failed to locate a usable Python interpreter (tried OMNIAGENT_PYTHON, python3, python)";
    }
    return {last_exec_error, true};
}

std::pair<json, std::string> parse_graph_plan_json(const json& plan) {
    const char* entries_key = nullptr;
    bool is_nodes_format = false;

    if (plan.contains("sections") && plan["sections"].is_array()) {
        entries_key = "sections";
    } else if (plan.contains("phases") && plan["phases"].is_array()) {
        entries_key = "phases";
    } else if (plan.contains("nodes") && plan["nodes"].is_array()) {
        entries_key = "nodes";
        is_nodes_format = true;
    }

    if (entries_key == nullptr) {
        return {{}, "PLAN.json missing 'sections', 'phases', or 'nodes' array"};
    }

    json normalized_phases = json::array();

    if (is_nodes_format) {
        std::vector<json> node_data;
        node_data.reserve(plan[entries_key].size());

        for (size_t index = 0; index < plan[entries_key].size(); ++index) {
            const auto& node = plan[entries_key][index];
            if (!node.contains("id") || !node["id"].is_string()) {
                return {{}, std::string{"'nodes["} + std::to_string(index)
                    + "]: missing required 'id' field"};
            }

            json expected_files = json::array();
            if (node.contains("expected_files") && node["expected_files"].is_array()) {
                for (const auto& expected_file : node["expected_files"]) {
                    if (expected_file.is_string()) {
                        expected_files.push_back(expected_file.get<std::string>());
                    }
                }
            }

            json entry = {
                {"node_id", node["id"]},
                {"file_path", node["id"]},
                {"prompt", node.value("prompt", "")},
                {"expected_files", expected_files},
            };
            if (node.contains("depends_on") && node["depends_on"].is_array()) {
                entry["depends_on"] = node["depends_on"];
            }
            if (node.contains("spec_sections") && node["spec_sections"].is_object()) {
                entry["spec_sections"] = node["spec_sections"];
            }
            node_data.push_back(std::move(entry));
        }

        for (size_t index = 0; index < node_data.size(); ++index) {
            const auto& node = node_data[index];
            json file_entry = {
                {"path", node["file_path"]},
                {"description", node["prompt"]},
                {"expected_files", node["expected_files"]},
            };

            if (node.contains("depends_on") && node["depends_on"].is_array()) {
                file_entry["depends_on"] = node["depends_on"];
            } else if (index > 0) {
                json auto_deps = json::array();
                std::unordered_set<std::string> current_expected_files;
                for (const auto& expected_file : node["expected_files"]) {
                    if (expected_file.is_string()) {
                        current_expected_files.insert(expected_file.get<std::string>());
                    }
                }

                for (size_t dep_index = 0; dep_index < index; ++dep_index) {
                    const auto& dep_node = node_data[dep_index];
                    bool needs_dep = false;
                    for (const auto& expected_file : dep_node["expected_files"]) {
                        if (expected_file.is_string()
                            && current_expected_files.count(expected_file.get<std::string>()) > 0) {
                            needs_dep = true;
                            break;
                        }
                    }
                    if (needs_dep) {
                        auto_deps.push_back(dep_node["node_id"]);
                    }
                }

                const bool has_expected_files = node.contains("expected_files")
                    && node["expected_files"].is_array()
                    && !node["expected_files"].empty();
                if (auto_deps.empty() && index > 0 && !has_expected_files) {
                    auto_deps.push_back(node_data[index - 1]["node_id"]);
                }
                if (!auto_deps.empty()) {
                    file_entry["depends_on"] = auto_deps;
                }
            }

            if (node.contains("spec_sections") && node["spec_sections"].is_object()) {
                file_entry["spec_sections"] = node["spec_sections"];
            }

            normalized_phases.push_back({
                {"phase", static_cast<int>(index)},
                {"name", node["node_id"]},
                {"files", json::array({std::move(file_entry)})},
            });
        }

        return {json{{"phases", normalized_phases}}, {}};
    }

    json validated_phases = json::array();
    std::unordered_set<std::string> all_files;
    std::unordered_set<std::string> all_phase_names;

    for (size_t phase_index = 0; phase_index < plan[entries_key].size(); ++phase_index) {
        const auto& phase = plan[entries_key][phase_index];
        if (phase.contains("name") && phase["name"].is_string()) {
            all_phase_names.insert(phase["name"].get<std::string>());
        }
        if (!phase.contains("tasks") || !phase["tasks"].is_array()) {
            continue;
        }
        for (size_t task_index = 0; task_index < phase["tasks"].size(); ++task_index) {
            const auto& task = phase["tasks"][task_index];
            if (!task.contains("file") || !task["file"].is_string()) {
                return {{}, std::string{"'"} + entries_key + "[" + std::to_string(phase_index)
                    + "]: task " + std::to_string(task_index)
                    + " missing required 'file' field"};
            }
            const std::string file = task["file"].get<std::string>();
            if (!all_files.insert(file).second) {
                return {{}, std::string{"duplicate file path '"} + file + "' in plan"};
            }
        }
    }

    for (const auto& phase : plan[entries_key]) {
        if (!phase.contains("tasks") || !phase["tasks"].is_array()) {
            continue;
        }
        for (const auto& task : phase["tasks"]) {
            if (!task.contains("depends_on") || !task["depends_on"].is_array()) {
                continue;
            }
            for (const auto& dependency : task["depends_on"]) {
                if (!dependency.is_string()) {
                    continue;
                }
                const std::string dep_str = dependency.get<std::string>();
                if (!all_files.contains(dep_str) && !all_phase_names.contains(dep_str)) {
                    return {{}, std::string{"depends_on references unknown file or group '"}
                        + dep_str + "'"};
                }
            }
        }
    }

    for (size_t phase_index = 0; phase_index < plan[entries_key].size(); ++phase_index) {
        const auto& phase = plan[entries_key][phase_index];
        json files = json::array();

        if (phase.contains("tasks") && phase["tasks"].is_array()) {
            for (const auto& task : phase["tasks"]) {
                json file_entry = {
                    {"path", task["file"]},
                    {"description", task.value("description", task["file"].get<std::string>())},
                    {"depends_on", task.value("depends_on", json::array())},
                };
                if (task.contains("spec_sections")) {
                    file_entry["spec_sections"] = task["spec_sections"];
                }
                if (task.contains("spec_section")) {
                    file_entry["spec_section"] = task["spec_section"];
                }
                file_entry["goal"] = "Create " + task["file"].get<std::string>()
                    + " — " + file_entry["description"].get<std::string>();
                files.push_back(std::move(file_entry));
            }
        }

        json entry = {
            {"phase", static_cast<int>(phase_index)},
            {"name", phase.value("name", "Phase " + std::to_string(phase_index))},
            {"files", std::move(files)},
        };
        if (phase.contains("depends_on") && phase["depends_on"].is_array()) {
            entry["depends_on"] = phase["depends_on"];
        }
        validated_phases.push_back(std::move(entry));
    }

    return {json{{"phases", validated_phases}}, {}};
}

json summarize_graph_validation(const json& original_plan,
                                const json& normalized_plan,
                                const std::string& error) {
    std::size_t file_count = 0;
    std::size_t phase_count = 0;
    if (normalized_plan.contains("phases") && normalized_plan["phases"].is_array()) {
        phase_count = normalized_plan["phases"].size();
        for (const auto& phase : normalized_plan["phases"]) {
            if (phase.contains("files") && phase["files"].is_array()) {
                file_count += phase["files"].size();
            }
        }
    }

    std::string input_format = "unknown";
    if (original_plan.contains("sections")) {
        input_format = "sections";
    } else if (original_plan.contains("phases")) {
        input_format = "phases";
    } else if (original_plan.contains("nodes")) {
        input_format = "nodes";
    }

    json summary = {
        {"valid", error.empty()},
        {"input_format", input_format},
        {"normalized_phase_count", phase_count},
        {"normalized_file_count", file_count},
    };
    if (!error.empty()) {
        summary["error"] = error;
    }
    return summary;
}

ToolCallResult attach_graph_validation(const fs::path& plan_path,
                                       const ToolContext& context,
                                       json& payload) {
    std::string error;
    const auto resolved_plan = resolve_workspace_path(plan_path, context, error);
    if (!resolved_plan) {
        return {error, true};
    }

    std::ifstream stream(*resolved_plan);
    if (!stream.is_open()) {
        return {"failed to open plan file '" + resolved_plan->string() + "'", true};
    }

    json plan_json;
    try {
        stream >> plan_json;
    } catch (const std::exception& ex) {
        return {std::string{"failed to parse plan JSON: "} + ex.what(), true};
    }

    const auto [normalized, parse_error] = parse_graph_plan_json(plan_json);
    payload["graph_validation"] = summarize_graph_validation(plan_json, normalized, parse_error);

    if (payload.contains("stage") && payload["stage"].is_object()) {
        payload["stage"]["passed"] = payload["stage"].value("passed", true) && parse_error.empty();
    }
    if (payload.contains("plan_validation") && payload["plan_validation"].is_object()) {
        payload["plan_validation"]["passed"] = payload["plan_validation"].value("passed", true)
            && parse_error.empty();
    }
    if (payload.contains("artifacts") && payload["artifacts"].is_object()) {
        payload["artifacts"]["plan_path_relative"] = relative_to_root(*resolved_plan, context);
        if (payload["artifacts"].contains("prompt_path") && payload["artifacts"]["prompt_path"].is_string()) {
            std::string prompt_error;
            const auto prompt_path = resolve_workspace_path(
                fs::path(payload["artifacts"]["prompt_path"].get<std::string>()),
                context,
                prompt_error,
                true);
            if (prompt_path) {
                payload["artifacts"]["prompt_path_relative"] = relative_to_root(*prompt_path, context);
            }
        }
    }
    if (payload.contains("spec_validation") && payload["spec_validation"].is_object()
        && payload.contains("plan_validation") && payload["plan_validation"].is_object()) {
        payload["workflow_passed"] = payload["spec_validation"].value("passed", false)
            && payload["plan_validation"].value("passed", false);
    }

    return {"", false};
}

std::optional<fs::path> optional_workspace_path(const json& args,
                                                const char* key,
                                                const ToolContext& context,
                                                std::string& error,
                                                bool allow_missing_leaf = false) {
    if (!args.contains(key) || args.at(key).is_null()) {
        return std::nullopt;
    }
    if (!args.at(key).is_string()) {
        error = std::string{"'"} + key + "' must be a string";
        return std::nullopt;
    }
    return resolve_workspace_path(fs::path(args.at(key).get<std::string>()), context, error, allow_missing_leaf);
}

int resolve_timeout(const json& args, int fallback) {
    if (!args.contains("timeout")) {
        return fallback;
    }
    const auto parsed = parse_optional_int(args.at("timeout"));
    if (!parsed.has_value() || *parsed <= 0) {
        return fallback;
    }
    return *parsed;
}

bool object_flag(const json& payload, const char* key, bool fallback = false) {
    return payload.contains(key) && payload.at(key).is_object()
        ? payload.at(key).value("passed", fallback)
        : fallback;
}

bool graph_validation_passed(const json& payload) {
    return payload.contains("graph_validation") && payload.at("graph_validation").is_object()
        ? payload.at("graph_validation").value("valid", false)
        : false;
}

void append_stage_failures(std::vector<std::string>& failures,
                           const json& stage,
                           const std::string& prefix = {}) {
    if (!stage.is_object()) {
        return;
    }

    auto add_failure = [&](std::string message) {
        if (!prefix.empty()) {
            message = prefix + ": " + message;
        }
        failures.push_back(std::move(message));
    };

    if (stage.contains("rubric_checks") && stage["rubric_checks"].is_array()) {
        for (const auto& check : stage["rubric_checks"]) {
            if (!check.is_object() || check.value("passed", true)) {
                continue;
            }
            const std::string name = check.value("name", std::string{"Unnamed check"});
            const std::string detail = check.value("detail", std::string{});
            add_failure(detail.empty() ? name : name + ": " + detail);
        }
    }

    if (!stage.contains("adversary") || !stage["adversary"].is_object()) {
        return;
    }

    const auto& adversary = stage["adversary"];
    const std::string adversary_error = adversary.value("error", std::string{});
    if (!adversary_error.empty()) {
        add_failure("Adversary error: " + adversary_error);
    }
    const int blocking_gaps = adversary.value("blocking_gaps", 0);
    if (blocking_gaps > 0) {
        add_failure("Blocking adversary gaps: " + std::to_string(blocking_gaps));
    }
    const int blocking_guesses = adversary.value("blocking_guesses", 0);
    if (blocking_guesses > 0) {
        add_failure("Blocking adversary guesses: " + std::to_string(blocking_guesses));
    }
    const int contradiction_count = adversary.value("contradiction_count", 0);
    if (contradiction_count > 0) {
        add_failure("Adversary contradictions: " + std::to_string(contradiction_count));
    }
}

std::vector<std::string> collect_validation_failures(const json& payload) {
    std::vector<std::string> failures;

    if (payload.contains("stage") && payload["stage"].is_object()) {
        append_stage_failures(failures, payload["stage"]);
    }
    if (payload.contains("spec_validation") && payload["spec_validation"].is_object()) {
        append_stage_failures(failures, payload["spec_validation"], "Spec validation");
    }
    if (payload.contains("plan_validation") && payload["plan_validation"].is_object()) {
        append_stage_failures(failures, payload["plan_validation"], "Plan validation");
    }
    if (payload.contains("graph_validation") && payload["graph_validation"].is_object()
        && !payload["graph_validation"].value("valid", false)) {
        failures.push_back(
            "Graph validation: "
            + payload["graph_validation"].value("error", std::string{"graph validation failed"}));
    }

    return failures;
}

std::string dump_payload(json payload) {
    return payload.dump(2);
}

ToolCallResult finalize_planner_result(const std::string& status_name,
                                       const std::string& primary_flag_name,
                                       bool overall_passed,
                                       const json& payload) {
    if (overall_passed) {
        return {dump_payload(payload), false};
    }

    std::vector<std::string> failures = collect_validation_failures(payload);
    if (failures.empty()) {
        failures.push_back("Validation failed without a detailed blocking check.");
    }

    std::ostringstream stream;
    stream << status_name << " STATUS: FAILED\n";
    stream << primary_flag_name << ": false\n";
    if (payload.contains("workflow_passed") && payload["workflow_passed"].is_boolean()) {
        stream << "workflow_passed: " << (payload["workflow_passed"].get<bool>() ? "true" : "false") << "\n";
    }
    if (payload.contains("stage") && payload["stage"].is_object()) {
        stream << "stage_passed: " << (object_flag(payload, "stage") ? "true" : "false") << "\n";
    }
    if (payload.contains("spec_validation") && payload["spec_validation"].is_object()) {
        stream << "spec_validation_passed: " << (object_flag(payload, "spec_validation") ? "true" : "false") << "\n";
    }
    if (payload.contains("plan_validation") && payload["plan_validation"].is_object()) {
        stream << "plan_validation_passed: " << (object_flag(payload, "plan_validation") ? "true" : "false") << "\n";
    }
    if (payload.contains("graph_validation") && payload["graph_validation"].is_object()) {
        stream << "graph_validation_passed: " << (graph_validation_passed(payload) ? "true" : "false") << "\n";
    }
    stream << "blocking_checks:\n";
    for (const auto& failure : failures) {
        stream << "- " << failure << "\n";
    }
    stream << "raw_json:\n" << dump_payload(payload);
    return {stream.str(), true};
}

}  // namespace

std::string PlannerValidateSpecTool::name() const {
    return "planner_validate_spec";
}

std::string PlannerValidateSpecTool::description() const {
    return "Validate a SPEC.md file with planner-harness and return structured rubric and adversary results.";
}

nlohmann::json PlannerValidateSpecTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"spec_path", {{"type", "string"}, {"description", "Path to the spec file inside the workspace."}, {"default", "SPEC.md"}}},
            {"config_path", {{"type", "string"}, {"description", "Optional planner-harness config path inside the workspace."}}},
            {"model", {{"type", "string"}, {"description", "Optional planner-harness model name override."}}},
            {"skip_adversary", {{"type", "boolean"}, {"description", "Skip the LLM adversary pass and run rubric checks only."}, {"default", true}}},
            {"timeout", {{"type", "integer"}, {"description", "Maximum execution time in milliseconds."}, {"default", kDefaultTimeoutMs}}}
        }},
        {"required", json::array({"spec_path"})}
    };
}

ToolCallResult PlannerValidateSpecTool::call(const json& args,
                                             const ToolContext& context) {
    std::string error;
    const auto spec_path = resolve_workspace_path(
        fs::path(args.value("spec_path", std::string{"SPEC.md"})),
        context,
        error);
    if (!spec_path) {
        return {error, true};
    }

    const auto config_path = optional_workspace_path(args, "config_path", context, error);
    if (!error.empty()) {
        return {error, true};
    }

    const int timeout_ms = resolve_timeout(args, kDefaultTimeoutMs);
    std::vector<std::string> bridge_args = {"validate-spec", spec_path->string()};
    if (args.value("skip_adversary", true)) {
        bridge_args.push_back("--skip-adversary");
    }
    if (args.contains("model") && args.at("model").is_string()) {
        bridge_args.push_back("--model");
        bridge_args.push_back(args.at("model").get<std::string>());
    }

    json payload;
    const auto result = invoke_bridge(context, config_path, bridge_args, timeout_ms, payload);
    if (result.is_error) {
        return result;
    }

    payload["spec_path_relative"] = relative_to_root(*spec_path, context);
    return finalize_planner_result(
        "PLANNER_VALIDATE_SPEC",
        "spec_validation_passed",
        object_flag(payload, "stage"),
        payload);
}

ToolCallResult PlannerValidateSpecTool::call(const json&) {
    return missing_context(name().c_str());
}

std::string PlannerValidatePlanTool::name() const {
    return "planner_validate_plan";
}

std::string PlannerValidatePlanTool::description() const {
    return "Validate a PLAN.json file with planner-harness and check graph-parser compatibility.";
}

nlohmann::json PlannerValidatePlanTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"spec_path", {{"type", "string"}, {"description", "Path to the spec file inside the workspace."}, {"default", "SPEC.md"}}},
            {"plan_path", {{"type", "string"}, {"description", "Path to PLAN.json inside the workspace."}, {"default", "PLAN.json"}}},
            {"config_path", {{"type", "string"}, {"description", "Optional planner-harness config path inside the workspace."}}},
            {"model", {{"type", "string"}, {"description", "Optional planner-harness model name override."}}},
            {"skip_adversary", {{"type", "boolean"}, {"description", "Skip the LLM adversary pass and run rubric checks only."}, {"default", true}}},
            {"timeout", {{"type", "integer"}, {"description", "Maximum execution time in milliseconds."}, {"default", kDefaultTimeoutMs}}}
        }},
        {"required", json::array({"plan_path"})}
    };
}

ToolCallResult PlannerValidatePlanTool::call(const json& args,
                                             const ToolContext& context) {
    std::string error;
    const auto spec_path = resolve_workspace_path(
        fs::path(args.value("spec_path", std::string{"SPEC.md"})),
        context,
        error);
    if (!spec_path) {
        return {error, true};
    }
    const auto plan_path = resolve_workspace_path(
        fs::path(args.value("plan_path", std::string{"PLAN.json"})),
        context,
        error);
    if (!plan_path) {
        return {error, true};
    }

    const auto config_path = optional_workspace_path(args, "config_path", context, error);
    if (!error.empty()) {
        return {error, true};
    }

    const int timeout_ms = resolve_timeout(args, kDefaultTimeoutMs);
    std::vector<std::string> bridge_args = {"validate-plan", spec_path->string(), plan_path->string()};
    if (args.value("skip_adversary", true)) {
        bridge_args.push_back("--skip-adversary");
    }
    if (args.contains("model") && args.at("model").is_string()) {
        bridge_args.push_back("--model");
        bridge_args.push_back(args.at("model").get<std::string>());
    }

    json payload;
    const auto result = invoke_bridge(context, config_path, bridge_args, timeout_ms, payload);
    if (result.is_error) {
        return result;
    }

    payload["spec_path_relative"] = relative_to_root(*spec_path, context);
    payload["plan_path_relative"] = relative_to_root(*plan_path, context);
    const auto graph_result = attach_graph_validation(*plan_path, context, payload);
    if (graph_result.is_error) {
        return graph_result;
    }

    return finalize_planner_result(
        "PLANNER_VALIDATE_PLAN",
        "plan_validation_passed",
        object_flag(payload, "stage") && graph_validation_passed(payload),
        payload);
}

ToolCallResult PlannerValidatePlanTool::call(const json&) {
    return missing_context(name().c_str());
}

std::string PlannerRepairPlanTool::name() const {
    return "planner_repair_plan";
}

std::string PlannerRepairPlanTool::description() const {
    return "Repair an existing PLAN.json with planner-harness patch operations and return the repaired artifact summary.";
}

nlohmann::json PlannerRepairPlanTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"spec_path", {{"type", "string"}, {"description", "Path to the spec file inside the workspace."}, {"default", "SPEC.md"}}},
            {"plan_path", {{"type", "string"}, {"description", "Path to the existing PLAN.json inside the workspace."}, {"default", "PLAN.json"}}},
            {"output_path", {{"type", "string"}, {"description", "Where to write the repaired plan. Defaults to overwriting plan_path when omitted."}}},
            {"config_path", {{"type", "string"}, {"description", "Optional planner-harness config path inside the workspace."}}},
            {"model", {{"type", "string"}, {"description", "Optional planner-harness model name override."}}},
            {"validation_json_path", {{"type", "string"}, {"description", "Optional planner validation JSON to derive repair feedback from."}}},
            {"repair_feedback", {{"type", "string"}, {"description", "Optional explicit repair feedback to send to planner-harness."}}},
            {"skip_adversary", {{"type", "boolean"}, {"description", "Skip LLM adversary checks when planner-harness must validate before repairing."}, {"default", true}}},
            {"timeout", {{"type", "integer"}, {"description", "Maximum execution time in milliseconds."}, {"default", kDefaultTimeoutMs}}}
        }},
        {"required", json::array({"plan_path"})}
    };
}

ToolCallResult PlannerRepairPlanTool::call(const json& args,
                                           const ToolContext& context) {
    std::string error;
    const auto spec_path = resolve_workspace_path(
        fs::path(args.value("spec_path", std::string{"SPEC.md"})),
        context,
        error);
    if (!spec_path) {
        return {error, true};
    }
    const auto plan_path = resolve_workspace_path(
        fs::path(args.value("plan_path", std::string{"PLAN.json"})),
        context,
        error);
    if (!plan_path) {
        return {error, true};
    }

    const auto output_path = resolve_workspace_path(
        fs::path(args.value("output_path", plan_path->string())),
        context,
        error,
        true);
    if (!output_path) {
        return {error, true};
    }

    const auto config_path = optional_workspace_path(args, "config_path", context, error);
    if (!error.empty()) {
        return {error, true};
    }

    const auto validation_json_path = optional_workspace_path(
        args,
        "validation_json_path",
        context,
        error);
    if (!error.empty()) {
        return {error, true};
    }

    std::optional<std::string> repair_feedback;
    if (args.contains("repair_feedback") && !args.at("repair_feedback").is_null()) {
        if (!args.at("repair_feedback").is_string()) {
            return {"'repair_feedback' must be a string", true};
        }
        repair_feedback = args.at("repair_feedback").get<std::string>();
    }

    const int timeout_ms = resolve_timeout(args, kDefaultTimeoutMs);
    std::vector<std::string> bridge_args = {
        "repair-plan",
        spec_path->string(),
        plan_path->string(),
        "-o",
        output_path->string(),
    };
    if (validation_json_path.has_value()) {
        bridge_args.push_back("--validation-json");
        bridge_args.push_back(validation_json_path->string());
    }
    if (repair_feedback.has_value()) {
        bridge_args.push_back("--repair-feedback");
        bridge_args.push_back(*repair_feedback);
    }
    if (args.value("skip_adversary", true)) {
        bridge_args.push_back("--skip-adversary");
    }
    if (args.contains("model") && args.at("model").is_string()) {
        bridge_args.push_back("--model");
        bridge_args.push_back(args.at("model").get<std::string>());
    }

    json payload;
    const auto result = invoke_bridge(context, config_path, bridge_args, timeout_ms, payload);
    if (result.is_error) {
        return result;
    }

    payload["spec_path_relative"] = relative_to_root(*spec_path, context);
    payload["plan_path_relative"] = relative_to_root(*plan_path, context);
    payload["output_path_relative"] = relative_to_root(*output_path, context);
    const auto graph_result = attach_graph_validation(*output_path, context, payload);
    if (graph_result.is_error) {
        return graph_result;
    }

    return finalize_planner_result(
        "PLANNER_REPAIR_PLAN",
        "repaired_plan_graph_valid",
        graph_validation_passed(payload),
        payload);
}

ToolCallResult PlannerRepairPlanTool::call(const json&) {
    return missing_context(name().c_str());
}

std::string PlannerBuildPlanTool::name() const {
    return "planner_build_plan";
}

std::string PlannerBuildPlanTool::description() const {
    return "Generate planner-prompt.md and PLAN.json with planner-harness, validate the result, and check graph-parser compatibility.";
}

nlohmann::json PlannerBuildPlanTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"spec_path", {{"type", "string"}, {"description", "Path to the spec file inside the workspace."}, {"default", "SPEC.md"}}},
            {"prompt_output_path", {{"type", "string"}, {"description", "Where to write the generated planner prompt."}, {"default", "planner-prompt.md"}}},
            {"plan_output_path", {{"type", "string"}, {"description", "Where to write the generated plan."}, {"default", "PLAN.json"}}},
            {"config_path", {{"type", "string"}, {"description", "Optional planner-harness config path inside the workspace."}}},
            {"model", {{"type", "string"}, {"description", "Optional planner-harness model name override."}}},
            {"skip_adversary", {{"type", "boolean"}, {"description", "Skip LLM adversary checks during validation."}, {"default", true}}},
            {"timeout", {{"type", "integer"}, {"description", "Maximum execution time in milliseconds."}, {"default", kDefaultTimeoutMs}}}
        }},
        {"required", json::array({"spec_path"})}
    };
}

ToolCallResult PlannerBuildPlanTool::call(const json& args,
                                          const ToolContext& context) {
    std::string error;
    const auto spec_path = resolve_workspace_path(
        fs::path(args.value("spec_path", std::string{"SPEC.md"})),
        context,
        error);
    if (!spec_path) {
        return {error, true};
    }
    const auto prompt_output = resolve_workspace_path(
        fs::path(args.value("prompt_output_path", std::string{"planner-prompt.md"})),
        context,
        error,
        true);
    if (!prompt_output) {
        return {error, true};
    }
    const auto plan_output = resolve_workspace_path(
        fs::path(args.value("plan_output_path", std::string{"PLAN.json"})),
        context,
        error,
        true);
    if (!plan_output) {
        return {error, true};
    }

    const auto config_path = optional_workspace_path(args, "config_path", context, error);
    if (!error.empty()) {
        return {error, true};
    }

    const int timeout_ms = resolve_timeout(args, kDefaultTimeoutMs);
    std::vector<std::string> bridge_args = {
        "run",
        spec_path->string(),
        "--prompt-output",
        prompt_output->string(),
        "--plan-output",
        plan_output->string(),
    };
    if (args.value("skip_adversary", true)) {
        bridge_args.push_back("--skip-adversary");
    }
    if (args.contains("model") && args.at("model").is_string()) {
        bridge_args.push_back("--model");
        bridge_args.push_back(args.at("model").get<std::string>());
    }

    json payload;
    const auto result = invoke_bridge(context, config_path, bridge_args, timeout_ms, payload);
    if (result.is_error) {
        return result;
    }

    payload["spec_path_relative"] = relative_to_root(*spec_path, context);
    const auto graph_result = attach_graph_validation(*plan_output, context, payload);
    if (graph_result.is_error) {
        return graph_result;
    }

    return finalize_planner_result(
        "PLANNER_BUILD_PLAN",
        "workflow_passed",
        payload.value("workflow_passed", false) && graph_validation_passed(payload),
        payload);
}

ToolCallResult PlannerBuildPlanTool::call(const json&) {
    return missing_context(name().c_str());
}

std::string PlannerBuildFromIdeaTool::name() const {
    return "planner_build_from_idea";
}

std::string PlannerBuildFromIdeaTool::description() const {
    return "Generate SPEC.md, planner-prompt.md, and PLAN.json from a project idea using planner-harness, with validation and failure-banner semantics.";
}

nlohmann::json PlannerBuildFromIdeaTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"idea", {{"type", "string"}, {"description", "Inline project idea text."}}},
            {"idea_path", {{"type", "string"}, {"description", "Path to a file containing the project idea inside the workspace."}}},
            {"spec_output_path", {{"type", "string"}, {"description", "Where to write the generated SPEC.md."}, {"default", "SPEC.md"}}},
            {"prompt_output_path", {{"type", "string"}, {"description", "Where to write the generated planner prompt."}, {"default", "planner-prompt.md"}}},
            {"plan_output_path", {{"type", "string"}, {"description", "Where to write the generated PLAN.json."}, {"default", "PLAN.json"}}},
            {"context_paths", {{"type", "array"}, {"description", "Optional list of additional workspace context files to include during spec generation."}, {"items", {{"type", "string"}}}}},
            {"overwrite", {{"type", "boolean"}, {"description", "Allow overwriting existing output artifacts."}, {"default", false}}},
            {"config_path", {{"type", "string"}, {"description", "Optional planner-harness config path inside the workspace."}}},
            {"model", {{"type", "string"}, {"description", "Optional planner-harness model name override."}}},
            {"skip_adversary", {{"type", "boolean"}, {"description", "Skip LLM adversary checks during spec and plan validation."}, {"default", true}}},
            {"timeout", {{"type", "integer"}, {"description", "Maximum execution time in milliseconds."}, {"default", kDefaultTimeoutMs}}}
        }}
    };
}

ToolCallResult PlannerBuildFromIdeaTool::call(const json& args,
                                              const ToolContext& context) {
    std::optional<std::string> idea;
    std::optional<fs::path> idea_path;
    std::string error;

    if (args.contains("idea") && !args.at("idea").is_null()) {
        if (!args.at("idea").is_string()) {
            return {"'idea' must be a string", true};
        }
        const std::string value = args.at("idea").get<std::string>();
        if (!value.empty()) {
            idea = value;
        }
    }

    if (args.contains("idea_path") && !args.at("idea_path").is_null()) {
        if (!args.at("idea_path").is_string()) {
            return {"'idea_path' must be a string", true};
        }
        idea_path = resolve_workspace_path(
            fs::path(args.at("idea_path").get<std::string>()),
            context,
            error);
        if (!idea_path) {
            return {error, true};
        }
    }

    if (idea.has_value() == idea_path.has_value()) {
        return {"exactly one of 'idea' or 'idea_path' must be provided", true};
    }

    const auto spec_output = resolve_workspace_path(
        fs::path(args.value("spec_output_path", std::string{"SPEC.md"})),
        context,
        error,
        true);
    if (!spec_output) {
        return {error, true};
    }
    const auto prompt_output = resolve_workspace_path(
        fs::path(args.value("prompt_output_path", std::string{"planner-prompt.md"})),
        context,
        error,
        true);
    if (!prompt_output) {
        return {error, true};
    }
    const auto plan_output = resolve_workspace_path(
        fs::path(args.value("plan_output_path", std::string{"PLAN.json"})),
        context,
        error,
        true);
    if (!plan_output) {
        return {error, true};
    }

    std::vector<fs::path> context_paths;
    if (args.contains("context_paths") && !args.at("context_paths").is_null()) {
        if (!args.at("context_paths").is_array()) {
            return {"'context_paths' must be an array of strings", true};
        }
        for (const auto& entry : args.at("context_paths")) {
            if (!entry.is_string()) {
                return {"'context_paths' must be an array of strings", true};
            }
            const auto resolved_context = resolve_workspace_path(
                fs::path(entry.get<std::string>()),
                context,
                error);
            if (!resolved_context) {
                return {error, true};
            }
            context_paths.push_back(*resolved_context);
        }
    }

    const auto config_path = optional_workspace_path(args, "config_path", context, error);
    if (!error.empty()) {
        return {error, true};
    }

    const int timeout_ms = resolve_timeout(args, kDefaultTimeoutMs);
    std::vector<std::string> bridge_args = {
        "build-from-idea",
        "--spec-output",
        spec_output->string(),
        "--prompt-output",
        prompt_output->string(),
        "--plan-output",
        plan_output->string(),
    };
    if (idea.has_value()) {
        bridge_args.push_back("--idea");
        bridge_args.push_back(*idea);
    } else {
        bridge_args.push_back("--idea-path");
        bridge_args.push_back(idea_path->string());
    }
    for (const auto& context_path : context_paths) {
        bridge_args.push_back("--context-path");
        bridge_args.push_back(context_path.string());
    }
    if (args.value("overwrite", false)) {
        bridge_args.push_back("--overwrite");
    }
    if (args.value("skip_adversary", true)) {
        bridge_args.push_back("--skip-adversary");
    }
    if (args.contains("model") && args.at("model").is_string()) {
        bridge_args.push_back("--model");
        bridge_args.push_back(args.at("model").get<std::string>());
    }

    json payload;
    const auto result = invoke_bridge(context, config_path, bridge_args, timeout_ms, payload);
    if (result.is_error) {
        return result;
    }

    if (!payload.contains("artifacts") || !payload["artifacts"].is_object()) {
        payload["artifacts"] = json::object();
    }

    auto annotate_artifact_relative = [&](const char* key) -> ToolCallResult {
        if (!payload["artifacts"].contains(key) || !payload["artifacts"][key].is_string()) {
            return {"", false};
        }
        std::string path_error;
        const auto artifact_path = resolve_workspace_path(
            fs::path(payload["artifacts"][key].get<std::string>()),
            context,
            path_error,
            true);
        if (!artifact_path) {
            return {path_error, true};
        }
        payload["artifacts"][std::string{key} + "_relative"] = relative_to_root(*artifact_path, context);
        return {"", false};
    };

    if (const auto annotate_spec = annotate_artifact_relative("spec_path"); annotate_spec.is_error) {
        return annotate_spec;
    }
    if (const auto annotate_prompt = annotate_artifact_relative("prompt_path"); annotate_prompt.is_error) {
        return annotate_prompt;
    }
    if (const auto annotate_plan = annotate_artifact_relative("plan_path"); annotate_plan.is_error) {
        return annotate_plan;
    }

    bool has_plan_artifact = payload["artifacts"].contains("plan_path")
        && payload["artifacts"]["plan_path"].is_string();
    if (has_plan_artifact) {
        const auto graph_result = attach_graph_validation(*plan_output, context, payload);
        if (graph_result.is_error) {
            return graph_result;
        }
    }

    bool workflow_passed = payload.value("workflow_passed", false);
    if (has_plan_artifact) {
        workflow_passed = workflow_passed && graph_validation_passed(payload);
    }

    return finalize_planner_result(
        "PLANNER_BUILD_FROM_IDEA",
        "workflow_passed",
        workflow_passed,
        payload);
}

ToolCallResult PlannerBuildFromIdeaTool::call(const json&) {
    return missing_context(name().c_str());
}

}  // namespace omni::engine