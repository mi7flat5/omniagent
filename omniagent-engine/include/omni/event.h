#pragma once

#include <omni/approval.h>
#include <omni/types.h>

#include <chrono>
#include <nlohmann/json.hpp>
#include <string>
#include <variant>

namespace omni::engine {

struct EventContext {
    std::string project_id;
    std::string session_id;
    std::string run_id;
    std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
};

struct TextDeltaEvent     { std::string text; EventContext context; };
struct ThinkingDeltaEvent { std::string text; EventContext context; };

struct ToolUseStartEvent {
    std::string    id;
    std::string    name;
    nlohmann::json input;
    EventContext   context;
};

struct ToolUseInputEvent {
    std::string id;
    std::string partial_json;  // streaming JSON fragment
    EventContext context;
};

struct ToolResultEvent {
    std::string tool_use_id;
    std::string tool_name;
    std::string content;
    bool        is_error = false;
    EventContext context;
};

struct ErrorEvent {
    std::string message;
    bool        recoverable = false;
    EventContext context;
};

struct DoneEvent { Usage usage; EventContext context; };

struct RunStartedEvent {
    std::string run_id;
    std::string profile;
    EventContext context;
};

struct RunPausedEvent {
    std::string run_id;
    std::string reason;
    EventContext context;
};

struct RunResumedEvent {
    std::string run_id;
    EventContext context;
};

struct RunStoppedEvent {
    std::string run_id;
    EventContext context;
};

struct RunCancelledEvent {
    std::string run_id;
    EventContext context;
};

struct SessionResetEvent {
    std::string session_id;
    EventContext context;
};

struct AgentSpawnedEvent {
    std::string agent_id;
    std::string task;
    std::string profile;
    EventContext context;
};

struct AgentCompletedEvent {
    std::string agent_id;
    bool success = false;
    EventContext context;
};

struct ApprovalRequestedEvent {
    std::string tool_name;
    nlohmann::json args;
    std::string description;
    EventContext context;
};

struct ApprovalResolvedEvent {
    std::string tool_name;
    ApprovalDecision decision = ApprovalDecision::Deny;
    EventContext context;
};

struct UsageUpdatedEvent {
    Usage delta;
    Usage cumulative;
    EventContext context;
};

struct CompactionEvent {
    int messages_before = 0;
    int messages_after = 0;
    EventContext context;
};

using Event = std::variant<
    TextDeltaEvent, ThinkingDeltaEvent, ToolUseStartEvent,
    ToolUseInputEvent, ToolResultEvent, ErrorEvent, DoneEvent,
    RunStartedEvent, RunPausedEvent, RunResumedEvent, RunStoppedEvent,
    RunCancelledEvent, SessionResetEvent, AgentSpawnedEvent,
    AgentCompletedEvent, ApprovalRequestedEvent, ApprovalResolvedEvent,
    UsageUpdatedEvent, CompactionEvent
>;

class EventObserver {
public:
    virtual ~EventObserver() = default;
    virtual void on_event(const Event& event) = 0;
};

}  // namespace omni::engine
