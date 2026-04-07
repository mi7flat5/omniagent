#include <gtest/gtest.h>

#include "cli/repl_internal.h"

using namespace omni::engine;

namespace {

using omni::engine::cli::detail::CliApprovalPolicy;

TEST(CliReplInternal, AutoReadOnlyPolicyIncludesPlannerWorkflowToolsWhenPresent) {
    const std::vector<ToolSummary> tools = {
        ToolSummary{.name = "read_file", .read_only = true},
        ToolSummary{.name = "replace_file", .read_only = false, .destructive = true},
        ToolSummary{.name = "planner_build_plan", .read_only = false, .network = true},
        ToolSummary{.name = "planner_build_from_idea", .read_only = false, .network = true},
        ToolSummary{.name = "planner_repair_plan", .read_only = false, .network = true},
    };

    const auto allowed = omni::engine::cli::detail::auto_approved_tool_names(
        tools,
        CliApprovalPolicy::AutoReadOnlyPrompt);

    EXPECT_EQ(allowed.count("read_file"), 1u);
    EXPECT_EQ(allowed.count("planner_build_plan"), 1u);
    EXPECT_EQ(allowed.count("planner_build_from_idea"), 1u);
    EXPECT_EQ(allowed.count("planner_repair_plan"), 1u);
    EXPECT_EQ(allowed.count("replace_file"), 0u);
}

TEST(CliReplInternal, PromptPolicyDoesNotAutoApprovePlannerWorkflowTools) {
    const std::vector<ToolSummary> tools = {
        ToolSummary{.name = "read_file", .read_only = true},
        ToolSummary{.name = "planner_build_plan", .read_only = false, .network = true},
        ToolSummary{.name = "planner_build_from_idea", .read_only = false, .network = true},
    };

    const auto allowed = omni::engine::cli::detail::auto_approved_tool_names(
        tools,
        CliApprovalPolicy::Prompt);

    EXPECT_EQ(allowed.count("read_file"), 1u);
    EXPECT_EQ(allowed.count("planner_build_plan"), 0u);
    EXPECT_EQ(allowed.count("planner_build_from_idea"), 0u);
}

TEST(CliReplInternal, ParseApprovalPolicyNormalizesUnderscoreVariant) {
    const auto parsed = omni::engine::cli::detail::parse_approval_policy_value(
        std::optional<std::string>{"auto_read_only_pause"},
        CliApprovalPolicy::Prompt);

    EXPECT_EQ(parsed, CliApprovalPolicy::AutoReadOnlyPause);
}

}  // namespace