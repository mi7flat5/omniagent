#include <gtest/gtest.h>

#include <omni/engine.h>
#include <omni/event.h>
#include <omni/permission.h>
#include <omni/provider.h>
#include <omni/session.h>
#include <omni/tool.h>
#include <providers/http_provider.h>
#include <mcp/mcp_client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

using namespace omni::engine;
using namespace std::chrono_literals;

namespace {

class RHAllowAllDelegate : public PermissionDelegate {
public:
    PermissionDecision on_permission_request(const std::string&,
                                             const nlohmann::json&,
                                             const std::string&) override {
        return PermissionDecision::Allow;
    }
};

class RHCollectingObserver : public EventObserver {
public:
    void on_event(const Event& event) override {
        events.push_back(event);
    }

    std::vector<Event> events;
};

class RHBlockingProvider : public LLMProvider {
public:
    Usage complete(const CompletionRequest&, StreamCallback cb,
                   std::atomic<bool>& stop_flag) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            started_ = true;
        }
        started_cv_.notify_all();

        std::unique_lock<std::mutex> lock(mutex_);
        release_cv_.wait(lock, [this]() { return released_; });
        lock.unlock();

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
        event.delta_text = "released";
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
    std::string name() const override { return "rh-blocking"; }

    void wait_until_started() {
        std::unique_lock<std::mutex> lock(mutex_);
        started_cv_.wait(lock, [this]() { return started_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        release_cv_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable started_cv_;
    std::condition_variable release_cv_;
    bool started_ = false;
    bool released_ = false;
};

class RHTwoTurnProvider : public LLMProvider {
public:
    Usage complete(const CompletionRequest&, StreamCallback cb,
                   std::atomic<bool>& stop_flag) override {
        if (stop_flag.load(std::memory_order_relaxed)) {
            return {};
        }

        StreamEventData event;
        event.type = StreamEventType::MessageStart;
        cb(event);

        if (call_count_ == 0) {
            event = {};
            event.type = StreamEventType::ContentBlockStart;
            event.index = 0;
            event.delta_type = "tool_use";
            event.tool_id = "tool-1";
            event.tool_name = "rh_big_result";
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
            event.delta_text = "done";
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

        ++call_count_;
        return {1, 1, 0};
    }

    ModelCapabilities capabilities() const override { return {}; }
    std::string name() const override { return "rh-two-turn"; }

private:
    int call_count_ = 0;
};

class RHBigResultTool : public Tool {
public:
    explicit RHBigResultTool(std::size_t size = 256)
        : size_(size) {}

    std::string name() const override { return "rh_big_result"; }
    std::string description() const override { return "Return a large tool payload"; }
    nlohmann::json input_schema() const override { return {}; }
    bool is_read_only() const override { return true; }
    bool is_destructive() const override { return false; }

    ToolCallResult call(const nlohmann::json&) override {
        return {std::string(size_, 'x'), false};
    }

private:
    std::size_t size_;
};

}  // namespace

TEST(RuntimeHardening, SessionDestructorWaitsForInFlightSubmit) {
    auto provider = std::make_unique<RHBlockingProvider>();
    auto* raw_provider = provider.get();

    Config config;
    Engine engine(config, std::move(provider));

    RHAllowAllDelegate delegate;
    RHCollectingObserver observer;
    auto session = engine.create_session(observer, delegate);

    session->submit("hello");
    raw_provider->wait_until_started();

    auto destroy_future = std::async(std::launch::async,
        [owned_session = std::move(session)]() mutable {
            owned_session.reset();
        });

    EXPECT_EQ(destroy_future.wait_for(100ms), std::future_status::timeout);

    raw_provider->release();

    EXPECT_EQ(destroy_future.wait_for(2s), std::future_status::ready);
}

TEST(RuntimeHardening, RejectsConcurrentSubmitOnSameSession) {
    auto provider = std::make_unique<RHBlockingProvider>();
    auto* raw_provider = provider.get();

    Config config;
    Engine engine(config, std::move(provider));

    RHAllowAllDelegate delegate;
    RHCollectingObserver observer;
    auto session = engine.create_session(observer, delegate);

    session->submit("first");
    raw_provider->wait_until_started();

    EXPECT_THROW(session->submit("second"), std::runtime_error);

    raw_provider->release();
    EXPECT_NO_THROW(session->wait());
}

TEST(RuntimeHardening, EngineConfigControlsToolResultTruncation) {
    Config config;
    config.max_turns = 4;
    config.max_result_chars = 32;

    Engine engine(config, std::make_unique<RHTwoTurnProvider>());
    engine.register_tool(std::make_unique<RHBigResultTool>());

    RHAllowAllDelegate delegate;
    RHCollectingObserver observer;
    auto session = engine.create_session(observer, delegate);

    session->submit("go");
    session->wait();

    ASSERT_EQ(session->messages().size(), 4u);
    ASSERT_EQ(session->messages()[2].tool_results.size(), 1u);
    const auto& tool_result = session->messages()[2].tool_results[0].content;
    EXPECT_NE(tool_result.find("[truncated"), std::string::npos);
    EXPECT_LT(tool_result.size(), 128u);
}

TEST(RuntimeHardening, DefaultEngineConfigPreservesReadSizedToolResults) {
    Config config;
    config.max_turns = 4;

    Engine engine(config, std::make_unique<RHTwoTurnProvider>());
    engine.register_tool(std::make_unique<RHBigResultTool>(4096));

    RHAllowAllDelegate delegate;
    RHCollectingObserver observer;
    auto session = engine.create_session(observer, delegate);

    session->submit("go");
    session->wait();

    ASSERT_EQ(session->messages().size(), 4u);
    ASSERT_EQ(session->messages()[2].tool_results.size(), 1u);
    const auto& tool_result = session->messages()[2].tool_results[0].content;
    EXPECT_EQ(tool_result.size(), 4096u);
    EXPECT_EQ(tool_result.find("[truncated"), std::string::npos);
}

TEST(RuntimeHardening, HttpProviderThrowsOnTransportFailure) {
    HttpProviderConfig config;
    config.base_url = "http://127.0.0.1:1/v1";
    config.model = "test-model";
    config.connect_timeout_ms = 100;
    config.read_timeout_ms = 100;

    HttpProvider provider(config);

    CompletionRequest request;
    Message user_message;
    user_message.role = Role::User;
    user_message.content = {ContentBlock{TextContent{"hello"}}};
    request.messages = {user_message};

    std::atomic<bool> stop_flag{false};

    try {
        provider.complete(request, [](const StreamEventData&) {}, stop_flag);
        FAIL() << "Expected complete() to throw on transport failure";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("HTTP transport failure"), std::string::npos);
    }
}

TEST(RuntimeHardening, MCPInitTimeoutIsEnforced) {
    MCPClient client;

    MCPServerConfig config;
    config.name = "hang";
    config.command = "/bin/cat";
    config.init_timeout = 1s;
    config.request_timeout = 1s;

    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(client.connect(config));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(client.is_degraded());
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 3000);
}