# OmniAgent Architecture And Remediation Review

Date: 2026-04-06

## Scope

This review covers the active workspace surfaces:

- `omniagent-core`: C++ backend, orchestration runtime, REST and WebSocket server, persistence, graph execution, harness validators.
- `omniagent-web`: Preact/TypeScript frontend.
- `planner-harness`: Python spec and plan quality harness.
- Supporting assets: tests, docs, bindings, and generated build state where relevant.

The goals of this document are:

1. Describe the current architecture and implemented functionality.
2. Identify dead code, stale features, and documentation drift.
3. Identify code-health and architecture problems that materially affect maintainability.
4. Provide a remediation plan that can be turned into implementation work.

## Status Update

Update applied later on 2026-04-06:

- The legacy engine path was extracted from `omniagent-core/src/engine` into the standalone top-level module `omniagent-engine`.
- `agentcore` no longer links against `omni-engine`.
- `omniagent-engine` now has its own root `CMakeLists.txt`, `vcpkg.json`, and `run_harness.sh`.
- The web app was pruned further toward the graph-first surface by removing unmounted app-shell components and related dead stores.

## Review Method

Evidence came from:

- Build manifests: `omniagent-core/CMakeLists.txt`, `omniagent-core/src/CMakeLists.txt`, `omniagent-core/test/CMakeLists.txt`, `omniagent-web/package.json`.
- Runtime entrypoints and route registration: `omniagent-core/src/server/headless_app.cpp`, `omniagent-core/src/server/rest_handlers.cpp`, `omniagent-core/src/server/project_routes.cpp`, `omniagent-core/src/server/run_routes.cpp`, `omniagent-web/src/app.tsx`, `omniagent-web/src/protocol.ts`.
- Planner harness entrypoints: `planner-harness/harness.py`, `planner-harness/pipeline.py`, `planner-harness/models.py`, `planner-harness/config.yaml`.
- Static inventory checks for source files present on disk but not compiled by the active targets.
- Verification commands:
  - `omniagent-web`: `npm run build` succeeded.
  - `planner-harness`: `pytest -q` succeeded with 27 passing tests.
  - `omniagent-core`: `ctest` from the existing build dir initially reported missing test executables; a subsequent `cmake --build build -j2` progressed but did not complete within the review window, and emitted multiple warnings.

Confidence labels used below:

- High: directly verified from current code, build manifests, or command output.
- Medium: strong evidence from structure and searches, but not fully executed end to end.
- Low: architectural hypothesis requiring follow-up validation.

## Executive Summary

The system is not one product with one coherent architecture today. It is three overlapping systems:

1. A large, actively developed C++ orchestration backend centered on project, run, graph, agent, tool, and persistence workflows.
2. A graph-first web frontend that uses only a subset of the backend surface and leaves several UI feature shells unrendered.
3. A standalone Python planner quality harness with its own runtime and testing story.

The highest-risk problem is not a single bug. It is architectural drift between:

- documented contracts,
- active backend routes,
- the frontend's actual call patterns,
- dormant feature code that still lives in the tree,
- tests that exist on disk but are not part of the active build.

The codebase still contains meaningful value, but it needs consolidation. The most important remediation is to choose and enforce a canonical product architecture around the graph-first/project-native flow, then delete or quarantine everything outside that path.

## Current Architecture

### Workspace-Level Architecture

| Component | Role | Current Status |
|---|---|---|
| `omniagent-core` | Main backend runtime and persistence system | Active, large, multi-subsystem monolith |
| `omniagent-web` | Main user-facing web UI | Active, but narrower than the backend surface |
| `planner-harness` | Offline spec/plan evaluation harness | Active and relatively healthy |
| `omniagent-core/bindings/python` | Optional Python bindings | Present, small, not part of the main review target |
| `omniagent-engine` | Standalone legacy engine module | Extracted from core, isolated with its own build and test harness |

### Backend Architecture: `omniagent-core`

The active backend architecture is centered on `HeadlessApp`, which owns process startup, server wiring, shared services, and orchestrator session lifecycle.

#### Backend Entry And Hosting Layer

Primary runtime responsibilities are split across:

- `src/server/headless_app.cpp`: application bootstrap, shared service initialization, WebSocket lifecycle, and several ad hoc REST endpoints.
- `src/server/ws_server.cpp`: HTTP and WebSocket hosting.
- `src/server/rest_handlers.cpp`: auth, admin, and system routes.
- `src/server/project_routes.cpp`: project, graph, planning, checkpoint, and agent registry routes.
- `src/server/run_routes.cpp`: run history, run drilldown, and message history routes.

This is functionally rich, but it also means routing concerns are spread across multiple files plus bootstrap code, which is one of the architecture problems discussed later.

#### Agent And Orchestration Layer

The main execution model is graph-first and agent-driven.

Major subsystems:

- `src/agent/orchestrator.cpp` and helper files: graph execution, agent spawning, completion flow, telemetry emission.
- `src/agent/base_agent.cpp`: shared agent loop and tool interaction lifecycle.
- Specialized agents: `coder_agent`, `tester_agent`, `fixer_agent`, `integration_agent`, `planner_agent`.
- `src/agent/agent_factory.cpp`: type-based agent construction.
- Context builders: `*_context_builder.cpp`, `context_prefetch.cpp`, `agent_context_builder.cpp`.

Observed active flow:

1. HTTP or WebSocket session is established.
2. Project or graph execution is initiated.
3. Orchestrator spawns agent actors.
4. Agents build context, call the LLM, execute tools, and stream state.
5. Graph and run state is persisted and exposed back to the frontend.

#### Tooling, Memory, And Validation Layer

The runtime also includes:

- `src/tools/*`: file, search, shell, git, web, memory, team, code search, graph, and delegation tools.
- `src/memory/*`: file-based memory, auto-memory, Qdrant integration, memory tools.
- `src/harness/*`: validators and post-task validation.
- `src/index/*`: code indexing and repo map generation.
- `src/persistence/*`: project and desktop persistence.
- `src/server/auth.cpp`, `user_store.cpp`, `key_vault.cpp`: user auth and storage.

#### Implemented Backend Functionality

High-confidence functionality present in the active backend:

- JWT auth and user management.
- Project CRUD and project hierarchy CRUD.
- Graph execution and resumable graph checkpoints.
- Run history and run drilldown endpoints.
- Agent drilldown snapshots and metrics streaming.
- Team and template storage.
- Tool execution with permission and ask-user flows.
- Session persistence and message persistence.
- LLM configuration hot-swap over REST.
- Emergency process-wide session kill endpoint.

### Frontend Architecture: `omniagent-web`

The frontend is a Preact + Signals app with a strongly centralized event model.

Core structure:

- `src/app.tsx`: root app and primary WebSocket message dispatcher.
- `src/hooks/useWebSocket.ts`: connection lifecycle.
- `src/stores/*`: state split across signals for auth, runtime, projects, graph, drilldown, notifications, board state, templates, and related views.
- `src/components/projects/*`: project pages and panels.
- `src/components/panels/*`: activity sidebar and drilldown UI.

The active UI path is narrower than the total component inventory. The app root renders:

- `ProjectsPage`
- `ActivitySidebar`
- `AgentDrilldown`
- `ToastContainer`

It does not currently render several global shell components that still exist in the tree.

#### Implemented Frontend Functionality

High-confidence functionality present in the active frontend:

- Login flow and token-backed auth state.
- WebSocket connection and reconnect handling.
- Project listing and project detail views.
- Graph execution monitoring and node control.
- Agent drilldown and tool event history.
- Permission and ask-user interaction UI.
- Template and team data fetching at connection time.

### Planner Harness Architecture: `planner-harness`

The planner harness is a standalone Python CLI for evaluating spec quality and plan quality.

Core structure:

- `harness.py`: CLI entrypoint.
- `pipeline.py`: orchestration for prompt generation, spec validation, plan generation, plan validation, and report generation.
- `models.py`: OpenAI-compatible client wrapper.
- `validators/*`: rubric and adversary validators.

Implemented harness functionality:

- Generate planner prompts from specs.
- Validate specs with deterministic and model-based checks.
- Generate plans from specs and prompts.
- Validate plans with deterministic and model-based checks.
- Compare prompt/model combinations.

This subsystem is the cleanest part of the workspace from a current-health standpoint.

## What Functionality Exists Today

### Product-Level Functional Inventory

| Area | Functionality | Status |
|---|---|---|
| Authentication | Login, token-backed requests, user/admin management | Implemented |
| Project Management | Project CRUD, hierarchy CRUD, plan export, auto-scaffold, scan, execute | Implemented |
| Agent Execution | Agent spawning, lifecycle streaming, tool invocation, completion and error events | Implemented |
| Graph Execution | Run creation, graph status, resumable graphs, node hold/release/retry/skip | Implemented |
| User Oversight | Permission requests, ask-user prompts, activity log, drilldown metrics | Implemented |
| Runtime Config | LLM config read and hot-swap | Implemented |
| Persistence | Unified run store, project store, desktop settings, session persistence | Implemented |
| Memory And Search | File memory, auto-memory, Qdrant-backed search hooks | Implemented with optional dependencies |
| Planner QA | Spec/plan validation harness | Implemented |
| Global UI Shell | Top nav, command palette, settings shell, status bar | Present in code but not wired into the live app root |
| Cost/KPI UI | Cost comparison and API pricing views | Present, but currently stubbed or incomplete |

## Confirmed Problems

### 1. API Contract Drift Is Severe

Confidence: High

The documented contract does not match the currently active system shape.

Verified examples:

- The frontend calls backend endpoints that are not in `API_CONTRACT.md`:
  - `/api/server-status`
  - `/api/llm-config` (GET and PUT)
  - `/api/kill-all`
  - `/api/graphs/resumable`
  - `/api/projects/:id/runs`
  - `/api/runs/:id/events`
  - `/api/agents/:id/drilldown`
- The contract still documents project session and chat/message-oriented surfaces that are no longer the primary frontend path.
- The backend route registration is split across `rest_handlers.cpp`, `project_routes.cpp`, `run_routes.cpp`, and ad hoc `headless_app.cpp` routes, which increases the chance of undocumented divergence.

Impact:

- Frontend and backend can drift independently.
- Documentation cannot be trusted as an integration contract.
- Regression risk is high because route ownership is fragmented.

### 2. The System Still Carries Old Session/Chat Shapes Beside The Graph/Run Model

Confidence: High

The active product experience is graph-first and run-centric, but session and message history surfaces still remain in the backend and contract.

Evidence:

- `run_routes.cpp` still exposes `/api/sessions/:session_id/messages`.
- `rest_handlers.cpp` still exposes `/api/sessions` as a placeholder phase endpoint.
- The contract still describes project sessions and message retrieval flows.
- The web app is centered on project pages, graphs, runs, and drilldown, not chat-centric messaging.

Impact:

- Identity and lifecycle concepts are blurred between transport session, project session, run, and graph execution.
- New work must guess which concept is canonical.

### 3. Shared Mutable Tool Context Is A Known Concurrency Debt

Confidence: High

`BaseAgent::initialize_run()` explicitly documents that agents still share a mutable `ToolRegistry` workspace pointer and can interleave `set_workspace()` calls.

Evidence:

- `src/agent/base_agent.cpp` includes a comment calling out the residual race and stating that proper remediation requires per-agent tool context instances.

Impact:

- Parallel execution safety is not fully guaranteed.
- Tool behavior can depend on agent interleaving rather than declared agent scope.

### 4. Backend Build And Test Health Is Not Trustworthy Today

Confidence: High

Observed state:

- `ctest` from the existing `omniagent-core/build` directory initially reported missing test executables.
- A subsequent build progressed for multiple minutes but did not complete within the review window.
- During that build, warnings were emitted from active code including:
  - partial initialization warnings in `src/core/types.h`
  - an unused function in `src/task/task_store.cpp`
  - an unused parameter in `src/telemetry/execution_telemetry.cpp`
  - `memset` on a non-trivial type in `src/diagnostics/ring_buffer.cpp`

Impact:

- The build directory is stale or incomplete.
- Test status cannot be treated as reliable without rebuilding from scratch in CI.
- Warning debt is already visible in core code paths.

### 5. The Frontend Root Only Uses A Subset Of The UI Codebase

Confidence: High

The root app renders only the project, sidebar, drilldown, and toast surfaces. Several global UI shell components are defined but never mounted from `src/app.tsx`.

Verified dormant or unreachable UI surfaces:

- `src/components/TopNav.tsx`
- `src/components/CommandPalette.tsx`
- `src/components/StatusBar.tsx`
- `src/components/settings/SettingsPanel.tsx`
- `src/components/settings/TeamBuilder.tsx`
- `src/components/settings/TemplateEditor.tsx`

Additional dormant state:

- `src/stores/commands.ts` is unreferenced.
- `src/stores/skills.ts` is only referenced by `SkillsEditor`, and `SkillsEditor` is reachable only through the unmounted settings shell.

Impact:

- The frontend tree contains code that looks productized but is not part of the actual app.
- Developers can easily modify unreachable code believing they are changing live behavior.

### 6. Cost And KPI Surfaces Are Stubbed Rather Than Disabled

Confidence: High

Two areas are clearly placeholders and should not remain in their current form:

- `src/stores/costEngine.ts` includes explicit comments that the telemetry store was deleted and API pricing was stubbed for later restoration.
- `src/components/projects/CostComparison.tsx` hardcodes `estimateCost()`, `formatCost()`, and `estimateApiDuration()` to zero-like values.

Impact:

- Users can be shown misleading pseudo-features.
- The code gives a false impression of capability completeness.

### 7. Dormant Backend Feature Branches Were Removed During Graph-First Cleanup

Confidence: High

A static inventory comparison originally found multiple backend implementation files on disk that were not compiled by `omniagent-core/src/CMakeLists.txt`. Those dormant branches were removed on 2026-04-06 as part of the graph-first cleanup and engine extraction.

Removed subsystems included:

- detached agent experiments such as adversarial verification, negotiation, build validation, capability probing, dream execution, personality, prompt evolution, and semantic merge
- detached memory experiments such as global pattern storage, archive/pruning/retrieval/scoring, and replay history
- detached monitoring and tracing helpers such as project heartbeat, trace storage, and the orphaned path validation implementation

Impact:

- The active core now matches the graph-first product surface more closely.
- Remaining documentation drift should be treated separately from dead-file cleanup.

### 8. Orphaned Tests Were Removed To Match The Active CMake Test Surface

Confidence: High

The corresponding orphaned tests that existed on disk without being listed in the active aggregate test target were also removed on 2026-04-06.

Impact:

- The test tree now reflects the test surface the build actually discovers.
- Split CI remains necessary so the extracted engine and graph-first core stay independently verifiable.

### 9. Documentation Drift Is Material, Not Cosmetic

Confidence: High

Verified examples:

- `omniagent-core/docs/feature_matrix.md` describes `src/market/*` and `src/indicators/*` modules that do not exist in the current repo.
- `omniagent-core/FEATURES.md` states strong product-level claims such as all tools wired and all skills working, but the web app contains whole settings and command surfaces that are not wired into the live root.
- The API contract is out of sync with both the backend route inventory and the frontend call graph.

Impact:

- Docs are currently an unreliable source of truth.
- Planning and onboarding are slowed by conflicting descriptions of the product.

### 10. Planner Harness Is Healthy But Operationally Brittle

Confidence: High

The planner harness passed its tests, but several operational weaknesses remain:

- `planner-harness/config.yaml` hardcodes remote IP-based model endpoints.
- `planner-harness/pipeline.py` parses generated JSON without guardrails in `generate_plan()`.
- `planner-harness/models.py` does not configure request timeouts.

Impact:

- The harness is testable in isolation but environment-sensitive in practice.
- Failures from malformed model output or unreachable endpoints will be abrupt.

## Dead Code And Drift Inventory

### High-Confidence Dead Or Orphaned Frontend Code

| Path | Why It Is Considered Dead Or Orphaned |
|---|---|
| `omniagent-web/src/components/chat/` | Empty directory from removed chat UI |
| `omniagent-web/src/components/TopNav.tsx` | Defined but never mounted from app root |
| `omniagent-web/src/components/CommandPalette.tsx` | Defined but never mounted from app root |
| `omniagent-web/src/components/StatusBar.tsx` | Defined but never mounted from app root |
| `omniagent-web/src/components/settings/SettingsPanel.tsx` | Defined but never mounted from app root |
| `omniagent-web/src/stores/commands.ts` | No live imports found |
| `omniagent-web/src/components/projects/CostComparison.tsx` | Stubbed calculations, not a truthful feature |
| `omniagent-web/src/stores/costEngine.ts` | Contains explicit restoration stubs and deleted-backend comments |

### High-Confidence Dormant Backend Code

These files exist in the source tree but are not included in the active `agentcore` build target.

| Area | Examples |
|---|---|
| Agent feature branches | `adversarial_verifier`, `agent_negotiation`, `build_validator`, `capability_probe`, `dream_engine`, `personality`, `prompt_evolution`, `semantic_merge` |
| Memory feature branches | `global_pattern_store`, `memory_archive`, `memory_pruner`, `memory_retriever`, `memory_scorer` |
| Monitoring and replay | `project_heartbeat`, `replay_store`, `trace_store` |
| Utilities | `path_validation` |

### Likely Legacy Subsystem

Confidence: Medium

`omniagent-core/src/engine` builds as a separate `omni-engine` sublibrary. Relative to the active `HeadlessApp` plus orchestrator/runtime architecture, it appears to be a legacy or alternate engine path rather than the primary product runtime.

This subsystem should be explicitly classified as one of:

- retained and actively supported,
- experimental and isolated,
- deprecated and scheduled for removal.

Leaving it in its current ambiguous state increases architecture confusion.

## Build And Validation Snapshot

### What Passed

- Frontend production build: passed.
- Planner harness tests: 27 passed.
- Workspace diagnostics query: no editor-reported errors were surfaced in the sampled folders.

### What Failed Or Remains Inconclusive

- Backend `ctest` from the current build directory failed initially because test executables were missing.
- Backend rebuild did not finish within the review window, so no trustworthy final pass/fail result exists for the C++ build from this review alone.

### Warnings Observed During Backend Build

High-confidence warnings observed in active code:

- repeated partial aggregate initialization warnings in `src/core/types.h`
- unused function warning in `src/task/task_store.cpp`
- unused parameter warning in `src/telemetry/execution_telemetry.cpp`
- unsafe `memset` on non-trivial `LogEntry` in `src/diagnostics/ring_buffer.cpp`

These are not the main architecture problem, but they are evidence that baseline build hygiene needs work.

## Remediation Plan

### Phase 1: Choose The Canonical Product Architecture

Goal: remove ambiguity about what the product is.

Actions:

1. Declare the graph-first, project-native flow as canonical if that is the intended direction.
2. Define one canonical identity model for transport session, project session, run, and graph execution.
3. Publish one source-of-truth API contract generated from real route registration or maintained next to route definitions.
4. Classify each subsystem as active, experimental, deprecated, or removed.

Deliverables:

- updated architecture doc
- updated API contract
- subsystem status matrix

### Phase 2: Remove Or Quarantine Dead Frontend Surfaces

Goal: make the frontend tree reflect the real app.

Actions:

1. Delete unmounted global shell components unless they are scheduled for immediate reintegration.
2. Delete the empty chat directory.
3. Remove `commands.ts` if the command palette is not part of the roadmap.
4. Either wire settings and skills into the live app root or move them to an explicit experimental area.
5. Remove stubbed cost/KPI code or gate it behind a clearly disabled feature flag.

Deliverables:

- leaner frontend tree
- fewer misleading pseudo-features
- clearer live UI ownership

### Phase 3: Remove Or Reintegrate Dormant Backend Feature Branches

Goal: stop carrying backend code that is not part of the shipped build.

Actions:

1. Audit every source file present on disk but absent from `src/CMakeLists.txt`.
2. For each dormant subsystem, choose one action:
   - integrate into active build and tests,
   - move to an experimental directory with explicit status,
   - delete.
3. Do the same for the associated tests in `omniagent-core/test`.

Priority candidates:

- agent personality and prompt evolution branch
- dream engine and negotiation branch
- replay, tracing, and heartbeat branch
- memory pruning/archive branch

Deliverables:

- source tree matches build graph
- test tree matches active features

### Phase 4: Fix Architectural Concurrency And Ownership Problems

Goal: remove unsafe shared mutable runtime context.

Actions:

1. Replace `ToolRegistry::set_workspace()` shared mutation with per-agent tool context.
2. Make route ownership explicit so `HeadlessApp` is bootstrap-only rather than a mixed bootstrap-plus-route container.
3. Reduce the number of conceptually overlapping persistence and execution surfaces.

Deliverables:

- safer parallel execution
- cleaner module boundaries
- simpler reasoning about request handling

### Phase 5: Rebuild Build/Test Trust

Goal: make it possible to answer "is the system healthy?" with one command.

Actions:

1. Create a clean CI path for `omniagent-core` that builds from scratch and runs only active tests.
2. Fail CI on stale route contract drift if feasible.
3. Add an inventory check that fails when source files exist under active directories but are not accounted for by the build.
4. Promote warning cleanup in active code, especially `types.h`, `ring_buffer.cpp`, and other core files surfaced by the build.

Deliverables:

- trustworthy backend build status
- trustworthy active test inventory
- reduced warning debt

### Phase 6: Repair Documentation

Goal: make docs usable again for planning and onboarding.

Actions:

1. Delete or archive docs that describe non-existent modules.
2. Regenerate the feature guide from active product surfaces, not aspirational ones.
3. Keep frontend and backend API contract copies synchronized from one source.

Deliverables:

- documentation aligned with shipped behavior
- reduced onboarding and planning friction

## Suggested Work Breakdown For Remediation

If this is turned into implementation work, the work should be split into these streams:

1. Architecture cleanup stream: canonical model, subsystem status matrix, API contract repair.
2. Frontend pruning stream: remove dead UI, settings shells, and KPI stubs.
3. Backend pruning stream: remove or reintegrate uncompiled sources and their tests.
4. Reliability stream: rebuild CI, clean test registration, and warning cleanup.
5. Documentation stream: replace stale docs with generated or actively maintained docs.

## Bottom Line

The repository contains a substantial amount of real, working functionality, especially in the backend orchestration runtime and the planner harness. The main problem is not absence of functionality. It is excess of partially retained functionality.

The codebase needs contraction before it needs expansion.

The fastest route to a healthier system is:

1. declare the canonical runtime model,
2. delete or quarantine everything outside it,
3. make build, test, and docs reflect that same boundary.