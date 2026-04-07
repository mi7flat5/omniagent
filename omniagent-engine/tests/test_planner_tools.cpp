#include <gtest/gtest.h>

#include "tools/planner_tools.h"
#include "tools/workspace_tools.h"

#include <filesystem>
#include <fstream>

using namespace omni::engine;

namespace {

namespace fs = std::filesystem;

std::string raw_json_text(const std::string& content) {
    const std::string marker = "raw_json:\n";
    const std::size_t pos = content.find(marker);
    if (pos == std::string::npos) {
        return content;
    }
    return content.substr(pos + marker.size());
}

void write_text(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open()) << path;
    stream << content;
    ASSERT_TRUE(stream.good()) << path;
}

class PlannerToolsTest : public ::testing::Test {
protected:
    void SetUp() override {
        workspace_root_ = fs::temp_directory_path() / fs::path("omni_engine_planner_tools");
        fs::remove_all(workspace_root_);
        fs::create_directories(workspace_root_ / "planner-harness");
        fs::create_directories(workspace_root_ / "src");

        context_.project_id = "planner-tools";
        context_.session_id = "session";
        context_.run_id = "run";
        context_.profile = "plan";
        context_.workspace_root = workspace_root_;
        context_.working_dir = workspace_root_;
    }

    void TearDown() override {
        fs::remove_all(workspace_root_);
    }

    void write_bridge_stub(bool validate_plan_passed = true,
                           bool run_plan_passed = true,
                           const std::string& failure_detail = {},
                           bool build_from_idea_spec_passed = true,
                           bool build_from_idea_plan_passed = true) {
        write_text(
            workspace_root_ / "planner-harness" / "bridge.py",
            "import json\n"
            "import sys\n"
            "from pathlib import Path\n"
            "\n"
            "def stage(passed=True, skipped=False, detail=''):\n"
            "    rubric_checks = []\n"
            "    if detail:\n"
            "        rubric_checks.append({\n"
            "            'name': 'Import completeness',\n"
            "            'weight': 5,\n"
            "            'passed': passed,\n"
            "            'detail': detail,\n"
            "        })\n"
            "    return {\n"
            "        'passed': passed,\n"
            "        'rubric_score': 100.0,\n"
            "        'adversary_score': 100.0,\n"
            "        'combined_score': 100.0,\n"
            "        'rubric_checks': rubric_checks,\n"
            "        'adversary': {\n"
            "            'skipped': skipped,\n"
            "            'error': '',\n"
            "            'blocking_gaps': 0,\n"
            "            'blocking_guesses': 0,\n"
            "            'contradiction_count': 0,\n"
            "            'gaps': [],\n"
            "            'guesses': [],\n"
            "            'contradictions': [],\n"
            "        },\n"
            "    }\n"
            "\n"
            "args = sys.argv[1:]\n"
            "if args and args[0] == '--config':\n"
            "    args = args[2:]\n"
            "skip = '--skip-adversary' in args\n"
            "command = args[0]\n"
            "if command == 'validate-spec':\n"
            "    payload = {'ok': True, 'command': 'validate-spec', 'stage': stage(True, skip), 'spec_path': str(Path(args[1]).resolve())}\n"
            "elif command == 'validate-plan':\n"
            "    payload = {'ok': True, 'command': 'validate-plan', 'stage': stage("
            + std::string(validate_plan_passed ? "True" : "False")
            + ", skip, "
            + (failure_detail.empty() ? "''" : ("'" + failure_detail + "'"))
            + "), 'spec_path': str(Path(args[1]).resolve()), 'plan_path': str(Path(args[2]).resolve())}\n"
            "elif command == 'repair-plan':\n"
            "    spec_path = Path(args[1]).resolve()\n"
            "    input_plan = Path(args[2]).resolve()\n"
            "    output_path = input_plan\n"
            "    if '-o' in args:\n"
            "        output_path = Path(args[args.index('-o') + 1]).resolve()\n"
            "    elif '--output' in args:\n"
            "        output_path = Path(args[args.index('--output') + 1]).resolve()\n"
            "    plan = {'phases': [{'name': 'Foundation', 'tasks': [{'file': 'src/main.py', 'description': 'Create entrypoint', 'depends_on': []}]}]}\n"
            "    output_path.write_text(json.dumps(plan), encoding='utf-8')\n"
            "    payload = {'ok': True, 'command': 'repair-plan', 'spec_path': str(spec_path), 'plan_path': str(input_plan), 'output_path': str(output_path), 'repair_strategy': 'patch', 'patch_operation_count': 1, 'summary': {'format': 'phases', 'phase_count': 1, 'task_count': 1}, 'skip_adversary': skip}\n"
            "elif command == 'run':\n"
            "    spec_path = Path(args[1]).resolve()\n"
            "    prompt_path = Path(args[args.index('--prompt-output') + 1]).resolve()\n"
            "    plan_path = Path(args[args.index('--plan-output') + 1]).resolve()\n"
            "    prompt_path.write_text('planner prompt\\n', encoding='utf-8')\n"
            "    plan = {'phases': [{'name': 'Foundation', 'tasks': [{'file': 'src/main.py', 'description': 'Create entrypoint', 'depends_on': []}]}]}\n"
            "    plan_path.write_text(json.dumps(plan), encoding='utf-8')\n"
            "    payload = {\n"
            "        'ok': True,\n"
            "        'command': 'run',\n"
            "        'spec_path': str(spec_path),\n"
            "        'artifacts': {'prompt_path': str(prompt_path), 'plan_path': str(plan_path)},\n"
            "        'spec_validation': stage(True, skip),\n"
            "        'plan_generation': {'format': 'phases', 'phase_count': 1, 'task_count': 1},\n"
            "        'plan_validation': stage("
            + std::string(run_plan_passed ? "True" : "False")
            + ", skip, "
            + (failure_detail.empty() ? "''" : ("'" + failure_detail + "'"))
            + "),\n"
            "    }\n"
            "elif command == 'build-from-idea':\n"
            "    spec_path = Path(args[args.index('--spec-output') + 1]).resolve()\n"
            "    prompt_path = Path(args[args.index('--prompt-output') + 1]).resolve()\n"
            "    plan_path = Path(args[args.index('--plan-output') + 1]).resolve()\n"
            "    spec_path.write_text('# Generated spec\\n', encoding='utf-8')\n"
            "    payload = {\n"
            "        'ok': True,\n"
            "        'command': 'build-from-idea',\n"
            "        'artifacts': {'spec_path': str(spec_path)},\n"
            "        'spec_validation': stage("
            + std::string(build_from_idea_spec_passed ? "True" : "False")
            + ", skip, "
            + (failure_detail.empty() ? "''" : ("'" + failure_detail + "'"))
            + "),\n"
            "        'spec_attempts': [{'attempt': 1, 'spec_path': str(spec_path), 'spec_validation': stage("
            + std::string(build_from_idea_spec_passed ? "True" : "False")
            + ", skip, "
            + (failure_detail.empty() ? "''" : ("'" + failure_detail + "'"))
            + ")}],\n"
            "        'workflow_passed': False,\n"
            "    }\n"
            "    if "
            + std::string(build_from_idea_spec_passed ? "True" : "False")
            + ":\n"
            "        prompt_path.write_text('planner prompt\\n', encoding='utf-8')\n"
            "        plan = {'phases': [{'name': 'Foundation', 'tasks': [{'file': 'src/main.py', 'description': 'Create entrypoint', 'depends_on': []}]}]}\n"
            "        plan_path.write_text(json.dumps(plan), encoding='utf-8')\n"
            "        payload['artifacts']['prompt_path'] = str(prompt_path)\n"
            "        payload['artifacts']['plan_path'] = str(plan_path)\n"
            "        payload['plan_generation'] = {'format': 'phases', 'phase_count': 1, 'task_count': 1}\n"
            "        payload['plan_validation'] = stage("
            + std::string(build_from_idea_plan_passed ? "True" : "False")
            + ", skip, "
            + (failure_detail.empty() ? "''" : ("'" + failure_detail + "'"))
            + ")\n"
            "        payload['workflow_passed'] = "
            + std::string(build_from_idea_plan_passed ? "True" : "False")
            + "\n"
            "else:\n"
            "    payload = {'ok': False, 'command': command, 'error': 'unsupported command'}\n"
            "print(json.dumps(payload))\n");
    }

    fs::path workspace_root_;
    ToolContext context_;
};

TEST_F(PlannerToolsTest, DefaultWorkspaceToolsIncludePlannerTools) {
    const auto tools = make_default_workspace_tools();
    std::vector<std::string> names;
    for (const auto& tool : tools) {
        names.push_back(tool->name());
    }

    EXPECT_NE(std::find(names.begin(), names.end(), "planner_validate_spec"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "planner_validate_plan"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "planner_repair_plan"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "planner_build_plan"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "planner_build_from_idea"), names.end());
}

#ifndef _WIN32
TEST_F(PlannerToolsTest, BuildPlanReturnsArtifactsAndGraphValidation) {
    write_bridge_stub();
    write_text(workspace_root_ / "SPEC.md", "# Spec\n");

    PlannerBuildPlanTool tool;
    const auto result = tool.call(
        nlohmann::json{{"spec_path", "SPEC.md"}, {"skip_adversary", true}},
        context_);

    ASSERT_FALSE(result.is_error) << result.content;
    const auto payload = nlohmann::json::parse(result.content);
    EXPECT_TRUE(payload.at("workflow_passed").get<bool>());
    EXPECT_TRUE(payload.at("graph_validation").at("valid").get<bool>());
    EXPECT_EQ(payload.at("artifacts").at("plan_path_relative").get<std::string>(), "PLAN.json");
    EXPECT_EQ(payload.at("artifacts").at("prompt_path_relative").get<std::string>(), "planner-prompt.md");
}

TEST_F(PlannerToolsTest, BuildFromIdeaReturnsArtifactsAndGraphValidation) {
    write_bridge_stub();

    PlannerBuildFromIdeaTool tool;
    const auto result = tool.call(
        nlohmann::json{{"idea", "Build a webhook relay"}, {"skip_adversary", true}},
        context_);

    ASSERT_FALSE(result.is_error) << result.content;
    const auto payload = nlohmann::json::parse(result.content);
    EXPECT_TRUE(payload.at("workflow_passed").get<bool>());
    EXPECT_TRUE(payload.at("graph_validation").at("valid").get<bool>());
    EXPECT_EQ(payload.at("artifacts").at("spec_path_relative").get<std::string>(), "SPEC.md");
    EXPECT_EQ(payload.at("artifacts").at("prompt_path_relative").get<std::string>(), "planner-prompt.md");
    EXPECT_EQ(payload.at("artifacts").at("plan_path_relative").get<std::string>(), "PLAN.json");
}

TEST_F(PlannerToolsTest, BuildPlanSkipsAdversaryByDefault) {
    write_bridge_stub();
    write_text(workspace_root_ / "SPEC.md", "# Spec\n");

    PlannerBuildPlanTool tool;
    const auto result = tool.call(
        nlohmann::json{{"spec_path", "SPEC.md"}},
        context_);

    ASSERT_FALSE(result.is_error) << result.content;
    const auto payload = nlohmann::json::parse(result.content);
    EXPECT_TRUE(payload.at("spec_validation").at("adversary").at("skipped").get<bool>());
    EXPECT_TRUE(payload.at("plan_validation").at("adversary").at("skipped").get<bool>());
}

TEST_F(PlannerToolsTest, ValidatePlanMarksGraphContractFailures) {
    write_bridge_stub();
    write_text(workspace_root_ / "SPEC.md", "# Spec\n");
    write_text(
        workspace_root_ / "PLAN.json",
        "{\"phases\":[{\"name\":\"Foundation\",\"tasks\":[{\"file\":\"src/main.py\",\"description\":\"Create entrypoint\",\"depends_on\":[\"missing.py\"]}]}]}");

    PlannerValidatePlanTool tool;
    const auto result = tool.call(
        nlohmann::json{{"spec_path", "SPEC.md"}, {"plan_path", "PLAN.json"}, {"skip_adversary", true}},
        context_);

    ASSERT_TRUE(result.is_error) << result.content;
    EXPECT_NE(result.content.find("PLANNER_VALIDATE_PLAN STATUS: FAILED"), std::string::npos);
    EXPECT_NE(result.content.find("missing.py"), std::string::npos);
    const auto payload = nlohmann::json::parse(raw_json_text(result.content));
    EXPECT_FALSE(payload.at("stage").at("passed").get<bool>());
    EXPECT_FALSE(payload.at("graph_validation").at("valid").get<bool>());
    EXPECT_NE(payload.at("graph_validation").at("error").get<std::string>().find("missing.py"), std::string::npos);
}

TEST_F(PlannerToolsTest, BuildPlanReturnsFailureBannerWhenWorkflowFails) {
    write_bridge_stub(true, false, "backend/app/api/auth.py uses Token without import");
    write_text(workspace_root_ / "SPEC.md", "# Spec\n");

    PlannerBuildPlanTool tool;
    const auto result = tool.call(
        nlohmann::json{{"spec_path", "SPEC.md"}, {"skip_adversary", true}},
        context_);

    ASSERT_TRUE(result.is_error) << result.content;
    EXPECT_NE(result.content.find("PLANNER_BUILD_PLAN STATUS: FAILED"), std::string::npos);
    EXPECT_NE(result.content.find("workflow_passed: false"), std::string::npos);
    EXPECT_NE(result.content.find("Import completeness: backend/app/api/auth.py uses Token without import"), std::string::npos);
    const auto payload = nlohmann::json::parse(raw_json_text(result.content));
    EXPECT_FALSE(payload.at("workflow_passed").get<bool>());
    EXPECT_FALSE(payload.at("plan_validation").at("passed").get<bool>());
}

TEST_F(PlannerToolsTest, BuildFromIdeaReturnsFailureBannerWhenSpecValidationFails) {
    write_bridge_stub(true, true, "spec missing explicit error handling", false, true);

    PlannerBuildFromIdeaTool tool;
    const auto result = tool.call(
        nlohmann::json{{"idea", "Build a markdown link checker"}, {"skip_adversary", true}},
        context_);

    ASSERT_TRUE(result.is_error) << result.content;
    EXPECT_NE(result.content.find("PLANNER_BUILD_FROM_IDEA STATUS: FAILED"), std::string::npos);
    EXPECT_NE(result.content.find("spec_validation_passed: false"), std::string::npos);
    EXPECT_NE(result.content.find("Import completeness: spec missing explicit error handling"), std::string::npos);
    const auto payload = nlohmann::json::parse(raw_json_text(result.content));
    EXPECT_FALSE(payload.at("workflow_passed").get<bool>());
    EXPECT_FALSE(payload.at("spec_validation").at("passed").get<bool>());
}

TEST_F(PlannerToolsTest, ValidatePlanSkipsAdversaryByDefault) {
    write_bridge_stub();
    write_text(workspace_root_ / "SPEC.md", "# Spec\n");
    write_text(
        workspace_root_ / "PLAN.json",
        "{\"phases\":[{\"name\":\"Foundation\",\"tasks\":[{\"file\":\"src/main.py\",\"description\":\"Create entrypoint\",\"depends_on\":[]}]}]}");

    PlannerValidatePlanTool tool;
    const auto result = tool.call(
        nlohmann::json{{"spec_path", "SPEC.md"}, {"plan_path", "PLAN.json"}},
        context_);

    ASSERT_FALSE(result.is_error) << result.content;
    const auto payload = nlohmann::json::parse(result.content);
    EXPECT_TRUE(payload.at("stage").at("adversary").at("skipped").get<bool>());
}

TEST_F(PlannerToolsTest, RepairPlanReturnsOutputAndGraphValidation) {
    write_bridge_stub();
    write_text(workspace_root_ / "SPEC.md", "# Spec\n");
    write_text(
        workspace_root_ / "PLAN.json",
        "{\"phases\":[{\"name\":\"Broken\",\"tasks\":[{\"file\":\"src/main.py\",\"description\":\"Broken entrypoint\",\"depends_on\":[]}]}]}");

    PlannerRepairPlanTool tool;
    const auto result = tool.call(
        nlohmann::json{{"spec_path", "SPEC.md"}, {"plan_path", "PLAN.json"}, {"output_path", "PLAN-fixed.json"}},
        context_);

    ASSERT_FALSE(result.is_error) << result.content;
    const auto payload = nlohmann::json::parse(result.content);
    EXPECT_EQ(payload.at("repair_strategy").get<std::string>(), "patch");
    EXPECT_EQ(payload.at("plan_path_relative").get<std::string>(), "PLAN.json");
    EXPECT_EQ(payload.at("output_path_relative").get<std::string>(), "PLAN-fixed.json");
    EXPECT_TRUE(payload.at("graph_validation").at("valid").get<bool>());
}

TEST_F(PlannerToolsTest, RepairPlanSkipsAdversaryByDefault) {
    write_bridge_stub();
    write_text(workspace_root_ / "SPEC.md", "# Spec\n");
    write_text(
        workspace_root_ / "PLAN.json",
        "{\"phases\":[{\"name\":\"Broken\",\"tasks\":[{\"file\":\"src/main.py\",\"description\":\"Broken entrypoint\",\"depends_on\":[]}]}]}");

    PlannerRepairPlanTool tool;
    const auto result = tool.call(
        nlohmann::json{{"spec_path", "SPEC.md"}, {"plan_path", "PLAN.json"}},
        context_);

    ASSERT_FALSE(result.is_error) << result.content;
    const auto payload = nlohmann::json::parse(result.content);
    EXPECT_TRUE(payload.at("skip_adversary").get<bool>());
}
#endif

}  // namespace