#pragma once

#include <omni/engine.h>
#include <omni/session.h>
#include <omni/tool.h>
#include "tools/tool_registry.h"
#include "services/cost_tracker.h"
#include "services/session_persistence.h"
#include "mcp/mcp_client.h"

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace omni::engine {

struct Engine::Impl {
    Config                       config;
    std::unique_ptr<LLMProvider> provider;
    ToolRegistry                 tools;
    CostTracker                  cost_tracker;

    /// Lazily constructed when session_storage_dir is non-empty.
    std::unique_ptr<SessionPersistence> session_persistence;

    /// MCP clients keyed by server name.
    std::unordered_map<std::string, std::unique_ptr<MCPClient>> mcp_clients;

    // Thread pool
    std::vector<std::thread>          pool;
    std::queue<std::function<void()>> work_queue;
    std::mutex                        queue_mutex;
    std::condition_variable           queue_cv;
    bool                              shutting_down = false;

    void start_pool(int num_threads);
    void stop_pool();
    void enqueue(std::function<void()> task);
};

}  // namespace omni::engine
