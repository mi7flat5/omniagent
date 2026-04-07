#include "agent_tool.h"
#include "agent_manager.h"

#include <sstream>

namespace omni::engine {

// ---------------------------------------------------------------------------
// AgentTool
// ---------------------------------------------------------------------------

AgentTool::AgentTool(AgentManager& manager)
    : manager_(manager)
{}

std::string AgentTool::description() const {
    return "Spawn a sub-agent to handle a task. The agent gets its own conversation context.";
}

nlohmann::json AgentTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"task",       {{"type", "string"},  {"description", "The task for the agent"}}},
            {"type",       {{"type", "string"},  {"enum", {"general", "explore", "plan"}},
                            {"description", "Agent type"}}},
            {"background", {{"type", "boolean"}, {"description", "Run in background"}}},
            {"name",       {{"type", "string"},  {"description", "Optional name for addressing via send_message"}}}
        }},
        {"required", {"task"}}
    };
}

ToolCallResult AgentTool::call(const nlohmann::json& args) {
    if (!args.contains("task") || !args["task"].is_string()) {
        return {"Error: 'task' field is required", true};
    }

    AgentConfig config;
    config.task = args["task"].get<std::string>();

    // Parse agent type (default: general)
    const std::string type_str = args.value("type", "general");
    if (type_str == "explore") {
        config.type = AgentType::Explore;
    } else if (type_str == "plan") {
        config.type = AgentType::Plan;
    } else {
        config.type = AgentType::GeneralPurpose;
    }

    config.run_in_background = args.value("background", false);
    config.name              = args.value("name", std::string{});

    std::string final_response;
    bool        got_result = false;

    const std::string agent_id = manager_.spawn(
        config,
        [&final_response, &got_result](const AgentResult& result) {
            got_result = true;
            if (result.is_error) {
                final_response = "Agent error: " + result.error_message;
            } else {
                final_response = result.response;
            }
        });

    if (config.run_in_background) {
        return {"Agent " + agent_id + " spawned in background", false};
    }

    // Foreground: callback was called synchronously by spawn().
    if (got_result) {
        return {final_response, false};
    }
    return {"Agent " + agent_id + " completed (no response)", false};
}

// ---------------------------------------------------------------------------
// SendMessageTool
// ---------------------------------------------------------------------------

SendMessageTool::SendMessageTool(AgentManager& manager)
    : manager_(manager)
{}

std::string SendMessageTool::description() const {
    return "Send a follow-up message to a running or completed sub-agent, continuing its conversation.";
}

nlohmann::json SendMessageTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"agent_id", {{"type", "string"}, {"description", "The agent ID returned by the agent tool"}}},
            {"message",  {{"type", "string"}, {"description", "The follow-up message to send"}}}
        }},
        {"required", {"agent_id", "message"}}
    };
}

ToolCallResult SendMessageTool::call(const nlohmann::json& args) {
    if (!args.contains("agent_id") || !args["agent_id"].is_string()) {
        return {"Error: 'agent_id' field is required", true};
    }
    if (!args.contains("message") || !args["message"].is_string()) {
        return {"Error: 'message' field is required", true};
    }

    const std::string agent_id = args["agent_id"].get<std::string>();
    const std::string message  = args["message"].get<std::string>();

    const bool ok = manager_.send_message(agent_id, message);
    if (!ok) {
        return {"Error: agent '" + agent_id + "' not found", true};
    }
    return {"Message sent to agent " + agent_id, false};
}

}  // namespace omni::engine
