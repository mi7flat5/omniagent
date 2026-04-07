#pragma once

#include <memory>
#include <filesystem>
#include <string>
#include <nlohmann/json.hpp>

namespace omni::engine {

struct ToolContext {
    std::string project_id;
    std::string session_id;
    std::string run_id;
    std::string profile;
    std::filesystem::path workspace_root;
    std::filesystem::path working_dir;
};

struct ToolCallResult {
    std::string content;
    bool        is_error = false;
};

class Tool {
public:
    virtual ~Tool() = default;

    virtual std::string    name() const = 0;
    virtual std::string    description() const = 0;
    virtual nlohmann::json input_schema() const = 0;
    virtual bool           is_read_only() const = 0;
    virtual bool           is_destructive() const = 0;
    virtual bool           is_shell() const { return false; }
    virtual bool           is_network() const { return false; }
    virtual bool           is_mcp() const { return false; }
    virtual bool           is_sub_agent_tool() const { return false; }
    virtual ToolCallResult call(const nlohmann::json& args,
                                const ToolContext& context) {
        (void)context;
        return call(args);
    }
    virtual ToolCallResult call(const nlohmann::json& args) = 0;
};

}  // namespace omni::engine
