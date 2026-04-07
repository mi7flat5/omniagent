# OmniAgent Engine Module Audit

Date: 2026-04-06

## Scope

This audit covers the standalone `omniagent-engine` module only.

Goals:

1. Inventory the features that exist in the engine today.
2. Separate implemented internals from product-reachable capabilities.
3. Identify correctness bugs, robustness gaps, and missing table-stakes runtime features.
4. Compare the module against established agent runtimes.
5. Produce a concrete plan to harden and flesh the module out.

## Executive Summary

`omniagent-engine` is not a stub. It is already a functional standalone agent loop with:

- stateful sessions,
- streaming provider integration,
- tool calling,
- permission checks,
- auto-compaction,
- MCP tool integration,
- plugin loading,
- child-agent spawning,
- JSONL session persistence,
- and a healthy unit test suite.

The module's main weakness is not missing breadth so much as missing runtime guarantees.

The current implementation behaves like a useful prototype runtime, but not yet a production-grade agent engine. The largest gaps are:

- unsafe session lifetime and per-session concurrency,
- silent transport failure handling in the HTTP provider,
- MCP timeouts and cancellation not actually enforced,
- no durable mid-run pause/resume model,
- no structured tracing/observability,
- hooks and memory existing internally but not exposed through the public API,
- incomplete policy modes and brittle agent capability filtering.

The good news is the module is compact and well-factored enough that these issues are fixable without redesigning the whole tree.

## Review Method

Evidence came from:

- public API headers under `omniagent-engine/include/omni/`,
- implementation files under `omniagent-engine/src/`,
- unit tests under `omniagent-engine/tests/`,
- build and harness configuration in `omniagent-engine/CMakeLists.txt` and `omniagent-engine/README.md`,
- a full `ctest --test-dir build --output-on-failure` run in `omniagent-engine`,
- prior-art research from LangGraph, AutoGen, OpenAI Agents SDK, OpenHands, and Aider documentation.

Verification:

- `omniagent-engine` test suite: `129/129` tests passing at audit time.

Confidence labels used below:

- High: directly verified from code or command output.
- Medium: strongly inferred from current structure, but not fully exercised end to end.
- Low: directional recommendation based on prior art rather than a current defect.

## Current Architecture

### Public Surface

The public API exposes a small but coherent engine model:

- `Engine`: owns provider, engine-level tools, MCP connections, and session creation.
- `Session`: submit, wait, cancel, register private tools, inspect messages/usage/cost, resume prior messages, and restrict visible tools.
- `LLMProvider`: provider abstraction with streaming completion.
- `Tool`: tool schema, metadata, and execution.
- `PermissionDelegate`: host-side permission decision surface.
- `EventObserver`: event callback surface for text, thinking, tool, error, and done events.
- `Plugin`: dynamic tool packs.

### Internal Runtime Shape

The engine is split into focused subsystems:

| Area | Files | Purpose |
|---|---|---|
| Engine/session lifecycle | `src/engine_impl.cpp`, `src/session_impl.cpp` | Thread pool, session creation, work scheduling |
| Agent loop | `src/core/query_engine.cpp` | Main LLM/tool loop, retries, compaction, persistence |
| Context management | `src/core/microcompact.cpp`, `src/core/auto_compact.cpp` | Message trimming and summary compaction |
| Tooling | `src/tools/tool_registry.cpp`, `src/tools/tool_executor.cpp` | Tool lookup, validation, permission checks, parallel execution |
| Child agents | `src/agents/agent_manager.cpp`, `src/agents/agent_tool.cpp` | Background/foreground sub-agents and follow-up messages |
| Permissions | `src/permissions/permission_checker.cpp` | Rule evaluation plus delegate fallback |
| Provider | `src/providers/http_provider.cpp` | OpenAI-compatible streaming HTTP provider |
| Services | `src/services/*` | Cost tracking, hooks, memory loading, session persistence |
| MCP | `src/mcp/*` | Subprocess-backed MCP client and tool wrappers |
| Plugins | `src/plugins/plugin_manager.cpp` | Shared-library tool loading |

### Runtime Control Flow

The runtime loop is straightforward:

1. `Session::submit()` enqueues work onto the engine thread pool.
2. `QueryEngine::submit()` appends the user message.
3. Each turn:
   - optionally auto-compacts context,
   - fires `PrePrompt` hook,
   - builds a request from system prompt, memory, message history, and tool schemas,
   - streams provider output through `StreamAssembler`,
   - emits observer events,
   - executes tool calls if present,
   - appends tool results,
   - loops again until no tool calls, cancel, or `max_turns`.
4. At the end of submit, it emits `DoneEvent` and optionally persists the transcript.

This is a reasonable baseline architecture for a standalone agent loop.

## What Exists Today

### Implemented Features

High-confidence capabilities present in the code today:

| Capability | Status | Notes |
|---|---|---|
| Stateful session history | Implemented | `Session`, `QueryEngine::messages()` |
| Streaming text deltas | Implemented | `TextDeltaEvent` via stream callback |
| Streaming thinking deltas | Implemented | `ThinkingDeltaEvent` supported by assembler/provider |
| Streaming tool input fragments | Implemented | `ToolUseInputEvent` emitted during tool-call assembly |
| Multi-turn tool loop | Implemented | Query loop continues after tool results |
| Max-turn protection | Implemented | `max_turns` in engine/query config |
| Retry on transient provider failures | Implemented | Retry and exponential backoff in `QueryEngine` |
| Context overflow recovery | Implemented | Reactive compaction and retry |
| Tool registry | Implemented | Engine-level and session-level tool registration |
| Tool schema validation | Implemented | Lightweight required-field validation |
| Permission checks | Implemented | Rules plus host delegate fallback |
| Tool parallelism | Implemented | Read-only tools run concurrently, write tools serialize |
| Doom-loop detection | Implemented | Consecutive same-tool failures abort execution |
| Session transcript persistence | Implemented | JSONL save/load/list/remove |
| Cost tracking | Implemented | Per-model aggregate cost snapshot |
| MCP tool integration | Implemented | stdio subprocess + tool wrapping |
| Dynamic plugins | Implemented | Shared-library loading and tool registration |
| Child agents | Implemented | Foreground/background sub-agents plus send-message tool |
| Slash commands | Implemented | `/clear`, `/help`, model/cost flow, custom registry |
| Auto-compaction | Implemented | Soft-limit proactive compaction |
| Memory file loading | Implemented internally | `MemoryLoader` exists and is tested |
| Hook execution | Implemented internally | `HookEngine` exists and is tested |

### Strong Test Coverage Areas

The existing test suite is solid on narrow unit behavior:

- message serialization,
- stream assembly,
- compaction logic,
- tool registry/executor behavior,
- query loop basics,
- cost tracking,
- permission rules,
- session persistence,
- hooks,
- memory loader parsing,
- child-agent behavior,
- plugin bookkeeping,
- MCP JSON mapping.

### Implemented But Not Properly Surfaced

These capabilities exist internally but are not meaningfully exposed as a usable product surface:

| Capability | Current State | Gap |
|---|---|---|
| Hook engine | `QueryEngine` can accept hooks | No public `Engine` or `Session` API wires hooks into created sessions |
| Memory loader | `QueryEngine` can accept memory loader | No public session creation/configuration path attaches it |
| Plan permission mode | Enum exists | Checker explicitly falls back to default behavior |

This distinction matters: the module has more code than the public surface suggests, but some of that code is effectively dormant.

## Prior-Art Comparison

Research sources:

- LangGraph overview, durable execution, and interrupts docs.
- Microsoft AutoGen Core and AgentChat docs.
- OpenAI Agents SDK overview, sessions, human-in-the-loop, and tracing docs.
- OpenHands README.
- Aider docs.

### Comparison Matrix

| Capability | OmniAgent Engine | LangGraph | AutoGen | OpenAI Agents SDK | OpenHands/Aider Signal |
|---|---|---|---|---|---|
| Built-in agent/tool loop | Yes | Yes | Yes | Yes | Yes |
| Session memory across turns | Yes | Yes | Yes | Yes | Yes |
| Durable mid-run pause/resume | No | Yes | Partial/architecture-dependent | Yes | Yes in product workflows |
| Human approval interrupts with resume state | No | Yes | Partial | Yes | Yes as product expectation |
| Structured tracing/spans | No | Yes via LangSmith | Yes | Yes | Expected in mature coding agents |
| Child agent delegation | Yes | Yes | Yes | Yes | Yes |
| Explicit agent-as-tool orchestration model | Partial | Yes | Yes | Yes | Yes |
| MCP support | Yes | Yes | Yes | Yes | Common |
| Tool-specific approval policies | Partial | Yes | Partial | Yes | Common |
| Provider/network robustness controls | Partial | Varies | Varies | Stronger surfaced controls | Expected |
| Tool/workspace sandboxing and quotas | No | No by default | Extensions | Partial | Strong product expectation |
| Structured output / typed result surface | No | Yes | Yes | Yes | Common |
| Replay/debuggable execution history | Partial | Yes | Yes | Yes | Strong expectation |
| Branching/versioned session history | No | Yes | Partial | Yes | Common in mature systems |

### What Prior Art Makes Table Stakes

Capabilities that show up repeatedly across the reference runtimes and should be treated as expected rather than optional:

1. Durable pause/resume for human approval or failure recovery.
2. Structured tracing with run IDs, spans, and exportable telemetry.
3. Strong session/run persistence rather than transcript-only persistence.
4. Clear agent orchestration semantics for delegation and handoffs.
5. Explicit policy surfaces for approvals, tool filtering, and safety.
6. Better runtime control over context growth, history selection, and compaction.

## Confirmed Code Findings

### 1. Session lifetime is unsafe when work is still in flight

Confidence: High

Severity: High

Evidence:

- `src/session_impl.cpp` documents that callers may "let the Session destructor call wait() before destruction".
- `Session::~Session()` is defaulted and does not wait.
- `Session::submit()` captures raw `QueryEngine*` and `Session::Impl*` into an async task.

Impact:

- Destroying a session while a submit is still running can leave queued work with dangling pointers.
- This is a real use-after-free hazard, not just a theoretical concern.

### 2. Same-session submits can execute concurrently and race shared state

Confidence: High

Severity: High

Evidence:

- `Engine::Impl` uses a shared thread pool with no per-session serialization.
- `Session::submit()` blindly enqueues a new task on every call.
- `QueryEngine` mutates `messages_`, `total_usage_`, and cancellation state without internal synchronization.
- `AgentManager::send_message()` exposes exactly this path for running agents.

Impact:

- Two messages sent to the same session can run simultaneously on different pool threads.
- That can corrupt session history ordering, race observer emissions, and break tool loops.

### 3. HTTP transport failures are masked as successful empty turns

Confidence: High

Severity: High

Evidence:

- `src/providers/http_provider.cpp` calls `client.Post(...)` and never checks whether the request succeeded.
- If the request fails, the provider still emits a fallback `MessageDelta`/`MessageStop` with `end_turn` and returns zero-ish usage.

Impact:

- Network failures, TLS failures, auth failures, and HTTP transport failures can disappear as apparently successful no-op turns.
- Retry logic in `QueryEngine` is bypassed because no exception is thrown.

### 4. MCP timeouts are declared but not enforced

Confidence: High

Severity: High

Evidence:

- `MCPServerConfig` contains `init_timeout`.
- `src/mcp/mcp_client.cpp` does blocking line reads with `read()` and no timeout handling.
- `send_request()` loops up to 200 messages but can still block forever on each `read_line()` call.

Impact:

- A stuck or half-dead MCP server can hang connection, tool listing, resource reads, or tool calls indefinitely.
- This is especially risky because MCP is subprocess-backed and expected to fail in messy ways.

### 5. Hooks and memory are implemented but effectively internal-only

Confidence: High

Severity: Medium

Evidence:

- `QueryEngine` exposes `set_hooks()` and `set_memory_loader()`.
- There is no public `Engine` or `Session` API that lets a caller attach either one to created sessions.
- Tests cover the isolated services, not end-to-end use through the engine surface.

Impact:

- The codebase suggests features the product surface does not actually deliver.
- This creates documentation drift and false confidence about extensibility.

### 6. `PermissionMode::Plan` is advertised but intentionally unimplemented

Confidence: High

Severity: Medium

Evidence:

- `include/omni/permission.h` describes `Plan` as phase-3 behavior.
- `src/permissions/permission_checker.cpp` explicitly says plan mode falls through to default behavior.

Impact:

- Callers cannot rely on plan/dry-run policy semantics.
- Safety behavior is less predictable than the API suggests.

### 7. Tool result truncation config is inconsistent between engine and executor

Confidence: High

Severity: Medium

Evidence:

- `Engine::Config.max_result_chars` defaults to `500`.
- `QueryEngineConfig.max_result_chars` is populated from engine config.
- `QueryEngine` constructs `ToolExecutor` with default config `{}`.
- `ToolExecutorConfig.max_result_chars` defaults to `50000`.

Impact:

- The public engine config does not actually control tool result truncation in execution.
- Operators can think the engine is enforcing one policy while runtime behavior uses another.

### 8. Hook timeouts do not cancel work; they detach it

Confidence: High

Severity: Medium

Evidence:

- `src/services/hooks.cpp` runs hook handlers on detached threads.
- On timeout, the engine simply drops the future and continues.

Impact:

- Timed-out hooks can keep running and mutating state or external systems after the engine has decided to ignore them.
- That is acceptable for advisory hooks, but unsafe for side-effecting hooks without stricter contracts.

### 9. Plugin unload correctness depends on manifest honesty

Confidence: Medium

Severity: Medium

Evidence:

- `PluginManager` unregisters tools using `manifest.tool_names`.
- Actual registered tool names come from `plugin.create_tools()`.

Impact:

- A plugin that returns tools whose names do not exactly match the manifest can leak tools into the registry after unload.
- This is survivable but brittle.

## Missing Features And Gaps

### Robustness Gaps

1. No durable run checkpointing or resumable mid-turn execution.
2. No first-class pause/approve/reject/resume flow for tool calls.
3. No strong cancellation semantics for HTTP or MCP work beyond stop flags.
4. No per-session execution serialization contract.
5. No explicit failure taxonomy or structured run result object.
6. No health checks, heartbeat, or reconnect strategy for MCP servers.
7. No quotas for memory, runtime, tool count, disk use, or subprocess count.

### Operability Gaps

1. No trace IDs, span model, or exportable structured telemetry.
2. No per-session/per-agent/per-tool cost breakdown.
3. No replayable run artifact beyond message transcript.
4. No debug surface for request/response envelopes, retry causes, or compaction events.

### Product-Surface Gaps

1. Hooks not exposed through engine/session construction.
2. Memory loader not exposed through engine/session construction.
3. No structured output / typed response API.
4. No multimodal request or response surface.
5. No tool context object carrying session/workspace/user/run metadata.
6. No explicit handoff state model for agents-as-tools.

### Safety And Policy Gaps

1. `Plan` mode not implemented.
2. Tool filtering for agent types is string-based and brittle.
3. No redaction or output filtering layer for tool results.
4. No sandbox abstraction for filesystem/process/network capabilities.
5. No policy layer for MCP server trust, allowed tools, or resource access.

### Testing Gaps

The passing unit suite is real, but it does not cover the highest-risk runtime behaviors:

1. No test for session destruction during in-flight submit.
2. No test for overlapping submits on the same session.
3. No test for HTTP provider transport failure propagation.
4. No test for non-200 HTTP responses, invalid SSE framing, or partial stream failures.
5. No real subprocess MCP handshake/timeout test.
6. No end-to-end hook/memory wiring test through `Engine`.
7. No trace/telemetry contract tests because the feature does not exist yet.

## What Is Missing To Make This A Robust Agent Engine

The current module has the loop mechanics. What it lacks is runtime governance.

The most important additions are:

### P0 Missing Features

1. Per-session execution serialization.
2. Safe session teardown semantics.
3. Correct propagation of provider transport failures.
4. MCP request timeouts and cancellation.
5. Real approval pause/resume model for sensitive tools.
6. Structured run IDs and tracing.

### P1 Missing Features

1. Public APIs for hooks, memory loaders, and run context injection.
2. Structured output support.
3. Better agent capability policies than substring filtering.
4. Per-agent/per-tool usage and cost accounting.
5. Health monitoring for providers and MCP servers.

### P2 Missing Features

1. Durable checkpoint/replay for long-running agent workflows.
2. Branching session history.
3. Distributed/background worker runtime.
4. Sandboxed tool execution and resource quotas.
5. Evaluation/replay infrastructure for agent trajectories.

## Recommended Plan

### Phase 0: Correctness First

Objective: remove hidden data corruption and silent failure paths.

1. Serialize submits per session.
   - Add a per-session work queue or mutex so only one `QueryEngine::submit()` runs at a time.
   - Decide whether later submits queue, reject, or coalesce.
2. Make session teardown safe.
   - `Session::~Session()` should cancel and wait, or the session impl must hold self-lifetime until work completes.
3. Fix HTTP error propagation.
   - Check `client.Post(...)` result.
   - Throw transport and non-success HTTP errors with retry classification.
4. Enforce MCP timeouts.
   - Use non-blocking I/O, `poll`/`select`, or a dedicated worker thread with timeout control.
   - Honor `init_timeout` and add call/read timeouts.
5. Wire executor truncation to engine config.
6. Add tests for the above before moving on.

### Phase 1: Make It Operable

Objective: turn the runtime from a black box into a diagnosable system.

1. Introduce `run_id`, `turn_id`, and `tool_call_id` propagation across events.
2. Add structured tracing.
   - Trace spans for submit, LLM call, compaction, tool execution, MCP call, and child-agent spawn.
3. Add richer run result objects.
   - Final status, error classification, retries, compactions, tool summary, timing, and usage.
4. Add per-tool and per-agent cost/usage accounting.
5. Persist richer run metadata alongside transcripts.

### Phase 2: Expose The Features Already Present Internally

Objective: close the gap between internal services and public API.

1. Add public configuration APIs for hooks.
2. Add public configuration APIs for memory loading.
3. Add a `RunContext` or `ToolContext` object passed to tools and hooks.
4. Expose structured output options on sessions.
5. Replace string-based child-agent tool filtering with declared capability classes.

### Phase 3: Add Human Oversight And Durable Resume

Objective: meet modern agent-runtime expectations.

1. Implement approval interrupts for tools.
   - `Allow`, `Deny`, `Ask`, `AlwaysAllow`, and resumable pending approval states.
2. Persist paused run state, not just transcripts.
3. Resume from durable run state after approval or crash.
4. Version run state so upgrades do not silently break resumed work.

### Phase 4: Harden Ecosystem Boundaries

Objective: make extensions and external tools safer.

1. Add MCP health and reconnect management.
2. Add plugin manifest validation against actual registered tools.
3. Add trust policies for plugins and MCP servers.
4. Add resource and sandbox controls for high-risk tools.

## Suggested Backlog

### P0

- Fix unsafe session destruction.
- Serialize same-session submits.
- Fail correctly on HTTP transport/HTTP status errors.
- Implement MCP read/request timeout handling.
- Add concurrency and transport regression tests.

### P1

- Introduce trace/run IDs and structured spans.
- Expose hooks and memory loader via public API.
- Implement true `PermissionMode::Plan`.
- Propagate truncation config consistently.
- Add per-tool/per-agent usage accounting.

### P2

- Approval pause/resume with durable state.
- Replace brittle agent tool filtering with explicit capabilities.
- Add tool context injection.
- Add plugin/MCP trust policies.

### P3

- Durable replay/checkpoint model.
- Branching session history.
- Sandboxed execution options.
- Evaluation and trajectory replay infrastructure.

## Bottom Line

`omniagent-engine` already has a credible agent-loop core. The missing work is not "add an agent loop" but "make the existing loop safe, resumable, observable, and governable."

If you want this module to become the robust standalone engine for future orchestration work, the right next move is:

1. fix the three high-severity correctness issues,
2. add tracing and durable approval/resume primitives,
3. expose hooks/memory/context cleanly,
4. then build richer orchestration on top.

That sequence preserves the value already present in the module instead of replacing it.