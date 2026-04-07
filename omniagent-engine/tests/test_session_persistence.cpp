#include <gtest/gtest.h>

#include <omni/event.h>
#include <omni/permission.h>
#include <omni/provider.h>
#include <omni/types.h>
#include "core/query_engine.h"
#include "permissions/permission_checker.h"
#include "services/session_persistence.h"
#include "tools/tool_registry.h"

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace omni::engine;

// ---------------------------------------------------------------------------
// Test fixtures
// ---------------------------------------------------------------------------

static const std::filesystem::path kTestDir = "/tmp/omni-engine-test-sessions";

class SessionPersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clean slate before each test
        std::filesystem::remove_all(kTestDir);
    }

    void TearDown() override {
        std::filesystem::remove_all(kTestDir);
    }

    SessionPersistence persistence_{kTestDir};

    static SessionRecord make_record(const std::string& id, int num_messages = 2) {
        SessionRecord r;
        r.id         = id;
        r.created_at = "2026-01-01T00:00:00Z";
        r.updated_at = "2026-01-01T00:00:01Z";
        r.total_usage = {10, 5, 2};

        for (int i = 0; i < num_messages; ++i) {
            Message m;
            m.role    = (i % 2 == 0) ? Role::User : Role::Assistant;
            m.content = {ContentBlock{TextContent{"message " + std::to_string(i)}}};
            r.messages.push_back(std::move(m));
        }
        return r;
    }
};

// ---------------------------------------------------------------------------
// SaveAndLoad
// ---------------------------------------------------------------------------

TEST_F(SessionPersistenceTest, SaveAndLoad) {
    const SessionRecord orig = make_record("session-abc", 4);
    persistence_.save(orig);

    const auto loaded = persistence_.load("session-abc");
    ASSERT_TRUE(loaded.has_value());

    EXPECT_EQ(loaded->id,         orig.id);
    EXPECT_EQ(loaded->created_at, orig.created_at);
    EXPECT_EQ(loaded->updated_at, orig.updated_at);

    EXPECT_EQ(loaded->total_usage.input_tokens,      orig.total_usage.input_tokens);
    EXPECT_EQ(loaded->total_usage.output_tokens,     orig.total_usage.output_tokens);
    EXPECT_EQ(loaded->total_usage.cache_read_tokens, orig.total_usage.cache_read_tokens);

    ASSERT_EQ(loaded->messages.size(), orig.messages.size());
    for (size_t i = 0; i < orig.messages.size(); ++i) {
        EXPECT_EQ(loaded->messages[i].role, orig.messages[i].role);

        ASSERT_FALSE(loaded->messages[i].content.empty());
        const auto* tc = std::get_if<TextContent>(&loaded->messages[i].content[0].data);
        ASSERT_NE(tc, nullptr);
        EXPECT_EQ(tc->text, "message " + std::to_string(i));
    }
}

// ---------------------------------------------------------------------------
// LoadNonExistent returns nullopt
// ---------------------------------------------------------------------------

TEST_F(SessionPersistenceTest, LoadNonExistent) {
    const auto result = persistence_.load("does-not-exist");
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// ListSessions — newest first
// ---------------------------------------------------------------------------

TEST_F(SessionPersistenceTest, ListSessions) {
    // Save three sessions; use a small sleep between saves so mtime differs.
    persistence_.save(make_record("session-1"));
    // Force distinct mtimes by modifying timestamps via touch-like logic.
    // Because filesystem mtime resolution may be coarse (1s on some systems),
    // we sleep briefly between saves.
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(10ms);
    persistence_.save(make_record("session-2"));
    std::this_thread::sleep_for(10ms);
    persistence_.save(make_record("session-3"));

    const auto ids = persistence_.list(50);
    ASSERT_EQ(ids.size(), 3u);

    // Newest (session-3) should be first
    EXPECT_EQ(ids[0], "session-3");
    EXPECT_EQ(ids[1], "session-2");
    EXPECT_EQ(ids[2], "session-1");
}

// ---------------------------------------------------------------------------
// ListSessions respects limit
// ---------------------------------------------------------------------------

TEST_F(SessionPersistenceTest, ListLimit) {
    persistence_.save(make_record("s1"));
    persistence_.save(make_record("s2"));
    persistence_.save(make_record("s3"));

    const auto ids = persistence_.list(2);
    EXPECT_EQ(ids.size(), 2u);
}

// ---------------------------------------------------------------------------
// Remove
// ---------------------------------------------------------------------------

TEST_F(SessionPersistenceTest, Remove) {
    persistence_.save(make_record("to-delete"));
    EXPECT_TRUE(persistence_.load("to-delete").has_value());

    const bool removed = persistence_.remove("to-delete");
    EXPECT_TRUE(removed);

    EXPECT_FALSE(persistence_.load("to-delete").has_value());
}

TEST_F(SessionPersistenceTest, RemoveNonExistentReturnsFalse) {
    EXPECT_FALSE(persistence_.remove("ghost"));
}

// ---------------------------------------------------------------------------
// Overwrite: save twice, load reflects latest
// ---------------------------------------------------------------------------

TEST_F(SessionPersistenceTest, SaveOverwrite) {
    auto r = make_record("overwrite-me", 2);
    persistence_.save(r);

    // Mutate and re-save
    r.updated_at = "2026-06-01T00:00:00Z";
    r.messages.push_back([] {
        Message m; m.role = Role::User;
        m.content = {ContentBlock{TextContent{"extra"}}};
        return m;
    }());
    persistence_.save(r);

    const auto loaded = persistence_.load("overwrite-me");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->messages.size(), 3u);
    EXPECT_EQ(loaded->updated_at, "2026-06-01T00:00:00Z");
}

// ---------------------------------------------------------------------------
// ResumeSession: QueryEngine → save → new QueryEngine → set_messages → check
// ---------------------------------------------------------------------------

class SPAllowAllDelegate : public PermissionDelegate {
public:
    PermissionDecision on_permission_request(const std::string&,
                                              const nlohmann::json&,
                                              const std::string&) override {
        return PermissionDecision::Allow;
    }
};

class SPCollectingObserver : public EventObserver {
public:
    void on_event(const Event&) override {}
};

class SPMockProvider : public LLMProvider {
public:
    Usage complete(const CompletionRequest&,
                   StreamCallback cb,
                   std::atomic<bool>& stop) override {
        if (stop.load()) return {};
        StreamEventData ev;
        ev.type = StreamEventType::MessageStart; cb(ev);
        ev = {}; ev.type = StreamEventType::ContentBlockStart; ev.index = 0; ev.delta_type = "text"; cb(ev);
        ev = {}; ev.type = StreamEventType::ContentBlockDelta; ev.index = 0;
        ev.delta_type = "text_delta"; ev.delta_text = "reply"; cb(ev);
        ev = {}; ev.type = StreamEventType::ContentBlockStop; ev.index = 0; cb(ev);
        ev = {}; ev.type = StreamEventType::MessageDelta;
        ev.stop_reason = "end_turn"; ev.usage = {1,1,0}; cb(ev);
        ev = {}; ev.type = StreamEventType::MessageStop; cb(ev);
        return {1, 1, 0};
    }
    ModelCapabilities capabilities() const override { return {}; }
    std::string       name()         const override { return "sp-mock"; }
};

TEST_F(SessionPersistenceTest, ResumeSession) {
    const std::string session_id = "resume-test";

    // --- Phase 1: run a submit, then save manually ---
    {
        auto mock = std::make_unique<SPMockProvider>();
        ToolRegistry      registry;
        SPAllowAllDelegate delegate;
        SPCollectingObserver observer;
        PermissionChecker checker(delegate);

        QueryEngineConfig cfg;
        cfg.max_turns = 5;

        QueryEngine qe(std::move(mock), registry, checker, observer, cfg);
        qe.submit("hello");

        // Snapshot and persist
        SessionRecord rec;
        rec.id          = session_id;
        rec.created_at  = "2026-01-01T00:00:00Z";
        rec.updated_at  = "2026-01-01T00:00:01Z";
        rec.messages    = qe.messages();
        rec.total_usage = qe.total_usage();
        persistence_.save(rec);

        ASSERT_EQ(rec.messages.size(), 2u);  // user + assistant
    }

    // --- Phase 2: load and resume into a fresh QueryEngine ---
    {
        const auto loaded = persistence_.load(session_id);
        ASSERT_TRUE(loaded.has_value());
        ASSERT_EQ(loaded->messages.size(), 2u);

        auto mock2 = std::make_unique<SPMockProvider>();
        ToolRegistry      registry;
        SPAllowAllDelegate delegate;
        SPCollectingObserver observer;
        PermissionChecker checker(delegate);

        QueryEngineConfig cfg;
        cfg.max_turns = 5;

        QueryEngine qe2(std::move(mock2), registry, checker, observer, cfg);
        qe2.set_messages(loaded->messages);

        // After resume, messages should be present before any new submit
        EXPECT_EQ(qe2.messages().size(), 2u);

        // A new submit should append to the resumed history
        qe2.submit("follow-up");
        EXPECT_EQ(qe2.messages().size(), 4u);  // prior 2 + new user + assistant
    }
}
