#pragma once

#include <omni/permission.h>
#include <mutex>
#include <string>
#include <vector>

namespace omni::engine {

class PermissionChecker {
public:
    explicit PermissionChecker(PermissionDelegate& fallback_delegate);

    /// Set the permission mode.
    void           set_mode(PermissionMode mode);
    PermissionMode mode() const;

    /// Add a rule from a given source.
    void add_rule(RuleSource source, PermissionRule rule);

    /// Clear all rules from a given source.
    void clear_rules(RuleSource source);

    /// Record an "always allow" from the delegate (creates a SessionLocal rule).
    void always_allow(const std::string& tool_name);

    /// Check permission for a tool call.
    /// Decision flow: deny rules first → mode check → allow rules → delegate
    PermissionDecision check(
        const std::string&    tool_name,
        const nlohmann::json& args,
        const std::string&    description);

private:
    PermissionDelegate& delegate_;
    PermissionMode      mode_ = PermissionMode::Default;
    mutable std::mutex  mutex_;

    struct SourcedRule {
        RuleSource     source;
        PermissionRule rule;
    };
    std::vector<SourcedRule> rules_;

    bool matches(const std::string& pattern, const std::string& text) const;
    bool is_edit_tool(const std::string& tool_name) const;
};

}  // namespace omni::engine
