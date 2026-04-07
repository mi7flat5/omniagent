#pragma once

namespace omni::engine {

class Engine;
class Session;
struct Config;

// Event system
// Event is a std::variant — no forward declaration needed; include omni/event.h
class EventObserver;

// Permission
class  PermissionDelegate;
enum class PermissionDecision;

// LLM
class  LLMProvider;
class  Tool;

// Value types
struct Message;
struct ContentBlock;
struct ToolResult;
struct Usage;
struct ModelCapabilities;
struct CompletionRequest;
struct StreamEventData;  // was StreamEvent in scaffold; renamed to match spec

}  // namespace omni::engine
