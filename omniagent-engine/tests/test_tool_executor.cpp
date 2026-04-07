#include <gtest/gtest.h>

#include <omni/event.h>
#include <omni/permission.h>
#include <omni/tool.h>
#include <omni/types.h>
#include "permissions/permission_checker.h"
#include "tools/tool_executor.h"
#include "tools/tool_registry.h"

#include <atomic>
#include <string>
#include <vector>

using namespace omni::engine;

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

// EchoTool: returns its input JSON stringified
class EchoTool : public Tool {
public:
    std::string    name()         const override { return "echo"; }
    std::string    description()  const override { return "Echo args back"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()    const override { return true; }
    bool           is_destructive()  const override { return false; }

    ToolCallResult call(const nlohmann::json& args) override {
        return {args.dump(), false};
    }
};

// RequiredFieldTool: has a required "query" field in schema
class RequiredFieldTool : public Tool {
public:
    std::string    name()         const override { return "required_tool"; }
    std::string    description()  const override { return "Requires query field"; }
    nlohmann::json input_schema() const override {
        return nlohmann::json{
            {"type", "object"},
            {"required", nlohmann::json::array({"query"})},
            {"properties", {{"query", {{"type", "string"}}}}}
        };
    }
    bool is_read_only()   const override { return true; }
    bool is_destructive() const override { return false; }

    ToolCallResult call(const nlohmann::json& args) override {
        return {args["query"].get<std::string>(), false};
    }
};

// FailTool: always returns an error result
class FailTool : public Tool {
public:
    std::string    name()         const override { return "fail"; }
    std::string    description()  const override { return "Always fails"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()    const override { return false; }
    bool           is_destructive()  const override { return false; }

    ToolCallResult call(const nlohmann::json&) override {
        return {"something went wrong", true};
    }
};

// ReadOnlyFailTool: read-only but always returns an error (for doom-loop testing)
class ReadOnlyFailTool : public Tool {
public:
    std::string    name()         const override { return "ro_fail"; }
    std::string    description()  const override { return "Read-only, always fails"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()    const override { return true; }
    bool           is_destructive()  const override { return false; }

    ToolCallResult call(const nlohmann::json&) override {
        return {"ro failure", true};
    }
};

// WriteTool: write (non-read-only), succeeds
class WriteTool : public Tool {
public:
    std::string    name()         const override { return "write"; }
    std::string    description()  const override { return "Write tool"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()    const override { return false; }
    bool           is_destructive()  const override { return false; }

    ToolCallResult call(const nlohmann::json&) override {
        return {"written", false};
    }
};

// BigResultTool: returns a string of configurable length
class BigResultTool : public Tool {
public:
    explicit BigResultTool(std::size_t size) : size_(size) {}

    std::string    name()         const override { return "big"; }
    std::string    description()  const override { return "Returns large result"; }
    nlohmann::json input_schema() const override { return {}; }
    bool           is_read_only()    const override { return true; }
    bool           is_destructive()  const override { return false; }

    ToolCallResult call(const nlohmann::json&) override {
        return {std::string(size_, 'x'), false};
    }

private:
    std::size_t size_;
};

class AllowAllDelegate : public PermissionDelegate {
public:
    PermissionDecision on_permission_request(const std::string&,
                                              const nlohmann::json&,
                                              const std::string&) override {
        return PermissionDecision::Allow;
    }
};

class DenyAllDelegate : public PermissionDelegate {
public:
    PermissionDecision on_permission_request(const std::string&,
                                              const nlohmann::json&,
                                              const std::string&) override {
        return PermissionDecision::Deny;
    }
};

class CollectingObserver : public EventObserver {
public:
    std::vector<Event> events;

    void on_event(const Event& e) override {
        events.push_back(e);
    }

    int count_starts() const {
        int n = 0;
        for (const auto& e : events) {
            if (std::holds_alternative<ToolUseStartEvent>(e)) ++n;
        }
        return n;
    }

    int count_results() const {
        int n = 0;
        for (const auto& e : events) {
            if (std::holds_alternative<ToolResultEvent>(e)) ++n;
        }
        return n;
    }
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class ToolExecutorTest : public ::testing::Test {
protected:
    ToolRegistry       registry;
    AllowAllDelegate   allow_delegate;
    DenyAllDelegate    deny_delegate;
    CollectingObserver observer;

    // Checkers wrap the delegates — constructed after delegates.
    PermissionChecker  allow_checker{allow_delegate};
    PermissionChecker  deny_checker{deny_delegate};

    void SetUp() override {
        registry.register_tool(std::make_unique<EchoTool>());
        registry.register_tool(std::make_unique<FailTool>());
        registry.register_tool(std::make_unique<RequiredFieldTool>());
    }
};

// ---------------------------------------------------------------------------
// Existing tests (constructor now has default config — still compiles 3-arg)
// ---------------------------------------------------------------------------

TEST_F(ToolExecutorTest, ExecutesToolAndReportsResult) {
    ToolExecutor executor(registry, allow_checker, observer);

    std::vector<ToolUseContent> calls = {
        {"id-1", "echo", nlohmann::json{{"msg", "hello"}}}
    };

    std::atomic<bool> cancelled{false};
    auto exec_r = executor.execute(calls, cancelled);
    auto& results = exec_r.results;

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].tool_use_id, "id-1");
    EXPECT_FALSE(results[0].is_error);
    const std::string expected_content = nlohmann::json{{"msg", "hello"}}.dump();
    EXPECT_EQ(results[0].content, expected_content);

    EXPECT_EQ(observer.count_starts(),  1);
    EXPECT_EQ(observer.count_results(), 1);
}

TEST_F(ToolExecutorTest, PermissionDenied) {
    ToolExecutor executor(registry, deny_checker, observer);

    std::vector<ToolUseContent> calls = {{"id-2", "echo", {}}};

    std::atomic<bool> cancelled{false};
    auto exec_r = executor.execute(calls, cancelled);
    auto& results = exec_r.results;

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].is_error);
    EXPECT_NE(results[0].content.find("denied"), std::string::npos);
}

TEST_F(ToolExecutorTest, UnknownTool) {
    ToolExecutor executor(registry, allow_checker, observer);

    std::vector<ToolUseContent> calls = {{"id-3", "nonexistent", {}}};

    std::atomic<bool> cancelled{false};
    auto exec_r = executor.execute(calls, cancelled);
    auto& results = exec_r.results;

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].is_error);
    EXPECT_NE(results[0].content.find("not found"), std::string::npos);
}

TEST_F(ToolExecutorTest, CancellationStopsExecution) {
    ToolExecutor executor(registry, allow_checker, observer);

    std::vector<ToolUseContent> calls = {
        {"id-4", "echo", {}},
        {"id-5", "echo", {}}
    };

    std::atomic<bool> cancelled{true};  // pre-cancelled
    auto exec_r = executor.execute(calls, cancelled);

    EXPECT_TRUE(exec_r.results.empty());
    EXPECT_EQ(observer.count_starts(), 0);
}

TEST_F(ToolExecutorTest, InvalidArgs_MissingRequiredField) {
    ToolExecutor executor(registry, allow_checker, observer);

    std::vector<ToolUseContent> calls = {
        {"id-20", "required_tool", nlohmann::json::object()}
    };

    std::atomic<bool> cancelled{false};
    auto exec_r = executor.execute(calls, cancelled);
    auto& results = exec_r.results;

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].is_error);
    EXPECT_NE(results[0].content.find("Missing required field"), std::string::npos);
    EXPECT_NE(results[0].content.find("query"), std::string::npos);

    EXPECT_EQ(observer.count_starts(),  1);
    EXPECT_EQ(observer.count_results(), 1);
}

TEST_F(ToolExecutorTest, InvalidArgs_NotAnObject) {
    ToolExecutor executor(registry, allow_checker, observer);

    std::vector<ToolUseContent> calls = {
        {"id-21", "required_tool", nlohmann::json("not an object")}
    };

    std::atomic<bool> cancelled{false};
    auto exec_r = executor.execute(calls, cancelled);
    auto& results = exec_r.results;

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].is_error);
    EXPECT_NE(results[0].content.find("JSON object"), std::string::npos);
}

TEST_F(ToolExecutorTest, ValidArgs_RequiredFieldPresent) {
    ToolExecutor executor(registry, allow_checker, observer);

    std::vector<ToolUseContent> calls = {
        {"id-22", "required_tool", nlohmann::json{{"query", "hello"}}}
    };

    std::atomic<bool> cancelled{false};
    auto exec_r = executor.execute(calls, cancelled);
    auto& results = exec_r.results;

    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].is_error);
    EXPECT_EQ(results[0].content, "hello");
}

TEST_F(ToolExecutorTest, MultipleToolCalls) {
    // echo + fail + echo → 3 results; middle is error, first and last succeed
    ToolExecutor executor(registry, allow_checker, observer);

    std::vector<ToolUseContent> calls = {
        {"id-10", "echo", {{"x", 1}}},
        {"id-11", "fail", {}},
        {"id-12", "echo", {{"x", 2}}}
    };

    std::atomic<bool> cancelled{false};
    auto exec_r = executor.execute(calls, cancelled);
    auto& results = exec_r.results;

    ASSERT_EQ(results.size(), 3u);

    EXPECT_FALSE(results[0].is_error);
    EXPECT_EQ(results[0].tool_use_id, "id-10");

    EXPECT_TRUE(results[1].is_error);
    EXPECT_EQ(results[1].tool_use_id, "id-11");
    EXPECT_EQ(results[1].content, "something went wrong");

    EXPECT_FALSE(results[2].is_error);
    EXPECT_EQ(results[2].tool_use_id, "id-12");

    EXPECT_EQ(observer.count_starts(),  3);
    EXPECT_EQ(observer.count_results(), 3);
}

// ---------------------------------------------------------------------------
// New tests: parallel execution, serial write, truncation, doom-loop
// ---------------------------------------------------------------------------

TEST(StreamingToolExecutorTest, ParallelReadOnlyExecution) {
    // 3 read-only echo tools should all execute and return results in call order.
    ToolRegistry reg;
    reg.register_tool(std::make_unique<EchoTool>());

    AllowAllDelegate   delegate;
    PermissionChecker  checker(delegate);
    CollectingObserver obs;

    ToolExecutor executor(reg, checker, obs);

    std::vector<ToolUseContent> calls = {
        {"p-1", "echo", nlohmann::json{{"n", 1}}},
        {"p-2", "echo", nlohmann::json{{"n", 2}}},
        {"p-3", "echo", nlohmann::json{{"n", 3}}},
    };

    std::atomic<bool> cancelled{false};
    auto exec_r = executor.execute(calls, cancelled);
    auto& results = exec_r.results;

    ASSERT_EQ(results.size(), 3u);

    // Order must be preserved
    EXPECT_EQ(results[0].tool_use_id, "p-1");
    EXPECT_FALSE(results[0].is_error);

    EXPECT_EQ(results[1].tool_use_id, "p-2");
    EXPECT_FALSE(results[1].is_error);

    EXPECT_EQ(results[2].tool_use_id, "p-3");
    EXPECT_FALSE(results[2].is_error);

    EXPECT_FALSE(exec_r.doom_loop_abort);

    // All 3 starts and 3 results emitted
    EXPECT_EQ(obs.count_starts(),  3);
    EXPECT_EQ(obs.count_results(), 3);
}

TEST(StreamingToolExecutorTest, SerialWriteBlocks) {
    // read-only echo → write → read-only echo
    // The write must be serialized; all 3 results must come back in order.
    ToolRegistry reg;
    reg.register_tool(std::make_unique<EchoTool>());
    reg.register_tool(std::make_unique<WriteTool>());

    AllowAllDelegate   delegate;
    PermissionChecker  checker(delegate);
    CollectingObserver obs;

    ToolExecutor executor(reg, checker, obs);

    std::vector<ToolUseContent> calls = {
        {"sw-1", "echo",  nlohmann::json{{"v", "a"}}},
        {"sw-2", "write", nlohmann::json{}},
        {"sw-3", "echo",  nlohmann::json{{"v", "b"}}},
    };

    std::atomic<bool> cancelled{false};
    auto exec_r = executor.execute(calls, cancelled);
    auto& results = exec_r.results;

    ASSERT_EQ(results.size(), 3u);

    EXPECT_EQ(results[0].tool_use_id, "sw-1");
    EXPECT_FALSE(results[0].is_error);
    EXPECT_NE(results[0].content.find("a"), std::string::npos);

    EXPECT_EQ(results[1].tool_use_id, "sw-2");
    EXPECT_FALSE(results[1].is_error);
    EXPECT_EQ(results[1].content, "written");

    EXPECT_EQ(results[2].tool_use_id, "sw-3");
    EXPECT_FALSE(results[2].is_error);
    EXPECT_NE(results[2].content.find("b"), std::string::npos);

    EXPECT_FALSE(exec_r.doom_loop_abort);
}

TEST(StreamingToolExecutorTest, LargeResultTruncation) {
    // A tool returning 100 000 chars should be truncated to max_result_chars.
    ToolRegistry reg;
    reg.register_tool(std::make_unique<BigResultTool>(100'000));

    AllowAllDelegate   delegate;
    PermissionChecker  checker(delegate);
    CollectingObserver obs;

    ToolExecutorConfig cfg;
    cfg.max_result_chars = 50'000;

    ToolExecutor executor(reg, checker, obs, cfg);

    std::vector<ToolUseContent> calls = {{"big-1", "big", {}}};

    std::atomic<bool> cancelled{false};
    auto exec_r = executor.execute(calls, cancelled);
    auto& results = exec_r.results;

    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].is_error);

    // Content should be truncated
    EXPECT_LE(results[0].content.size(),
              static_cast<std::size_t>(cfg.max_result_chars) + 80u);  // +80 for suffix
    EXPECT_NE(results[0].content.find("[truncated"), std::string::npos);
    EXPECT_NE(results[0].content.find("100000"), std::string::npos);
}

TEST(StreamingToolExecutorTest, DoomLoopDetection) {
    // 6 consecutive failures of the same tool → doom_loop_abort = true
    ToolRegistry reg;
    reg.register_tool(std::make_unique<ReadOnlyFailTool>());

    AllowAllDelegate   delegate;
    PermissionChecker  checker(delegate);
    CollectingObserver obs;

    ToolExecutorConfig cfg;
    cfg.doom_loop_threshold = 6;

    ToolExecutor executor(reg, checker, obs, cfg);

    // Fire 6 calls to ro_fail, each in its own execute() so records accumulate.
    // (We test accumulated history across multiple execute() calls.)
    std::atomic<bool> cancelled{false};

    ExecutorResult last_result;
    for (int i = 0; i < 6; ++i) {
        std::vector<ToolUseContent> calls = {
            {"dl-" + std::to_string(i), "ro_fail", {}}
        };
        last_result = executor.execute(calls, cancelled);
    }

    EXPECT_TRUE(last_result.doom_loop_abort);
    EXPECT_NE(last_result.doom_loop_hint.find("ro_fail"), std::string::npos);
}
