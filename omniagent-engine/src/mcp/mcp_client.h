#pragma once

#include <omni/mcp.h>

#include <nlohmann/json.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace omni::engine {

struct MCPToolInfo {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
    bool read_only_hint = false;
    bool destructive_hint = false;
};

struct MCPResourceInfo {
    std::string uri;
    std::string name;
    std::string description;
    std::string mime_type;
};

class MCPClient {
public:
    MCPClient();
    ~MCPClient();

    // Non-copyable, non-movable (owns pipe FDs and child process)
    MCPClient(const MCPClient&)            = delete;
    MCPClient& operator=(const MCPClient&) = delete;
    MCPClient(MCPClient&&)                 = delete;
    MCPClient& operator=(MCPClient&&)      = delete;

    /// Connect to an MCP server. Spawns the process, performs initialize handshake.
    bool connect(const MCPServerConfig& config);

    /// Disconnect and kill the server process.
    void disconnect();

    /// Is the server connected and responsive?
    bool is_connected() const;

    /// Has the server entered a degraded state due to timeouts or I/O failures?
    bool is_degraded() const;

    /// List available tools from the server.
    std::vector<MCPToolInfo> list_tools();

    /// Call a tool on the server.
    nlohmann::json call_tool(const std::string& name, const nlohmann::json& args);

    /// List available resources.
    std::vector<MCPResourceInfo> list_resources();

    /// Read a resource by URI.
    std::string read_resource(const std::string& uri);

    /// Server name (from config).
    const std::string& server_name() const;

private:
    /// Send a JSON-RPC request and wait for the matching response.
    /// Returns nullopt on error.
    std::optional<nlohmann::json> send_request(const std::string& method,
                                               const nlohmann::json& params,
                                               std::chrono::milliseconds timeout);

    /// Send a JSON-RPC notification (no response expected).
    void send_notification(const std::string& method, const nlohmann::json& params);

    /// Write a newline-terminated string to the child's stdin.
    bool write_line(const std::string& line);

    /// Read one newline-terminated line from the child's stdout.
    std::string read_line(std::chrono::milliseconds timeout);

    // Configuration stored after successful connect.
    std::string name_;

    // Pipe file descriptors (POSIX only — this code is Linux-only per WSL env).
    int stdin_write_fd_{-1};
    int stdout_read_fd_{-1};
    pid_t child_pid_{-1};

    bool connected_{false};
    bool degraded_{false};
    std::int64_t next_id_{1};
    std::chrono::milliseconds init_timeout_{std::chrono::seconds{10}};
    std::chrono::milliseconds request_timeout_{std::chrono::seconds{30}};
    mutable std::mutex io_mutex_;
};

}  // namespace omni::engine
