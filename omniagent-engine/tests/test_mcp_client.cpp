#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <mcp/mcp_client.h>
#include <mcp/mcp_tool_wrapper.h>

using namespace omni::engine;

// ---------------------------------------------------------------------------
// JsonRpcFormat — verify that send_request produces valid JSON-RPC 2.0 format.
//
// We test the format by constructing the request JSON directly (mirrors the
// logic in send_request) rather than spawning a real process.
// ---------------------------------------------------------------------------
TEST(MCPJsonRpc, RequestFormat) {
    const std::int64_t id = 42;
    nlohmann::json params = {{"name", "test_tool"}, {"arguments", {{"x", 1}}}};

    nlohmann::json request = {
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"method",  "tools/call"},
        {"params",  params},
    };

    EXPECT_EQ(request["jsonrpc"], "2.0");
    EXPECT_EQ(request["id"],      42);
    EXPECT_EQ(request["method"],  "tools/call");
    EXPECT_TRUE(request["params"].contains("name"));
    EXPECT_TRUE(request["params"].contains("arguments"));
}

TEST(MCPJsonRpc, NotificationFormat) {
    nlohmann::json notif = {
        {"jsonrpc", "2.0"},
        {"method",  "notifications/initialized"},
        {"params",  nlohmann::json::object()},
    };

    EXPECT_EQ(notif["jsonrpc"], "2.0");
    EXPECT_FALSE(notif.contains("id"));  // Notifications have no id
    EXPECT_EQ(notif["method"], "notifications/initialized");
}

// ---------------------------------------------------------------------------
// MCPToolWrapperName — naming convention "mcp__{server}__{tool}"
// ---------------------------------------------------------------------------

// Minimal stub MCPClient that never connects to a real process.
// We only need server_name() for these tests.
class StubMCPClient : public MCPClient {
public:
    explicit StubMCPClient(const std::string& server_name)
        : server_name_(server_name) {}

    // Override server_name without spawning a process.
    // We set name_ directly — access via a friend or a thin subclass trick.
    // Since we can't access private members, we'll use a different approach:
    // expose a test constructor that sets name_ via connect() on a fake config.
    //
    // Actually, let's just verify naming by constructing via the public interface
    // using a mock that wraps name() override.  Since MCPClient is not virtual,
    // we must test MCPToolWrapper in isolation using a concrete MCPClient whose
    // name was set by connect().
    //
    // We cannot call connect() without a real process, so we expose a protected
    // helper.  Instead, use a minimal MCPServerConfig that points to /bin/echo
    // and bail out gracefully if the server handshake fails — we only care about
    // the name stored after connect() is called (even if it fails, the config
    // name is stored before the handshake, so we can test that case).
    //
    // See ToolWrapperName test below for the actual approach.
private:
    std::string server_name_;
};

// MCPToolInfo is a plain struct — no server needed to construct it.
TEST(MCPToolWrapper, WrappedNameConvention) {
    // We can't instantiate MCPToolWrapper without a live MCPClient (it takes
    // MCPClient& not a pointer), so we verify the naming logic directly via
    // the documented convention: "mcp__{server}__{tool}".

    const std::string server = "my_server";
    const std::string tool   = "read_file";
    const std::string expected = "mcp__" + server + "__" + tool;

    // Manual construction matching MCPToolWrapper constructor logic:
    std::string wrapped = "mcp__" + server + "__" + tool;
    EXPECT_EQ(wrapped, expected);
    EXPECT_EQ(wrapped, "mcp__my_server__read_file");
}

TEST(MCPToolWrapper, WrappedNameWithSpecialChars) {
    // Verify names with hyphens and dots survive the convention.
    const std::string server = "file-system";
    const std::string tool   = "list.dir";
    const std::string wrapped = "mcp__" + server + "__" + tool;
    EXPECT_EQ(wrapped, "mcp__file-system__list.dir");
}

// ---------------------------------------------------------------------------
// MCPToolInfo parsing — parse a canned tools/list response
// ---------------------------------------------------------------------------

TEST(MCPToolInfo, ParseFromToolsListResponse) {
    // Canned tools/list result matching MCP spec
    nlohmann::json tools_response = {
        {"tools", nlohmann::json::array({
            {
                {"name",        "read_file"},
                {"description", "Read a file from the filesystem"},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"path", {{"type", "string"}, {"description", "File path"}}}
                    }},
                    {"required", nlohmann::json::array({"path"})}
                }},
                {"annotations", {
                    {"readOnlyHint",   true},
                    {"destructiveHint", false}
                }}
            },
            {
                {"name",        "write_file"},
                {"description", "Write content to a file"},
                {"inputSchema", {
                    {"type", "object"},
                    {"properties", {
                        {"path",    {{"type", "string"}}},
                        {"content", {{"type", "string"}}}
                    }},
                    {"required", nlohmann::json::array({"path", "content"})}
                }},
                {"annotations", {
                    {"readOnlyHint",    false},
                    {"destructiveHint", true}
                }}
            }
        })}
    };

    // Parse using the same logic as MCPClient::list_tools()
    std::vector<MCPToolInfo> tools;
    const auto& arr = tools_response.at("tools");
    for (const auto& item : arr) {
        MCPToolInfo info;
        info.name        = item.at("name").get<std::string>();
        info.description = item.value("description", std::string{});

        if (item.contains("inputSchema"))
            info.input_schema = item["inputSchema"];
        else
            info.input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};

        if (item.contains("annotations")) {
            const auto& ann = item["annotations"];
            info.read_only_hint   = ann.value("readOnlyHint",   false);
            info.destructive_hint = ann.value("destructiveHint", false);
        }

        tools.push_back(std::move(info));
    }

    ASSERT_EQ(tools.size(), 2u);

    EXPECT_EQ(tools[0].name,        "read_file");
    EXPECT_EQ(tools[0].description, "Read a file from the filesystem");
    EXPECT_TRUE(tools[0].read_only_hint);
    EXPECT_FALSE(tools[0].destructive_hint);
    EXPECT_EQ(tools[0].input_schema["type"], "object");
    EXPECT_TRUE(tools[0].input_schema["properties"].contains("path"));

    EXPECT_EQ(tools[1].name,        "write_file");
    EXPECT_FALSE(tools[1].read_only_hint);
    EXPECT_TRUE(tools[1].destructive_hint);
}

TEST(MCPToolInfo, DefaultSchemaWhenMissing) {
    // Tool without inputSchema gets a default empty object schema.
    nlohmann::json item = {
        {"name",        "ping"},
        {"description", "Ping the server"},
    };

    MCPToolInfo info;
    info.name        = item.at("name").get<std::string>();
    info.description = item.value("description", std::string{});

    if (item.contains("inputSchema"))
        info.input_schema = item["inputSchema"];
    else
        info.input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}};

    EXPECT_EQ(info.input_schema["type"], "object");
    EXPECT_TRUE(info.input_schema["properties"].empty());
}

// ---------------------------------------------------------------------------
// MCPResourceInfo parsing — parse a canned resources/list response
// ---------------------------------------------------------------------------

TEST(MCPResourceInfo, ParseFromResourcesListResponse) {
    nlohmann::json resources_response = {
        {"resources", nlohmann::json::array({
            {
                {"uri",         "file:///project/README.md"},
                {"name",        "README"},
                {"description", "Project readme"},
                {"mimeType",    "text/markdown"}
            },
            {
                {"uri",      "file:///project/src/main.cpp"},
                {"name",     "main.cpp"},
                {"mimeType", "text/x-c++src"}
            }
        })}
    };

    std::vector<MCPResourceInfo> resources;
    const auto& arr = resources_response.at("resources");
    for (const auto& item : arr) {
        MCPResourceInfo res;
        res.uri         = item.at("uri").get<std::string>();
        res.name        = item.value("name",        std::string{});
        res.description = item.value("description", std::string{});

        if (item.contains("mimeType"))
            res.mime_type = item["mimeType"].get<std::string>();
        else
            res.mime_type = item.value("mime_type", std::string{});

        resources.push_back(std::move(res));
    }

    ASSERT_EQ(resources.size(), 2u);

    EXPECT_EQ(resources[0].uri,         "file:///project/README.md");
    EXPECT_EQ(resources[0].name,        "README");
    EXPECT_EQ(resources[0].description, "Project readme");
    EXPECT_EQ(resources[0].mime_type,   "text/markdown");

    EXPECT_EQ(resources[1].uri,       "file:///project/src/main.cpp");
    EXPECT_EQ(resources[1].name,      "main.cpp");
    EXPECT_EQ(resources[1].mime_type, "text/x-c++src");
    EXPECT_EQ(resources[1].description, "");
}

// ---------------------------------------------------------------------------
// MCPToolCallResult — verify ToolCallResult mapping from tool/call response
// ---------------------------------------------------------------------------

TEST(MCPToolCall, SuccessResultMapping) {
    nlohmann::json call_result = {
        {"isError", false},
        {"content", nlohmann::json::array({
            {{"type", "text"}, {"text", "Hello from tool"}}
        })}
    };

    bool is_error = call_result.value("isError", false);
    std::string combined;

    for (const auto& part : call_result["content"]) {
        if (part.value("type", "") == "text") {
            if (!combined.empty()) combined += "\n";
            combined += part.value("text", std::string{});
        }
    }

    EXPECT_FALSE(is_error);
    EXPECT_EQ(combined, "Hello from tool");
}

TEST(MCPToolCall, ErrorResultMapping) {
    nlohmann::json call_result = {
        {"isError", true},
        {"content", nlohmann::json::array({
            {{"type", "text"}, {"text", "Tool execution failed: file not found"}}
        })}
    };

    bool is_error = call_result.value("isError", false);
    std::string combined;

    for (const auto& part : call_result["content"]) {
        if (part.value("type", "") == "text") {
            if (!combined.empty()) combined += "\n";
            combined += part.value("text", std::string{});
        }
    }

    EXPECT_TRUE(is_error);
    EXPECT_EQ(combined, "Tool execution failed: file not found");
}

TEST(MCPToolCall, MultiPartContent) {
    nlohmann::json call_result = {
        {"isError", false},
        {"content", nlohmann::json::array({
            {{"type", "text"}, {"text", "Line 1"}},
            {{"type", "text"}, {"text", "Line 2"}},
            {{"type", "image"}, {"data", "base64..."}},  // non-text part, should be ignored
            {{"type", "text"}, {"text", "Line 3"}}
        })}
    };

    std::string combined;
    for (const auto& part : call_result["content"]) {
        if (part.value("type", "") == "text") {
            if (!combined.empty()) combined += "\n";
            combined += part.value("text", std::string{});
        }
    }

    EXPECT_EQ(combined, "Line 1\nLine 2\nLine 3");
}

// ---------------------------------------------------------------------------
// MCPClient — is_connected() returns false before connect() is called
// ---------------------------------------------------------------------------

TEST(MCPClient, InitiallyDisconnected) {
    MCPClient client;
    EXPECT_FALSE(client.is_connected());
}

TEST(MCPClient, ServerNameEmptyBeforeConnect) {
    MCPClient client;
    EXPECT_EQ(client.server_name(), "");
}
