#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace omni::engine {

enum class PermissionDecision { Allow, AlwaysAllow, Deny };

class PermissionDelegate {
public:
    virtual ~PermissionDelegate() = default;

    virtual PermissionDecision on_permission_request(
        const std::string& tool_name,
        const nlohmann::json& args,
        const std::string& description) = 0;
};

// ---------------------------------------------------------------------------
// Permission system types — public API for host configuration
// ---------------------------------------------------------------------------

enum class PermissionMode {
    Default,      // Ask delegate for everything
    AcceptEdits,  // Auto-allow file edit/write tools, ask for others
    Bypass,       // Allow everything without asking
    Plan,         // Show what would happen, ask to proceed (Phase 3; Default now)
};

enum class RuleBehavior { Allow, Deny, Ask };

struct PermissionRule {
    std::string  tool_pattern;  // glob pattern (e.g., "bash", "edit*", "*")
    std::string  arg_pattern;   // optional: match against args string
    RuleBehavior behavior       = RuleBehavior::Allow;
    int          priority       = 0;  // higher = checked first within same source
};

enum class RuleSource {
    Programmatic = 0,  // highest priority — set by host code
    SessionLocal = 1,  // session-scoped (e.g., from AlwaysAllow)
    Project      = 2,  // project .omniagent/permissions.json
    User         = 3,  // user ~/.omniagent/permissions.json (lowest)
};

}  // namespace omni::engine
