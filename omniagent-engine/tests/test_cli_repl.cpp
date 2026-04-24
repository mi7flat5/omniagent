#include <gtest/gtest.h>

#include "cli/repl.h"
#include "cli/repl_internal.h"

#include <chrono>
#include <filesystem>

using namespace omni::engine;

namespace {

using omni::engine::cli::detail::CliApprovalPolicy;

std::filesystem::path make_temp_repl_history_dir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("omni-repl-history-" + std::to_string(stamp));
}

TEST(CliReplInternal, UsageTextListsWorkflowProfilesAndSmokeExamples) {
    const std::string help = omni::engine::cli::usage_text("omni-engine");

    EXPECT_NE(help.find("Supported profiles: coordinator, explore, feature, refactor, audit, spec, plan, research, bugfix, general"),
              std::string::npos);
    EXPECT_NE(help.find("--profile feature --prompt \"Add a JSON export command and tests\""),
              std::string::npos);
    EXPECT_NE(help.find("--profile refactor --prompt \"Extract run formatting without changing behavior\""),
              std::string::npos);
    EXPECT_NE(help.find("--profile audit --prompt \"Review approval handling for operator-facing risks\""),
              std::string::npos);
    EXPECT_NE(help.find("--profile bugfix --prompt \"Fix paused-run resume and add a regression test\""),
              std::string::npos);
}

TEST(CliReplInternal, ReplHelpTextListsWorkflowProfilesAndSmokePrompts) {
    const std::string help = omni::engine::cli::repl_help_text();

    EXPECT_NE(help.find("Profiles: coordinator, explore, feature, refactor, audit, spec, plan, research, bugfix, general"),
              std::string::npos);
    EXPECT_NE(help.find("/profile feature \"Add a JSON export command and tests\""),
              std::string::npos);
    EXPECT_NE(help.find("/profile refactor \"Extract run formatting without changing behavior\""),
              std::string::npos);
    EXPECT_NE(help.find("/profile audit \"Review approval handling for operator-facing risks\""),
              std::string::npos);
    EXPECT_NE(help.find("/profile bugfix \"Fix paused-run resume and add a regression test\""),
              std::string::npos);
    EXPECT_NE(help.find("/cancel [run-id]     cancel the current or named run"),
              std::string::npos);
    EXPECT_NE(help.find("Up/Down arrows       traverse persisted input history"),
              std::string::npos);
    EXPECT_NE(help.find("Ctrl-C               stop the active run; press again to cancel"),
              std::string::npos);
}

TEST(CliReplInternal, ProjectPromptUsesOnlyProjectName) {
    EXPECT_EQ(omni::engine::cli::detail::format_project_prompt("omniagent"), "[omniagent> ");
    EXPECT_EQ(omni::engine::cli::detail::format_project_prompt(""), "[project> ");
}

TEST(CliReplInternal, ActivityIndicatorsStayInline) {
    const std::string indicator = omni::engine::cli::detail::activity_indicator_text();
    const std::string tick = omni::engine::cli::detail::activity_tick_text();
    const std::string status = omni::engine::cli::detail::format_activity_status("read file");

    EXPECT_EQ(indicator, "[working]");
    EXPECT_EQ(tick, "[working]");
    EXPECT_EQ(status, "[working] read file");
    EXPECT_EQ(indicator.find('\n'), std::string::npos);
    EXPECT_EQ(tick.find('\n'), std::string::npos);
    EXPECT_EQ(status.find('\n'), std::string::npos);
}

TEST(CliReplInternal, ToolAndAgentIndicatorsStayInline) {
    const std::string tool = omni::engine::cli::detail::tool_indicator_text(
        "read_file",
        nlohmann::json{{"path", "src/app/main.py"}, {"start_line", 10}, {"end_line", 40}});
    const std::string grep = omni::engine::cli::detail::tool_indicator_text(
        "grep",
        nlohmann::json{{"pattern", "tenant_id"}, {"path", "src"}, {"glob", "**/*.py"}});
    const std::string agent = omni::engine::cli::detail::agent_indicator_text("feature");
    const std::string done = omni::engine::cli::detail::agent_completion_indicator_text(true);
    const std::string failed = omni::engine::cli::detail::agent_completion_indicator_text(false);

    EXPECT_EQ(tool, "read file src/app/main.py:10-40");
    EXPECT_EQ(grep, "grep tenant_id in src [**/*.py]");
    EXPECT_EQ(agent, "feature agent");
    EXPECT_EQ(done, "agent done");
    EXPECT_EQ(failed, "agent failed");
    EXPECT_EQ(tool.find('\n'), std::string::npos);
    EXPECT_EQ(grep.find('\n'), std::string::npos);
    EXPECT_EQ(agent.find('\n'), std::string::npos);
    EXPECT_EQ(done.find('\n'), std::string::npos);
    EXPECT_EQ(failed.find('\n'), std::string::npos);
}

TEST(CliReplInternal, RunIndicatorsStayInline) {
    const std::string started = omni::engine::cli::detail::run_started_indicator_text();
    const std::string paused = omni::engine::cli::detail::run_paused_indicator_text();

    EXPECT_EQ(started, "[run]");
    EXPECT_EQ(paused, "[paused]");
    EXPECT_EQ(started.find('\n'), std::string::npos);
    EXPECT_EQ(paused.find('\n'), std::string::npos);
}

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

TEST(CliReplInternal, ParsePlannerClarificationStateExtractsQuestions) {
    const std::string content =
        "PLANNER_BUILD_PLAN STATUS: CLARIFICATION_REQUIRED\n"
        "workflow_passed: false\n"
        "clarification_required: true\n"
        "questions:\n"
        "- spec-clar-gap-001: Should queues be tenant scoped?\n"
        "raw_json:\n"
        "{\n"
        "  \"clarification_required\": true,\n"
        "  \"clarification\": {\n"
        "    \"clarification_required\": true,\n"
        "    \"clarification_mode\": \"required\",\n"
        "    \"clarification_message\": \"Need one blocker resolved\",\n"
        "    \"pending_clarification_ids\": [\"spec-clar-gap-001\"],\n"
        "    \"clarifications\": [\n"
        "      {\n"
        "        \"id\": \"spec-clar-gap-001\",\n"
        "        \"stage\": \"spec\",\n"
        "        \"severity\": \"BLOCKING\",\n"
        "        \"question\": \"Should queues be tenant scoped?\",\n"
        "        \"recommended_default\": \"Enforce tenant_id at dequeue\"\n"
        "      }\n"
        "    ]\n"
        "  }\n"
        "}";

    const auto state = omni::engine::cli::detail::parse_planner_clarification_state(
        "planner_build_plan",
        content);

    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->tool_name, "planner_build_plan");
    EXPECT_EQ(state->clarification_mode, "required");
    EXPECT_EQ(state->clarification_message, "Need one blocker resolved");
    ASSERT_EQ(state->questions.size(), 1u);
    EXPECT_EQ(state->questions[0].id, "spec-clar-gap-001");
    EXPECT_EQ(state->questions[0].question, "Should queues be tenant scoped?");
    EXPECT_EQ(state->questions[0].recommended_default, "Enforce tenant_id at dequeue");
    EXPECT_TRUE(state->questions[0].pending);
}

TEST(CliReplInternal, BuildClarificationResumeInputIncludesAnswersAndDelegation) {
    omni::engine::cli::detail::PendingClarificationState state;
    state.tool_name = "planner_build_from_idea";
    state.run_id = "run-123";
    state.delegate_unanswered = true;
    state.questions = {
        omni::engine::cli::detail::ClarificationQuestion{
            .id = "spec-clar-gap-001",
            .stage = "spec",
            .severity = "BLOCKING",
            .question = "Should queues be tenant scoped?",
            .recommended_default = "Enforce tenant_id at dequeue",
            .pending = true,
        },
    };
    state.staged_answers["spec-clar-gap-001"] = "Yes, enforce tenant_id at dequeue.";

    const std::string input = omni::engine::cli::detail::build_clarification_resume_input(state);

    EXPECT_NE(input.find("Continue the pending planner clarification flow"), std::string::npos);
    EXPECT_NE(input.find("spec-clar-gap-001: Yes, enforce tenant_id at dequeue."), std::string::npos);
    EXPECT_NE(input.find("you decide for me"), std::string::npos);
}

TEST(CliReplInternal, PendingClarificationStateFromRunUsesStructuredPayload) {
    RunResult result;
    result.run_id = "run-123";
    result.pending_clarification = PendingClarification{
        .tool_name = "planner_build_plan",
        .clarification_mode = "required",
        .clarification_message = "Need one blocker resolved",
        .pending_question_ids = {"spec-clar-gap-001"},
        .questions = {
            ClarificationQuestion{
                .id = "spec-clar-gap-001",
                .stage = "spec",
                .severity = "BLOCKING",
                .quote = "Tenant isolation missing",
                .question = "Should queues be tenant scoped?",
                .recommended_default = "Enforce tenant_id at dequeue",
                .answer_type = "text",
                .options = nlohmann::json::array(),
            },
        },
    };

    const auto state = omni::engine::cli::detail::pending_clarification_state_from_run(result);

    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->run_id, "run-123");
    EXPECT_EQ(state->tool_name, "planner_build_plan");
    ASSERT_EQ(state->questions.size(), 1u);
    EXPECT_TRUE(state->questions[0].pending);
}

TEST(CliReplInternal, DelegatePhraseRecognizesNaturalLanguageShortcut) {
    EXPECT_TRUE(omni::engine::cli::detail::is_delegate_phrase("you decide for me"));
    EXPECT_TRUE(omni::engine::cli::detail::is_delegate_phrase("use recommended defaults"));
    EXPECT_FALSE(omni::engine::cli::detail::is_delegate_phrase("tenant scoped queues"));
}

TEST(CliReplInternal, ReplHistoryTraversesPreviousEntriesAndRestoresDraft) {
    omni::engine::cli::detail::ReplInputHistory history;
    omni::engine::cli::detail::remember_repl_history_entry(history, "first prompt");
    omni::engine::cli::detail::remember_repl_history_entry(history, "second prompt");

    const auto latest = omni::engine::cli::detail::previous_repl_history_entry(history, "draft text");
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(*latest, "second prompt");

    const auto earliest = omni::engine::cli::detail::previous_repl_history_entry(history, "ignored");
    ASSERT_TRUE(earliest.has_value());
    EXPECT_EQ(*earliest, "first prompt");

    const auto forward = omni::engine::cli::detail::next_repl_history_entry(history);
    ASSERT_TRUE(forward.has_value());
    EXPECT_EQ(*forward, "second prompt");

    const auto restored = omni::engine::cli::detail::next_repl_history_entry(history);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(*restored, "draft text");

    EXPECT_FALSE(history.browse_index.has_value());
}

TEST(CliReplInternal, ReplHistoryIgnoresEmptyEntriesAndResetsAfterSubmit) {
    omni::engine::cli::detail::ReplInputHistory history;
    omni::engine::cli::detail::remember_repl_history_entry(history, "");
    EXPECT_TRUE(history.entries.empty());

    omni::engine::cli::detail::remember_repl_history_entry(history, "/help");
    const auto recalled = omni::engine::cli::detail::previous_repl_history_entry(history, "partial");
    ASSERT_TRUE(recalled.has_value());
    EXPECT_EQ(*recalled, "/help");

    omni::engine::cli::detail::remember_repl_history_entry(history, "new prompt");
    EXPECT_EQ(history.entries.size(), 2u);
    EXPECT_FALSE(history.browse_index.has_value());
    EXPECT_TRUE(history.draft.empty());
}

TEST(CliReplInternal, ReplHistoryPersistsAcrossReloads) {
    const auto temp_dir = make_temp_repl_history_dir();

    omni::engine::cli::detail::ReplInputHistory history;
    omni::engine::cli::detail::remember_repl_history_entry(history, "first prompt");
    omni::engine::cli::detail::remember_repl_history_entry(history, "/help");
    omni::engine::cli::detail::persist_repl_history(temp_dir, history);

    const auto loaded = omni::engine::cli::detail::load_persisted_repl_history(temp_dir);
    EXPECT_EQ(loaded.entries.size(), 2u);
    EXPECT_EQ(loaded.entries[0], "first prompt");
    EXPECT_EQ(loaded.entries[1], "/help");
    EXPECT_FALSE(loaded.browse_index.has_value());
    EXPECT_TRUE(loaded.draft.empty());

    std::filesystem::remove_all(temp_dir);
}

TEST(CliReplInternal, ReplHistoryPersistenceTrimsOldEntries) {
    const auto temp_dir = make_temp_repl_history_dir();

    omni::engine::cli::detail::ReplInputHistory history;
    history.entries = {"one", "two", "three"};
    omni::engine::cli::detail::persist_repl_history(temp_dir, history, 2);

    const auto loaded = omni::engine::cli::detail::load_persisted_repl_history(temp_dir, 2);
    ASSERT_EQ(loaded.entries.size(), 2u);
    EXPECT_EQ(loaded.entries[0], "two");
    EXPECT_EQ(loaded.entries[1], "three");

    std::filesystem::remove_all(temp_dir);
}

}  // namespace