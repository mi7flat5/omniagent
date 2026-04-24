#include "permission_checker.h"

#include <algorithm>

namespace omni::engine {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

PermissionChecker::PermissionChecker(PermissionDelegate& fallback_delegate)
    : delegate_(fallback_delegate)
{}

// ---------------------------------------------------------------------------
// Mode accessors
// ---------------------------------------------------------------------------

void PermissionChecker::set_mode(PermissionMode mode) {
    std::lock_guard<std::mutex> lk(mutex_);
    mode_ = mode;
}

PermissionMode PermissionChecker::mode() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return mode_;
}

// ---------------------------------------------------------------------------
// Rule management
// ---------------------------------------------------------------------------

void PermissionChecker::add_rule(RuleSource source, PermissionRule rule) {
    std::lock_guard<std::mutex> lk(mutex_);
    rules_.push_back({source, std::move(rule)});
}

void PermissionChecker::clear_rules(RuleSource source) {
    std::lock_guard<std::mutex> lk(mutex_);
    rules_.erase(
        std::remove_if(rules_.begin(), rules_.end(),
                       [source](const SourcedRule& sr) { return sr.source == source; }),
        rules_.end());
}

void PermissionChecker::always_allow(const std::string& tool_name) {
    // Creates a SessionLocal allow rule for the exact tool name.
    // Called without holding the lock — add_rule will acquire it.
    PermissionRule rule;
    rule.tool_pattern = tool_name;
    rule.behavior     = RuleBehavior::Allow;
    rule.priority     = 0;
    add_rule(RuleSource::SessionLocal, std::move(rule));
}

// ---------------------------------------------------------------------------
// matches — simple glob: * matches any sequence, ? matches a single char
// An empty pattern matches everything.
// ---------------------------------------------------------------------------

bool PermissionChecker::matches(const std::string& pattern,
                                 const std::string& text) const
{
    if (pattern.empty()) return true;

    // Iterative glob matching with two pointers into pattern and text.
    // star_pat / star_txt track the last '*' position for backtracking.
    const char* p    = pattern.c_str();
    const char* t    = text.c_str();
    const char* star_pat = nullptr;
    const char* star_txt = nullptr;

    while (*t != '\0') {
        if (*p == '*') {
            star_pat = p++;
            star_txt = t;
        } else if (*p == '?' || *p == *t) {
            ++p;
            ++t;
        } else if (star_pat != nullptr) {
            // Backtrack: advance the text position after the last '*'
            p = star_pat + 1;
            t = ++star_txt;
        } else {
            return false;
        }
    }

    // Skip trailing '*' characters
    while (*p == '*') ++p;
    return *p == '\0';
}

// ---------------------------------------------------------------------------
// is_edit_tool
// ---------------------------------------------------------------------------

bool PermissionChecker::is_edit_tool(const std::string& tool_name) const {
    // Exact matches
    if (tool_name == "edit" || tool_name == "write" || tool_name == "notebook_edit") {
        return true;
    }
    // Substring: any tool whose name contains "edit" or "write"
    if (tool_name.find("edit")  != std::string::npos) return true;
    if (tool_name.find("write") != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------
// check — main entry point
// ---------------------------------------------------------------------------

PermissionDecision PermissionChecker::check(
    const std::string&    tool_name,
    const nlohmann::json& args,
    const std::string&    description,
    bool                  tool_is_read_only,
    bool                  tool_is_destructive)
{
    std::unique_lock<std::mutex> lk(mutex_);

    // Build an ordered view of rules: Programmatic (0) → SessionLocal (1) →
    // Project (2) → User (3), then by descending rule priority within each source.
    // We work on a copy so we can release the lock before calling the delegate.
    std::vector<SourcedRule> sorted_rules = rules_;
    lk.unlock();

    std::stable_sort(sorted_rules.begin(), sorted_rules.end(),
        [](const SourcedRule& a, const SourcedRule& b) {
            if (static_cast<int>(a.source) != static_cast<int>(b.source)) {
                return static_cast<int>(a.source) < static_cast<int>(b.source);
            }
            return a.rule.priority > b.rule.priority;  // higher priority first
        });

    // Pre-compute the args string once for arg_pattern matching.
    const std::string args_str = args.dump();

    // -----------------------------------------------------------------------
    // 1. Deny pass: any Deny rule matching tool → immediate deny.
    // -----------------------------------------------------------------------
    for (const SourcedRule& sr : sorted_rules) {
        if (sr.rule.behavior != RuleBehavior::Deny) continue;
        if (!matches(sr.rule.tool_pattern, tool_name))  continue;
        if (!sr.rule.arg_pattern.empty() &&
            !matches(sr.rule.arg_pattern, args_str))    continue;
        return PermissionDecision::Deny;
    }

    // -----------------------------------------------------------------------
    // 2. Mode check.
    // -----------------------------------------------------------------------
    lk.lock();
    const PermissionMode current_mode = mode_;
    lk.unlock();

    if (current_mode == PermissionMode::Bypass) {
        return PermissionDecision::Allow;
    }

    if (current_mode == PermissionMode::AcceptEdits && is_edit_tool(tool_name)) {
        return PermissionDecision::Allow;
    }

    if (current_mode == PermissionMode::Plan) {
        // Plan mode favors low-risk inspection tools and requires approval for
        // anything that can mutate state or execute potentially dangerous work.
        if (tool_is_read_only && !tool_is_destructive && !is_edit_tool(tool_name)) {
            return PermissionDecision::Allow;
        }
    }

    // -----------------------------------------------------------------------
    // 3. Allow pass: any Allow rule matching tool → allow without asking.
    // -----------------------------------------------------------------------
    for (const SourcedRule& sr : sorted_rules) {
        if (sr.rule.behavior != RuleBehavior::Allow) continue;
        if (!matches(sr.rule.tool_pattern, tool_name))  continue;
        if (!sr.rule.arg_pattern.empty() &&
            !matches(sr.rule.arg_pattern, args_str))    continue;
        return PermissionDecision::Allow;
    }

    // -----------------------------------------------------------------------
    // 4. Fallback: ask the delegate.
    // -----------------------------------------------------------------------
    const PermissionDecision decision =
        delegate_.on_permission_request(tool_name, args, description);

    if (decision == PermissionDecision::AlwaysAllow) {
        always_allow(tool_name);
        return PermissionDecision::Allow;
    }

    return decision;
}

}  // namespace omni::engine
