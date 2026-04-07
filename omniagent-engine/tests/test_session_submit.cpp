#include <gtest/gtest.h>

#include <omni/engine.h>
#include <omni/event.h>
#include <omni/permission.h>
#include <omni/provider.h>
#include <omni/session.h>
#include <omni/tool.h>
#include <omni/types.h>

#include <atomic>
#include <string>
#include <vector>

using namespace omni::engine;

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

class SSAllowAllDelegate : public PermissionDelegate {
public:
    PermissionDecision on_permission_request(const std::string&,
                                              const nlohmann::json&,
                                              const std::string&) override {
        return PermissionDecision::Allow;
    }
};

class SSCollectingObserver : public EventObserver {
public:
    std::vector<Event> events;

    void on_event(const Event& e) override {
        events.push_back(e);
    }

    int count_done() const {
        int n = 0;
        for (const auto& e : events) {
            if (std::holds_alternative<DoneEvent>(e)) ++n;
        }
        return n;
    }

    std::string all_text() const {
        std::string out;
        for (const auto& e : events) {
            if (const auto* td = std::get_if<TextDeltaEvent>(&e)) {
                out += td->text;
            }
        }
        return out;
    }
};

class SSMockProvider : public LLMProvider {
public:
    Usage complete(const CompletionRequest& /*req*/,
                   StreamCallback cb,
                   std::atomic<bool>& /*stop*/) override
    {
        { StreamEventData ev; ev.type = StreamEventType::MessageStart; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::ContentBlockStart; ev.index = 0; ev.delta_type = "text"; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::ContentBlockDelta;
          ev.index = 0; ev.delta_type = "text_delta"; ev.delta_text = "hello"; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::ContentBlockStop; ev.index = 0; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::MessageDelta;
          ev.stop_reason = "end_turn"; ev.usage = Usage{1, 1, 0}; cb(ev); }
        { StreamEventData ev; ev.type = StreamEventType::MessageStop; cb(ev); }
        return {1, 1, 0};
    }

    ModelCapabilities capabilities() const override { return {}; }
    std::string       name()         const override { return "ss-mock"; }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(SessionSubmit, SubmitWaitNonBlocking) {
    Config cfg;
    cfg.max_turns = 5;

    auto engine = std::make_unique<Engine>(cfg, std::make_unique<SSMockProvider>());

    SSAllowAllDelegate   delegate;
    SSCollectingObserver observer;

    auto session = engine->create_session(observer, delegate);

    // submit() should return immediately (non-blocking)
    session->submit("hi");

    // wait() should block until the work completes
    session->wait();

    EXPECT_EQ(observer.count_done(), 1);
    EXPECT_EQ(observer.all_text(), "hello");
}
