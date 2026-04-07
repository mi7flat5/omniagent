#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace omni::engine {

enum class ApprovalDecision {
    Approve,
    ApproveAlways,
    Deny,
    Pause,
};

class ApprovalDelegate {
public:
    virtual ~ApprovalDelegate() = default;

    virtual ApprovalDecision on_approval_requested(
        const std::string& tool_name,
        const nlohmann::json& args,
        const std::string& description) = 0;
};

}  // namespace omni::engine