#include <gtest/gtest.h>

#include <omni/approval.h>
#include <omni/host.h>
#include <omni/observer.h>
#include <omni/project_session.h>
#include <omni/provider.h>
#include <omni/tool.h>
#include <omni/types.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace omni::engine;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

class PHRunObserver : public RunObserver {
public:
    void on_event(const Event& event,
                  const std::string& project_id,
                  const std::string& session_id,
                  const std::string& run_id) override {
        events.push_back(event);
        project_ids.push_back(project_id);
        session_ids.push_back(session_id);
        run_ids.push_back(run_id);
    }

    std::vector<Event> events;
    std::vector<std::string> project_ids;
    std::vector<std::string> session_ids;
    std::vector<std::string> run_ids;
};

class PHAutoApprove : public ApprovalDelegate {
public:
    ApprovalDecision on_approval_requested(const std::string&,
                                           const nlohmann::json&,
                                           const std::string&) override {
        return ApprovalDecision::Approve;
    }
};

struct ProviderState {
    std::mutex mutex;
    std::condition_variable started_cv;
    std::condition_variable release_cv;
    bool started = false;
    bool released = false;
    bool blocking = false;
    std::string text = "reply";
    std::vector<std::vector<std::string>> seen_tool_names;
    std::vector<std::string> seen_system_prompts;
};

class PHProvider : public LLMProvider {
public:
    explicit PHProvider(std::shared_ptr<ProviderState> state)
        : state_(std::move(state)) {}

    Usage complete(const CompletionRequest& request,
                   StreamCallback cb,
                   std::atomic<bool>& stop_flag) override {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            std::vector<std::string> tool_names;
            for (const auto& tool : request.tools) {
                tool_names.push_back(tool.at("function").at("name").get<std::string>());
            }
            state_->seen_tool_names.push_back(std::move(tool_names));
            state_->seen_system_prompts.push_back(request.system_prompt);

            if (state_->blocking) {
                state_->started = true;
                state_->started_cv.notify_all();
            }
        }

        if (state_->blocking) {
            std::unique_lock<std::mutex> lock(state_->mutex);
            state_->release_cv.wait(lock, [this]() { return state_->released; });
        }

        if (stop_flag.load(std::memory_order_relaxed)) {
            return {};
        }

        StreamEventData event;
        event.type = StreamEventType::MessageStart;
        cb(event);

        event = {};
        event.type = StreamEventType::ContentBlockStart;
        event.index = 0;
        event.delta_type = "text";
        cb(event);

        event = {};
        event.type = StreamEventType::ContentBlockDelta;
        event.index = 0;
        event.delta_type = "text_delta";
        event.delta_text = state_->text;
        cb(event);

        event = {};
        event.type = StreamEventType::ContentBlockStop;
        event.index = 0;
        cb(event);

        event = {};
        event.type = StreamEventType::MessageDelta;
        event.stop_reason = "end_turn";
        event.usage = {1, 1, 0};
        cb(event);

        event = {};
        event.type = StreamEventType::MessageStop;
        cb(event);

        return {1, 1, 0};
    }

    ModelCapabilities capabilities() const override { return {}; }
    std::string name() const override { return "project-host-provider"; }

private:
    std::shared_ptr<ProviderState> state_;
};

class PHReadTool : public Tool {
public:
    std::string name() const override { return "ph_read"; }
    std::string description() const override { return "Read tool"; }
    nlohmann::json input_schema() const override { return {}; }
    bool is_read_only() const override { return true; }
    bool is_destructive() const override { return false; }
    ToolCallResult call(const nlohmann::json&) override { return {"read", false}; }
};

class PHWriteTool : public Tool {
public:
    std::string name() const override { return "ph_write"; }
    std::string description() const override { return "Write tool"; }
    nlohmann::json input_schema() const override { return {}; }
    bool is_read_only() const override { return false; }
    bool is_destructive() const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"write", false}; }
};

class PHWriteFileTool : public Tool {
public:
    std::string name() const override { return "write_file"; }
    std::string description() const override { return "Write file tool"; }
    nlohmann::json input_schema() const override { return {}; }
    bool is_read_only() const override { return false; }
    bool is_destructive() const override { return false; }
    ToolCallResult call(const nlohmann::json&) override { return {"write file", false}; }
};

class PHNetworkTool : public Tool {
public:
    std::string name() const override { return "ph_network"; }
    std::string description() const override { return "Network tool"; }
    nlohmann::json input_schema() const override { return {}; }
    bool is_read_only() const override { return true; }
    bool is_destructive() const override { return false; }
    bool is_network() const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"network", false}; }
};

class PHPlannerBuildTool : public Tool {
public:
    std::string name() const override { return "planner_build_plan"; }
    std::string description() const override { return "Planner build tool"; }
    nlohmann::json input_schema() const override { return {}; }
    bool is_read_only() const override { return false; }
    bool is_destructive() const override { return false; }
    bool is_network() const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"planner", false}; }
};

class PHPlannerValidateReviewTool : public Tool {
public:
    std::string name() const override { return "planner_validate_review"; }
    std::string description() const override { return "Planner review validation tool"; }
    nlohmann::json input_schema() const override { return {}; }
    bool is_read_only() const override { return true; }
    bool is_destructive() const override { return false; }
    bool is_network() const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"planner review", false}; }
};

class PHPlannerValidateBugfixTool : public Tool {
public:
    std::string name() const override { return "planner_validate_bugfix"; }
    std::string description() const override { return "Planner bugfix validation tool"; }
    nlohmann::json input_schema() const override { return {}; }
    bool is_read_only() const override { return true; }
    bool is_destructive() const override { return false; }
    bool is_network() const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"planner bugfix", false}; }
};

class PHPlannerRepairTool : public Tool {
public:
    std::string name() const override { return "planner_repair_plan"; }
    std::string description() const override { return "Planner repair tool"; }
    nlohmann::json input_schema() const override { return {}; }
    bool is_read_only() const override { return false; }
    bool is_destructive() const override { return false; }
    bool is_network() const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"planner repair", false}; }
};

class PHPlannerBuildFromIdeaTool : public Tool {
public:
    std::string name() const override { return "planner_build_from_idea"; }
    std::string description() const override { return "Planner idea workflow tool"; }
    nlohmann::json input_schema() const override { return {}; }
    bool is_read_only() const override { return false; }
    bool is_destructive() const override { return false; }
    bool is_network() const override { return true; }
    ToolCallResult call(const nlohmann::json&) override { return {"planner idea", false}; }
};

class PHPlannerClarificationTool : public Tool {
public:
    std::string name() const override { return "planner_build_plan"; }
    std::string description() const override { return "Planner build tool requiring clarification"; }
    nlohmann::json input_schema() const override { return {}; }
    bool is_read_only() const override { return false; }
    bool is_destructive() const override { return false; }
    bool is_network() const override { return true; }
    ToolCallResult call(const nlohmann::json&) override {
        return {
            "PLANNER_BUILD_PLAN STATUS: CLARIFICATION_REQUIRED\n"
            "workflow_passed: false\n"
            "clarification_required: true\n"
            "questions:\n"
            "- spec-clar-gap-001: Should queues be tenant scoped?\n"
            "raw_json:\n"
            "{\n"
            "  \"command\": \"run\",\n"
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
            "        \"quote\": \"Tenant isolation is not specified\",\n"
            "        \"question\": \"Should queues be tenant scoped?\",\n"
            "        \"recommended_default\": \"Enforce tenant_id at dequeue\",\n"
            "        \"answer_type\": \"text\",\n"
            "        \"options\": []\n"
            "      }\n"
            "    ]\n"
            "  }\n"
            "}",
            true,
        };
    }
};

class PHToolTurnProvider : public LLMProvider {
public:
    Usage complete(const CompletionRequest&, StreamCallback cb,
                   std::atomic<bool>& stop_flag) override {
        if (stop_flag.load(std::memory_order_relaxed)) {
            return {};
        }

        StreamEventData event;
        event.type = StreamEventType::MessageStart;
        cb(event);

        if (call_count_++ == 0) {
            event = {};
            event.type = StreamEventType::ContentBlockStart;
            event.index = 0;
            event.delta_type = "tool_use";
            event.tool_id = "tool-1";
            event.tool_name = "ph_write";
            cb(event);

            event = {};
            event.type = StreamEventType::ContentBlockStop;
            event.index = 0;
            event.tool_input = nlohmann::json::object();
            cb(event);

            event = {};
            event.type = StreamEventType::MessageDelta;
            event.stop_reason = "tool_use";
            event.usage = {1, 1, 0};
            cb(event);
        } else {
            event = {};
            event.type = StreamEventType::ContentBlockStart;
            event.index = 0;
            event.delta_type = "text";
            cb(event);

            event = {};
            event.type = StreamEventType::ContentBlockDelta;
            event.index = 0;
            event.delta_type = "text_delta";
            event.delta_text = "after approval";
            cb(event);

            event = {};
            event.type = StreamEventType::ContentBlockStop;
            event.index = 0;
            cb(event);

            event = {};
            event.type = StreamEventType::MessageDelta;
            event.stop_reason = "end_turn";
            event.usage = {1, 1, 0};
            cb(event);
        }

        event = {};
        event.type = StreamEventType::MessageStop;
        cb(event);
        return {1, 1, 0};
    }

    ModelCapabilities capabilities() const override { return {}; }
    std::string name() const override { return "pause-provider"; }

private:
    int call_count_ = 0;
};

class PHClarificationProvider : public LLMProvider {
public:
    Usage complete(const CompletionRequest&, StreamCallback cb,
                   std::atomic<bool>& stop_flag) override {
        if (stop_flag.load(std::memory_order_relaxed)) {
            return {};
        }

        StreamEventData event;
        event.type = StreamEventType::MessageStart;
        cb(event);

        if (call_count_++ == 0) {
            event = {};
            event.type = StreamEventType::ContentBlockStart;
            event.index = 0;
            event.delta_type = "tool_use";
            event.tool_id = "tool-clar-1";
            event.tool_name = "planner_build_plan";
            cb(event);

            event = {};
            event.type = StreamEventType::ContentBlockStop;
            event.index = 0;
            event.tool_input = nlohmann::json::object();
            cb(event);

            event = {};
            event.type = StreamEventType::MessageDelta;
            event.stop_reason = "tool_use";
            event.usage = {1, 1, 0};
            cb(event);
        } else if (call_count_ == 2) {
            event = {};
            event.type = StreamEventType::ContentBlockStart;
            event.index = 0;
            event.delta_type = "text";
            cb(event);

            event = {};
            event.type = StreamEventType::ContentBlockDelta;
            event.index = 0;
            event.delta_type = "text_delta";
            event.delta_text = "Need clarification before I can continue.";
            cb(event);

            event = {};
            event.type = StreamEventType::ContentBlockStop;
            event.index = 0;
            cb(event);

            event = {};
            event.type = StreamEventType::MessageDelta;
            event.stop_reason = "end_turn";
            event.usage = {1, 1, 0};
            cb(event);
        } else {
            event = {};
            event.type = StreamEventType::ContentBlockStart;
            event.index = 0;
            event.delta_type = "text";
            cb(event);

            event = {};
            event.type = StreamEventType::ContentBlockDelta;
            event.index = 0;
            event.delta_type = "text_delta";
            event.delta_text = "Clarification applied. Planning can continue.";
            cb(event);

            event = {};
            event.type = StreamEventType::ContentBlockStop;
            event.index = 0;
            cb(event);

            event = {};
            event.type = StreamEventType::MessageDelta;
            event.stop_reason = "end_turn";
            event.usage = {1, 1, 0};
            cb(event);
        }

        event = {};
        event.type = StreamEventType::MessageStop;
        cb(event);
        return {1, 1, 0};
    }

    ModelCapabilities capabilities() const override { return {}; }
    std::string name() const override { return "clarification-provider"; }

private:
    int call_count_ = 0;
};

class PHPauseApprove : public ApprovalDelegate {
public:
    ApprovalDecision on_approval_requested(const std::string& tool_name,
                                           const nlohmann::json&,
                                           const std::string&) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requested_tool_ = tool_name;
            requested_ = true;
        }
        requested_cv_.notify_all();
        return ApprovalDecision::Pause;
    }

    void wait_until_requested() {
        std::unique_lock<std::mutex> lock(mutex_);
        requested_cv_.wait(lock, [this]() { return requested_; });
    }

    const std::string& requested_tool() const {
        return requested_tool_;
    }

private:
    std::mutex mutex_;
    std::condition_variable requested_cv_;
    bool requested_ = false;
    std::string requested_tool_;
};

ProjectRuntimeConfig make_config(const fs::path& workspace_root,
                                 const fs::path& storage_dir,
                                 std::shared_ptr<ProviderState> provider_state) {
    ProjectRuntimeConfig config;
    config.workspace.project_id = "demo-project";
    config.workspace.workspace_root = workspace_root;
    config.workspace.working_dir = workspace_root;
    config.engine.session_storage_dir = storage_dir;
    config.provider_factory = [provider_state]() {
        return std::make_unique<PHProvider>(provider_state);
    };
    return config;
}

ProjectRuntimeConfig make_pause_config(const fs::path& workspace_root,
                                       const fs::path& storage_dir) {
    ProjectRuntimeConfig config;
    config.workspace.project_id = "demo-project";
    config.workspace.workspace_root = workspace_root;
    config.workspace.working_dir = workspace_root;
    config.engine.session_storage_dir = storage_dir;
    ToolCapabilityPolicy approval_policy;
    approval_policy.allow_write = true;
    approval_policy.allow_destructive = true;
    config.profiles.push_back(AgentProfileManifest{
        .name = "approval-bugfix",
        .system_prompt = "Use edit tools, but require explicit approval before doing so.",
        .tool_policy = approval_policy,
        .default_permission_mode = PermissionMode::Default,
        .sub_agents_allowed = false,
        .max_parallel_tools = 10,
    });
    config.provider_factory = []() {
        return std::make_unique<PHToolTurnProvider>();
    };
    return config;
}

ProjectRuntimeConfig make_clarification_config(const fs::path& workspace_root,
                                               const fs::path& storage_dir) {
    ProjectRuntimeConfig config;
    config.workspace.project_id = "demo-project";
    config.workspace.workspace_root = workspace_root;
    config.workspace.working_dir = workspace_root;
    config.engine.session_storage_dir = storage_dir;
    config.provider_factory = []() {
        return std::make_unique<PHClarificationProvider>();
    };
    return config;
}

bool wait_for_run_status(ProjectEngineHost& host,
                         const std::string& run_id,
                         RunStatus status,
                         std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto run = host.get_run(run_id);
        if (run && run->status == status) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

class ProjectHostTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = fs::temp_directory_path() / fs::path("omni_project_host_workspace");
        storage_dir_ = fs::temp_directory_path() / fs::path("omni_project_host_sessions");
        fs::remove_all(root_dir_);
        fs::remove_all(storage_dir_);
        fs::create_directories(root_dir_ / "subdir");
        provider_state_ = std::make_shared<ProviderState>();
    }

    void TearDown() override {
        fs::remove_all(root_dir_);
        fs::remove_all(storage_dir_);
    }

    fs::path root_dir_;
    fs::path storage_dir_;
    std::shared_ptr<ProviderState> provider_state_;
};

}  // namespace

TEST_F(ProjectHostTest, SessionsPreserveHistoryWithinSessionOnly) {
    auto host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));

    PHAutoApprove approvals;
    PHRunObserver observer;

    auto first_session = host->open_session();
    auto first_run = first_session->submit_turn("hello", observer, approvals);
    first_run->wait();
    auto first_snapshot = first_session->snapshot();
    EXPECT_EQ(first_snapshot.messages.size(), 2u);

    auto second_run = first_session->submit_turn("again", observer, approvals);
    second_run->wait();
    first_snapshot = first_session->snapshot();
    EXPECT_EQ(first_snapshot.messages.size(), 4u);

    auto second_session = host->open_session();
    auto second_session_snapshot = second_session->snapshot();
    EXPECT_TRUE(second_session_snapshot.messages.empty());

    auto list = host->list_sessions();
    EXPECT_EQ(list.size(), 2u);
}

TEST_F(ProjectHostTest, ResumeSessionRestoresPersistedMessages) {
    auto host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));

    PHAutoApprove approvals;
    PHRunObserver observer;

    std::string session_id;
    {
        auto session = host->open_session();
        session_id = session->session_id();
        auto run = session->submit_turn("persist me", observer, approvals);
        run->wait();
        session->close();
    }

    auto resumed = host->resume_session(session_id);
    auto snapshot = resumed->snapshot();
    EXPECT_EQ(snapshot.session_id, session_id);
    EXPECT_EQ(snapshot.messages.size(), 2u);
}

TEST_F(ProjectHostTest, RewindMessagesRemovesTailAndPersists) {
    auto host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));

    PHAutoApprove approvals;
    PHRunObserver observer;

    auto session = host->open_session(SessionOptions{
        .profile = "bugfix",
        .session_id = std::nullopt,
        .working_dir_override = root_dir_ / "subdir",
    });
    auto first_run = session->submit_turn("first", observer, approvals);
    first_run->wait();
    auto second_run = session->submit_turn("second", observer, approvals);
    second_run->wait();

    EXPECT_EQ(session->snapshot().messages.size(), 4u);
    EXPECT_EQ(session->rewind_messages(), 1u);
    EXPECT_EQ(session->snapshot().messages.size(), 3u);
    EXPECT_EQ(session->rewind_messages(10), 3u);
    EXPECT_TRUE(session->snapshot().messages.empty());
    EXPECT_EQ(session->rewind_messages(), 0u);

    const auto session_id = session->session_id();
    session->close();

    auto resumed = host->resume_session(session_id);
    EXPECT_TRUE(resumed->snapshot().messages.empty());
}

TEST_F(ProjectHostTest, RewindMessagesRejectsActiveRun) {
    provider_state_->blocking = true;
    auto host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));

    PHAutoApprove approvals;
    PHRunObserver observer;

    auto session = host->open_session();
    auto run = session->submit_turn("hold", observer, approvals);
    {
        std::unique_lock<std::mutex> lock(provider_state_->mutex);
        provider_state_->started_cv.wait(lock, [this]() { return provider_state_->started; });
    }

    EXPECT_THROW(session->rewind_messages(), std::runtime_error);

    run->cancel();
    {
        std::lock_guard<std::mutex> lock(provider_state_->mutex);
        provider_state_->released = true;
    }
    provider_state_->release_cv.notify_all();
    run->wait();
}

TEST_F(ProjectHostTest, ForkSessionClonesMessagesAndPreservesContext) {
    auto host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));

    PHAutoApprove approvals;
    PHRunObserver observer;

    auto source = host->open_session(SessionOptions{
        .profile = "bugfix",
        .session_id = std::nullopt,
        .working_dir_override = root_dir_ / "subdir",
    });
    auto source_run = source->submit_turn("source turn", observer, approvals);
    source_run->wait();

    const auto source_snapshot = source->snapshot();
    auto forked = host->fork_session(source->session_id());
    const auto fork_snapshot = forked->snapshot();

    EXPECT_NE(forked->session_id(), source->session_id());
    EXPECT_EQ(fork_snapshot.messages.size(), source_snapshot.messages.size());
    EXPECT_EQ(fork_snapshot.active_profile, source_snapshot.active_profile);
    EXPECT_EQ(fork_snapshot.working_dir, source_snapshot.working_dir);

    auto fork_run = forked->submit_turn("fork only", observer, approvals);
    fork_run->wait();

    EXPECT_EQ(source->snapshot().messages.size(), source_snapshot.messages.size());
    EXPECT_GT(forked->snapshot().messages.size(), source_snapshot.messages.size());
}

TEST_F(ProjectHostTest, ForkSessionRejectsDuplicateExplicitSessionId) {
    auto host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));

    PHAutoApprove approvals;
    PHRunObserver observer;

    auto source = host->open_session();
    auto source_run = source->submit_turn("source turn", observer, approvals);
    source_run->wait();

    SessionOptions options;
    options.session_id = source->session_id();
    EXPECT_THROW(host->fork_session(source->session_id(), options), std::runtime_error);
}

TEST_F(ProjectHostTest, RejectsWorkingDirectoryOutsideWorkspace) {
    auto host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));
    const auto outside = fs::temp_directory_path();

    EXPECT_THROW(host->open_session(SessionOptions{
        .profile = "explore",
        .session_id = std::nullopt,
        .working_dir_override = outside,
    }), std::invalid_argument);
}

TEST_F(ProjectHostTest, SystemPromptIncludesProjectWorkspaceIdentity) {
    auto host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));

    PHAutoApprove approvals;
    PHRunObserver observer;

    auto session = host->open_session(SessionOptions{
        .profile = "bugfix",
        .session_id = std::nullopt,
        .working_dir_override = root_dir_ / "subdir",
    });
    auto run = session->submit_turn("where am i?", observer, approvals);
    run->wait();

    ASSERT_FALSE(provider_state_->seen_system_prompts.empty());
    const auto& prompt = provider_state_->seen_system_prompts.back();

    EXPECT_NE(prompt.find("project-owned software engineering agent"), std::string::npos);
    EXPECT_NE(prompt.find("Project id: demo-project"), std::string::npos);
    EXPECT_NE(prompt.find("Workspace root: " + root_dir_.string()), std::string::npos);
    EXPECT_NE(prompt.find("Current working directory: " + (root_dir_ / "subdir").string()), std::string::npos);
    EXPECT_NE(prompt.find("Active profile: bugfix"), std::string::npos);
    EXPECT_NE(prompt.find("Default to concise, task-focused responses."), std::string::npos);
    EXPECT_NE(prompt.find("Do not introduce yourself, restate your role, or narrate routine background work"),
              std::string::npos);
    EXPECT_NE(prompt.find("Do not claim that you lack a filesystem"), std::string::npos);
    EXPECT_NE(prompt.find("Do not assume the omniagent repository is relevant unless it is the selected workspace."),
              std::string::npos);
    EXPECT_NE(prompt.find("root cause"), std::string::npos);
    EXPECT_NE(prompt.find("smallest defensible fix"), std::string::npos);
}

TEST_F(ProjectHostTest, CoordinatorConversationalTurnsCompleteWithoutDelegation) {
    auto host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));

    PHAutoApprove approvals;
    PHRunObserver observer;

    auto session = host->open_session(SessionOptions{
        .profile = "coordinator",
        .session_id = std::nullopt,
        .working_dir_override = std::nullopt,
    });
    provider_state_->text = "GitHub Copilot.";
    auto run = session->submit_turn("what is you rname", observer, approvals);
    run->wait();

    const auto result = run->result();
    EXPECT_EQ(result.status, RunStatus::Completed);
    ASSERT_EQ(provider_state_->seen_tool_names.size(), 1u);
    EXPECT_TRUE(provider_state_->seen_tool_names.back().empty());
    ASSERT_EQ(provider_state_->seen_system_prompts.size(), 1u);

    const auto snapshot = session->snapshot();
    ASSERT_EQ(snapshot.messages.size(), 2u);
    const auto& assistant = snapshot.messages.back();
    ASSERT_EQ(assistant.role, Role::Assistant);
    ASSERT_EQ(assistant.content.size(), 1u);
    const auto* text = std::get_if<TextContent>(&assistant.content.front().data);
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->text, "GitHub Copilot.");

    bool saw_tool = false;
    bool saw_agent = false;
    bool saw_text = false;
    for (const auto& event : observer.events) {
        saw_tool = saw_tool || std::holds_alternative<ToolUseStartEvent>(event);
        saw_agent = saw_agent || std::holds_alternative<AgentSpawnedEvent>(event)
            || std::holds_alternative<AgentCompletedEvent>(event);
        if (const auto* delta = std::get_if<TextDeltaEvent>(&event)) {
            saw_text = saw_text || delta->text == "GitHub Copilot.";
        }
    }
    EXPECT_FALSE(saw_tool);
    EXPECT_FALSE(saw_agent);
    EXPECT_TRUE(saw_text);
}

TEST_F(ProjectHostTest, CoordinatorAuditTurnsUseAuditProfileDirectly) {
    auto host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));
    host->register_tool(std::make_unique<PHReadTool>());
    host->register_tool(std::make_unique<PHWriteTool>());

    PHAutoApprove approvals;
    PHRunObserver observer;

    auto session = host->open_session(SessionOptions{
        .profile = "coordinator",
        .session_id = std::nullopt,
        .working_dir_override = std::nullopt,
    });
    provider_state_->text = "No findings.";
    auto run = session->submit_turn("can you do a code audit for me?", observer, approvals);
    run->wait();

    const auto result = run->result();
    EXPECT_EQ(result.status, RunStatus::Completed);
    EXPECT_EQ(result.profile, "audit");
    ASSERT_FALSE(provider_state_->seen_tool_names.empty());
    const auto audit_request = std::find_if(
        provider_state_->seen_tool_names.rbegin(),
        provider_state_->seen_tool_names.rend(),
        [](const std::vector<std::string>& names) {
            return std::find(names.begin(), names.end(), "ph_read") != names.end();
        });
    ASSERT_NE(audit_request, provider_state_->seen_tool_names.rend());
    const auto& tool_names = *audit_request;
    EXPECT_NE(std::find(tool_names.begin(), tool_names.end(), "ph_read"), tool_names.end());
    EXPECT_EQ(std::find(tool_names.begin(), tool_names.end(), "ph_write"), tool_names.end());
    EXPECT_EQ(std::find(tool_names.begin(), tool_names.end(), "agent"), tool_names.end());
    EXPECT_EQ(std::find(tool_names.begin(), tool_names.end(), "send_message"), tool_names.end());

    ASSERT_FALSE(provider_state_->seen_system_prompts.empty());
    EXPECT_NE(provider_state_->seen_system_prompts.back().find("Active profile: audit"), std::string::npos);

    bool saw_agent = false;
    for (const auto& event : observer.events) {
        saw_agent = saw_agent || std::holds_alternative<AgentSpawnedEvent>(event)
            || std::holds_alternative<AgentCompletedEvent>(event);
    }
    EXPECT_FALSE(saw_agent);
}

TEST_F(ProjectHostTest, RunCancelAndStopUpdateStatus) {
    provider_state_->blocking = true;
    auto host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));

    PHAutoApprove approvals;
    PHRunObserver observer;

    auto cancel_session = host->open_session();
    auto cancel_run = cancel_session->submit_turn("cancel me", observer, approvals);
    {
        std::unique_lock<std::mutex> lock(provider_state_->mutex);
        provider_state_->started_cv.wait(lock, [this]() { return provider_state_->started; });
    }
    cancel_run->cancel();
    {
        std::lock_guard<std::mutex> lock(provider_state_->mutex);
        provider_state_->released = true;
    }
    provider_state_->release_cv.notify_all();
    cancel_run->wait();
    EXPECT_EQ(cancel_run->result().status, RunStatus::Cancelled);

    provider_state_->blocking = true;
    provider_state_->started = false;
    provider_state_->released = false;

    auto stop_session = host->open_session();
    auto stop_run = stop_session->submit_turn("stop me", observer, approvals);
    {
        std::unique_lock<std::mutex> lock(provider_state_->mutex);
        provider_state_->started_cv.wait(lock, [this]() { return provider_state_->started; });
    }
    stop_run->stop();
    {
        std::lock_guard<std::mutex> lock(provider_state_->mutex);
        provider_state_->released = true;
    }
    provider_state_->release_cv.notify_all();
    stop_run->wait();
    EXPECT_EQ(stop_run->result().status, RunStatus::Stopped);
}

TEST_F(ProjectHostTest, HostDestructorFinalizesActiveRun) {
    provider_state_->blocking = true;
    auto host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));

    PHAutoApprove approvals;
    PHRunObserver observer;

    auto session = host->open_session();
    auto run = session->submit_turn("persist on shutdown", observer, approvals);
    {
        std::unique_lock<std::mutex> lock(provider_state_->mutex);
        provider_state_->started_cv.wait(lock, [this]() { return provider_state_->started; });
    }

    std::thread destroy_host([&host]() {
        host.reset();
    });

    {
        std::lock_guard<std::mutex> lock(provider_state_->mutex);
        provider_state_->released = true;
    }
    provider_state_->release_cv.notify_all();
    destroy_host.join();

    run->wait();
    EXPECT_NE(run->result().status, RunStatus::Running);
    EXPECT_NE(run->result().finished_at, std::chrono::system_clock::time_point{});

    auto reloaded_host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));
    auto persisted = reloaded_host->get_run(run->run_id());
    ASSERT_TRUE(persisted.has_value());
    EXPECT_NE(persisted->status, RunStatus::Running);
    EXPECT_NE(persisted->finished_at, std::chrono::system_clock::time_point{});
}

TEST_F(ProjectHostTest, ProfileFilteringChangesVisibleTools) {
    auto host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));
    host->register_tool(std::make_unique<PHReadTool>());
    host->register_tool(std::make_unique<PHWriteTool>());
    host->register_tool(std::make_unique<PHWriteFileTool>());
    host->register_tool(std::make_unique<PHNetworkTool>());
    host->register_tool(std::make_unique<PHPlannerBuildTool>());
    host->register_tool(std::make_unique<PHPlannerValidateReviewTool>());
    host->register_tool(std::make_unique<PHPlannerValidateBugfixTool>());

    PHAutoApprove approvals;
    PHRunObserver observer;

    SessionOptions options;
    options.profile = "explore";
    auto session = host->open_session(options);
    auto run = session->submit_turn("tools", observer, approvals);
    run->wait();

    ASSERT_FALSE(provider_state_->seen_tool_names.empty());
    const auto explore_tools = provider_state_->seen_tool_names.back();
    EXPECT_NE(std::find(explore_tools.begin(), explore_tools.end(), "ph_read"), explore_tools.end());
    EXPECT_EQ(std::find(explore_tools.begin(), explore_tools.end(), "ph_write"), explore_tools.end());
    EXPECT_EQ(std::find(explore_tools.begin(), explore_tools.end(), "write_file"), explore_tools.end());
    EXPECT_EQ(std::find(explore_tools.begin(), explore_tools.end(), "ph_network"), explore_tools.end());
    EXPECT_EQ(std::find(explore_tools.begin(), explore_tools.end(), "planner_build_plan"), explore_tools.end());
    EXPECT_EQ(std::find(explore_tools.begin(), explore_tools.end(), "planner_validate_review"), explore_tools.end());
    EXPECT_EQ(std::find(explore_tools.begin(), explore_tools.end(), "planner_validate_bugfix"), explore_tools.end());

    session->set_profile("audit");
    auto audit_run = session->submit_turn("audit tools", observer, approvals);
    audit_run->wait();

    ASSERT_GE(provider_state_->seen_tool_names.size(), 3u);
    const auto& audit_tools = provider_state_->seen_tool_names[provider_state_->seen_tool_names.size() - 2];
    EXPECT_NE(std::find(audit_tools.begin(), audit_tools.end(), "ph_read"), audit_tools.end());
    EXPECT_EQ(std::find(audit_tools.begin(), audit_tools.end(), "ph_write"), audit_tools.end());
    EXPECT_EQ(std::find(audit_tools.begin(), audit_tools.end(), "write_file"), audit_tools.end());
    EXPECT_EQ(std::find(audit_tools.begin(), audit_tools.end(), "ph_network"), audit_tools.end());
    EXPECT_EQ(std::find(audit_tools.begin(), audit_tools.end(), "planner_build_plan"), audit_tools.end());
    EXPECT_NE(std::find(audit_tools.begin(), audit_tools.end(), "planner_validate_review"), audit_tools.end());
    EXPECT_EQ(std::find(audit_tools.begin(), audit_tools.end(), "planner_validate_bugfix"), audit_tools.end());

    session->set_profile("feature");
    auto feature_run = session->submit_turn("feature tools", observer, approvals);
    feature_run->wait();

    const auto feature_tools = provider_state_->seen_tool_names.back();
    EXPECT_NE(std::find(feature_tools.begin(), feature_tools.end(), "ph_read"), feature_tools.end());
    EXPECT_EQ(std::find(feature_tools.begin(), feature_tools.end(), "ph_write"), feature_tools.end());
    EXPECT_NE(std::find(feature_tools.begin(), feature_tools.end(), "write_file"), feature_tools.end());
    EXPECT_EQ(std::find(feature_tools.begin(), feature_tools.end(), "ph_network"), feature_tools.end());
    EXPECT_NE(std::find(feature_tools.begin(), feature_tools.end(), "planner_build_plan"), feature_tools.end());
    EXPECT_EQ(std::find(feature_tools.begin(), feature_tools.end(), "planner_validate_review"), feature_tools.end());
    EXPECT_EQ(std::find(feature_tools.begin(), feature_tools.end(), "planner_validate_bugfix"), feature_tools.end());

    session->set_profile("refactor");
    auto refactor_run = session->submit_turn("refactor tools", observer, approvals);
    refactor_run->wait();

    const auto refactor_tools = provider_state_->seen_tool_names.back();
    EXPECT_NE(std::find(refactor_tools.begin(), refactor_tools.end(), "ph_read"), refactor_tools.end());
    EXPECT_EQ(std::find(refactor_tools.begin(), refactor_tools.end(), "ph_write"), refactor_tools.end());
    EXPECT_NE(std::find(refactor_tools.begin(), refactor_tools.end(), "write_file"), refactor_tools.end());
    EXPECT_EQ(std::find(refactor_tools.begin(), refactor_tools.end(), "ph_network"), refactor_tools.end());
    EXPECT_NE(std::find(refactor_tools.begin(), refactor_tools.end(), "planner_build_plan"), refactor_tools.end());
    EXPECT_EQ(std::find(refactor_tools.begin(), refactor_tools.end(), "planner_validate_review"), refactor_tools.end());
    EXPECT_EQ(std::find(refactor_tools.begin(), refactor_tools.end(), "planner_validate_bugfix"), refactor_tools.end());

    session->set_profile("bugfix");
    auto bugfix_run = session->submit_turn("tools again", observer, approvals);
    bugfix_run->wait();

    const auto bugfix_tools = provider_state_->seen_tool_names.back();
    EXPECT_NE(std::find(bugfix_tools.begin(), bugfix_tools.end(), "ph_read"), bugfix_tools.end());
    EXPECT_EQ(std::find(bugfix_tools.begin(), bugfix_tools.end(), "ph_write"), bugfix_tools.end());
    EXPECT_NE(std::find(bugfix_tools.begin(), bugfix_tools.end(), "write_file"), bugfix_tools.end());
    EXPECT_EQ(std::find(bugfix_tools.begin(), bugfix_tools.end(), "ph_network"), bugfix_tools.end());
    EXPECT_EQ(std::find(bugfix_tools.begin(), bugfix_tools.end(), "planner_build_plan"), bugfix_tools.end());
    EXPECT_EQ(std::find(bugfix_tools.begin(), bugfix_tools.end(), "planner_validate_review"), bugfix_tools.end());
    EXPECT_NE(std::find(bugfix_tools.begin(), bugfix_tools.end(), "planner_validate_bugfix"), bugfix_tools.end());
}

TEST_F(ProjectHostTest, CoordinatorProfileOnlyExposesDelegationTools) {
    auto host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));
    host->register_tool(std::make_unique<PHReadTool>());
    host->register_tool(std::make_unique<PHWriteTool>());
    host->register_tool(std::make_unique<PHNetworkTool>());

    PHAutoApprove approvals;
    PHRunObserver observer;

    auto session = host->open_session(SessionOptions{.profile = "coordinator"});
    auto run = session->submit_turn("delegate this", observer, approvals);
    run->wait();

    ASSERT_FALSE(provider_state_->seen_tool_names.empty());
    const auto coordinator_tools = provider_state_->seen_tool_names.back();
    EXPECT_NE(std::find(coordinator_tools.begin(), coordinator_tools.end(), "agent"), coordinator_tools.end());
    EXPECT_NE(std::find(coordinator_tools.begin(), coordinator_tools.end(), "send_message"), coordinator_tools.end());
    EXPECT_EQ(std::find(coordinator_tools.begin(), coordinator_tools.end(), "ph_read"), coordinator_tools.end());
    EXPECT_EQ(std::find(coordinator_tools.begin(), coordinator_tools.end(), "ph_write"), coordinator_tools.end());
    EXPECT_EQ(std::find(coordinator_tools.begin(), coordinator_tools.end(), "ph_network"), coordinator_tools.end());
}

TEST_F(ProjectHostTest, PlanProfileAllowsPlannerToolsWithoutGenericNetworkAccess) {
    auto host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));
    host->register_tool(std::make_unique<PHReadTool>());
    host->register_tool(std::make_unique<PHNetworkTool>());
    host->register_tool(std::make_unique<PHPlannerRepairTool>());
    host->register_tool(std::make_unique<PHPlannerBuildTool>());
    host->register_tool(std::make_unique<PHPlannerBuildFromIdeaTool>());

    PHAutoApprove approvals;
    PHRunObserver observer;

    auto session = host->open_session(SessionOptions{.profile = "plan"});
    auto run = session->submit_turn("tools", observer, approvals);
    run->wait();

    ASSERT_FALSE(provider_state_->seen_tool_names.empty());
    const auto plan_tools = provider_state_->seen_tool_names.back();
    EXPECT_NE(std::find(plan_tools.begin(), plan_tools.end(), "ph_read"), plan_tools.end());
    EXPECT_NE(std::find(plan_tools.begin(), plan_tools.end(), "planner_repair_plan"), plan_tools.end());
    EXPECT_NE(std::find(plan_tools.begin(), plan_tools.end(), "planner_build_plan"), plan_tools.end());
    EXPECT_NE(std::find(plan_tools.begin(), plan_tools.end(), "planner_build_from_idea"), plan_tools.end());
    EXPECT_EQ(std::find(plan_tools.begin(), plan_tools.end(), "ph_network"), plan_tools.end());
}

TEST_F(ProjectHostTest, CustomProfilesApplyCapabilityPolicies) {
    auto config = make_config(root_dir_, storage_dir_, provider_state_);
    ToolCapabilityPolicy network_policy;
    network_policy.allow_read_only = true;
    network_policy.allow_network = true;
    config.profiles.push_back(AgentProfileManifest{
        .name = "network-spec",
        .system_prompt = "Use network tools when necessary.",
        .tool_policy = network_policy,
        .default_permission_mode = PermissionMode::Default,
        .sub_agents_allowed = false,
        .max_parallel_tools = 10,
    });

    auto host = ProjectEngineHost::create(std::move(config));
    host->register_tool(std::make_unique<PHReadTool>());
    host->register_tool(std::make_unique<PHNetworkTool>());

    PHAutoApprove approvals;
    PHRunObserver observer;

    auto explore_session = host->open_session(SessionOptions{.profile = "explore"});
    auto explore_run = explore_session->submit_turn("tools", observer, approvals);
    explore_run->wait();
    const auto explore_tools = provider_state_->seen_tool_names.back();
    EXPECT_EQ(std::find(explore_tools.begin(), explore_tools.end(), "ph_network"), explore_tools.end());

    auto session = host->open_session(SessionOptions{.profile = "network-spec"});
    auto run = session->submit_turn("tools", observer, approvals);
    run->wait();

    const auto custom_tools = provider_state_->seen_tool_names.back();
    EXPECT_NE(std::find(custom_tools.begin(), custom_tools.end(), "ph_network"), custom_tools.end());

    const auto tool_summaries = session->tools();
    const auto it = std::find_if(tool_summaries.begin(), tool_summaries.end(),
                                 [](const ToolSummary& summary) {
                                     return summary.name == "ph_network";
                                 });
    ASSERT_NE(it, tool_summaries.end());
    EXPECT_TRUE(it->network);
}

TEST_F(ProjectHostTest, PausedApprovalCanBeResumedByRunId) {
    auto host = ProjectEngineHost::create(make_pause_config(root_dir_, storage_dir_));
    host->register_tool(std::make_unique<PHWriteTool>());

    PHPauseApprove approvals;
    PHRunObserver observer;

    auto session = host->open_session(SessionOptions{.profile = "approval-bugfix"});
    auto run = session->submit_turn("needs approval", observer, approvals);

    approvals.wait_until_requested();
    ASSERT_EQ(approvals.requested_tool(), "ph_write");
    ASSERT_TRUE(wait_for_run_status(*host, run->run_id(), RunStatus::Paused));

    auto paused = host->get_run(run->run_id());
    ASSERT_TRUE(paused.has_value());
    EXPECT_EQ(paused->status, RunStatus::Paused);
    ASSERT_TRUE(paused->pending_approval.has_value());
    EXPECT_EQ(paused->pending_approval->tool_name, "ph_write");

    EXPECT_TRUE(host->resume_run(run->run_id(), "approve"));
    run->wait();

    const auto result = run->result();
    EXPECT_EQ(result.status, RunStatus::Completed);
    EXPECT_EQ(result.output, "after approval");
}

TEST_F(ProjectHostTest, PlannerClarificationPersistsAsPausedRunState) {
    auto host = ProjectEngineHost::create(make_clarification_config(root_dir_, storage_dir_));
    host->register_tool(std::make_unique<PHPlannerClarificationTool>());

    PHAutoApprove approvals;
    PHRunObserver observer;

    auto session = host->open_session();
    auto run = session->submit_turn("build plan", observer, approvals);
    run->wait();

    const auto result = run->result();
    EXPECT_EQ(result.status, RunStatus::Paused);
    ASSERT_TRUE(result.pending_clarification.has_value());
    EXPECT_EQ(result.pause_reason, std::optional<std::string>{"clarification_required"});
    EXPECT_EQ(result.pending_clarification->tool_name, "planner_build_plan");
    EXPECT_EQ(result.pending_clarification->clarification_mode, "required");
    ASSERT_EQ(result.pending_clarification->questions.size(), 1u);
    EXPECT_EQ(result.pending_clarification->questions[0].id, "spec-clar-gap-001");

    auto loaded = host->get_run(run->run_id());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->status, RunStatus::Paused);
    ASSERT_TRUE(loaded->pending_clarification.has_value());
    EXPECT_EQ(loaded->pending_clarification->questions[0].question,
              "Should queues be tenant scoped?");

    EXPECT_TRUE(std::any_of(observer.events.begin(), observer.events.end(), [](const Event& event) {
        return std::holds_alternative<ClarificationRequestedEvent>(event);
    }));
    EXPECT_TRUE(std::any_of(observer.events.begin(), observer.events.end(), [](const Event& event) {
        if (const auto* paused = std::get_if<RunPausedEvent>(&event)) {
            return paused->reason == "clarification_required";
        }
        return false;
    }));
}

TEST_F(ProjectHostTest, ClarificationPausedRunCanBeResumedByRunId) {
    auto host = ProjectEngineHost::create(make_clarification_config(root_dir_, storage_dir_));
    host->register_tool(std::make_unique<PHPlannerClarificationTool>());

    PHAutoApprove approvals;
    PHRunObserver observer;

    auto session = host->open_session();
    auto run = session->submit_turn("build plan", observer, approvals);
    run->wait();

    ASSERT_EQ(run->result().status, RunStatus::Paused);
    ASSERT_EQ(session->snapshot().active_run_id, std::optional<std::string>{run->run_id()});
    EXPECT_THROW(session->submit_turn("another turn", observer, approvals), std::runtime_error);

    ASSERT_TRUE(host->resume_run(run->run_id(), "Queues must be tenant scoped."));
    run->wait();

    const auto result = run->result();
    EXPECT_EQ(result.status, RunStatus::Completed);
    EXPECT_FALSE(result.pending_clarification.has_value());
    EXPECT_FALSE(result.pause_reason.has_value());
    EXPECT_NE(result.output.find("Clarification applied. Planning can continue."), std::string::npos);
    EXPECT_EQ(session->snapshot().active_run_id, std::nullopt);

    EXPECT_TRUE(std::any_of(observer.events.begin(), observer.events.end(), [](const Event& event) {
        return std::holds_alternative<RunResumedEvent>(event);
    }));
}

TEST_F(ProjectHostTest, RunsPersistAndCanBeDeleted) {
    auto host = ProjectEngineHost::create(make_config(root_dir_, storage_dir_, provider_state_));

    PHAutoApprove approvals;
    PHRunObserver observer;

    auto session = host->open_session();
    auto run = session->submit_turn("persist run", observer, approvals);
    run->wait();

    const auto run_id = run->run_id();
    auto loaded = host->get_run(run_id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->status, RunStatus::Completed);
    EXPECT_EQ(loaded->output, "reply");

    const auto runs = host->list_runs();
    EXPECT_TRUE(std::any_of(runs.begin(), runs.end(),
                            [&run_id](const RunSummary& summary) {
                                return summary.run_id == run_id;
                            }));

    EXPECT_TRUE(host->delete_run(run_id));
    EXPECT_FALSE(host->get_run(run_id).has_value());
}