#pragma once

#include <omni/tool.h>

namespace omni::engine {

class AgentManager;

/// Built-in tool that allows the model to spawn sub-agents.
class AgentTool : public Tool {
public:
    explicit AgentTool(AgentManager& manager);

    std::string    name()         const override { return "agent"; }
    std::string    description()  const override;
    nlohmann::json input_schema() const override;
    bool           is_read_only()   const override { return false; }
    bool           is_destructive() const override { return false; }
    bool           is_sub_agent_tool() const override { return true; }
    ToolCallResult call(const nlohmann::json& args) override;

private:
    AgentManager& manager_;
};

/// Built-in tool that sends a message to a running agent.
class SendMessageTool : public Tool {
public:
    explicit SendMessageTool(AgentManager& manager);

    std::string    name()         const override { return "send_message"; }
    std::string    description()  const override;
    nlohmann::json input_schema() const override;
    bool           is_read_only()   const override { return false; }
    bool           is_destructive() const override { return false; }
    bool           is_sub_agent_tool() const override { return true; }
    ToolCallResult call(const nlohmann::json& args) override;

private:
    AgentManager& manager_;
};

}  // namespace omni::engine
