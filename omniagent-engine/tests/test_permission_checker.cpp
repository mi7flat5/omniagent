#include <gtest/gtest.h>

#include <omni/permission.h>
#include "permissions/permission_checker.h"

#include <string>
#include <vector>

using namespace omni::engine;

// ---------------------------------------------------------------------------
// Test delegates
// ---------------------------------------------------------------------------

class PCAllowDelegate : public PermissionDelegate {
public:
    int call_count = 0;

    PermissionDecision on_permission_request(const std::string&,
                                              const nlohmann::json&,
                                              const std::string&) override {
        ++call_count;
        return PermissionDecision::Allow;
    }
};

class PCDenyDelegate : public PermissionDelegate {
public:
    int call_count = 0;

    PermissionDecision on_permission_request(const std::string&,
                                              const nlohmann::json&,
                                              const std::string&) override {
        ++call_count;
        return PermissionDecision::Deny;
    }
};

class PCAlwaysAllowDelegate : public PermissionDelegate {
public:
    int call_count = 0;

    PermissionDecision on_permission_request(const std::string&,
                                              const nlohmann::json&,
                                              const std::string&) override {
        ++call_count;
        return PermissionDecision::AlwaysAllow;
    }
};

// ---------------------------------------------------------------------------
// DefaultMode_DelegateCalled: Default mode, no rules → delegate called
// ---------------------------------------------------------------------------

TEST(PermissionChecker, DefaultMode_DelegateCalled) {
    PCAllowDelegate delegate;
    PermissionChecker checker(delegate);

    const auto decision = checker.check("bash", nlohmann::json::object(), "run shell");

    EXPECT_EQ(decision, PermissionDecision::Allow);
    EXPECT_EQ(delegate.call_count, 1);
}

// ---------------------------------------------------------------------------
// BypassMode_NeverAsks: Bypass mode → always Allow, delegate never called
// ---------------------------------------------------------------------------

TEST(PermissionChecker, BypassMode_NeverAsks) {
    PCAllowDelegate delegate;
    PermissionChecker checker(delegate);
    checker.set_mode(PermissionMode::Bypass);

    const auto d1 = checker.check("bash",      nlohmann::json::object(), "run shell");
    const auto d2 = checker.check("read_file", nlohmann::json::object(), "read");
    const auto d3 = checker.check("edit",      nlohmann::json::object(), "edit file");

    EXPECT_EQ(d1, PermissionDecision::Allow);
    EXPECT_EQ(d2, PermissionDecision::Allow);
    EXPECT_EQ(d3, PermissionDecision::Allow);
    EXPECT_EQ(delegate.call_count, 0);
}

// ---------------------------------------------------------------------------
// AcceptEditsMode: "edit" tool → Allow without delegate; "bash" → delegate called
// ---------------------------------------------------------------------------

TEST(PermissionChecker, AcceptEditsMode) {
    PCAllowDelegate delegate;
    PermissionChecker checker(delegate);
    checker.set_mode(PermissionMode::AcceptEdits);

    // Edit tools: allowed without asking delegate
    EXPECT_EQ(checker.check("edit",          {}, "edit"), PermissionDecision::Allow);
    EXPECT_EQ(checker.check("write",         {}, "write"), PermissionDecision::Allow);
    EXPECT_EQ(checker.check("notebook_edit", {}, "nb"),    PermissionDecision::Allow);
    EXPECT_EQ(checker.check("file_edit",     {}, "edit"),  PermissionDecision::Allow);
    EXPECT_EQ(delegate.call_count, 0);

    // Non-edit tool: delegate should be consulted
    checker.check("bash", {}, "run");
    EXPECT_EQ(delegate.call_count, 1);
}

// ---------------------------------------------------------------------------
// PlanMode_ReadOnlyTools_AutoAllow: safe read-only tools are auto-allowed
// ---------------------------------------------------------------------------

TEST(PermissionChecker, PlanMode_ReadOnlyTools_AutoAllow) {
    PCDenyDelegate delegate;
    PermissionChecker checker(delegate);
    checker.set_mode(PermissionMode::Plan);

    const auto decision = checker.check(
        "read_file", {}, "read project file", true, false);

    EXPECT_EQ(decision, PermissionDecision::Allow);
    EXPECT_EQ(delegate.call_count, 0);
}

// ---------------------------------------------------------------------------
// PlanMode_NonReadOnly_UsesDelegate: state-changing tools still ask delegate
// ---------------------------------------------------------------------------

TEST(PermissionChecker, PlanMode_NonReadOnly_UsesDelegate) {
    PCAllowDelegate delegate;
    PermissionChecker checker(delegate);
    checker.set_mode(PermissionMode::Plan);

    const auto decision = checker.check(
        "bash", {}, "run command", false, true);

    EXPECT_EQ(decision, PermissionDecision::Allow);
    EXPECT_EQ(delegate.call_count, 1);
}

// ---------------------------------------------------------------------------
// DenyRule_BlocksTool: Deny rule for "bash" → Deny even in Default mode
// ---------------------------------------------------------------------------

TEST(PermissionChecker, DenyRule_BlocksTool) {
    PCAllowDelegate delegate;
    PermissionChecker checker(delegate);

    PermissionRule rule;
    rule.tool_pattern = "bash";
    rule.behavior     = RuleBehavior::Deny;
    checker.add_rule(RuleSource::Programmatic, rule);

    const auto decision = checker.check("bash", {}, "run");

    EXPECT_EQ(decision, PermissionDecision::Deny);
    // Delegate must NOT be called — deny is evaluated before fallback
    EXPECT_EQ(delegate.call_count, 0);
}

// ---------------------------------------------------------------------------
// AllowRule_SkipsDelegate: Allow rule for "read_file" → Allow, delegate not called
// ---------------------------------------------------------------------------

TEST(PermissionChecker, AllowRule_SkipsDelegate) {
    PCDenyDelegate delegate;  // Would deny if reached
    PermissionChecker checker(delegate);

    PermissionRule rule;
    rule.tool_pattern = "read_file";
    rule.behavior     = RuleBehavior::Allow;
    checker.add_rule(RuleSource::SessionLocal, rule);

    const auto decision = checker.check("read_file", {}, "read");

    EXPECT_EQ(decision, PermissionDecision::Allow);
    EXPECT_EQ(delegate.call_count, 0);
}

// ---------------------------------------------------------------------------
// AlwaysAllow_CreatesSessionRule: delegate returns AlwaysAllow → subsequent calls auto-allow
// ---------------------------------------------------------------------------

TEST(PermissionChecker, AlwaysAllow_CreatesSessionRule) {
    PCAlwaysAllowDelegate delegate;
    PermissionChecker checker(delegate);

    // First call: hits delegate, returns AlwaysAllow, creates session rule
    const auto first = checker.check("echo", {}, "echo");
    EXPECT_EQ(first, PermissionDecision::Allow);
    EXPECT_EQ(delegate.call_count, 1);

    // Second call: session rule exists → auto-allow, delegate NOT called again
    const auto second = checker.check("echo", {}, "echo");
    EXPECT_EQ(second, PermissionDecision::Allow);
    EXPECT_EQ(delegate.call_count, 1);  // still 1

    // A different tool: delegate is still consulted
    checker.check("bash", {}, "run");
    EXPECT_EQ(delegate.call_count, 2);
}

// ---------------------------------------------------------------------------
// RulePriority_DenyFirst: both Allow and Deny rules for same tool → Deny wins
// (Deny pass is scanned before Allow pass regardless of source/priority)
// ---------------------------------------------------------------------------

TEST(PermissionChecker, RulePriority_DenyFirst) {
    PCAllowDelegate delegate;
    PermissionChecker checker(delegate);

    // Allow rule — lower source number (Programmatic) but Deny pass runs first
    PermissionRule allow_rule;
    allow_rule.tool_pattern = "bash";
    allow_rule.behavior     = RuleBehavior::Allow;
    allow_rule.priority     = 100;
    checker.add_rule(RuleSource::Programmatic, allow_rule);

    // Deny rule — same tool
    PermissionRule deny_rule;
    deny_rule.tool_pattern = "bash";
    deny_rule.behavior     = RuleBehavior::Deny;
    deny_rule.priority     = 0;
    checker.add_rule(RuleSource::SessionLocal, deny_rule);

    const auto decision = checker.check("bash", {}, "run");

    // Deny always wins because the deny pass runs first
    EXPECT_EQ(decision, PermissionDecision::Deny);
    EXPECT_EQ(delegate.call_count, 0);
}

// ---------------------------------------------------------------------------
// GlobPattern: rule with "bash*" matches "bash" and "bash_tool"
// ---------------------------------------------------------------------------

TEST(PermissionChecker, GlobPattern) {
    PCDenyDelegate delegate;
    PermissionChecker checker(delegate);

    PermissionRule rule;
    rule.tool_pattern = "bash*";
    rule.behavior     = RuleBehavior::Allow;
    checker.add_rule(RuleSource::Programmatic, rule);

    EXPECT_EQ(checker.check("bash",      {}, ""), PermissionDecision::Allow);
    EXPECT_EQ(checker.check("bash_tool", {}, ""), PermissionDecision::Allow);
    EXPECT_EQ(checker.check("bashtool",  {}, ""), PermissionDecision::Allow);

    // "read_file" does NOT match "bash*" → hits deny delegate
    const auto other = checker.check("read_file", {}, "");
    EXPECT_EQ(other, PermissionDecision::Deny);
    EXPECT_EQ(delegate.call_count, 1);
}

// ---------------------------------------------------------------------------
// WildcardRule: "*" rule allows everything
// ---------------------------------------------------------------------------

TEST(PermissionChecker, WildcardRule) {
    PCDenyDelegate delegate;
    PermissionChecker checker(delegate);

    PermissionRule rule;
    rule.tool_pattern = "*";
    rule.behavior     = RuleBehavior::Allow;
    checker.add_rule(RuleSource::Programmatic, rule);

    EXPECT_EQ(checker.check("bash",      {}, ""), PermissionDecision::Allow);
    EXPECT_EQ(checker.check("read_file", {}, ""), PermissionDecision::Allow);
    EXPECT_EQ(checker.check("anything",  {}, ""), PermissionDecision::Allow);
    EXPECT_EQ(delegate.call_count, 0);
}

// ---------------------------------------------------------------------------
// ClearRules: rules from a source can be removed
// ---------------------------------------------------------------------------

TEST(PermissionChecker, ClearRules) {
    PCAllowDelegate delegate;
    PermissionChecker checker(delegate);

    PermissionRule rule;
    rule.tool_pattern = "bash";
    rule.behavior     = RuleBehavior::Deny;
    checker.add_rule(RuleSource::Programmatic, rule);

    // With the deny rule active it should deny
    EXPECT_EQ(checker.check("bash", {}, ""), PermissionDecision::Deny);
    EXPECT_EQ(delegate.call_count, 0);

    // After clearing Programmatic rules the delegate is consulted
    checker.clear_rules(RuleSource::Programmatic);
    EXPECT_EQ(checker.check("bash", {}, ""), PermissionDecision::Allow);
    EXPECT_EQ(delegate.call_count, 1);
}
