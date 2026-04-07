#include "workspace_tools.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace omni::engine {

namespace {

namespace fs = std::filesystem;

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
    const fs::path base = context.working_dir.empty()
        ? root
        : context.working_dir;
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

bool file_looks_binary(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }

    char buffer[1024];
    stream.read(buffer, sizeof(buffer));
    const std::streamsize read_count = stream.gcount();
    return std::find(buffer, buffer + read_count, '\0') != buffer + read_count;
}

std::string read_file_bytes(const fs::path& path,
                            std::size_t max_bytes,
                            bool& truncated,
                            std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "failed to open file '" + path.string() + "'";
        return {};
    }

    std::string content;
    content.resize(max_bytes + 1);
    stream.read(content.data(), static_cast<std::streamsize>(max_bytes + 1));
    const std::streamsize read_count = stream.gcount();
    truncated = static_cast<std::size_t>(read_count) > max_bytes;
    content.resize(static_cast<std::size_t>(std::min<std::streamsize>(read_count, max_bytes)));
    return content;
}

std::vector<std::string> split_lines(const std::string& content) {
    std::vector<std::string> lines;
    std::stringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    if (!content.empty() && content.back() == '\n') {
        return lines;
    }
    if (lines.empty() && !content.empty()) {
        lines.push_back(content);
    }
    return lines;
}

std::string format_line_range(const std::vector<std::string>& lines,
                              std::size_t start_line,
                              std::size_t end_line,
                              bool truncated) {
    std::ostringstream output;
    for (std::size_t index = start_line; index <= end_line && index <= lines.size(); ++index) {
        output << index << ": " << lines[index - 1] << '\n';
    }
    if (truncated) {
        output << "[truncated]\n";
    }
    return output.str();
}

std::string escape_regex_char(char ch) {
    switch (ch) {
        case '.':
        case '^':
        case '$':
        case '+':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case '|':
        case '\\':
            return std::string{"\\"} + ch;
        default:
            return std::string(1, ch);
    }
}

std::regex glob_to_regex(const std::string& pattern) {
    std::string regex = "^";
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const char ch = pattern[index];
        if (ch == '*') {
            if (index + 1 < pattern.size() && pattern[index + 1] == '*') {
                if (index + 2 < pattern.size() && pattern[index + 2] == '/') {
                    regex += "(?:.*/)?";
                    index += 2;
                } else {
                    regex += ".*";
                    ++index;
                }
            } else {
                regex += "[^/]*";
            }
            continue;
        }
        if (ch == '?') {
            regex += "[^/]";
            continue;
        }
        if (ch == '\\') {
            regex += "/";
            continue;
        }
        regex += escape_regex_char(ch);
    }
    regex += "$";
    return std::regex(regex, std::regex::ECMAScript | std::regex::optimize);
}

std::optional<int> parse_optional_int(const nlohmann::json& value) {
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

#ifndef _WIN32
ToolCallResult run_bash_command(const std::string& command,
                                int timeout_ms,
                                const fs::path& working_dir) {
    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        return {"failed to create pipe: " + std::string(std::strerror(errno)), true};
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return {"failed to fork shell process: " + std::string(std::strerror(errno)), true};
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

        execl("/bin/bash", "bash", "-lc", command.c_str(), nullptr);
        std::fprintf(stderr, "failed to exec bash: %s\n", std::strerror(errno));
        _exit(127);
    }

    close(pipe_fds[1]);

    std::string output;
    std::size_t total_bytes = 0;
    bool timed_out = false;
    bool truncated = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buffer[4096];

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timed_out = true;
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
            return {"poll failed: " + std::string(std::strerror(errno)), true};
        }
        if (poll_result == 0) {
            timed_out = true;
            break;
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            const ssize_t read_count = read(pipe_fds[0], buffer, sizeof(buffer));
            if (read_count > 0) {
                total_bytes += static_cast<std::size_t>(read_count);
                if (output.size() < BashTool::kMaxOutputBytes) {
                    const std::size_t remaining_capacity = BashTool::kMaxOutputBytes - output.size();
                    output.append(buffer, buffer + std::min<std::size_t>(remaining_capacity,
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
            return {"read failed: " + std::string(std::strerror(errno)), true};
        }
        if (read_count == 0) {
            break;
        }

        total_bytes += static_cast<std::size_t>(read_count);
        if (output.size() < BashTool::kMaxOutputBytes) {
            const std::size_t remaining_capacity = BashTool::kMaxOutputBytes - output.size();
            output.append(buffer, buffer + std::min<std::size_t>(remaining_capacity,
                                                                 static_cast<std::size_t>(read_count)));
            truncated = truncated || static_cast<std::size_t>(read_count) > remaining_capacity;
        } else {
            truncated = true;
        }
    }

    close(pipe_fds[0]);

    int status = 0;
    if (timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        if (truncated) {
            output += "\n[output truncated]";
        }
        output += "\n[command timed out after " + std::to_string(timeout_ms) + " ms]";
        return {output, true};
    }

    waitpid(pid, &status, 0);
    if (truncated) {
        output += "\n[output truncated]";
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return {output, false};
    }

    std::ostringstream error_output;
    error_output << output;
    if (!output.empty() && output.back() != '\n') {
        error_output << '\n';
    }
    if (WIFEXITED(status)) {
        error_output << "[exit_code: " << WEXITSTATUS(status) << ']';
    } else if (WIFSIGNALED(status)) {
        error_output << "[terminated by signal: " << WTERMSIG(status) << ']';
    } else {
        error_output << "[command failed]";
    }
    return {error_output.str(), true};
}
#endif

}  // namespace

std::string ReadFileTool::name() const { return "read_file"; }

std::string ReadFileTool::description() const {
    return "Read a text file from the project workspace. Use start_line and end_line to limit large files.";
}

nlohmann::json ReadFileTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"path", {{"type", "string"}, {"description", "Path to the file, relative to the current working directory or absolute inside the workspace."}}},
            {"start_line", {{"type", "integer"}, {"description", "1-based start line. Defaults to 1."}, {"default", 1}}},
            {"end_line", {{"type", "integer"}, {"description", "1-based inclusive end line. Defaults to the end of the file or the truncation limit."}}}
        }},
        {"required", nlohmann::json::array({"path"})}
    };
}

ToolCallResult ReadFileTool::call(const nlohmann::json& args,
                                  const ToolContext& context) {
    std::string error;
    const auto resolved = resolve_workspace_path(args.at("path").get<std::string>(),
                                                 context,
                                                 error);
    if (!resolved) {
        return {error, true};
    }
    if (!fs::is_regular_file(*resolved)) {
        return {"not a regular file: " + resolved->string(), true};
    }
    if (file_looks_binary(*resolved)) {
        return {"refusing to read binary file: " + resolved->string(), true};
    }

    bool truncated = false;
    std::string read_error;
    const std::string content = read_file_bytes(*resolved, kMaxBytes, truncated, read_error);
    if (!read_error.empty()) {
        return {read_error, true};
    }

    const std::vector<std::string> lines = split_lines(content);
    const std::size_t start_line = [&]() {
        if (!args.contains("start_line")) {
            return std::size_t{1};
        }
        const auto value = parse_optional_int(args.at("start_line"));
        return value.has_value() && *value > 0 ? static_cast<std::size_t>(*value) : std::size_t{1};
    }();
    const std::size_t end_line = [&]() {
        if (!args.contains("end_line")) {
            return lines.empty() ? std::size_t{0} : lines.size();
        }
        const auto value = parse_optional_int(args.at("end_line"));
        if (!value.has_value() || *value < 1) {
            return lines.empty() ? std::size_t{0} : lines.size();
        }
        return static_cast<std::size_t>(*value);
    }();

    if (start_line > lines.size() && !lines.empty()) {
        return {"start_line is past the end of the file", true};
    }

    std::ostringstream output;
    output << relative_to_root(*resolved, context) << '\n';
    if (!lines.empty() && end_line >= start_line) {
        output << format_line_range(lines, start_line, std::min(end_line, lines.size()), truncated);
    } else if (lines.empty()) {
        output << "[empty file]\n";
    }
    return {output.str(), false};
}

ToolCallResult ReadFileTool::call(const nlohmann::json&) {
    return missing_context(name().c_str());
}

std::string WriteFileTool::name() const { return "write_file"; }

std::string WriteFileTool::description() const {
    return "Create or overwrite a text file inside the project workspace. Parent directories are created automatically.";
}

nlohmann::json WriteFileTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"path", {{"type", "string"}, {"description", "Target file path inside the workspace."}}},
            {"content", {{"type", "string"}, {"description", "Full file contents to write."}}}
        }},
        {"required", nlohmann::json::array({"path", "content"})}
    };
}

ToolCallResult WriteFileTool::call(const nlohmann::json& args,
                                   const ToolContext& context) {
    std::string error;
    const auto resolved = resolve_workspace_path(args.at("path").get<std::string>(),
                                                 context,
                                                 error,
                                                 true);
    if (!resolved) {
        return {error, true};
    }

    std::error_code ec;
    fs::create_directories(resolved->parent_path(), ec);
    if (ec) {
        return {"failed to create parent directories: " + ec.message(), true};
    }

    std::ofstream stream(*resolved, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return {"failed to open file for writing: " + resolved->string(), true};
    }
    const std::string content = args.at("content").get<std::string>();
    stream << content;
    if (!stream) {
        return {"failed to write file: " + resolved->string(), true};
    }
    return {"wrote " + relative_to_root(*resolved, context), false};
}

ToolCallResult WriteFileTool::call(const nlohmann::json&) {
    return missing_context(name().c_str());
}

std::string EditFileTool::name() const { return "edit_file"; }

std::string EditFileTool::description() const {
    return "Replace an exact string inside a text file in the project workspace.";
}

nlohmann::json EditFileTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"path", {{"type", "string"}, {"description", "Target file path inside the workspace."}}},
            {"old_string", {{"type", "string"}, {"description", "Exact text to replace."}}},
            {"new_string", {{"type", "string"}, {"description", "Replacement text."}}},
            {"replace_all", {{"type", "boolean"}, {"description", "Replace all occurrences instead of exactly one."}, {"default", false}}}
        }},
        {"required", nlohmann::json::array({"path", "old_string", "new_string"})}
    };
}

ToolCallResult EditFileTool::call(const nlohmann::json& args,
                                  const ToolContext& context) {
    std::string error;
    const auto resolved = resolve_workspace_path(args.at("path").get<std::string>(),
                                                 context,
                                                 error);
    if (!resolved) {
        return {error, true};
    }
    if (!fs::is_regular_file(*resolved)) {
        return {"not a regular file: " + resolved->string(), true};
    }
    if (file_looks_binary(*resolved)) {
        return {"refusing to edit binary file: " + resolved->string(), true};
    }

    bool truncated = false;
    std::string read_error;
    std::string content = read_file_bytes(*resolved, 4 * ReadFileTool::kMaxBytes, truncated, read_error);
    if (!read_error.empty()) {
        return {read_error, true};
    }
    if (truncated) {
        return {"refusing to edit very large file with exact replacement: " + resolved->string(), true};
    }

    const std::string old_string = args.at("old_string").get<std::string>();
    const std::string new_string = args.at("new_string").get<std::string>();
    const bool replace_all = args.value("replace_all", false);

    if (old_string.empty()) {
        return {"old_string must not be empty", true};
    }

    std::size_t replacements = 0;
    std::size_t position = 0;
    while ((position = content.find(old_string, position)) != std::string::npos) {
        content.replace(position, old_string.size(), new_string);
        position += new_string.size();
        ++replacements;
        if (!replace_all) {
            break;
        }
    }

    if (replacements == 0) {
        return {"old_string not found in " + relative_to_root(*resolved, context), true};
    }

    std::ofstream stream(*resolved, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return {"failed to open file for editing: " + resolved->string(), true};
    }
    stream << content;
    if (!stream) {
        return {"failed to write edited file: " + resolved->string(), true};
    }

    std::ostringstream output;
    output << "edited " << relative_to_root(*resolved, context)
           << " (replacements: " << replacements << ')';
    return {output.str(), false};
}

ToolCallResult EditFileTool::call(const nlohmann::json&) {
    return missing_context(name().c_str());
}

std::string DeleteFileTool::name() const { return "delete_file"; }

std::string DeleteFileTool::description() const {
    return "Delete a file or an empty directory inside the project workspace.";
}

nlohmann::json DeleteFileTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"path", {{"type", "string"}, {"description", "File or empty directory path inside the workspace."}}}
        }},
        {"required", nlohmann::json::array({"path"})}
    };
}

ToolCallResult DeleteFileTool::call(const nlohmann::json& args,
                                    const ToolContext& context) {
    std::string error;
    const auto resolved = resolve_workspace_path(args.at("path").get<std::string>(),
                                                 context,
                                                 error);
    if (!resolved) {
        return {error, true};
    }
    if (!fs::exists(*resolved)) {
        return {"path does not exist: " + resolved->string(), true};
    }

    std::error_code ec;
    const bool removed = fs::remove(*resolved, ec);
    if (ec) {
        return {"failed to delete path: " + ec.message(), true};
    }
    if (!removed) {
        return {"refusing to delete non-empty directory: " + relative_to_root(*resolved, context), true};
    }
    return {"deleted " + relative_to_root(*resolved, context), false};
}

ToolCallResult DeleteFileTool::call(const nlohmann::json&) {
    return missing_context(name().c_str());
}

std::string ListDirTool::name() const { return "list_dir"; }

std::string ListDirTool::description() const {
    return "List files and directories under a workspace path. Directory names end with /.";
}

nlohmann::json ListDirTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"path", {{"type", "string"}, {"description", "Directory path to list. Defaults to the current working directory."}, {"default", "."}}}
        }}
    };
}

ToolCallResult ListDirTool::call(const nlohmann::json& args,
                                 const ToolContext& context) {
    const std::string requested = args.value("path", std::string{"."});
    std::string error;
    const auto resolved = resolve_workspace_path(requested, context, error);
    if (!resolved) {
        return {error, true};
    }
    if (!fs::is_directory(*resolved)) {
        return {"not a directory: " + resolved->string(), true};
    }

    std::vector<std::string> entries;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(*resolved, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        std::string name = entry.path().filename().string();
        if (entry.is_directory(ec)) {
            name += '/';
        }
        entries.push_back(std::move(name));
        if (entries.size() >= kMaxEntries) {
            break;
        }
    }
    std::sort(entries.begin(), entries.end());

    std::ostringstream output;
    output << relative_to_root(*resolved, context) << '\n';
    for (const auto& entry : entries) {
        output << entry << '\n';
    }
    if (entries.size() >= kMaxEntries) {
        output << "[results capped]\n";
    }
    return {output.str(), false};
}

ToolCallResult ListDirTool::call(const nlohmann::json&) {
    return missing_context(name().c_str());
}

std::string GlobTool::name() const { return "glob"; }

std::string GlobTool::description() const {
    return "Find workspace files whose relative paths match a glob pattern such as src/**/*.cpp.";
}

nlohmann::json GlobTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"pattern", {{"type", "string"}, {"description", "Glob pattern to match relative file paths."}}},
            {"path", {{"type", "string"}, {"description", "Directory path to search from. Defaults to the current working directory."}, {"default", "."}}}
        }},
        {"required", nlohmann::json::array({"pattern"})}
    };
}

ToolCallResult GlobTool::call(const nlohmann::json& args,
                              const ToolContext& context) {
    const std::string pattern = args.at("pattern").get<std::string>();
    const std::string requested = args.value("path", std::string{"."});
    std::string error;
    const auto root = resolve_workspace_path(requested, context, error);
    if (!root) {
        return {error, true};
    }
    if (!fs::is_directory(*root)) {
        return {"not a directory: " + root->string(), true};
    }

    const std::regex matcher = glob_to_regex(pattern);
    std::vector<std::string> matches;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(*root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!entry.is_regular_file(ec)) {
            ec.clear();
            continue;
        }

        const fs::path relative = entry.path().lexically_relative(*root);
        const std::string candidate = relative.generic_string();
        if (std::regex_match(candidate, matcher)) {
            matches.push_back(relative_to_root(entry.path(), context));
            if (matches.size() >= kMaxResults) {
                break;
            }
        }
    }
    std::sort(matches.begin(), matches.end());

    if (matches.empty()) {
        return {"(no matches found)", false};
    }

    std::ostringstream output;
    for (const auto& match : matches) {
        output << match << '\n';
    }
    if (matches.size() >= kMaxResults) {
        output << "[results capped]\n";
    }
    return {output.str(), false};
}

ToolCallResult GlobTool::call(const nlohmann::json&) {
    return missing_context(name().c_str());
}

std::string GrepTool::name() const { return "grep"; }

std::string GrepTool::description() const {
    return "Search workspace files with a regular expression and return file:line:text matches.";
}

nlohmann::json GrepTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"pattern", {{"type", "string"}, {"description", "Regular expression to search for."}}},
            {"path", {{"type", "string"}, {"description", "File or directory to search. Defaults to the current working directory."}, {"default", "."}}},
            {"glob", {{"type", "string"}, {"description", "Optional glob filter for files inside a directory search."}}}
        }},
        {"required", nlohmann::json::array({"pattern"})}
    };
}

ToolCallResult GrepTool::call(const nlohmann::json& args,
                              const ToolContext& context) {
    const std::string pattern = args.at("pattern").get<std::string>();
    const std::string requested = args.value("path", std::string{"."});
    const std::string glob_filter = args.value("glob", std::string{});

    std::regex matcher;
    try {
        matcher = std::regex(pattern, std::regex::ECMAScript | std::regex::optimize);
    } catch (const std::regex_error& error) {
        return {"invalid regex: " + std::string(error.what()), true};
    }

    std::optional<std::regex> glob_matcher;
    if (!glob_filter.empty()) {
        glob_matcher = glob_to_regex(glob_filter);
    }

    std::string error;
    const auto root = resolve_workspace_path(requested, context, error);
    if (!root) {
        return {error, true};
    }

    std::vector<fs::path> files;
    std::error_code ec;
    if (fs::is_regular_file(*root)) {
        files.push_back(*root);
    } else if (fs::is_directory(*root)) {
        for (const auto& entry : fs::recursive_directory_iterator(*root, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (!entry.is_regular_file(ec)) {
                ec.clear();
                continue;
            }

            const fs::path relative = entry.path().lexically_relative(*root);
            if (glob_matcher && !std::regex_match(relative.generic_string(), *glob_matcher)) {
                continue;
            }
            files.push_back(entry.path());
        }
    } else {
        return {"path does not exist: " + root->string(), true};
    }

    std::sort(files.begin(), files.end());
    std::ostringstream output;
    for (const auto& file : files) {
        if (file_looks_binary(file)) {
            continue;
        }

        bool truncated = false;
        std::string read_error;
        const std::string content = read_file_bytes(file, 2 * ReadFileTool::kMaxBytes, truncated, read_error);
        if (!read_error.empty()) {
            continue;
        }

        const auto lines = split_lines(content);
        for (std::size_t line_number = 0; line_number < lines.size(); ++line_number) {
            if (!std::regex_search(lines[line_number], matcher)) {
                continue;
            }
            output << relative_to_root(file, context)
                   << ':' << (line_number + 1)
                   << ':' << lines[line_number] << '\n';
            if (output.tellp() >= static_cast<std::streamoff>(kMaxOutputBytes)) {
                output << "[output truncated]\n";
                return {output.str(), false};
            }
        }
        if (truncated) {
            output << relative_to_root(file, context) << ": [file truncated while searching]\n";
        }
    }

    if (output.tellp() == std::streampos(0)) {
        return {"(no matches found)", false};
    }
    return {output.str(), false};
}

ToolCallResult GrepTool::call(const nlohmann::json&) {
    return missing_context(name().c_str());
}

std::string BashTool::name() const { return "bash"; }

std::string BashTool::description() const {
    return "Execute a shell command inside the project workspace and return combined stdout and stderr. Use working_dir instead of cd when possible.";
}

nlohmann::json BashTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"command", {{"type", "string"}, {"description", "Shell command to execute."}}},
            {"timeout", {{"type", "integer"}, {"description", "Maximum execution time in milliseconds. Defaults to 120000."}, {"default", kDefaultTimeoutMs}}},
            {"working_dir", {{"type", "string"}, {"description", "Optional working directory inside the workspace."}}}
        }},
        {"required", nlohmann::json::array({"command"})}
    };
}

ToolCallResult BashTool::call(const nlohmann::json& args,
                              const ToolContext& context) {
    const std::string command = args.at("command").get<std::string>();
    const int timeout_ms = [&]() {
        if (!args.contains("timeout")) {
            return kDefaultTimeoutMs;
        }
        const auto parsed = parse_optional_int(args.at("timeout"));
        if (!parsed.has_value() || *parsed <= 0) {
            return kDefaultTimeoutMs;
        }
        return *parsed;
    }();

    const fs::path requested_cwd = args.contains("working_dir")
        ? fs::path(args.at("working_dir").get<std::string>())
        : fs::path{};
    std::string error;
    const auto resolved_cwd = resolve_workspace_path(requested_cwd,
                                                     context,
                                                     error);
    if (!resolved_cwd) {
        return {error, true};
    }
    if (!fs::is_directory(*resolved_cwd)) {
        return {"working_dir is not a directory: " + resolved_cwd->string(), true};
    }

#ifdef _WIN32
    (void)command;
    (void)timeout_ms;
    return {"bash tool is not implemented on Windows in omniagent-engine.", true};
#else
    return run_bash_command(command, timeout_ms, *resolved_cwd);
#endif
}

ToolCallResult BashTool::call(const nlohmann::json&) {
    return missing_context(name().c_str());
}

std::vector<std::unique_ptr<Tool>> make_default_workspace_tools() {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<ReadFileTool>());
    tools.push_back(std::make_unique<WriteFileTool>());
    tools.push_back(std::make_unique<EditFileTool>());
    tools.push_back(std::make_unique<DeleteFileTool>());
    tools.push_back(std::make_unique<GlobTool>());
    tools.push_back(std::make_unique<GrepTool>());
    tools.push_back(std::make_unique<BashTool>());
    tools.push_back(std::make_unique<ListDirTool>());
    tools.push_back(std::make_unique<WebFetchTool>());
    tools.push_back(std::make_unique<WebSearchTool>());
    tools.push_back(std::make_unique<PlannerValidateSpecTool>());
    tools.push_back(std::make_unique<PlannerValidatePlanTool>());
    tools.push_back(std::make_unique<PlannerRepairPlanTool>());
    tools.push_back(std::make_unique<PlannerBuildPlanTool>());
    tools.push_back(std::make_unique<PlannerBuildFromIdeaTool>());
    return tools;
}

}  // namespace omni::engine