# OmniAgent Engine Consumption And Integration Spec

Date: 2026-04-06

## Summary

This spec defines how `omniagent-engine` should be consumed in two phases:

1. An immediate CLI REPL for direct interactive testing.
2. A stable embeddable SDK/API so other applications, especially `omniagent-core`, can host one engine runtime per project and run project-scoped specialist agents in that project's working directory.

The target model is not a single global engine. The target model is a project-owned runtime:

- each project has its own engine host,
- each host is bound to one workspace root and working directory,
- each host exposes sessions and runs,
- each session can execute one or more specialist agent profiles,
- host applications can observe, control, pause, resume, and persist execution.

## What The User Is Asking For

Core intent:

- make `omniagent-engine` consumable as a productized subsystem rather than a low-level library,
- add a terminal REPL to exercise it directly,
- later embed it in `omniagent-core`,
- scope runtime state and tool execution to an owning project directory,
- support specialist agents for spec, planning, research, exploration, and bugfix work,
- define a fleshed-out API for use by other applications.

Explicit requirements:

- CLI REPL for manual testing,
- one project owns one runtime,
- runtime executes in project working directory,
- engine must support specialist agent roles,
- engine must expose an integration API suitable for host applications.

Implicit requirements:

- host apps need lifecycle control, not just `submit()`/`wait()`,
- project scoping must cover tools, persistence, permissions, and subprocesses,
- a REPL needs slash commands and good streaming UX,
- embedding in `omniagent-core` requires events that can bridge to REST and WebSocket surfaces,
- the public API must distinguish project host, session, run, and agent concepts.

## Prior Art Research

Sources consulted:

- OpenAI Agents SDK REPL utility: terminal demo loop with streaming and persistent conversation history.
- OpenAI Agents SDK orchestration docs: manager-agent versus handoff patterns, code-driven orchestration, specialist agents.
- OpenAI Agents SDK sessions, human-in-the-loop, and tracing docs from the earlier audit: project-grade session persistence, approvals, resume state, structured tracing.
- AutoGen AgentChat Agents and Teams docs: stateful agents, streaming observation, reset/resume/stop/abort, single-agent loops, team orchestration.
- Aider in-chat commands docs: slash-command REPL ergonomics and terminal interaction expectations.

### What Prior Art Suggests

Common patterns across mature agent SDKs and terminal tools:

1. REPL is a thin shell over the same session/run API the host application uses.
2. Sessions own conversation state; runs own one execution of a task or turn.
3. Streaming events are first-class and structured.
4. Specialist agents are either manager-controlled tools or explicit handoffs.
5. Reset, resume, cancel, stop, and inspect are standard controls.
6. Terminal UX relies heavily on slash commands.
7. Production integrations require tracing, approvals, and resumable state.

### Comparison Matrix

| Capability | OpenAI Agents SDK | AutoGen | Aider | This Project Target |
|---|---|---|---|---|
| Interactive terminal loop | Yes | Yes via console patterns | Yes | P0 |
| Persistent session state | Yes | Yes | Yes | P0 |
| Slash commands | Light | Light | Heavy | P0 |
| Streaming events | Yes | Yes | Yes | P0 |
| Specialist agents | Yes | Yes | Mode-based | P0 |
| Manager vs handoff orchestration | Yes | Yes | Partial | P1 |
| Reset/resume/stop/cancel controls | Yes | Yes | Yes | P0 |
| Project/workspace scoping | Partial | Host-defined | Yes | P0 |
| Structured tracing | Yes | Yes | Partial | P1 |
| Approval/pause/resume | Yes | Yes | Partial | P1 |
| Embeddable host API | Yes | Yes | No strong SDK boundary | P0 |

## Product Model

The engine should be consumed through four layers.

### Layer 1: Engine Runtime Library

This is the low-level C++ engine implementation.

Responsibilities:

- provider integration,
- tool calling loop,
- child agents,
- session/run state,
- permissions,
- persistence,
- tracing and approvals.

### Layer 2: Host Integration API

This is the embeddable application-facing API.

Responsibilities:

- create or acquire a project-owned runtime,
- open and manage sessions,
- submit turns or tasks,
- stream events,
- cancel or resume runs,
- inject project config, working directory, permissions, and tool sets.

This is the layer `omniagent-core` should use.

### Layer 3: CLI REPL

This is a thin executable over Layer 2.

Responsibilities:

- manual testing,
- session inspection,
- profile switching,
- slash commands,
- approvals in terminal,
- runtime diagnostics.

### Layer 4: Core Adapter

This is the `omniagent-core` integration surface that maps project/session/run concepts to REST and WebSocket APIs.

Responsibilities:

- project lifecycle to engine host lifecycle,
- auth/user/project policy mapping,
- WS event translation,
- persistence and project ownership,
- tool and workspace boundary enforcement.

## Target Public API

The current API is too thin for host applications. The target public API should introduce explicit project, session, run, and profile concepts.

### Core Types

| Type | Responsibility |
|---|---|
| `ProjectRuntimeConfig` | Declares project ID, workspace root, working directory, persistence paths, model config, permissions, tool policy, MCP config |
| `ProjectEngineHost` | Owns one runtime for one project/workspace |
| `ProjectSession` | Owns conversational state within a project host |
| `ProjectRun` | Represents one in-flight or completed execution |
| `AgentProfile` | Declares specialist role such as spec, plan, research, explore, bugfix |
| `RunObserver` | Receives structured events for streaming and state changes |
| `ApprovalDelegate` | Handles sensitive tool approvals and resume decisions |
| `WorkspaceContext` | Carries project path, cwd, repository metadata, user/session identity |
| `ToolContext` | Carries run/session/project metadata into tools |

### Required Host Operations

The host-facing API should support the following operations.

#### Project Host Lifecycle

| Operation | Meaning |
|---|---|
| `create_project_host(config)` | Create a runtime bound to one project/workspace |
| `get_project_host(project_id)` | Reuse an existing runtime |
| `shutdown_project_host(project_id)` | Stop and persist the runtime |
| `reload_project_host(project_id, config)` | Reconfigure models, permissions, tools, or MCP |
| `project_host_status(project_id)` | Runtime health, sessions, active runs, MCP/tool state |

#### Session Lifecycle

| Operation | Meaning |
|---|---|
| `open_session(project_id, session_options)` | Start or resume a conversational session |
| `resume_session(project_id, session_id)` | Restore prior session state |
| `reset_session(project_id, session_id)` | Clear conversation state but keep the session object |
| `close_session(project_id, session_id)` | End the session |
| `list_sessions(project_id)` | Enumerate session summaries |
| `session_snapshot(project_id, session_id)` | Inspect messages, usage, costs, current profile, active run |

#### Run Lifecycle

| Operation | Meaning |
|---|---|
| `submit_turn(session_id, input, profile)` | Send a conversational turn into a session |
| `start_task(session_id, task, profile, run_mode)` | Start a bounded agent task |
| `stream_run(run_id)` | Subscribe to structured runtime events |
| `cancel_run(run_id)` | Abort immediately |
| `stop_run(run_id)` | Request graceful stop after current turn/tool |
| `resume_run(run_id, resume_input)` | Continue from approval pause or interruption |
| `run_result(run_id)` | Final status, usage, timing, output, errors |
| `run_trace(run_id)` | Trace/span data for debugging and observability |

#### Configuration And Tooling

| Operation | Meaning |
|---|---|
| `set_profile(session_id, profile)` | Change default specialist profile for a session |
| `set_tool_policy(project_id, policy)` | Enable/disable or constrain tool bundles |
| `register_project_tool(project_id, tool)` | Register a project-scoped tool |
| `connect_project_mcp(project_id, server_config)` | Connect project-specific MCP servers |
| `set_memory_sources(project_id, memory_config)` | Attach project memory sources |
| `set_hooks(project_id, hook_config)` | Attach lifecycle hooks |

## Project Ownership Model

`omniagent-core` integration should treat the engine as a project-owned service, not a global singleton.

### Ownership Rules

1. One runtime host per `{project_id, workspace_root}`.
2. All subprocesses, tools, file access, and MCP servers inherit that project's working directory context.
3. Session state is scoped under the project host.
4. Tool registries may contain both engine-global tools and project-scoped tools, but execution context must always be project-scoped.
5. Destroying a project host must cancel active runs, flush persistence, and release MCP/plugin resources.

### Working Directory Rules

Every run must have:

- `workspace_root`: the repository or project root,
- `working_dir`: the effective cwd for tool execution,
- `project_id`: ownership key,
- optional `user_id`: if the host app is multi-user.

Default:

- `working_dir` defaults to the project's workspace root,
- host app may override it per session or per run,
- tools may not escape the workspace root unless policy explicitly allows it.

## Specialist Agent Profiles

The current `GeneralPurpose`, `Explore`, and `Plan` model is too narrow. The target should use named profiles with explicit capability bundles.

### Required Profiles

| Profile | Purpose | Default Tool Policy |
|---|---|---|
| `spec` | Requirements gathering, prior art research, gap analysis, spec writing | read-only repo tools, web/MCP fetch/search, doc writing gated |
| `plan` | Task decomposition, architecture planning, rollout sequencing | read-only repo tools, graph/planning tools, no direct edit tools by default |
| `research` | Open-ended information gathering and synthesis | read-only tools, web tools, MCP research tools |
| `explore` | Codebase inspection and understanding | read-only repo/search tools only |
| `bugfix` | Diagnose, edit, test, and verify fixes | read/write code tools, shell/test tools, approvals required for destructive actions |
| `custom` | Host-defined profile | host-specified capability bundle |

### Profile Requirements

Each profile must define:

- system prompt/instructions,
- allowed tool categories,
- default permission mode,
- approval policy,
- maximum parallelism,
- default run mode,
- whether sub-agents are allowed,
- whether the profile is manager-style or handoff-style.

String matching on tool names is not sufficient. Profiles must be based on declared tool capabilities.

## CLI REPL Spec

The first consumable product should be a dedicated executable, for example `omni-engine-cli`.

### CLI Modes

| Mode | Purpose |
|---|---|
| `repl` | Interactive testing against a project host/session |
| `run` | One-shot task execution for scripts and harnesses |
| `inspect` | Print session/run/tool/runtime state |
| `resume` | Resume a paused run |

### Required CLI Flags

| Flag | Meaning |
|---|---|
| `--project-id` | Stable project ownership key |
| `--workspace-root` | Project root directory |
| `--cwd` | Effective working directory inside the project |
| `--profile` | Default agent profile |
| `--model` | Provider/model override |
| `--session-id` | Resume an existing session |
| `--resume-run` | Resume a paused run |
| `--config` | Config file for models, policies, MCP, and tools |
| `--non-interactive` | Disable prompts for approvals and shell UI |

### Required REPL Commands

The REPL should provide a minimal but practical slash-command set.

| Command | Meaning |
|---|---|
| `/help` | Show available commands |
| `/profile <name>` | Switch specialist profile |
| `/tools` | List visible tools and capability classes |
| `/sessions` | List sessions for the current project |
| `/use <session_id>` | Attach to another session |
| `/reset` | Clear current session state |
| `/clear` | Clear terminal output or soft-reset turn context |
| `/cwd [path]` | Show or change working directory within workspace policy |
| `/model [name]` | Show or change model |
| `/approve` | Approve the current pending tool call |
| `/deny` | Deny the current pending tool call |
| `/runs` | List recent runs |
| `/inspect run <id>` | Show structured run state |
| `/cancel` | Abort in-flight run |
| `/stop` | Graceful stop |
| `/trace [run_id]` | Print a concise runtime trace |
| `/quit` | Exit the REPL |

### REPL Behavior

1. Conversation history persists across turns within the session.
2. Output streams incrementally.
3. Tool calls and approvals are visible in the terminal.
4. Pending approvals pause the run and prompt the user.
5. `Ctrl-C` requests stop on first press and abort on second press.
6. REPL can resume a paused session or run.

## Event Model

The current event API is too narrow for embedding into richer applications.

### Current Events

Current public events:

- text delta,
- thinking delta,
- tool start,
- tool input delta,
- tool result,
- error,
- done.

### Target Events

The public observer/event stream should add:

| Event | Meaning |
|---|---|
| `RunStarted` | Run metadata and profile |
| `RunPaused` | Waiting for approval or host input |
| `RunResumed` | Run resumed after interruption |
| `RunStopped` | Graceful stop completed |
| `RunCancelled` | Immediate abort |
| `SessionReset` | Session state cleared |
| `AgentSpawned` | Sub-agent created |
| `AgentCompleted` | Sub-agent completed |
| `ApprovalRequested` | Tool approval requested |
| `ApprovalResolved` | Approval granted or denied |
| `TraceSpan` | Structured trace export item |
| `UsageUpdated` | Incremental cost/token usage |
| `CompactionPerformed` | Context compaction happened |

Each event must include:

- `project_id`,
- `session_id`,
- `run_id`,
- timestamp,
- agent/profile identity where applicable.

## Persistence Model

Transcript-only persistence is not enough for host integration.

### Required Persistence Objects

| Object | Purpose |
|---|---|
| Project host state | Current config, tools, MCP state, health |
| Session state | Messages, profile, usage, last working dir |
| Run state | Status, timing, interruptions, trace summary, output |
| Approval state | Pending tool approvals and decisions |
| Trace state | Span export or persisted trace summary |

### Persistence Defaults

P0 default:

- persist sessions and completed runs to local project-owned storage,
- persist paused runs so approvals can resume later,
- trace persistence may initially be file-backed.

P1:

- add pluggable storage backends for host applications.

## `omniagent-core` Integration Shape

The core integration should be an adapter/service layer, not a direct leak of engine internals into server handlers.

### Recommended Core-Side Components

| Component | Responsibility |
|---|---|
| `ProjectEngineService` | Owns project host lifecycle |
| `ProjectEngineRegistry` | Maps project IDs to runtime hosts |
| `ProjectEngineSessionStore` | Persists and restores session/run state |
| `ProjectEngineWsBridge` | Converts engine events into WS protocol messages |
| `ProjectEngineRestAdapter` | Exposes session/run CRUD over REST |
| `ProjectToolPolicyAdapter` | Maps core user/project policy into engine policy |

### Integration Rules

1. `omniagent-core` creates or reuses a host when a project session starts.
2. The host uses the project's workspace path as the default workspace root and cwd.
3. Core-provided tools are registered as project-scoped tools.
4. WS clients subscribe to sessions and runs, not directly to raw engine internals.
5. Project deletion or archive must shut down and clean up the associated host.

## Requirements

### Must Have (P0)

- One project-scoped runtime host per project/workspace.
- Explicit workspace root and working directory passed into every host/session/run.
- CLI REPL executable for interactive testing.
- Public host/session/run API, not just low-level `Engine` and `Session`.
- Streaming event API with run/session/project identity.
- Specialist agent profiles for `spec`, `plan`, `research`, `explore`, and `bugfix`.
- Session reset, run cancel, run stop, and session resume support.
- Project-scoped persistence for sessions and completed runs.
- Tool capability-based policy model instead of name-substring filtering.
- Safe integration into other applications without assuming a global singleton.

### Should Have (P1)

- Approval pause/resume flow for sensitive tools.
- Structured tracing with run IDs and spans.
- Pluggable storage backends.
- Public APIs for hooks and memory sources.
- Graceful MCP reconnect and health reporting.
- Host-side inspection endpoints for sessions, runs, tools, and traces.

### Nice To Have (P2)

- Manager-agent versus handoff-agent orchestration modes.
- Branching session history.
- Scriptable non-interactive batch runner.
- Export/import of session and run artifacts.
- Multiple frontend shells over the same host API.

## Interactions

### Terminal

- REPL prompt shows current project, session, profile, and cwd.
- Assistant output streams live.
- Tool events print compactly but inspectably.
- Approval prompts block with a clear approve/deny workflow.
- Slash commands are always available.

### Host Application

- Host opens project host, then session, then run.
- Host subscribes to event stream.
- Host can cancel, stop, or resume.
- Host can query snapshots at any time.

## Edge Cases

- Missing workspace root: host creation fails fast with structured error.
- Working dir outside project root: reject unless policy explicitly allows it.
- Project moved on disk: host must detect stale path and require rebind or reload.
- Paused run on restart: host can resume from persisted interruption state.
- MCP server unavailable: host reports degraded state instead of hanging.
- Two runs started on one session: API must define whether this queues or rejects; default should be reject or serialize, never race.
- REPL exit during run: stop or persist according to configured policy.

## Architecture Notes

Modules/layers affected:

- `omniagent-engine/include/omni/*`
- `omniagent-engine/src/engine_impl.*`
- `omniagent-engine/src/session_impl.*`
- `omniagent-engine/src/core/query_engine.*`
- `omniagent-engine/src/agents/*`
- `omniagent-engine/src/services/*`
- new CLI entrypoint under `omniagent-engine/app/` or `omniagent-engine/src/cli/`
- later: core-side adapter layer in `omniagent-core`

Architectural boundaries to respect:

- keep the engine runtime standalone and embeddable,
- keep the REPL as a thin client over public APIs,
- keep `omniagent-core` integration in an adapter/service layer,
- do not let core-specific REST/WS contracts leak into the engine runtime layer.

Likely files affected in implementation:

- `omniagent-engine/include/omni/engine.h`
- `omniagent-engine/include/omni/session.h`
- `omniagent-engine/include/omni/event.h`
- `omniagent-engine/include/omni/tool.h`
- new headers for host/run/profile/config concepts
- `omniagent-engine/CMakeLists.txt`
- new CLI executable source files

## Validation Criteria

### Concrete Assertions

- Creating a project host with `project_id=A`, `workspace_root=/repo`, and no cwd override yields an effective working directory of `/repo`.
- Starting a session under project `A` and submitting two sequential turns preserves message history within that session.
- Starting a second session under the same project does not reuse the first session's conversation history.
- Running profile `explore` exposes only read-only capability bundles.
- Running profile `bugfix` exposes edit/test/exec capability bundles subject to policy.
- REPL `repl --project-id demo --workspace-root /repo` starts an interactive prompt and accepts `quit`/`/quit` without crash.
- REPL `/profile bugfix` updates the active session profile without restarting the project host.
- Host API can list recent runs for a project after at least one completed run.
- A paused approval run can be resumed using persisted run state after process restart.

### Golden Flows

- Flow 1: `repl` open project -> send message -> stream output -> `/reset` -> send another message -> session remains usable.
- Flow 2: host app open project -> open session -> start task with `spec` profile -> stream events -> completed run snapshot available.
- Flow 3: host app start `bugfix` run -> approval requested for destructive tool -> deny -> run ends with structured denied state, not crash.

### Invariants

- Every run belongs to exactly one session.
- Every session belongs to exactly one project host.
- Effective working directory must always be within the owning project root unless explicitly overridden by policy.
- Two in-flight runs may not mutate the same session state concurrently.
- All emitted events include project, session, and run identifiers once a run exists.

### Integration Checks

- CLI entry point builds and runs without `omniagent-core` linkage.
- `omniagent-core` can host a project engine instance without importing REPL-only code.
- Project-scoped tool execution uses the owning project's cwd.
- Project shutdown cancels active runs and frees project-owned runtime resources.

## Recommended Defaults

- Default orchestration pattern: manager-style session with specialist agents as bounded helpers.
- Default per-project ownership: one host per project path.
- Default run concurrency: many sessions per project host, one in-flight run per session.
- Default cwd: project workspace root.
- Default REPL behavior: streaming on, approvals interactive, slash commands enabled.
- Default profile on REPL startup: `explore`.

## Open Questions

- Should a project host be shared across users for the same project, or user-scoped inside the project?
- Should paused runs survive only process restarts, or also deploy/version boundaries?
- Does `bugfix` own shell execution directly, or should shell/code-edit tools remain host-controlled adapters from `omniagent-core`?
- Should specialist profiles be hardcoded in the engine, or registered by the host application as profile manifests?
- For `omniagent-core`, should project engine events ride the existing project session WS channels or a dedicated engine session channel?

## Recommended Next Step

Implement this in two slices:

1. Build the embeddable host/session/run API first, because the REPL should consume that layer rather than bypass it.
2. Add the CLI REPL second as a thin executable over the new API, then integrate the same API into `omniagent-core` through a project-engine adapter service.

That ordering avoids building a one-off test shell that has to be thrown away later.