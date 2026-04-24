#include "engine_impl.h"
#include "session_impl.h"
#include "agents/agent_manager.h"
#include "agents/agent_tool.h"
#include "core/query_engine.h"
#include "permissions/permission_checker.h"
#include "mcp/mcp_client.h"
#include "mcp/mcp_tool_wrapper.h"

#include <omni/session.h>
#include <omni/tool.h>

#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

namespace omni::engine {

// ---------------------------------------------------------------------------
// Engine::Impl thread pool
// ---------------------------------------------------------------------------

void Engine::Impl::start_pool(int num_threads) {
    for (int i = 0; i < num_threads; ++i) {
        pool.emplace_back([this]() {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    queue_cv.wait(lock, [this]() {
                        return shutting_down || !work_queue.empty();
                    });
                    if (shutting_down && work_queue.empty()) return;
                    task = std::move(work_queue.front());
                    work_queue.pop();
                }
                task();
            }
        });
    }
}

void Engine::Impl::stop_pool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        shutting_down = true;
    }
    queue_cv.notify_all();
    for (auto& t : pool) {
        if (t.joinable()) t.join();
    }
    pool.clear();
}

void Engine::Impl::enqueue(std::function<void()> task) {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        work_queue.push(std::move(task));
    }
    queue_cv.notify_one();
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

Engine::Engine(Config config, std::unique_ptr<LLMProvider> provider)
    : impl_(std::make_unique<Impl>())
{
    impl_->config   = std::move(config);
    impl_->provider = std::move(provider);

    // Construct session persistence if a storage directory was configured.
    if (!impl_->config.session_storage_dir.empty()) {
        impl_->session_persistence =
            std::make_unique<SessionPersistence>(impl_->config.session_storage_dir);
    }

    const int nthreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    impl_->start_pool(nthreads);
}

Engine::~Engine() {
    impl_->stop_pool();
}

Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;

std::unique_ptr<Session> Engine::create_session(EventObserver& observer,
                                                PermissionDelegate& delegate,
                                                std::optional<std::string> session_id)
{
    QueryEngineConfig qcfg;
    qcfg.max_turns               = impl_->config.max_turns;
    qcfg.preserve_tail           = impl_->config.preserve_tail;
    qcfg.max_result_chars        = impl_->config.max_result_chars;
    qcfg.compact_max_result_chars = impl_->config.compact_max_result_chars;
    qcfg.temperature             = impl_->config.temperature;
    qcfg.top_p                   = impl_->config.top_p;
    qcfg.top_k                   = impl_->config.top_k;
    qcfg.min_p                   = impl_->config.min_p;
    qcfg.presence_penalty        = impl_->config.presence_penalty;
    qcfg.frequency_penalty       = impl_->config.frequency_penalty;
    qcfg.initial_max_tokens      = impl_->config.initial_max_tokens;
    qcfg.system_prompt           = impl_->config.system_prompt;
    qcfg.max_parallel_tools      = 10;
    qcfg.compact_soft_limit_pct  = impl_->config.compact_soft_limit_pct;
    qcfg.compact_preserve_tail   = impl_->config.compact_preserve_tail;
    qcfg.enable_auto_compact     = impl_->config.enable_auto_compact;

    auto provider_ref = std::make_unique<ProviderRef>(*impl_->provider);

    // Use a separate compaction provider if configured, otherwise nullptr (falls
    // back to the main provider inside QueryEngine).
    LLMProvider* compact_prov = impl_->config.compaction_provider;

    // Build and configure the session-scoped permission checker.
    auto checker = std::make_unique<PermissionChecker>(delegate);
    checker->set_mode(impl_->config.permission_mode);
    for (const PermissionRule& rule : impl_->config.permission_rules) {
        checker->add_rule(RuleSource::Programmatic, rule);
    }

    // Build Session::Impl first so we can wire session_tools into QueryEngine.
    auto session_impl          = std::make_unique<Session::Impl>();
    session_impl->checker      = std::move(checker);
    session_impl->engine_tools = &impl_->tools;
    session_impl->cost_tracker = &impl_->cost_tracker;
    session_impl->persistence  = impl_->session_persistence.get();

    auto qe = std::make_unique<QueryEngine>(
        std::move(provider_ref), impl_->tools, *session_impl->checker, observer, qcfg,
        compact_prov, &impl_->cost_tracker, &session_impl->session_tools);

    // Build a unique session ID now so we can wire it into QueryEngine before
    // the Session constructor runs (which would otherwise generate a new one).
    if (session_id.has_value()) {
        session_impl->session_id = *session_id;
    } else {
        using Clock = std::chrono::system_clock;
        const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                Clock::now().time_since_epoch()).count();
        std::mt19937 rng{std::random_device{}()};
        const uint32_t rnd = rng() & 0xFFFFFF;
        std::ostringstream oss;
        oss << now_us << '-' << std::hex << std::setw(6) << std::setfill('0') << rnd;
        session_impl->session_id = oss.str();
    }

    // Wire persistence into QueryEngine so it auto-saves after each submit().
    if (session_impl->persistence) {
        qe->set_persistence(session_impl->persistence, session_impl->session_id);
    }

    session_impl->query_engine = std::move(qe);

    // Create a session-scoped AgentManager and register built-in agent tools.
    // AgentManager stores references to engine and delegate — both outlive the session.
    session_impl->agent_manager = std::make_unique<AgentManager>(*this, delegate, observer);
    session_impl->session_tools.register_tool(
        std::make_unique<AgentTool>(*session_impl->agent_manager));
    session_impl->session_tools.register_tool(
        std::make_unique<SendMessageTool>(*session_impl->agent_manager));

    // Bind enqueue through a lambda so Session::Impl doesn't need Engine::Impl type
    Engine::Impl* raw_impl = impl_.get();
    session_impl->enqueue_work = [raw_impl](std::function<void()> task) {
        raw_impl->enqueue(std::move(task));
    };

    return std::unique_ptr<Session>(new Session(std::move(session_impl)));
}

void Engine::register_tool(std::unique_ptr<Tool> tool) {
    impl_->tools.register_tool(std::move(tool));
}

bool Engine::connect_mcp_server(const MCPServerConfig& config) {
    // Reject duplicate names.
    if (impl_->mcp_clients.count(config.name)) {
        return false;
    }

    auto client = std::make_unique<MCPClient>();
    if (!client->connect(config)) {
        return false;
    }

    // Discover tools and wrap each one.
    const std::vector<MCPToolInfo> tool_infos = client->list_tools();
    for (const MCPToolInfo& info : tool_infos) {
        impl_->tools.register_tool(
            std::make_unique<MCPToolWrapper>(*client, info));
    }

    impl_->mcp_clients.emplace(config.name, std::move(client));
    return true;
}

void Engine::disconnect_mcp_server(const std::string& name) {
    auto it = impl_->mcp_clients.find(name);
    if (it == impl_->mcp_clients.end()) return;

    it->second->disconnect();
    impl_->mcp_clients.erase(it);

    // Unregister all tools belonging to this server (prefix "mcp__{name}__").
    const std::string prefix = "mcp__" + name + "__";
    const std::vector<std::string> names = impl_->tools.names();
    for (const std::string& tool_name : names) {
        if (tool_name.rfind(prefix, 0) == 0) {
            impl_->tools.unregister_tool(tool_name);
        }
    }
}

std::vector<std::string> Engine::tool_names() const {
    return impl_->tools.names();
}

Tool* Engine::find_tool(const std::string& name) const {
    return impl_->tools.get(name);
}

LLMProvider& Engine::provider() {
    return *impl_->provider;
}

const LLMProvider& Engine::provider() const {
    return *impl_->provider;
}

const Config& Engine::config() const {
    return impl_->config;
}

}  // namespace omni::engine
