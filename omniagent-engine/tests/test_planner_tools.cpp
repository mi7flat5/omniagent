#include <gtest/gtest.h>

#include "tools/planner_tools.h"
#include "tools/workspace_tools.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace omni::engine;

namespace {

namespace fs = std::filesystem;

class ScopedEnvVar {
public:
    explicit ScopedEnvVar(const char* name)
        : name_(name) {
        if (const char* existing = std::getenv(name_)) {
            had_value_ = true;
            old_value_ = existing;
        }
    }

    ~ScopedEnvVar() {
        if (had_value_) {
            set(old_value_);
        } else {
            unset();
        }
    }

    void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s(name_, value.c_str());
#else
        setenv(name_, value.c_str(), 1);
#endif
    }

    void unset() {
#ifdef _WIN32
        _putenv_s(name_, "");
#else
        unsetenv(name_);
#endif
    }

private:
    const char* name_;
    bool had_value_ = false;
    std::string old_value_;
};

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
            "elif command == 'validate-review':\n"
            "    payload = {'ok': True, 'command': 'validate-review', 'case_path': str(Path(args[1]).resolve()), 'report_path': str(Path(args[2]).resolve()), 'case_id': Path(args[1]).stem, 'stage': stage(True, skip)}\n"
            "elif command == 'validate-bugfix':\n"
            "    payload = {'ok': True, 'command': 'validate-bugfix', 'case_path': str(Path(args[1]).resolve()), 'report_path': str(Path(args[2]).resolve()), 'case_id': Path(args[1]).stem, 'stage': stage(True, skip)}\n"
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

    void write_bridge_clarification_stub(const std::string& mode) {
        write_text(
            workspace_root_ / "planner-harness" / "bridge.py",
            "import json\n"
            "import sys\n"
            "from pathlib import Path\n"
            "args = sys.argv[1:]\n"
            "if args and args[0] == '--config':\n"
            "    args = args[2:]\n"
            "command = args[0]\n"
            "payload = {\n"
            "    'ok': True,\n"
            "    'command': command,\n"
            "    'workflow_passed': False,\n"
            "    'clarification_required': True,\n"
            "    'clarifications': [\n"
            "        {\n"
            "            'id': 'spec-clar-gap-001',\n"
            "            'stage': 'spec',\n"
            "            'severity': 'BLOCKING',\n"
            "            'quote': 'Tenant model unclear',\n"
            "            'question': 'Should worker queue enforcement be tenant-scoped?',\n"
            "            'recommended_default': 'enforce tenant_id at dequeue and DB boundary',\n"
            "            'answer_type': 'text',\n"
            "            'options': []\n"
            "        }\n"
            "    ],\n"
            "    'pending_clarification_ids': ['spec-clar-gap-001'],\n"
            "    'clarification': {\n"
            "        'clarification_required': True,\n"
            "        'clarification_mode': 'required',\n"
            "        'clarifications': [\n"
            "            {\n"
            "                'id': 'spec-clar-gap-001',\n"
            "                'question': 'Should worker queue enforcement be tenant-scoped?'\n"
            "            }\n"
            "        ],\n"
            "        'pending_clarification_ids': ['spec-clar-gap-001'],\n"
            "        'clarification_message': ''\n"
            "    }\n"
            "}\n"
            "if command == 'build-from-idea':\n"
            "    spec_path = Path(args[args.index('--spec-output') + 1]).resolve()\n"
            "    payload['artifacts'] = {'spec_path': str(spec_path)}\n"
            "elif command == 'run':\n"
            "    prompt_path = Path(args[args.index('--prompt-output') + 1]).resolve()\n"
            "    plan_path = Path(args[args.index('--plan-output') + 1]).resolve()\n"
            "    payload['artifacts'] = {'prompt_path': str(prompt_path), 'plan_path': str(plan_path)}\n"
            "    payload['spec_validation'] = {'passed': False, 'adversary': {'blocking_gaps': 1}}\n"
            "    payload['plan_validation'] = {'passed': False, 'adversary': {'blocking_gaps': 1}}\n"
            "print(json.dumps(payload))\n");
        (void)mode;
    }

    void write_tracked_case(const fs::path& repo_root,
                            const std::string& kind,
                            const std::string& filename,
                            const std::string& content = "{}\n") {
        write_text(repo_root / "planner-harness" / "tests" / "data" / kind / filename, content);
    }

    void write_external_bridge_stub(const fs::path& repo_root) {
        write_text(
            repo_root / "planner-harness" / "bridge.py",
            "import json\n"
            "import sys\n"
            "from pathlib import Path\n"
            "args = sys.argv[1:]\n"
            "if args and args[0] == '--config':\n"
            "    args = args[2:]\n"
            "skip = '--skip-adversary' in args\n"
            "command = args[0]\n"
            "payload = {\n"
            "    'ok': True,\n"
            "    'command': command,\n"
            "    'case_path': str(Path(args[1]).resolve()),\n"
            "    'report_path': str(Path(args[2]).resolve()),\n"
            "    'case_id': Path(args[1]).stem,\n"
            "    'stage': {\n"
            "        'passed': True,\n"
            "        'rubric_score': 100.0,\n"
            "        'adversary_score': 100.0,\n"
            "        'combined_score': 100.0,\n"
            "        'rubric_checks': [],\n"
            "        'adversary': {\n"
            "            'skipped': skip,\n"
            "            'error': '',\n"
            "            'blocking_gaps': 0,\n"
            "            'blocking_guesses': 0,\n"
            "            'contradiction_count': 0,\n"
            "            'gaps': [],\n"
            "            'guesses': [],\n"
            "            'contradictions': [],\n"
            "        },\n"
            "    },\n"
            "}\n"
            "print(json.dumps(payload))\n");
    }

    void write_review_failure_bridge_stub() {
        write_text(
            workspace_root_ / "planner-harness" / "bridge.py",
            "import json\n"
            "import sys\n"
            "from pathlib import Path\n"
            "args = sys.argv[1:]\n"
            "if args and args[0] == '--config':\n"
            "    args = args[2:]\n"
            "skip = '--skip-adversary' in args\n"
            "payload = {\n"
            "    'ok': True,\n"
            "    'command': 'validate-review',\n"
            "    'case_path': str(Path(args[1]).resolve()),\n"
            "    'report_path': str(Path(args[2]).resolve()),\n"
            "    'case_id': 'confctl-source-of-truth-2026-04-08',\n"
            "    'stage': {\n"
            "        'passed': False,\n"
            "        'rubric_score': 12.0,\n"
            "        'adversary_score': 100.0,\n"
            "        'combined_score': 12.0,\n"
            "        'rubric_checks': [\n"
            "            {'name': 'Baseline reflected', 'weight': 3, 'passed': False, 'detail': 'Missing baseline terms: 42 failed, 72 passed'},\n"
            "            {'name': 'Required findings covered', 'weight': 6, 'passed': False, 'detail': 'Missing findings: schema-fields-root-contract, store-schema-model-mismatch'},\n"
            "            {'name': 'Distinct clusters preserved', 'weight': 5, 'passed': False, 'detail': 'Only 1/4 clusters covered: schema'}\n"
            "        ],\n"
            "        'adversary': {\n"
            "            'skipped': skip,\n"
            "            'error': '',\n"
            "            'blocking_gaps': 0,\n"
            "            'blocking_guesses': 0,\n"
            "            'contradiction_count': 0,\n"
            "            'gaps': [],\n"
            "            'guesses': [],\n"
            "            'contradictions': [],\n"
            "        },\n"
            "    },\n"
            "}\n"
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
    EXPECT_NE(std::find(names.begin(), names.end(), "planner_validate_review"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "planner_validate_bugfix"), names.end());
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

TEST_F(PlannerToolsTest, BuildPlanRunsAdversaryByDefault) {
    write_bridge_stub();
    write_text(workspace_root_ / "SPEC.md", "# Spec\n");

    PlannerBuildPlanTool tool;
    const auto result = tool.call(
        nlohmann::json{{"spec_path", "SPEC.md"}},
        context_);

    ASSERT_FALSE(result.is_error) << result.content;
    const auto payload = nlohmann::json::parse(result.content);
    EXPECT_FALSE(payload.at("spec_validation").at("adversary").at("skipped").get<bool>());
    EXPECT_FALSE(payload.at("plan_validation").at("adversary").at("skipped").get<bool>());
}

TEST_F(PlannerToolsTest, BuildPlanCanSkipAdversaryWhenRequested) {
    write_bridge_stub();
    write_text(workspace_root_ / "SPEC.md", "# Spec\n");

    PlannerBuildPlanTool tool;
    const auto result = tool.call(
        nlohmann::json{{"spec_path", "SPEC.md"}, {"skip_adversary", true}},
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

TEST_F(PlannerToolsTest, BuildFromIdeaReturnsClarificationRequiredBanner) {
    write_bridge_clarification_stub("build-from-idea");

    PlannerBuildFromIdeaTool tool;
    const auto result = tool.call(
        nlohmann::json{{"idea", "Build a markdown link checker"}},
        context_);

    ASSERT_TRUE(result.is_error) << result.content;
    EXPECT_NE(result.content.find("PLANNER_BUILD_FROM_IDEA STATUS: CLARIFICATION_REQUIRED"), std::string::npos);
    EXPECT_NE(result.content.find("spec-clar-gap-001"), std::string::npos);

    const auto payload = nlohmann::json::parse(raw_json_text(result.content));
    EXPECT_TRUE(payload.at("clarification_required").get<bool>());
    EXPECT_EQ(payload.at("pending_clarification_ids").size(), 1u);
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

TEST_F(PlannerToolsTest, ValidateReviewUsesExternalBridgeFallbackAndInlineText) {
    const fs::path external_repo_root = fs::temp_directory_path() / "omni_engine_external_planner_repo";
    const fs::path external_workspace_parent = fs::temp_directory_path() / "omni_engine_external_review_workspace";
    const fs::path external_workspace = external_workspace_parent / "confctl";
    fs::remove_all(external_repo_root);
    fs::remove_all(external_workspace_parent);
    fs::create_directories(external_workspace);
    write_external_bridge_stub(external_repo_root);
    write_tracked_case(
        external_repo_root,
        "review",
        "confctl_source_of_truth_case.json",
        "{\"id\": \"confctl-source-of-truth\", \"kind\": \"review\"}\n");

    ScopedEnvVar planner_bridge("OMNIAGENT_PLANNER_BRIDGE");
    ScopedEnvVar planner_repo_root("OMNIAGENT_REPO_ROOT");
    planner_bridge.unset();
    planner_repo_root.set(external_repo_root.string());
    fs::remove_all(workspace_root_ / "planner-harness");

    ToolContext external_context = context_;
    external_context.project_id = "confctl";
    external_context.profile = "audit";
    external_context.workspace_root = external_workspace;
    external_context.working_dir = external_workspace;

    PlannerValidateReviewTool tool;
    const auto result = tool.call(
        nlohmann::json{{"report_text", "Findings first\n\n- one issue\n"}},
        external_context);

    fs::remove_all(external_repo_root);
    fs::remove_all(external_workspace_parent);

    ASSERT_FALSE(result.is_error) << result.content;
    const auto payload = nlohmann::json::parse(result.content);
    EXPECT_EQ(payload.at("command").get<std::string>(), "validate-review");
    EXPECT_EQ(payload.at("report_source").get<std::string>(), "inline_text");
    EXPECT_NE(payload.at("case_path").get<std::string>().find("confctl_source_of_truth_case.json"), std::string::npos);
    EXPECT_EQ(payload.at("report_path").get<std::string>(), "[inline report text]");
}

TEST_F(PlannerToolsTest, ValidateReviewFailureIncludesTrackedCaseRequirements) {
    write_review_failure_bridge_stub();
    write_text(
        workspace_root_ / "planner-harness" / "tests" / "data" / "review" / "confctl_source_of_truth_case.json",
        "{\n"
        "  \"id\": \"confctl-source-of-truth-2026-04-08\",\n"
        "  \"kind\": \"review\",\n"
        "  \"baseline\": {\n"
        "    \"required_terms\": [\"42 failed\", \"72 passed\"]\n"
        "  },\n"
        "  \"required_findings\": [\n"
        "    {\n"
        "      \"id\": \"schema-fields-root-contract\",\n"
        "      \"cluster\": \"schema\",\n"
        "      \"severity\": \"HIGH\",\n"
        "      \"required_groups\": [[\"load_schema\", \"confctl/schema.py\"], [\"fields\", \"top-level yaml\"]]\n"
        "    },\n"
        "    {\n"
        "      \"id\": \"store-schema-model-mismatch\",\n"
        "      \"cluster\": \"store\",\n"
        "      \"severity\": \"HIGH\",\n"
        "      \"required_groups\": [[\"ConfigStore\", \"confctl/store.py\", \"_cast_value\"], [\"field.type\", \"value_type\", \"AttributeError\"]]\n"
        "    }\n"
        "  ],\n"
        "  \"min_required_clusters\": 4\n"
        "}\n");

    PlannerValidateReviewTool tool;
    const auto result = tool.call(
        nlohmann::json{{"case_path", "planner-harness/tests/data/review/confctl_source_of_truth_case.json"},
                       {"report_text", "High: only covered schema."},
                       {"skip_adversary", true}},
        context_);

    ASSERT_TRUE(result.is_error) << result.content;
    EXPECT_NE(result.content.find("PLANNER_VALIDATE_REVIEW STATUS: FAILED"), std::string::npos);
    EXPECT_NE(result.content.find("tracked_case_requirements:"), std::string::npos);
    EXPECT_NE(result.content.find("Baseline terms required"), std::string::npos);
    EXPECT_NE(result.content.find("42 failed, 72 passed"), std::string::npos);
    EXPECT_NE(result.content.find("Minimum distinct clusters required: 4"), std::string::npos);
    EXPECT_NE(result.content.find("schema-fields-root-contract [HIGH/schema]"), std::string::npos);
    EXPECT_NE(result.content.find("load_schema | confctl/schema.py; fields | top-level yaml"), std::string::npos);
    EXPECT_NE(result.content.find("store-schema-model-mismatch [HIGH/store]"), std::string::npos);

    const auto payload = nlohmann::json::parse(raw_json_text(result.content));
    EXPECT_EQ(payload.at("command").get<std::string>(), "validate-review");
    EXPECT_FALSE(payload.at("stage").at("passed").get<bool>());
}

TEST_F(PlannerToolsTest, ValidateBugfixAcceptsCaseIdAndInlineText) {
    const fs::path external_repo_root = fs::temp_directory_path() / "omni_engine_external_bugfix_repo";
    fs::remove_all(external_repo_root);
    write_external_bridge_stub(external_repo_root);
    write_tracked_case(
        external_repo_root,
        "bugfix",
        "concurrent_submit_guard_case.json",
        "{\"id\": \"concurrent-submit-guard\", \"kind\": \"bugfix\"}\n");

    ScopedEnvVar planner_bridge("OMNIAGENT_PLANNER_BRIDGE");
    ScopedEnvVar planner_repo_root("OMNIAGENT_REPO_ROOT");
    planner_bridge.unset();
    planner_repo_root.set(external_repo_root.string());
    fs::remove_all(workspace_root_ / "planner-harness");

    PlannerValidateBugfixTool tool;
    const auto result = tool.call(
        nlohmann::json{{"case_id", "concurrent_submit_guard_case"}, {"report_text", "Repro, root cause, fix, verification"}},
        context_);

    fs::remove_all(external_repo_root);

    ASSERT_FALSE(result.is_error) << result.content;
    const auto payload = nlohmann::json::parse(result.content);
    EXPECT_EQ(payload.at("command").get<std::string>(), "validate-bugfix");
    EXPECT_NE(payload.at("case_path").get<std::string>().find("concurrent_submit_guard_case.json"), std::string::npos);
    EXPECT_EQ(payload.at("report_source").get<std::string>(), "inline_text");
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