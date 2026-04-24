# Engine Consumption & Integration — Validation + Implementation Plan

Date: 2026-04-06

## Part 1: Spec Validation

### Verdict

The spec is **sound and well-structured**. Its claims about the current engine state are accurate, the product model is coherent, the layered architecture (runtime → host API → REPL → core adapter) is the right ordering, and the prior-art backing is solid. Below are specific findings.

### Accurate Claims

| Spec Claim | Verified Against |
|---|---|
| Current events limited to 7 variant types | `event.h`: TextDelta, ThinkingDelta, ToolUseStart, ToolUseInput, ToolResult, Error, Done |
| Current profiles limited to 3 enum values | `agent_manager.h`: GeneralPurpose, Explore, Plan |
| Persistence is transcript-only JSONL | `session_persistence.h`: SessionRecord = id + timestamps + messages + usage |
| Tool filtering is name-substring based | `Session::set_tool_filter(vector<string>)` — string matching, not capability bundles |
| No explicit project/host/run concepts | Engine creates Sessions directly; no project ownership, no run object |
| No structured tracing | Confirmed; no span/trace types anywhere |
| No mid-run pause/resume | Session has `cancel()` but no stop/pause/resume |
| Hooks/memory exist but aren't wired publicly | `HookEngine` and `MemoryLoader` tested internally, no `Engine`/`Session` API |
| PermissionMode::Plan has concrete behavior | Auto-allows safe read-only tools and delegates non-read-only checks in `permission_checker.cpp` |

### Known Bugs The Spec Must Account For

These come from the engine audit and must be fixed **before or during** the host API layer, not after.

| Bug | Impact on Spec | Priority |
|---|---|---|
| `Session::~Session()` does not wait for in-flight submit work | ProjectSession lifecycle will leak tasks on close/destroy | Blocker for Phase 1 |
| Same-session submits are not serialized | Spec requires "two in-flight runs may not mutate the same session concurrently" — currently violated | Blocker for Phase 1 |
| `HttpProvider` ignores failed `client.Post()` results | Silent transport failures produce phantom empty turns, breaking reliable run semantics | Blocker for Phase 1 |
| `MCPServerConfig.init_timeout` unused; blocking pipe reads | MCP health reporting and "degraded state" events require timeout enforcement | Blocker for Phase 2 |
| `Engine::Config.max_result_chars` doesn't propagate to `ToolExecutor` | ToolExecutorConfig is default-constructed; host config ≠ effective config | Fix in Phase 1 |

### Spec Gaps To Address

1. **No migration path**: The spec introduces `ProjectEngineHost`, `ProjectSession`, `ProjectRun` but doesn't describe how the existing `Engine`/`Session` API relates. Decision needed: wrap or replace?
2. **Open questions need decisions** before implementation starts (see resolutions below).
3. **No explicit dependency on audit bug fixes**: The implementation plan below sequences them as prerequisites.
4. **`RunObserver` vs `EventObserver`**: Spec introduces `RunObserver` but doesn't clarify whether it replaces or extends `EventObserver`. They should be a clean wrapper.
5. **`ApprovalDelegate` vs `PermissionDelegate`**: Same issue. The existing `PermissionDelegate` is the right low-level primitive; `ApprovalDelegate` should compose it with run-level pause/resume semantics.

### Open Question Resolutions (Recommended)

| Question | Recommended Decision | Rationale |
|---|---|---|
| Project host: shared across users or user-scoped? | **Project-scoped, single-user by default**; multi-user is a host-app concern | Engine should not own user auth; `omniagent-core` can layer user mapping over project IDs |
| Paused runs: survive process restarts only, or deploy boundaries? | **Process restarts only** for P0; deploy/version boundaries are P2 | Simplifies persistence; approval state serialized to disk suffices for restarts |
| `bugfix` shell/code-edit: engine-owned or host adapters? | **Host-provided tools registered as project-scoped** | Engine doesn't own filesystem editing; tools come from `omniagent-core` or CLI |
| Profiles: hardcoded or host-registered manifests? | **Host-registered manifests with engine-provided defaults** | Hardcoded defaults give REPL instant value; host override gives `omniagent-core` full control |
| Core WS channels: existing project sessions or dedicated engine channel? | **Dedicated engine event channel** within project WS namespace | Avoids conflating graph execution events with engine agent events |

---

## Part 2: Implementation Plan

### Architecture

The implementation follows the spec's recommended ordering: **host API first → REPL second → core adapter third**. Audit bug fixes are inserted as prerequisites, not deferred.

```
Phase 0: Runtime Hardening (audit bugs)
Phase 1: Host/Session/Run API Layer
Phase 2: Extended Events + Agent Profiles
Phase 3: CLI REPL
Phase 4: Persistence Expansion
Phase 5: omniagent-core Adapter (active integration track)
```

---

### Phase 0: Runtime Hardening

**Goal**: Fix the audit-identified bugs that would undermine the new API's correctness invariants.

#### 0.1 — Session lifetime safety

**Files**: `session_impl.h`, `session_impl.cpp`

- Add `std::future<void>` or join-on-destroy for the async submit task.
- `Session::~Session()` must `cancel()` + wait for any in-flight `QueryEngine::run_turn()` to finish before destroying resources.
- Add a `std::atomic<bool> destroying_` flag to prevent new submits during teardown.

**Test**: Destroy a session during an active submit; assert no use-after-free (ASAN/TSAN build).

#### 0.2 — Per-session submit serialization

**Files**: `session_impl.cpp`

- Add a `std::mutex submit_mutex_` to `Session::Impl`.
- `Session::submit()` must serialize: reject or queue concurrent submits on the same session.
- Recommended: reject with a structured error rather than silently queue (matches spec: "two in-flight runs may not mutate the same session state concurrently").

**Test**: Two concurrent `submit()` calls on one session; assert second returns error, not data race.

#### 0.3 — HttpProvider transport error handling

**Files**: `providers/http_provider.cpp`

- Check `httplib::Result` for error, null, or non-2xx status.
- Return structured error to `QueryEngine` so retry/backoff logic can engage.
- Map common failures: connection refused, timeout, 429, 500, 503.

**Test**: Mock provider returning failed Result; assert `QueryEngine` retries or surfaces `ErrorEvent`.

#### 0.4 — Config propagation fix

**Files**: `engine_impl.cpp`

- When constructing `ToolExecutor`, pass `config.max_result_chars` into `ToolExecutorConfig` instead of using the default.

**Test**: Set `max_result_chars=100` on Engine::Config; assert tool results are truncated at 100.

#### 0.5 — MCP timeout enforcement

**Files**: `mcp/mcp_client.cpp`

- Use `poll()`/`select()` with `init_timeout` on the pipe read during handshake.
- Add per-request timeout for `call_tool()` (configurable, default 30s).
- On timeout: return structured error, mark client degraded.

**Test**: MCP server that hangs on init; assert connection fails within timeout, not blocks forever.

**Estimated scope**: ~5 focused changes, each independently testable. These are small surgical fixes, not rewrites.

---

### Phase 1: Host / Session / Run API Layer

**Goal**: Introduce the project-ownership model and lifecycle types the REPL and core adapter will consume.

#### 1.1 — New public types

**New header**: `include/omni/project.h`

```cpp
struct WorkspaceContext {
    std::string project_id;
    std::filesystem::path workspace_root;
    std::filesystem::path working_dir;        // defaults to workspace_root
    std::optional<std::string> user_id;
};

struct ProjectRuntimeConfig {
    WorkspaceContext workspace;
    Engine::Config engine;                     // reuse existing engine config
    std::vector<MCPServerConfig> mcp_servers;
    std::vector<std::unique_ptr<Tool>> project_tools;
    // Profile manifests (see Phase 2); empty = use defaults
    std::vector<AgentProfileManifest> profiles;
};
```

**New header**: `include/omni/host.h`

```cpp
class ProjectEngineHost {
public:
    static std::unique_ptr<ProjectEngineHost> create(ProjectRuntimeConfig config);

    const std::string& project_id() const;
    const WorkspaceContext& workspace() const;

    // Session lifecycle
    std::unique_ptr<ProjectSession> open_session(SessionOptions opts = {});
    std::unique_ptr<ProjectSession> resume_session(const std::string& session_id);
    std::vector<SessionSummary> list_sessions() const;

    // Runtime management
    void reload(ProjectRuntimeConfig config);
    HostStatus status() const;
    void shutdown();

    // Tool / MCP
    void register_tool(std::unique_ptr<Tool> tool);
    bool connect_mcp(const MCPServerConfig& config);
    void disconnect_mcp(const std::string& name);

    ~ProjectEngineHost();
};
```

**Implementation strategy**: `ProjectEngineHost` internally owns an `Engine` instance and a `SessionPersistence`. It wraps current `Engine::create_session()` calls and adds project scoping. This is a **wrapper layer**, not a rewrite of `Engine`.

#### 1.2 — ProjectSession and ProjectRun

**New header**: `include/omni/project_session.h`

```cpp
struct SessionOptions {
    std::string profile = "explore";           // default profile name
    std::optional<std::string> session_id;     // resume specific session
    std::optional<std::filesystem::path> working_dir_override;
};

class ProjectSession {
public:
    const std::string& session_id() const;
    const std::string& project_id() const;
    const std::string& active_profile() const;

    // Turn/task submission → produces a Run
    std::unique_ptr<ProjectRun> submit_turn(
        const std::string& input,
        RunObserver& observer,
        ApprovalDelegate& approvals);

    // Profile switching
    void set_profile(const std::string& profile_name);

    // State
    SessionSnapshot snapshot() const;    // messages, usage, profile, active run
    void reset();                        // clear conversation, keep session identity
    void close();

    ~ProjectSession();
};
```

**New header**: `include/omni/run.h`

```cpp
enum class RunStatus {
    Running, Paused, Completed, Stopped, Cancelled, Failed
};

class ProjectRun {
public:
    const std::string& run_id() const;
    RunStatus status() const;

    void cancel();     // immediate abort
    void stop();       // graceful stop after current turn/tool

    // For approval-paused runs
    void resume(const std::string& resume_input = "");

    void wait();       // block until terminal state
    RunResult result() const;

    ~ProjectRun();
};
```

**Implementation strategy**: `ProjectRun` wraps the existing `Session::submit()`/`wait()`/`cancel()` into a first-class object. The `stop()` semantic requires a new `std::atomic<bool> stop_requested_` flag checked by `QueryEngine::run_turn()` after each tool-execution round (low-effort addition).

#### 1.3 — RunObserver and ApprovalDelegate

**New header**: `include/omni/observer.h`

```cpp
// Extends EventObserver with run-level identity
class RunObserver {
public:
    virtual ~RunObserver() = default;
    virtual void on_event(const Event& e,
                          const std::string& project_id,
                          const std::string& session_id,
                          const std::string& run_id) = 0;
};
```

**New header**: `include/omni/approval.h`

```cpp
class ApprovalDelegate {
public:
    virtual ~ApprovalDelegate() = default;

    // Called when a tool requires approval; run pauses until resolved
    virtual ApprovalDecision on_approval_requested(
        const std::string& tool_name,
        const nlohmann::json& args,
        const std::string& description) = 0;
};

enum class ApprovalDecision { Approve, ApproveAlways, Deny };
```

**Implementation**: `ApprovalDelegate` wraps the existing `PermissionDelegate` interface. Internally, `ProjectRun` creates a `PermissionDelegate` adapter that:
1. Forwards to the host-provided `ApprovalDelegate`.
2. Emits `ApprovalRequested`/`ApprovalResolved` events.
3. Supports run pause/resume semantics via condition variable.

#### 1.4 — Workspace scoping enforcement

**Files**: `include/omni/project.h`, tool execution path

- `WorkspaceContext` validation: `working_dir` must be under `workspace_root` unless policy allows escape.
- `ProjectEngineHost::create()` fails fast if `workspace_root` doesn't exist.
- Pass `WorkspaceContext` into tool execution via a `ToolContext` parameter (add `ToolContext` field to `ToolCallResult` or as a new `call()` overload).

**This is the most invasive change in Phase 1** because it touches the `Tool::call()` virtual. Two approaches:
- **(A) Break the interface**: Add `ToolContext` parameter to `Tool::call()`. All tools must update. Clean but breaking.
- **(B) Thread-local / ambient**: Store `ToolContext` in a thread-local before dispatch. No interface change needed. Pragmatic for P0.

**Recommendation**: **(A)** with a default-constructed `ToolContext` overload so existing tools continue to compile. Mark the old `call(args)` overload as deprecated.

#### 1.5 — Build integration

**Files**: `CMakeLists.txt`

- New public headers added to `PUBLIC_HEADER` install set.
- No new library targets; types are part of the `omni-engine` static library.
- Add Phase 1 unit tests: `test_project_host.cpp`, `test_project_session.cpp`, `test_project_run.cpp`.

---

### Phase 2: Extended Events + Agent Profiles

**Goal**: Flesh out event model and specialist profiles so host apps and the REPL have rich runtime information.

#### 2.1 — Extended event types

**Files**: `include/omni/event.h`

Add to the `Event` variant:

```cpp
struct RunStartedEvent      { std::string run_id; std::string profile; };
struct RunPausedEvent        { std::string run_id; std::string reason; };
struct RunResumedEvent       { std::string run_id; };
struct RunStoppedEvent       { std::string run_id; };
struct RunCancelledEvent     { std::string run_id; };
struct SessionResetEvent     { std::string session_id; };
struct AgentSpawnedEvent     { std::string agent_id; std::string task; std::string profile; };
struct AgentCompletedEvent   { std::string agent_id; bool success; };
struct ApprovalRequestedEvent{ std::string tool_name; nlohmann::json args; std::string description; };
struct ApprovalResolvedEvent { std::string tool_name; ApprovalDecision decision; };
struct UsageUpdatedEvent     { Usage delta; Usage cumulative; };
struct CompactionEvent       { int messages_before; int messages_after; };
```

Each event struct also carries: `std::string project_id`, `std::string session_id`, `std::string run_id`, `std::chrono::system_clock::time_point timestamp`.

**Emit points**:
- `RunStarted`: at top of `submit_turn()`/`start_task()`.
- `RunPaused`/`RunResumed`: in the `ApprovalDelegate` adapter when blocking/unblocking.
- `RunStopped`/`RunCancelled`: in `ProjectRun::stop()`/`cancel()` after the run terminates.
- `AgentSpawned`/`AgentCompleted`: in `AgentManager::spawn()` and its completion callback.
- `ApprovalRequested`/`ApprovalResolved`: in the permission adapter.
- `UsageUpdated`: in `QueryEngine` after each provider response.
- `CompactionEvent`: in auto_compact after summarization.
- `SessionReset`: in `ProjectSession::reset()`.

#### 2.2 — Agent profile manifests

**New header**: `include/omni/profile.h`

```cpp
struct AgentProfileManifest {
    std::string name;                    // "spec", "plan", "explore", "bugfix", etc.
    std::string system_prompt;
    ToolCapabilityPolicy tool_policy;    // capability-based, not name-based
    PermissionMode default_permission_mode;
    bool sub_agents_allowed = false;
    int max_parallel_tools = 10;
};

struct ToolCapabilityPolicy {
    bool allow_read_only = true;
    bool allow_write = false;
    bool allow_destructive = false;
    bool allow_shell = false;
    bool allow_network = false;
    bool allow_mcp = false;
    std::vector<std::string> explicit_allow;   // override: allow these tools by name
    std::vector<std::string> explicit_deny;    // override: deny these tools by name
};
```

**Engine-provided defaults** (compiled in, overridable by host):

| Profile | read_only | write | destructive | shell | network | mcp | sub_agents | permission_mode |
|---|---|---|---|---|---|---|---|---|
| `explore` | yes | no | no | no | no | no | no | Default |
| `spec` | yes | no | no | no | yes | yes | no | Default |
| `plan` | yes | no | no | no | no | no | no | Default |
| `research` | yes | no | no | no | yes | yes | no | Default |
| `bugfix` | yes | yes | yes | yes | no | no | no | AcceptEdits |

**Tool capability tagging**: Extend `Tool` interface:

```cpp
class Tool {
    // ... existing methods ...
    virtual bool is_read_only() const = 0;      // already exists
    virtual bool is_destructive() const = 0;     // already exists
    // New:
    virtual bool is_shell() const { return false; }
    virtual bool is_network() const { return false; }
};
```

**Filtering**: Replace `set_tool_filter(vector<string>)` with capability-aware filtering in `ToolExecutor`. When a profile is active, `ToolExecutor` checks each tool's capabilities against `ToolCapabilityPolicy` before including it in the LLM request and before executing.

#### 2.3 — Wire profiles into session/run

- `ProjectSession` stores the active `AgentProfileManifest`.
- `set_profile(name)` looks up the manifest from the host's registered set.
- `submit_turn()` passes the active profile's `ToolCapabilityPolicy` and `system_prompt` into the `QueryEngine` config for that run.
- `AgentManager::spawn()` uses profile for sub-agent capability scoping.

---

### Phase 3: CLI REPL

**Goal**: Thin executable over the Phase 1+2 API for interactive testing.

#### 3.1 — Build target

**Files**: `CMakeLists.txt`

- New executable target: `omni-engine-cli`
- Links: `omni-engine` (static), plus terminal I/O (no extra deps beyond what engine already uses).
- Source dir: `omniagent-engine/src/cli/`

#### 3.2 — CLI entry point

**New files**: `src/cli/main.cpp`, `src/cli/repl.h`, `src/cli/repl.cpp`

**Modes** (from CLI flags):
- `repl` (default): interactive loop
- `run`: one-shot task, exit on completion
- `inspect`: dump session/run state, exit
- `resume`: resume a paused run

**Flag parsing**: Use a lightweight header-only arg parser (e.g., bundled `CLI11.hpp` or manual `getopt_long`). Keep it simple.

**Startup flow**:
1. Parse flags → build `ProjectRuntimeConfig` from `--project-id`, `--workspace-root`, `--cwd`, `--profile`, `--model`, `--config`.
2. `ProjectEngineHost::create(config)`.
3. Open or resume session.
4. Enter mode.

#### 3.3 — REPL implementation

**File**: `src/cli/repl.cpp`

**Core loop**:
```
prompt = "[project:session:profile:cwd]> "
while (line = readline(prompt)):
    if line starts with "/":
        dispatch_slash_command(line)
    else:
        run = session.submit_turn(line, terminal_observer, terminal_approvals)
        run.wait()
```

**Terminal observer**: Implements `RunObserver`. Prints text deltas inline, tool calls as compact summaries, approvals as interactive prompts.

**Terminal approval delegate**: Implements `ApprovalDelegate`. On `on_approval_requested()`: print tool + args + description, prompt `[a]pprove / [d]eny`, block until user input.

**Slash commands**: Route via `CommandRegistry` (already exists in engine under `commands/command_registry.cpp`). Add REPL-specific commands.

| Command | Implementation |
|---|---|
| `/help` | Print command list |
| `/profile <name>` | `session->set_profile(name)` |
| `/tools` | Print tool names + capability tags |
| `/sessions` | `host->list_sessions()` |
| `/use <id>` | Close current session, `host->resume_session(id)` |
| `/reset` | `session->reset()` |
| `/clear` | Clear terminal |
| `/cwd [path]` | Show or set working dir (validated within workspace_root) |
| `/model [name]` | Show or swap model on engine config |
| `/runs` | List recent runs from persistence |
| `/inspect run <id>` | Print run result + trace summary |
| `/cancel` | `active_run->cancel()` |
| `/stop` | `active_run->stop()` |
| `/quit` | Shutdown host, exit |

**Ctrl-C handling**: Install `SIGINT` handler.
- First Ctrl-C: set `stop_requested` on active run.
- Second Ctrl-C within 2s: `cancel()` + force exit.

#### 3.4 — One-shot `run` mode

```
host = create(config)
session = host->open_session({profile})
run = session->submit_turn(task_text, stdout_observer, auto_approve_delegate)
run->wait()
print(run->result())
host->shutdown()
exit(run->result().success ? 0 : 1)
```

---

### Phase 4: Persistence Expansion

**Goal**: Persist run state, approval state, and project host state beyond transcript-only JSONL.

#### 4.1 — Run persistence

**New file**: `src/services/run_persistence.h`

```cpp
struct RunRecord {
    std::string run_id;
    std::string session_id;
    std::string project_id;
    RunStatus status;
    std::string profile;
    std::string input;
    std::string output;
    Usage usage;
    std::chrono::system_clock::time_point started_at;
    std::chrono::system_clock::time_point finished_at;
    std::optional<std::string> error;
};
```

- Saved to `{storage_dir}/runs/{run_id}.json` on completion, stop, cancel, or failure.
- Paused runs additionally save approval context for resume.

#### 4.2 — Project host state persistence

- On `shutdown()`: serialize current config, active MCP connections, session list to `{storage_dir}/host_state.json`.
- On create with existing `storage_dir`: optionally restore from persisted state.

#### 4.3 — Approval state persistence

- When a run pauses for approval: write `{storage_dir}/pending/{run_id}.json` with tool call + context.
- On `resume_run`: read pending state, resume `QueryEngine` run from tool-call point.
- This is the most complex piece. For P0, support resume **within the same process**. Cross-process resume (surviving restart) is P1.

---

### Phase 5: omniagent-core Adapter

This phase is now part of the active architecture track for project-scoped
engine loops in `omniagent-core` (outside graph execution).

#### 5.1 — Core-side service layer

| Component | Maps To |
|---|---|
| `ProjectEngineService` | Lifecycle management of `ProjectEngineHost` per project |
| `ProjectEngineRegistry` | `std::unordered_map<project_id, ProjectEngineHost>` |
| `ProjectEngineWsBridge` | `RunObserver` → WS event serializer per project channel |
| `ProjectEngineRestAdapter` | REST endpoints for session/run CRUD |
| `ProjectToolPolicyAdapter` | Maps core user/project auth into `ToolCapabilityPolicy` |

#### 5.2 — REST endpoints (indicative)

```
POST   /api/projects/:id/engine/sessions           → open_session
GET    /api/projects/:id/engine/sessions            → list_sessions
DELETE /api/projects/:id/engine/sessions/:sid        → close_session
POST   /api/projects/:id/engine/sessions/:sid/turns  → submit_turn
POST   /api/projects/:id/engine/runs/:rid/cancel     → cancel_run
POST   /api/projects/:id/engine/runs/:rid/stop       → stop_run
GET    /api/projects/:id/engine/runs/:rid             → run_result
```

#### 5.3 — WS event channel

- New WS message type `engine_event` on the existing project WS channel.
- Events serialized as JSON matching the `Event` variant tag + fields.
- Client subscribes per session or per run.

---

## Implementation Sequence & Dependencies

```
Phase 0                          Phase 1
  0.1 Session lifetime ──┐
  0.2 Submit serialization┼──→ 1.1 Public types ──→ 1.2 Session/Run ──→ 1.3 Observer/Approval
  0.3 HttpProvider fix ───┘                                    │
  0.4 Config propagation                                       ▼
  0.5 MCP timeout ──────────────────────────────────→ 1.4 Workspace scoping
                                                               │
                                                               ▼
                                                         1.5 Build integration
                                                               │
                                          ┌────────────────────┤
                                          ▼                    ▼
                                    Phase 2              Phase 3
                                    2.1 Events           3.1 Build target
                                    2.2 Profiles         3.2 CLI entry
                                    2.3 Wire profiles    3.3 REPL impl
                                          │              3.4 One-shot mode
                                          │                    │
                                          ▼                    ▼
                                    Phase 4              Phase 5
                                    4.1 Run persist      5.x Core adapter
                                    4.2 Host persist     (separate spec)
                                    4.3 Approval persist
```

**Key dependency**: Phase 3 (REPL) can start on 3.1–3.2 as soon as Phase 1 compiles. It doesn't need Phase 2 events to be complete — it can use the existing 7 event types for initial terminal output and upgrade incrementally.

**Parallelizable**:
- Phase 0 items (0.1–0.5) are independent of each other.
- Phase 2 and Phase 3 can proceed in parallel after Phase 1.
- Phase 4 can start after Phase 1 completes.

---

## File Inventory

### New files to create

| File | Phase | Purpose |
|---|---|---|
| `include/omni/project.h` | 1 | WorkspaceContext, ProjectRuntimeConfig |
| `include/omni/host.h` | 1 | ProjectEngineHost |
| `include/omni/project_session.h` | 1 | ProjectSession, SessionOptions, SessionSnapshot |
| `include/omni/run.h` | 1 | ProjectRun, RunStatus, RunResult |
| `include/omni/observer.h` | 1 | RunObserver |
| `include/omni/approval.h` | 1 | ApprovalDelegate, ApprovalDecision |
| `include/omni/profile.h` | 2 | AgentProfileManifest, ToolCapabilityPolicy |
| `src/host_impl.cpp` | 1 | ProjectEngineHost implementation |
| `src/project_session_impl.cpp` | 1 | ProjectSession implementation |
| `src/project_run_impl.cpp` | 1 | ProjectRun implementation |
| `src/profiles/default_profiles.cpp` | 2 | Built-in spec/plan/explore/research/bugfix profiles |
| `src/cli/main.cpp` | 3 | CLI entrypoint and flag parsing |
| `src/cli/repl.h` | 3 | REPL class declaration |
| `src/cli/repl.cpp` | 3 | REPL loop, slash commands, terminal observer |
| `src/services/run_persistence.h` | 4 | RunRecord + persistence |
| `src/services/run_persistence.cpp` | 4 | Run persistence implementation |
| `tests/test_project_host.cpp` | 1 | Host lifecycle tests |
| `tests/test_project_session.cpp` | 1 | Session lifecycle tests |
| `tests/test_project_run.cpp` | 1 | Run lifecycle + stop/cancel tests |
| `tests/test_profiles.cpp` | 2 | Profile capability filtering tests |
| `tests/test_extended_events.cpp` | 2 | New event emission tests |

### Existing files to modify

| File | Phase | Change |
|---|---|---|
| `src/session_impl.h` | 0 | Add submit mutex, destroy wait |
| `src/session_impl.cpp` | 0 | Serialize submits, safe destructor |
| `src/providers/http_provider.cpp` | 0 | Check Result status, return errors |
| `src/engine_impl.cpp` | 0 | Propagate max_result_chars to ToolExecutorConfig |
| `src/mcp/mcp_client.cpp` | 0 | Add pipe read timeout using poll() |
| `include/omni/event.h` | 2 | Add 13 new event types to variant |
| `include/omni/tool.h` | 2 | Add `is_shell()`, `is_network()` virtual methods |
| `src/tools/tool_executor.cpp` | 2 | Capability-based filtering |
| `src/agents/agent_manager.h` | 2 | Replace AgentType enum with profile reference |
| `src/agents/agent_manager.cpp` | 2 | Use profile manifests for agent scoping |
| `src/core/query_engine.cpp` | 1+2 | Emit new events, check stop_requested flag |
| `CMakeLists.txt` | 1+3 | New headers, new cli executable target, new tests |

---

## Testing Strategy

### Phase 0 tests

- **ASAN/TSAN** builds for all session lifetime and concurrency fixes.
- Targeted unit tests per fix (see per-item descriptions above).

### Phase 1 tests

- `test_project_host`: create host → check status → shutdown → assert clean teardown.
- `test_project_session`: open session → submit turn → check snapshot → reset → verify empty history.
- `test_project_run`: submit → wait → check result. Submit → cancel → check Cancelled. Submit → stop → check Stopped.
- Workspace validation: reject working_dir outside workspace_root.
- Two concurrent submits on same session: second rejected.

### Phase 2 tests

- Profile tool filtering: `explore` profile hides write tools. `bugfix` profile exposes them.
- Event emission: submit a turn, collect events, assert `RunStarted` and `UsageUpdated` present.
- Profile switching: set `explore`, submit, set `bugfix`, submit — tool sets differ.

### Phase 3 tests

- CLI builds and links without `omniagent-core`.
- `omni-engine-cli --help` exits 0.
- `omni-engine-cli run --project-id test --workspace-root /tmp --non-interactive` with a mock config completes.
- REPL slash commands parse correctly (unit test the command dispatcher).

### Phase 4 tests

- Run persistence: complete a run, assert `{storage_dir}/runs/{run_id}.json` exists with correct fields.
- Host state persistence: shutdown → recreate with same storage_dir → assert sessions list matches.

---

## Risk Assessment

| Risk | Likelihood | Mitigation |
|---|---|---|
| `Tool::call()` interface change breaks downstream | Medium | Provide deprecated overload; engine owns all current tools |
| REPL streaming UX is hard to get right on raw terminal | Medium | Start with simple line-buffered output; iterate |
| Approval pause/resume across process restarts is complex | High | Defer cross-process resume to P1; in-process pause/resume is straightforward |
| Phase 2 event variant grows large (20+ types) | Low | Variants are cheap; consider `std::visit` ergonomics |
| MCP timeout fix may break slow-starting servers | Low | Make timeout configurable per-server; generous defaults |

---

## Summary

The spec is validated and accurate. The implementation plan sequences around five concrete phases:

1. **Phase 0** (prerequisite): 5 surgical bug fixes from the audit.
2. **Phase 1** (foundation): Project/Session/Run types wrapping current Engine.
3. **Phase 2** (richness): Extended events + capability-based profiles.
4. **Phase 3** (product): CLI REPL as thin client over the API.
5. **Phase 4** (durability): Run + host + approval persistence.
6. **Phase 5** (integration): Core adapter for `omniagent-core` project-scoped agent loops.

Each phase is independently testable and shippable. Phase 0+1 is the critical path.
