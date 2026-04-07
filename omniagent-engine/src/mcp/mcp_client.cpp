#include "mcp_client.h"

#include <cstdint>
#include <optional>
#include <sstream>
#include <chrono>

#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

namespace omni::engine {

// ===========================================================================
// MCPClient
// ===========================================================================

MCPClient::MCPClient() = default;

MCPClient::~MCPClient() {
    if (connected_) {
        disconnect();
    }
}

// ---------------------------------------------------------------------------
// connect — spawn subprocess and perform initialize handshake
// ---------------------------------------------------------------------------

bool MCPClient::connect(const MCPServerConfig& config) {
    std::lock_guard lock(io_mutex_);

    if (connected_) {
        return true;
    }

    // Create two pipes: parent->child (stdin) and child->parent (stdout).
    int pipe_in[2]{-1, -1};   // parent writes [1], child reads [0]
    int pipe_out[2]{-1, -1};  // child writes [1], parent reads [0]

    if (pipe(pipe_in) != 0 || pipe(pipe_out) != 0) {
        return false;
    }

    child_pid_ = fork();
    if (child_pid_ < 0) {
        close(pipe_in[0]);  close(pipe_in[1]);
        close(pipe_out[0]); close(pipe_out[1]);
        return false;
    }

    if (child_pid_ == 0) {
        // --- Child process ---
        // Wire stdin/stdout to the pipes.
        dup2(pipe_in[0],  STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);

        close(pipe_in[0]);  close(pipe_in[1]);
        close(pipe_out[0]); close(pipe_out[1]);

        // Apply extra environment variables.
        for (const auto& [k, v] : config.env) {
            setenv(k.c_str(), v.c_str(), 1);
        }

        // Build argv: command + args + null terminator.
        std::vector<const char*> argv;
        argv.push_back(config.command.c_str());
        for (const auto& a : config.args) {
            argv.push_back(a.c_str());
        }
        argv.push_back(nullptr);

        execvp(config.command.c_str(), const_cast<char**>(argv.data()));
        _exit(127);  // exec failed
    }

    // --- Parent process ---
    close(pipe_in[0]);
    close(pipe_out[1]);

    stdin_write_fd_  = pipe_in[1];
    stdout_read_fd_ = pipe_out[0];
    name_           = config.name;
    connected_      = true;
    degraded_       = false;
    init_timeout_   = std::chrono::duration_cast<std::chrono::milliseconds>(config.init_timeout);
    request_timeout_ = std::chrono::duration_cast<std::chrono::milliseconds>(config.request_timeout);

    // ---- JSON-RPC initialize handshake ----
    nlohmann::json init_params = {
        {"protocolVersion", "2024-11-05"},
        {"capabilities",    nlohmann::json::object()},
        {"clientInfo",      {{"name", "omni-engine"}, {"version", "0.1.0"}}},
    };

    auto result = send_request("initialize", init_params, init_timeout_);
    if (!result) {
        // Handshake failed: clean up.
        connected_ = false;
        degraded_ = true;
        kill(child_pid_, SIGTERM);
        waitpid(child_pid_, nullptr, 0);
        child_pid_ = -1;
        close(stdin_write_fd_);  stdin_write_fd_  = -1;
        close(stdout_read_fd_); stdout_read_fd_ = -1;
        return false;
    }

    // Send initialized notification (fire-and-forget).
    send_notification("notifications/initialized", nlohmann::json::object());

    return true;
}

// ---------------------------------------------------------------------------
// disconnect
// ---------------------------------------------------------------------------

void MCPClient::disconnect() {
    std::lock_guard lock(io_mutex_);

    if (!connected_) return;
    connected_ = false;

    // Best-effort shutdown notification.
    send_notification("shutdown", nlohmann::json::object());

    if (child_pid_ > 0) {
        kill(child_pid_, SIGTERM);
        waitpid(child_pid_, nullptr, 0);
        child_pid_ = -1;
    }

    if (stdin_write_fd_  >= 0) { close(stdin_write_fd_);  stdin_write_fd_  = -1; }
    if (stdout_read_fd_ >= 0) { close(stdout_read_fd_); stdout_read_fd_ = -1; }
}

// ---------------------------------------------------------------------------
// is_connected
// ---------------------------------------------------------------------------

bool MCPClient::is_connected() const {
    std::lock_guard lock(io_mutex_);
    return connected_;
}

bool MCPClient::is_degraded() const {
    std::lock_guard lock(io_mutex_);
    return degraded_;
}

// ---------------------------------------------------------------------------
// server_name
// ---------------------------------------------------------------------------

const std::string& MCPClient::server_name() const {
    return name_;
}

// ---------------------------------------------------------------------------
// list_tools
// ---------------------------------------------------------------------------

std::vector<MCPToolInfo> MCPClient::list_tools() {
    std::lock_guard lock(io_mutex_);

    if (!connected_) return {};

    auto result = send_request("tools/list", nlohmann::json::object(), request_timeout_);
    if (!result) {
        degraded_ = true;
        return {};
    }

    degraded_ = false;

    std::vector<MCPToolInfo> tools;

    try {
        const auto& arr = result->at("tools");
        for (const auto& item : arr) {
            MCPToolInfo info;
            info.name        = item.at("name").get<std::string>();
            info.description = item.value("description", std::string{});

            if (item.contains("inputSchema"))
                info.input_schema = item["inputSchema"];
            else
                info.input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};

            // MCP 2025-03-26 tool annotations
            if (item.contains("annotations")) {
                const auto& ann = item["annotations"];
                info.read_only_hint   = ann.value("readOnlyHint",   false);
                info.destructive_hint = ann.value("destructiveHint", false);
            }

            tools.push_back(std::move(info));
        }
    }
    catch (...) {}

    return tools;
}

// ---------------------------------------------------------------------------
// call_tool
// ---------------------------------------------------------------------------

nlohmann::json MCPClient::call_tool(const std::string& name, const nlohmann::json& args) {
    std::lock_guard lock(io_mutex_);

    if (!connected_) {
        return {{"isError", true}, {"content", nlohmann::json::array()}};
    }

    nlohmann::json params = {
        {"name",      name},
        {"arguments", args},
    };

    auto result = send_request("tools/call", params, request_timeout_);
    if (!result) {
        degraded_ = true;
        return {{"isError", true}, {"content", nlohmann::json::array()}};
    }

    degraded_ = false;

    return *result;
}

// ---------------------------------------------------------------------------
// list_resources
// ---------------------------------------------------------------------------

std::vector<MCPResourceInfo> MCPClient::list_resources() {
    std::lock_guard lock(io_mutex_);

    if (!connected_) return {};

    auto result = send_request("resources/list", nlohmann::json::object(), request_timeout_);
    if (!result) {
        degraded_ = true;
        return {};
    }

    degraded_ = false;

    std::vector<MCPResourceInfo> resources;

    try {
        if (result->contains("resources") && (*result)["resources"].is_array()) {
            for (const auto& item : (*result)["resources"]) {
                MCPResourceInfo res;
                res.uri         = item.at("uri").get<std::string>();
                res.name        = item.value("name", std::string{});
                res.description = item.value("description", std::string{});

                if (item.contains("mimeType"))
                    res.mime_type = item["mimeType"].get<std::string>();
                else
                    res.mime_type = item.value("mime_type", std::string{});

                resources.push_back(std::move(res));
            }
        }
    }
    catch (...) {}

    return resources;
}

// ---------------------------------------------------------------------------
// read_resource
// ---------------------------------------------------------------------------

std::string MCPClient::read_resource(const std::string& uri) {
    std::lock_guard lock(io_mutex_);

    if (!connected_) return {};

    nlohmann::json params = {{"uri", uri}};

    auto result = send_request("resources/read", params, request_timeout_);
    if (!result) {
        degraded_ = true;
        return {};
    }

    degraded_ = false;

    try {
        if (result->contains("contents") && (*result)["contents"].is_array() &&
            !(*result)["contents"].empty()) {
            return (*result)["contents"][0].value("text", std::string{});
        }
    }
    catch (...) {}

    return {};
}

// ---------------------------------------------------------------------------
// send_request — internal, assumes io_mutex_ is held by caller
// ---------------------------------------------------------------------------

std::optional<nlohmann::json> MCPClient::send_request(
    const std::string& method,
    const nlohmann::json& params,
    std::chrono::milliseconds timeout)
{
    const std::int64_t id = next_id_++;

    nlohmann::json request = {
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"method",  method},
        {"params",  params},
    };

    if (!write_line(request.dump())) {
        return std::nullopt;
    }

    // Read lines until we find the matching response.  Cap at 200 iterations
    // to avoid spinning forever on a misbehaving server.
    for (int i = 0; i < 200; ++i) {
        std::string line = read_line(timeout);
        if (line.empty()) {
            return std::nullopt;
        }

        nlohmann::json resp;
        try {
            resp = nlohmann::json::parse(line);
        }
        catch (...) {
            // Non-JSON startup noise — skip.
            continue;
        }

        // Notifications have a "method" but no "id" (or null id) — skip.
        if (resp.contains("method") && (!resp.contains("id") || resp["id"].is_null())) {
            continue;
        }

        if (!resp.contains("id") || resp["id"].is_null()) continue;

        if (resp["id"].get<std::int64_t>() != id) {
            continue;  // Different request's response, skip.
        }

        if (resp.contains("error")) {
            return std::nullopt;
        }

        if (resp.contains("result")) {
            return resp["result"];
        }

        return std::nullopt;
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// send_notification — fire-and-forget, assumes io_mutex_ is held
// ---------------------------------------------------------------------------

void MCPClient::send_notification(const std::string& method, const nlohmann::json& params) {
    nlohmann::json notif = {
        {"jsonrpc", "2.0"},
        {"method",  method},
        {"params",  params},
    };
    write_line(notif.dump());
}

// ---------------------------------------------------------------------------
// write_line
// ---------------------------------------------------------------------------

bool MCPClient::write_line(const std::string& line) {
    if (stdin_write_fd_ < 0) return false;

    const std::string data = line + "\n";
    std::size_t total = 0;
    while (total < data.size()) {
        ssize_t n = write(stdin_write_fd_, data.c_str() + total, data.size() - total);
        if (n <= 0) return false;
        total += static_cast<std::size_t>(n);
    }
    return true;
}

// ---------------------------------------------------------------------------
// read_line
// ---------------------------------------------------------------------------

std::string MCPClient::read_line(std::chrono::milliseconds timeout) {
    if (stdout_read_fd_ < 0) return {};

    std::string result;
    result.reserve(256);

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    char ch = '\0';
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return {};
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        struct pollfd pfd {
            stdout_read_fd_,
            POLLIN,
            0,
        };
        const int poll_result = poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (poll_result <= 0) {
            return {};
        }
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return {};
        }

        ssize_t n = read(stdout_read_fd_, &ch, 1);
        if (n <= 0) break;
        if (ch == '\n') break;
        result += ch;
    }

    return result;
}

}  // namespace omni::engine
