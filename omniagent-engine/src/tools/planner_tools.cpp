#include "planner_tools.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <random>
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

std::optional<fs::path> resolve_workspace_path(const fs::path& requested,
                                               const ToolContext& context,
                                               std::string& error,
                                               bool allow_missing_leaf = false);

enum class ValidationCaseKind {
    Review,
    Bugfix,
};

struct ScopedTempPath {
    ScopedTempPath() = default;

    explicit ScopedTempPath(fs::path value)
        : path(std::move(value)) {}

    ScopedTempPath(const ScopedTempPath&) = delete;
    ScopedTempPath& operator=(const ScopedTempPath&) = delete;

    ScopedTempPath(ScopedTempPath&& other) noexcept
        : path(std::move(other.path)) {}

    ScopedTempPath& operator=(ScopedTempPath&& other) noexcept {
        if (this != &other) {
            cleanup();
            path = std::move(other.path);
        }
        return *this;
    }

    ~ScopedTempPath() {
        cleanup();
    }

    void cleanup() {
        if (path.empty()) {
            return;
        }
        std::error_code ec;
        fs::remove(path, ec);
        path.clear();
    }

    fs::path path;
};

const char* validation_case_directory(ValidationCaseKind kind) {
    switch (kind) {
        case ValidationCaseKind::Review:
            return "review";
        case ValidationCaseKind::Bugfix:
            return "bugfix";
    }
    return "review";
}

const char* validation_bridge_command(ValidationCaseKind kind) {
    switch (kind) {
        case ValidationCaseKind::Review:
            return "validate-review";
        case ValidationCaseKind::Bugfix:
            return "validate-bugfix";
    }
    return "validate-review";
}

const char* validation_status_name(ValidationCaseKind kind) {
    switch (kind) {
        case ValidationCaseKind::Review:
            return "PLANNER_VALIDATE_REVIEW";
        case ValidationCaseKind::Bugfix:
            return "PLANNER_VALIDATE_BUGFIX";
    }
    return "PLANNER_VALIDATE_REVIEW";
}

const char* validation_primary_flag(ValidationCaseKind kind) {
    switch (kind) {
        case ValidationCaseKind::Review:
            return "review_validation_passed";
        case ValidationCaseKind::Bugfix:
            return "bugfix_validation_passed";
    }
    return "review_validation_passed";
}

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::optional<fs::path> getenv_path(const char* name) {
    if (const char* raw = std::getenv(name)) {
        if (*raw != '\0') {
            return fs::path(raw);
        }
    }
    return std::nullopt;
}

std::optional<fs::path> canonical_existing_file(const fs::path& candidate,
                                                std::string& error) {
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(candidate, ec);
    if (ec) {
        error = "failed to resolve path '" + candidate.string() + "': " + ec.message();
        return std::nullopt;
    }
    if (!fs::exists(resolved)) {
        error = "path does not exist: " + candidate.string();
        return std::nullopt;
    }
    if (!fs::is_regular_file(resolved)) {
        error = "path is not a file: " + candidate.string();
        return std::nullopt;
    }
    return resolved;
}

std::optional<fs::path> resolve_file_relative_to(const fs::path& root,
                                                 const fs::path& requested,
                                                 std::string& error) {
    fs::path candidate = requested;
    if (candidate.is_relative()) {
        candidate = root / candidate;
    }
    return canonical_existing_file(candidate, error);
}

std::optional<fs::path> resolve_workspace_or_external_file(const fs::path& requested,
                                                           const ToolContext& context,
                                                           std::string& error) {
    if (requested.is_absolute()) {
        return canonical_existing_file(requested, error);
    }
    return resolve_workspace_path(requested, context, error);
}

std::optional<fs::path> source_repo_root() {
    fs::path source_path = fs::path(__FILE__);
    for (int depth = 0; depth < 4 && !source_path.empty(); ++depth) {
        source_path = source_path.parent_path();
    }
    if (source_path.empty()) {
        return std::nullopt;
    }
    return source_path;
}

std::optional<fs::path> write_temp_text_file(const std::string& prefix,
                                             const std::string& extension,
                                             const std::string& content,
                                             std::string& error) {
    std::error_code ec;
    fs::path temp_root = fs::temp_directory_path(ec);
    if (ec) {
        error = "failed to locate temp directory: " + ec.message();
        return std::nullopt;
    }

    temp_root /= "omniagent-engine";
    fs::create_directories(temp_root, ec);
    if (ec) {
        error = "failed to create temp directory '" + temp_root.string() + "': " + ec.message();
        return std::nullopt;
    }

    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<unsigned long long> dist;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path candidate = temp_root
            / (prefix + "-" + std::to_string(stamp) + "-" + std::to_string(dist(rng)) + extension);
        if (fs::exists(candidate)) {
            continue;
        }

        std::ofstream stream(candidate, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            continue;
        }
        stream << content;
        if (!stream.good()) {
            std::error_code remove_error;
            fs::remove(candidate, remove_error);
            error = "failed to write temporary file '" + candidate.string() + "'";
            return std::nullopt;
        }
        return candidate;
    }

    error = "failed to create temporary validation file";
    return std::nullopt;
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
                                               bool allow_missing_leaf) {
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

    std::vector<fs::path> candidates;
    if (const auto configured_bridge = getenv_path("OMNIAGENT_PLANNER_BRIDGE"); configured_bridge.has_value()) {
        candidates.push_back(*configured_bridge);
    }
    candidates.push_back(*root / "planner-harness" / "bridge.py");
    if (const auto repo_root = getenv_path("OMNIAGENT_REPO_ROOT"); repo_root.has_value()) {
        candidates.push_back(*repo_root / "planner-harness" / "bridge.py");
    }
    if (const auto engine_root = getenv_path("OMNIAGENT_ENGINE_ROOT"); engine_root.has_value()) {
        candidates.push_back(*engine_root / "planner-harness" / "bridge.py");
        candidates.push_back(engine_root->parent_path() / "planner-harness" / "bridge.py");
    }
    if (const auto source_root = source_repo_root(); source_root.has_value()) {
        candidates.push_back(*source_root / "planner-harness" / "bridge.py");
    }

    for (const auto& candidate : candidates) {
        std::string candidate_error;
        const auto bridge = canonical_existing_file(candidate, candidate_error);
        if (bridge) {
            return bridge;
        }
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

    error = "could not locate planner-harness/bridge.py under the workspace root or OMNIAGENT_PLANNER_BRIDGE/OMNIAGENT_REPO_ROOT fallbacks";
    return std::nullopt;
}

std::optional<fs::path> locate_planner_harness_root(const ToolContext& context,
                                                    std::string& error) {
    const auto bridge = locate_planner_bridge(context, error);
    if (!bridge) {
        return std::nullopt;
    }
    return bridge->parent_path();
}

std::string case_filename_from_id(const std::string& case_id) {
    return case_id.size() >= 5 && case_id.substr(case_id.size() - 5) == ".json"
        ? case_id
        : case_id + ".json";
}

std::optional<fs::path> resolve_tracked_case_path(ValidationCaseKind kind,
                                                  const json& args,
                                                  const ToolContext& context,
                                                  std::string& error) {
    const auto harness_root = locate_planner_harness_root(context, error);
    if (!harness_root) {
        return std::nullopt;
    }
    const fs::path case_root = *harness_root / "tests" / "data" / validation_case_directory(kind);

    if (args.contains("case_path") && !args.at("case_path").is_null()) {
        if (!args.at("case_path").is_string()) {
            error = "'case_path' must be a string";
            return std::nullopt;
        }

        const fs::path requested = fs::path(args.at("case_path").get<std::string>());
        if (requested.is_absolute()) {
            return canonical_existing_file(requested, error);
        }

        std::string workspace_error;
        if (const auto workspace_path = resolve_workspace_path(requested, context, workspace_error)) {
            return workspace_path;
        }

        std::string tracked_error;
        const auto tracked_path = resolve_file_relative_to(case_root, requested, tracked_error);
        if (tracked_path) {
            return tracked_path;
        }

        error = tracked_error.empty() ? workspace_error : tracked_error;
        return std::nullopt;
    }

    if (args.contains("case_id") && !args.at("case_id").is_null()) {
        if (!args.at("case_id").is_string()) {
            error = "'case_id' must be a string";
            return std::nullopt;
        }
        const fs::path requested = case_filename_from_id(args.at("case_id").get<std::string>());
        return resolve_file_relative_to(case_root, requested, error);
    }

    if (kind != ValidationCaseKind::Review) {
        error = "either 'case_id' or 'case_path' is required";
        return std::nullopt;
    }

    std::vector<std::string> tokens;
    if (!context.project_id.empty()) {
        tokens.push_back(lowercase_copy(context.project_id));
    }
    if (const auto workspace_root = canonical_workspace_root(context, error); workspace_root.has_value()) {
        const std::string workspace_name = lowercase_copy(workspace_root->filename().string());
        if (!workspace_name.empty()
            && std::find(tokens.begin(), tokens.end(), workspace_name) == tokens.end()) {
            tokens.push_back(workspace_name);
        }
    }

    if (tokens.empty()) {
        error = "could not infer a tracked review case from the workspace; provide 'case_id' or 'case_path'";
        return std::nullopt;
    }

    std::vector<fs::path> matches;
    std::error_code ec;
    for (fs::directory_iterator it(case_root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            ec.clear();
            continue;
        }
        const fs::path candidate = it->path();
        if (candidate.extension() != ".json") {
            continue;
        }

        const std::string stem = lowercase_copy(candidate.stem().string());
        const bool matched = std::any_of(tokens.begin(), tokens.end(), [&](const std::string& token) {
            return !token.empty() && (stem.rfind(token, 0) == 0 || stem.find(token) != std::string::npos);
        });
        if (matched) {
            matches.push_back(candidate);
        }
    }

    if (matches.size() == 1) {
        return canonical_existing_file(matches.front(), error);
    }
    if (matches.empty()) {
        error = "no tracked review case matched the current workspace; provide 'case_id' or 'case_path'";
        return std::nullopt;
    }

    error = "multiple tracked review cases matched the current workspace; provide 'case_id' or 'case_path'";
    return std::nullopt;
}

std::optional<fs::path> resolve_validation_report_path(ValidationCaseKind kind,
                                                       const json& args,
                                                       const ToolContext& context,
                                                       ScopedTempPath& temp_report,
                                                       bool& used_inline_report,
                                                       std::string& error) {
    const bool has_report_path = args.contains("report_path") && !args.at("report_path").is_null();
    const bool has_report_text = args.contains("report_text") && !args.at("report_text").is_null();
    if (has_report_path == has_report_text) {
        error = "exactly one of 'report_path' or 'report_text' must be provided";
        return std::nullopt;
    }

    if (has_report_path) {
        if (!args.at("report_path").is_string()) {
            error = "'report_path' must be a string";
            return std::nullopt;
        }
        return resolve_workspace_or_external_file(
            fs::path(args.at("report_path").get<std::string>()),
            context,
            error);
    }

    if (!args.at("report_text").is_string()) {
        error = "'report_text' must be a string";
        return std::nullopt;
    }

    const auto temp_path = write_temp_text_file(
        kind == ValidationCaseKind::Review ? "planner-review-report" : "planner-bugfix-report",
        ".md",
        args.at("report_text").get<std::string>(),
        error);
    if (!temp_path) {
        return std::nullopt;
    }

    used_inline_report = true;
    temp_report = ScopedTempPath(*temp_path);
    return temp_report.path;
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

std::optional<fs::path> optional_workspace_or_external_path(const json& args,
                                                            const char* key,
                                                            const ToolContext& context,
                                                            std::string& error) {
    if (!args.contains(key) || args.at(key).is_null()) {
        return std::nullopt;
    }
    if (!args.at(key).is_string()) {
        error = std::string{"'"} + key + "' must be a string";
        return std::nullopt;
    }
    return resolve_workspace_or_external_file(fs::path(args.at(key).get<std::string>()), context, error);
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

bool clarification_required(const json& payload) {
    if (payload.contains("clarification_required") && payload["clarification_required"].is_boolean()) {
        return payload["clarification_required"].get<bool>();
    }
    if (payload.contains("clarification") && payload["clarification"].is_object()) {
        return payload["clarification"].value("clarification_required", false);
    }
    return false;
}

json clarification_block(const json& payload) {
    if (payload.contains("clarification") && payload["clarification"].is_object()) {
        return payload["clarification"];
    }
    return payload;
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

std::optional<json> load_json_file(const fs::path& path) {
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open()) {
            return std::nullopt;
        }
        return json::parse(stream);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string trim_copy(std::string value) {
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), [](char ch) {
                    return !std::isspace(static_cast<unsigned char>(ch));
                }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](char ch) {
                    return !std::isspace(static_cast<unsigned char>(ch));
                }).base(),
                value.end());
    return value;
}

std::string join_strings(const std::vector<std::string>& values,
                         const std::string& separator) {
    std::ostringstream stream;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            stream << separator;
        }
        stream << values[index];
    }
    return stream.str();
}

std::optional<std::string> failed_stage_check_detail(const json& payload,
                                                     const std::string& check_name) {
    if (!payload.contains("stage") || !payload["stage"].is_object()) {
        return std::nullopt;
    }
    const auto& stage = payload["stage"];
    if (!stage.contains("rubric_checks") || !stage["rubric_checks"].is_array()) {
        return std::nullopt;
    }
    for (const auto& check : stage["rubric_checks"]) {
        if (!check.is_object() || check.value("passed", true)) {
            continue;
        }
        if (check.value("name", std::string{}) != check_name) {
            continue;
        }
        return check.value("detail", std::string{});
    }
    return std::nullopt;
}

std::vector<std::string> comma_separated_items(const std::string& text) {
    std::vector<std::string> items;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ',')) {
        item = trim_copy(std::move(item));
        if (!item.empty()) {
            items.push_back(std::move(item));
        }
    }
    return items;
}

std::vector<std::string> missing_review_finding_ids(const json& payload) {
    const auto detail = failed_stage_check_detail(payload, "Required findings covered");
    if (!detail.has_value()) {
        return {};
    }
    constexpr const char* kPrefix = "Missing findings:";
    if (detail->rfind(kPrefix, 0) != 0) {
        return {};
    }
    return comma_separated_items(detail->substr(std::strlen(kPrefix)));
}

std::string format_required_groups(const json& required_groups) {
    if (!required_groups.is_array()) {
        return {};
    }

    std::vector<std::string> group_summaries;
    for (const auto& group : required_groups) {
        if (!group.is_array()) {
            continue;
        }
        std::vector<std::string> terms;
        for (const auto& value : group) {
            if (!value.is_string()) {
                continue;
            }
            const std::string term = trim_copy(value.get<std::string>());
            if (!term.empty()) {
                terms.push_back(term);
            }
        }
        if (!terms.empty()) {
            group_summaries.push_back(join_strings(terms, " | "));
        }
    }
    return join_strings(group_summaries, "; ");
}

void append_review_case_requirements(std::string& content,
                                     const json& payload,
                                     const fs::path& case_path) {
    const auto case_data = load_json_file(case_path);
    if (!case_data.has_value() || !case_data->is_object()) {
        return;
    }

    std::ostringstream stream;
    bool wrote_anything = false;

    const auto baseline_detail = failed_stage_check_detail(payload, "Baseline reflected");
    if (case_data->contains("baseline") && (*case_data)["baseline"].is_object()) {
        const auto& baseline = (*case_data)["baseline"];
        if (baseline.contains("required_terms") && baseline["required_terms"].is_array()) {
            std::vector<std::string> terms;
            for (const auto& value : baseline["required_terms"]) {
                if (!value.is_string()) {
                    continue;
                }
                const std::string term = trim_copy(value.get<std::string>());
                if (!term.empty()) {
                    terms.push_back(term);
                }
            }
            if (!terms.empty()) {
                stream << "tracked_case_requirements:\n";
                stream << "- Baseline terms required";
                if (baseline_detail.has_value() && !baseline_detail->empty()) {
                    stream << " (validator failed: " << *baseline_detail << ")";
                }
                stream << ": " << join_strings(terms, ", ") << "\n";
                wrote_anything = true;
            }
        }
    }

    const int min_required_clusters = case_data->value("min_required_clusters", 0);
    if (min_required_clusters > 0) {
        if (!wrote_anything) {
            stream << "tracked_case_requirements:\n";
        }
        stream << "- Minimum distinct clusters required: " << min_required_clusters << "\n";
        wrote_anything = true;
    }

    if (case_data->contains("required_findings") && (*case_data)["required_findings"].is_array()) {
        const std::vector<std::string> missing_ids = missing_review_finding_ids(payload);
        const bool cluster_check_failed = failed_stage_check_detail(payload, "Distinct clusters preserved").has_value();
        const std::unordered_set<std::string> missing_set(missing_ids.begin(), missing_ids.end());
        bool wrote_findings = false;
        for (const auto& finding : (*case_data)["required_findings"]) {
            if (!finding.is_object()) {
                continue;
            }
            const std::string finding_id = finding.value("id", std::string{});
            if (!cluster_check_failed
                && !missing_set.empty()
                && missing_set.find(finding_id) == missing_set.end()) {
                continue;
            }

            if (!wrote_anything) {
                stream << "tracked_case_requirements:\n";
                wrote_anything = true;
            }
            if (!wrote_findings) {
                stream << "- Required findings to cover:\n";
                wrote_findings = true;
            }

            stream << "  - " << finding_id;
            const std::string severity = finding.value("severity", std::string{});
            const std::string cluster = finding.value("cluster", std::string{});
            if (!severity.empty() || !cluster.empty()) {
                stream << " [";
                if (!severity.empty()) {
                    stream << severity;
                }
                if (!severity.empty() && !cluster.empty()) {
                    stream << "/";
                }
                if (!cluster.empty()) {
                    stream << cluster;
                }
                stream << "]";
            }
            if (finding.contains("required_groups")) {
                const std::string anchors = format_required_groups(finding["required_groups"]);
                if (!anchors.empty()) {
                    stream << ": " << anchors;
                }
            }
            stream << "\n";
        }
    }

    if (!wrote_anything) {
        return;
    }

    const std::string guidance = stream.str();
    const std::string marker = "raw_json:\n";
    const std::size_t marker_pos = content.find(marker);
    if (marker_pos == std::string::npos) {
        if (!content.empty() && content.back() != '\n') {
            content.push_back('\n');
        }
        content += guidance;
        return;
    }

    content.insert(marker_pos, guidance);
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

    if (clarification_required(payload)) {
        const json clar = clarification_block(payload);
        std::ostringstream stream;
        stream << status_name << " STATUS: CLARIFICATION_REQUIRED\n";
        stream << primary_flag_name << ": false\n";
        stream << "clarification_required: true\n";
        if (clar.contains("clarification_mode") && clar["clarification_mode"].is_string()) {
            stream << "clarification_mode: " << clar["clarification_mode"].get<std::string>() << "\n";
        }
        if (clar.contains("pending_clarification_ids") && clar["pending_clarification_ids"].is_array()) {
            stream << "pending_clarifications: " << clar["pending_clarification_ids"].size() << "\n";
        }
        const std::string message = clar.value("clarification_message", std::string{});
        if (!message.empty()) {
            stream << "clarification_message: " << message << "\n";
        }

        stream << "questions:\n";
        if (clar.contains("clarifications") && clar["clarifications"].is_array()) {
            for (const auto& item : clar["clarifications"]) {
                if (!item.is_object()) {
                    continue;
                }
                const std::string qid = item.value("id", std::string{"unknown"});
                const std::string question = item.value("question", std::string{});
                stream << "- " << qid << ": " << question << "\n";
            }
        }
        stream << "raw_json:\n" << dump_payload(payload);
        return {stream.str(), true};
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

ToolCallResult run_case_validation_tool(ValidationCaseKind kind,
                                        const json& args,
                                        const ToolContext& context,
                                        int default_timeout_ms) {
    std::string error;
    const auto case_path = resolve_tracked_case_path(kind, args, context, error);
    if (!case_path) {
        return {error, true};
    }

    ScopedTempPath temp_report;
    bool used_inline_report = false;
    const auto report_path = resolve_validation_report_path(
        kind,
        args,
        context,
        temp_report,
        used_inline_report,
        error);
    if (!report_path) {
        return {error, true};
    }

    const auto config_path = optional_workspace_or_external_path(args, "config_path", context, error);
    if (!error.empty()) {
        return {error, true};
    }

    const int timeout_ms = resolve_timeout(args, default_timeout_ms);
    std::vector<std::string> bridge_args = {
        validation_bridge_command(kind),
        case_path->string(),
        report_path->string(),
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

    if (used_inline_report) {
        payload["report_source"] = "inline_text";
        payload["report_path"] = "[inline report text]";
    }

    const bool overall_passed = object_flag(payload, "stage");
    ToolCallResult final_result = finalize_planner_result(
        validation_status_name(kind),
        validation_primary_flag(kind),
        overall_passed,
        payload);
    if (final_result.is_error && kind == ValidationCaseKind::Review) {
        append_review_case_requirements(final_result.content, payload, *case_path);
    }
    return final_result;
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

std::string PlannerValidateReviewTool::name() const {
    return "planner_validate_review";
}

std::string PlannerValidateReviewTool::description() const {
    return "Validate an audit or review report against a tracked planner-harness review case.";
}

nlohmann::json PlannerValidateReviewTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"case_id", {{"type", "string"}, {"description", "Tracked review case id or filename. When omitted, the tool tries to match the current workspace name to a tracked review case."}}},
            {"case_path", {{"type", "string"}, {"description", "Optional absolute path, workspace-relative path, or tracked-case-relative path to the review case JSON."}}},
            {"report_path", {{"type", "string"}, {"description", "Path to the review report text file. Can be workspace-relative or absolute."}}},
            {"report_text", {{"type", "string"}, {"description", "Inline review report text to validate without writing a workspace file."}}},
            {"config_path", {{"type", "string"}, {"description", "Optional planner-harness config path. Can be workspace-relative or absolute."}}},
            {"model", {{"type", "string"}, {"description", "Optional planner-harness model name override."}}},
            {"skip_adversary", {{"type", "boolean"}, {"description", "Skip the LLM adversary pass and run rubric checks only."}, {"default", true}}},
            {"timeout", {{"type", "integer"}, {"description", "Maximum execution time in milliseconds."}, {"default", kDefaultTimeoutMs}}}
        }}
    };
}

ToolCallResult PlannerValidateReviewTool::call(const json& args,
                                               const ToolContext& context) {
    return run_case_validation_tool(ValidationCaseKind::Review, args, context, kDefaultTimeoutMs);
}

ToolCallResult PlannerValidateReviewTool::call(const json&) {
    return missing_context(name().c_str());
}

std::string PlannerValidateBugfixTool::name() const {
    return "planner_validate_bugfix";
}

std::string PlannerValidateBugfixTool::description() const {
    return "Validate a bugfix writeup against a tracked planner-harness bugfix case.";
}

nlohmann::json PlannerValidateBugfixTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"case_id", {{"type", "string"}, {"description", "Tracked bugfix case id or filename."}}},
            {"case_path", {{"type", "string"}, {"description", "Optional absolute path, workspace-relative path, or tracked-case-relative path to the bugfix case JSON."}}},
            {"report_path", {{"type", "string"}, {"description", "Path to the bugfix report text file. Can be workspace-relative or absolute."}}},
            {"report_text", {{"type", "string"}, {"description", "Inline bugfix writeup text to validate without writing a workspace file."}}},
            {"config_path", {{"type", "string"}, {"description", "Optional planner-harness config path. Can be workspace-relative or absolute."}}},
            {"model", {{"type", "string"}, {"description", "Optional planner-harness model name override."}}},
            {"skip_adversary", {{"type", "boolean"}, {"description", "Skip the LLM adversary pass and run rubric checks only."}, {"default", true}}},
            {"timeout", {{"type", "integer"}, {"description", "Maximum execution time in milliseconds."}, {"default", kDefaultTimeoutMs}}}
        }}
    };
}

ToolCallResult PlannerValidateBugfixTool::call(const json& args,
                                               const ToolContext& context) {
    return run_case_validation_tool(ValidationCaseKind::Bugfix, args, context, kDefaultTimeoutMs);
}

ToolCallResult PlannerValidateBugfixTool::call(const json&) {
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
            {"skip_adversary", {{"type", "boolean"}, {"description", "Skip LLM adversary checks during validation."}, {"default", false}}},
            {"clarification_mode", {{"type", "string"}, {"description", "Clarification policy: required, assume, or off."}, {"default", "required"}}},
            {"clarification_answers", {{"description", "Optional structured answers map (or array of {id, value}) for pending clarifications."}}},
            {"clarification_answers_text", {{"type", "string"}, {"description", "Optional conversational answer text for one or many pending clarifications."}}},
            {"delegate_unanswered", {{"type", "boolean"}, {"description", "Apply recommended defaults for unresolved clarification questions."}, {"default", false}}},
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
    if (args.value("skip_adversary", false)) {
        bridge_args.push_back("--skip-adversary");
    }
    bridge_args.push_back("--clarification-mode");
    bridge_args.push_back(args.value("clarification_mode", std::string{"required"}));
    if (args.contains("clarification_answers") && !args.at("clarification_answers").is_null()) {
        bridge_args.push_back("--answers-json-text");
        bridge_args.push_back(args.at("clarification_answers").dump());
    }
    if (args.contains("clarification_answers_text") && args.at("clarification_answers_text").is_string()) {
        bridge_args.push_back("--answers-text");
        bridge_args.push_back(args.at("clarification_answers_text").get<std::string>());
    }
    if (args.value("delegate_unanswered", false)) {
        bridge_args.push_back("--delegate-unanswered");
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
            {"skip_adversary", {{"type", "boolean"}, {"description", "Skip LLM adversary checks during spec and plan validation."}, {"default", false}}},
            {"clarification_mode", {{"type", "string"}, {"description", "Clarification policy: required, assume, or off."}, {"default", "required"}}},
            {"clarification_answers", {{"description", "Optional structured answers map (or array of {id, value}) for pending clarifications."}}},
            {"clarification_answers_text", {{"type", "string"}, {"description", "Optional conversational answer text for one or many pending clarifications."}}},
            {"delegate_unanswered", {{"type", "boolean"}, {"description", "Apply recommended defaults for unresolved clarification questions."}, {"default", false}}},
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
    if (args.value("skip_adversary", false)) {
        bridge_args.push_back("--skip-adversary");
    }
    bridge_args.push_back("--clarification-mode");
    bridge_args.push_back(args.value("clarification_mode", std::string{"required"}));
    if (args.contains("clarification_answers") && !args.at("clarification_answers").is_null()) {
        bridge_args.push_back("--answers-json-text");
        bridge_args.push_back(args.at("clarification_answers").dump());
    }
    if (args.contains("clarification_answers_text") && args.at("clarification_answers_text").is_string()) {
        bridge_args.push_back("--answers-text");
        bridge_args.push_back(args.at("clarification_answers_text").get<std::string>());
    }
    if (args.value("delegate_unanswered", false)) {
        bridge_args.push_back("--delegate-unanswered");
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