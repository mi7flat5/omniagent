# Existing-Codebase Development Workflow Patterns Spec

Date: 2026-04-08

## Summary

This spec defines first-class workflow patterns for working in existing codebases
inside `omniagent-engine` and, when embedded, inside the core-integrated ad hoc
agent loop.

The target workflow families are:

- existing-codebase exploration,
- feature addition,
- refactoring,
- code audit,
- bug fixing.

The goal is to stop relying on one coarse "general coder" behavior for all
repository tasks. Instead, the engine should expose explicit profile and
delegated-worker patterns with distinct tool access, prompt guidance, approval
defaults, and completion criteria.

These workflow patterns must work through the existing host/session/run model,
must preserve the current clarification and pause/resume contract, and must stay
UI-agnostic so the same semantics can later surface through REPL, core-backed
API clients, and future chat UI adapters. They also must remain distinct from
the graph runtime, which is reserved for well-defined speced and planned tasks.

## What The User Is Asking For

Core intent:

- make repository work feel deliberate and mode-aware,
- support the main kinds of real software maintenance work in existing repos,
- keep iteration REPL-first while defining a reusable engine-level contract,
- use the embedded engine loop for ad hoc repository work while keeping graph
  execution separate.

Explicit requirements:

- spec out patterns for working in existing codebases,
- spec out patterns for feature addition,
- spec out patterns for refactoring,
- spec out patterns for code audit,
- spec out patterns for bug fixing,
- produce a full implementation plan rather than ad hoc prompt suggestions.

Implicit requirements:

- reuse the current engine profile and delegated-agent architecture where it is
  already aligned,
- add missing workflow families where the current runtime is too coarse,
- preserve planner-harness as the spec/plan contract boundary for complex work,
- allow feature and refactor workflows to either stay in the ad hoc loop or
  escalate into spec/plan and graph-setup work when warranted,
- keep graph execution separate; the embedded loop may prepare work for graphs
  but should not absorb the graph executor role,
- treat worktree or isolation design as out of scope for this effort,
- keep workflow semantics transport-agnostic and independent from REPL-only
  slash commands,
- keep repository work safe by aligning tool access and permission modes with
  the actual workflow intent.

## Prior Art Research

Sources consulted:

- [Aider chat modes](https://aider.chat/docs/usage/modes.html) - separates
  `ask`, `code`, and `architect`, and explicitly recommends planning before
  editing.
- [Aider repository map](https://aider.chat/docs/repomap.html) - emphasizes
  whole-repo structure mapping for existing codebase work.
- [Aider linting and testing](https://aider.chat/docs/usage/lint-test.html) -
  treats automatic lint/test/build verification as a normal post-edit loop.
- [Aider README](https://github.com/Aider-AI/aider) - positions the tool for
  both new projects and existing codebases, with repo mapping and edit
  ergonomics as core product surface.
- [Claude Code common workflows](https://code.claude.com/docs/en/common-workflows)
  - breaks out codebase understanding, bug fixing, refactoring, tests,
  specialized subagents, plan mode, session resume, and worktree isolation.
- [Claude Code subagents](https://code.claude.com/docs/en/sub-agents) -
  provides built-in `Explore` and `Plan`, example `code-reviewer` and
  `debugger` agents, tool restrictions, memory, background tasks, and worktree
  isolation.
- [Roo Code modes](https://docs.roocode.com/basic-usage/using-modes) - exposes
  distinct `Ask`, `Architect`, `Debug`, `Code`, and `Orchestrator` modes with
  explicit tool-group restrictions.
- [OpenHands CLI terminal mode](https://docs.openhands.dev/openhands/usage/run-openhands/cli-mode)
  - emphasizes direct task entry, confirmation modes, pause/resume, and CLI
  workflow continuity.
- [OpenHands README](https://github.com/OpenHands/OpenHands) - reinforces the
  split between CLI, GUI, and SDK surfaces while keeping a familiar coding-agent
  interaction model.

### What Prior Art Suggests

Common patterns across mature coding-agent tools:

1. Existing-codebase understanding is treated as a distinct read-only workflow,
   not just the first step of coding mode.
2. Planning or architecture work is separated from implementation.
3. Debugging and code review are specialized workflows with different success
   criteria from feature work.
4. Verification after edits is table stakes; lint/test/build loops are not
   optional polish.
5. Orchestration matters. Mature tools either offer an explicit orchestrator or
   strongly encourage chaining specialized workers.
6. Safe read-only analysis and resumable interruption states are expected,
   especially for longer interactive runs.
7. Parallel work benefits from isolation. When tools support background workers,
   they either restrict clarifying questions or use worktree-style isolation to
   avoid collisions.

### Comparison Matrix

| Capability | Aider | Claude Code | Roo Code | OpenHands CLI | This Project Target |
|---|---|---|---|---|---|
| Read-only existing-codebase exploration mode | Yes | Yes | Yes | Partial | P0 |
| Separate planning / architect workflow | Yes | Yes | Yes | Partial | P0 |
| Dedicated feature implementation workflow | Yes | Partial | Yes | Partial | P0 |
| Dedicated debug / bugfix workflow | Partial | Yes | Yes | Partial | P0 |
| Dedicated read-only audit / review workflow | Partial | Yes | Partial | Partial | P0 |
| Automatic lint / test / build verification loop | Yes | Yes | Partial | Partial | P0 |
| Explicit orchestration across specialists | Partial | Yes | Yes | Partial | P1 |
| Resumable pause / clarification flow | Indirect | Yes | Partial | Yes | P1 |
| Parallel isolation / worktrees | Indirect | Yes | Partial | Partial | P2 |
| Project-specific custom workflow definitions | Partial | Yes | Yes | SDK-based | P1 |

### Gaps Surfaced By Prior Art

Relative to the current `omniagent-engine` implementation:

- the runtime already has `coordinator`, `explore`, `spec`, `plan`, `research`,
  `bugfix`, and `general` profiles, but it does not have first-class `feature`,
  `refactor`, or `audit` profiles,
- delegated agent types only cover `Explore`, `Research`, `Spec`, `Plan`, and
  `GeneralPurpose`, so the existing `bugfix` profile has no matching child-agent
  type,
- current worker prompts are planner-centric and do not define a findings-first
  audit workflow, an invariant-driven refactor workflow, or a root-cause-first
  bugfix workflow,
- child-agent permission behavior is currently too coarse because typed agents
  default to `PermissionMode::Bypass`, which does not align with safer review
  and repository-maintenance workflows,
- verification expectations are not consistently encoded per workflow family,
- there is no explicit coordinator routing contract that says when to use
  exploration, feature, refactor, audit, or bugfix workers.

## User Stories

- As a user entering an unfamiliar repository, I want a read-only exploration
  workflow so I can understand structure, ownership, and execution flow before
  asking for code changes.
- As a user adding a feature to an existing repo, I want the agent to inspect
  current patterns, ask clarifying questions when necessary, plan complex work,
  and verify the final change.
- As a user requesting a refactor, I want the agent to preserve behavior unless
  I explicitly ask for a semantic change, and I want the behavior-preservation
  evidence called out.
- As a user asking for a code audit, I want findings first, ordered by severity,
  without the agent silently editing code.
- As a user asking for a bug fix, I want the agent to capture the repro, isolate
  the root cause, make the minimal fix, and rerun verification.
- As a maintainer, I want workflow families to map onto engine profiles,
  delegated workers, and session/run semantics rather than living only in long
  coordinator prompts.

## Recommended Defaults

- Keep `coordinator` as the default top-level interactive profile.
- Reuse `explore` as the existing-codebase understanding workflow rather than
  creating a second redundant codebase-analysis profile.
- Keep `research`, `spec`, and `plan` as supporting specialist workflows.
- Add three new first-class workflow profiles:
  - `feature`
  - `refactor`
  - `audit`
- Promote bug fixing to a first-class delegated workflow by adding a `Bugfix`
  agent type instead of relying on a profile-only concept.
- Add the following delegated worker types:
  - `Feature`
  - `Refactor`
  - `Audit`
  - `Bugfix`
- Keep generic network access off by default for `feature`, `refactor`,
  `audit`, and `bugfix`; if external research is required, the coordinator
  should route that subtask to `research`.
- Use the following permission defaults for delegated workflows:
  - `Explore`, `Research`, `Audit`: `PermissionMode::Plan`
  - `Feature`, `Refactor`, `Bugfix`: `PermissionMode::AcceptEdits`
  - `Spec`, `Plan`: keep current explicit-tool behavior, but do not rely on
    blanket child-agent bypass for all future workflow families
- Treat `feature` and `refactor` as dual-path workflows. Some requests should be
  handled directly inside the ad hoc loop, while larger or cross-cutting work
  may escalate into spec/plan artifacts and new graph setup when the workload
  warrants it.
- Keep audit read-only by default. If the user wants fixes after an audit, the
  coordinator should explicitly chain to `bugfix` or `refactor` rather than
  letting the audit workflow mutate the workspace.
- Keep audit on read-only local tools only in the first revision. Do not add
  shell access until a separate targeted design exists for safe read-only
  command execution.
- Require every edit-capable workflow to either run a verification step or
  explicitly explain why verification could not be run.
- Keep worktree and isolated parallel editing out of scope for this effort.
  Background edit workflows remain deferred to a separate targeted spec and
  implementation pass.

## Workflow Definitions

### Existing-Codebase Exploration

Profile / worker:

- session profile: `explore`
- delegated agent type: `Explore`

Purpose:

- understand repository structure,
- locate relevant files,
- trace execution flow,
- summarize architecture and conventions,
- prepare for a later feature, refactor, audit, or bugfix task.

Tool policy:

- read-only local tools only,
- no file writes,
- no generic network access,
- no shell required by default.

Required behavior:

- start broad, then narrow to the subsystem the task actually touches,
- identify entry points, key data models, ownership boundaries, and validation
  surfaces,
- return concise findings with concrete file references,
- avoid prematurely proposing code changes unless the user asks for a next step.

Completion criteria:

- the user can identify the relevant files, flow, and likely next workflow,
- no workspace mutation occurs.

### Feature Addition

Profile / worker:

- session profile: `feature`
- delegated agent type: `Feature`

Purpose:

- add new behavior to an existing repository while respecting local patterns,
  tests, and architecture,
- support both direct implementation of simpler work and escalation into
  spec/plan plus new graph setup when the workload warrants it.

Tool policy:

- read and write local tools,
- shell access for verification,
- planner tools for optional escalation into spec/plan and graph-setup work,
- no generic network tools by default,
- destructive operations off by default.

Required behavior:

1. inspect the existing code paths, surrounding abstractions, and test style,
2. clarify missing requirements when the request is ambiguous,
3. decide whether the request should stay in the ad hoc loop or escalate into
  spec/plan and new graph setup based on scope, ambiguity, and user intent,
4. implement incrementally rather than in one opaque rewrite when staying in
  the ad hoc loop,
5. run targeted validation before finishing,
6. summarize changed files, verification, residual risk, and any graph-setup
  handoff artifacts created.

Completion criteria:

- the requested feature is implemented,
- relevant docs/tests are updated where needed,
- at least one validation action is run or an explicit reason is given.

### Refactor

Profile / worker:

- session profile: `refactor`
- delegated agent type: `Refactor`

Purpose:

- improve structure, readability, maintainability, or modularity while
  preserving behavior unless the user explicitly requests semantic change.

Tool policy:

- same baseline tool access as `feature`,
- stronger behavioral constraints in prompts and completion rules,
- destructive operations off by default.

Required behavior:

1. identify the behavior that must remain invariant,
2. state the invariants or regression checks before making sweeping edits,
3. support both direct refactors and escalation into spec/plan when the scope,
   risk, or user preference warrants it,
4. prefer smaller, testable increments,
5. rerun regression checks before reporting completion,
6. if behavior must change, state that explicitly or route to `feature`.

Completion criteria:

- structure improves without unacknowledged behavior change,
- invariants or regression checks are named,
- verification is reported.

### Code Audit

Profile / worker:

- session profile: `audit`
- delegated agent type: `Audit`

Purpose:

- review code or recent changes for correctness, security, maintainability,
  performance, and regression risk without changing the repository.

Tool policy:

- read-only local tools only,
- no edit or write tools,
- no generic network tools by default,
- no shell by default in the initial implementation.

Required behavior:

- inspect the named scope or changed files,
- produce findings-first output ordered by severity,
- include evidence and affected files for each finding,
- state explicitly when no findings are present,
- do not make code changes unless the user explicitly requests a workflow
  switch after the audit.

Completion criteria:

- the audit output is actionable,
- findings are ordered by severity,
- no silent edits occur.

### Bug Fix

Profile / worker:

- session profile: `bugfix`
- delegated agent type: `Bugfix`

Purpose:

- fix failing behavior with the smallest defensible change and a clear root
  cause statement.

Tool policy:

- read and write local tools,
- shell access for repro and verification,
- planner tools available when the fix requires broader plan-first work,
- generic network access off by default,
- destructive operations off by default unless explicitly approved.

Required behavior:

1. capture the repro, failing command, or observable error,
2. isolate the failing code path,
3. state the root cause,
4. implement the minimal change that addresses the cause,
5. rerun the repro or targeted tests,
6. summarize the fix, evidence, and remaining risk.

Completion criteria:

- the failure no longer reproduces or verification demonstrates the fix,
- the root cause is stated explicitly,
- the workflow does not stop at a symptom patch without evidence.

## Requirements

### Must Have (P0)

- Define workflow-family semantics for:
  - `explore`
  - `feature`
  - `refactor`
  - `audit`
  - `bugfix`
- Add `Feature`, `Refactor`, `Audit`, and `Bugfix` to the engine delegated
  worker surface.
- Add `feature`, `refactor`, and `audit` to the runtime profile manifest list.
- Keep naming consistent across:
  - runtime profiles,
  - delegated worker types,
  - user-visible documentation.
- Update coordinator routing guidance so it knows when to use exploration,
  feature, refactor, audit, and bugfix workers.
- Update delegated worker prompts so each workflow has explicit success
  criteria rather than generic "complete the task" behavior.
- Remove the assumption that all typed child agents should run with blanket
  `PermissionMode::Bypass`.
- Ensure `Explore` and `Audit` cannot mutate the workspace.
- Ensure `Audit` is findings-first and read-only.
- Ensure `Feature`, `Refactor`, and `Bugfix` require verification or an explicit
  limitation statement before success is reported.
- Ensure `Refactor` prompts emphasize invariant preservation.
- Ensure `Bugfix` prompts emphasize reproduction, root cause, minimal fix, and
  verification.
- Ensure `Feature` and `Refactor` can use the existing spec/planning workflow
  for non-trivial tasks.
- Keep clarification-required, approval-required, paused, resumed, failed, and
  stopped states on the same run/session contract already established for the
  engine.
- Update engine docs so users can intentionally pick the right workflow family.

### Should Have (P1)

- Add optional structured report artifacts for audit output.
- Add explicit coordinator heuristics for when feature/refactor work should stay
  in the ad hoc loop versus produce `SPEC.md`, `PLAN.json`, and new graph
  setup artifacts.
- Add project-memory guidance per workflow family once the public memory wiring
  is exposed through normal session creation.

### Nice To Have (P2)

- Add automatic workflow suggestions when the user starts from a vague prompt.
- Add scheduled or CI-friendly audit workflows.
- Add more granular review subtypes such as security-audit or performance-audit
  after the base audit workflow is stable.

## Interactions

### Direct Profile Selection

Users should be able to intentionally switch workflows at the session level,
for example through REPL profile selection or a future API/profile field.

Expected direct profile set:

- `coordinator`
- `explore`
- `feature`
- `refactor`
- `audit`
- `bugfix`
- `research`
- `spec`
- `plan`
- `general`

### Coordinator Routing

The default coordinator should route by task intent:

- repo understanding -> `Explore`
- external prior-art or documentation gathering -> `Research`
- feature work -> `Feature`, which may either implement directly or escalate to
  `Spec` then `Plan` and new graph setup when the workload warrants it
- behavior-preserving structural cleanup -> `Refactor`
- review / findings / security scan -> `Audit`
- failing tests or broken behavior -> `Bugfix`

### Workflow Chaining

Expected chain examples:

- `Explore` -> `Feature`
- `Explore` -> `Refactor`
- `Audit` -> `Bugfix`
- `Feature` -> `Spec` -> `Plan` -> graph setup / handoff
- `Feature` -> planner clarification flow -> direct implementation -> verification
- `Bugfix` -> `Refactor` when the underlying fix requires structural cleanup

### Clarification And Run Control

These workflow families must not introduce new protocol forks.

- Clarification-required outcomes use the same paused-run contract already used
  by planner clarification.
- Approval-required outcomes continue to use the existing run pause and resume
  flow.
- Session resume, run inspection, and later API transport mappings must not need
  workflow-specific state machines.

## Edge Cases

- Small one-file features may not need full spec/plan artifacts, but the agent
  must still inspect local patterns before editing.
- A refactor request that clearly changes behavior should either ask for
  confirmation or route to `feature`.
- An audit with no findings must say so explicitly instead of returning a vague
  summary.
- A bugfix request without a repro, failing test, or observable symptom must ask
  clarifying questions before editing.
- A monorepo task with multiple plausible subsystems should begin with focused
  exploration rather than guessing the target directory.
- Existing failing tests unrelated to the requested bugfix must be called out as
  residual risk, not silently "fixed" unless the user asked for that.
- A larger feature request may be best handled by generating spec/plan artifacts
  and setting up new graph work rather than fully executing inside the ad hoc
  loop.
- Background workers that require clarification should remain out of scope until
  isolated parallel editing exists.
- Audit work that requires external security guidance should route through
  `research` rather than enabling broad network access in the audit worker.

## Architecture Notes

### Modules And Layers Affected

- `omniagent-engine/src/project_runtime_internal.h`
- `omniagent-engine/src/agents/agent_manager.h`
- `omniagent-engine/src/agents/agent_manager.cpp`
- `omniagent-engine/src/agents/agent_tool.cpp`
- `omniagent-engine/tests/test_agent_manager.cpp`
- `omniagent-engine/README.md`
- this `docs/` directory

### Architectural Boundaries To Respect

- Keep workflow semantics inside the engine profile and delegated-worker model,
  not inside REPL-only command handling.
- Reuse `planner-harness` for spec/plan and clarification behavior rather than
  inventing a second planning stack.
- Keep graph execution as a separate runtime path that consumes well-defined
  speced and planned tasks; the embedded agent loop handles ad hoc repository
  work and graph-setup handoffs rather than replacing the graph runtime.
- Keep the coordinator as the orchestrator; avoid nested workflow logic spread
  across many unrelated prompts.
- Keep workflow behavior UI-agnostic so the same semantics can be embedded in
  `omniagent-core` later without re-specifying each workflow.
- Preserve engine standalone build/testability while evolving the runtime
  profile and delegated-agent surface.

### Files Likely Affected

- `omniagent-engine/src/project_runtime_internal.h`
- `omniagent-engine/src/agents/agent_manager.h`
- `omniagent-engine/src/agents/agent_manager.cpp`
- `omniagent-engine/src/agents/agent_tool.cpp`
- `omniagent-engine/tests/test_agent_manager.cpp`
- `omniagent-engine/README.md`

## Validation Criteria

The following assertions must pass before this feature is considered complete.

### Concrete Assertions

- `default_profiles()` exposes `feature`, `refactor`, and `audit` in addition to
  the existing workflow and support profiles.
- `AgentType` includes `Feature`, `Refactor`, `Audit`, and `Bugfix`.
- The public `agent` tool surface accepts worker type strings for:
  - `feature`
  - `refactor`
  - `audit`
  - `bugfix`
- `filter_tools_for_type(AgentType::Audit)` excludes write, edit, and shell
  tools.
- `filter_tools_for_type(AgentType::Feature)` and
  `filter_tools_for_type(AgentType::Refactor)` include planner tools plus the
  local edit and verification surfaces needed for implementation.
- `filter_tools_for_type(AgentType::Bugfix)` includes edit and verification
  tools and excludes generic network access by default.
- `permission_mode_for_type(AgentType::Explore)` and
  `permission_mode_for_type(AgentType::Audit)` are not `PermissionMode::Bypass`.
- `system_prompt_for_type(AgentType::Audit)` requires findings-first,
  severity-ordered review output and explicitly forbids silent edits.
- `system_prompt_for_type(AgentType::Refactor)` requires behavior-preservation
  checks or explicit confirmation of semantic change.
- `system_prompt_for_type(AgentType::Bugfix)` requires reproduction, root cause,
  minimal fix, and verification.

### Golden Dataset

Use at least one representative prompt per workflow family.

- Explore prompt: `Give me an overview of this repo and trace the login flow.`
  - Expected behavior:
    - routes to read-only exploration,
    - returns file paths and architecture summary,
    - does not modify files.
- Feature prompt: `Add tenant-scoped webhook retry history to the existing API.`
  - Expected behavior:
    - inspects current patterns,
    - clarifies ambiguous requirements if necessary,
    - either implements directly or escalates to spec/plan and graph setup when
      the change is large enough,
    - reports verification.
- Refactor prompt: `Refactor auth utility code to modern C++ while preserving behavior.`
  - Expected behavior:
    - states invariants or regression checks,
    - performs structural edits,
    - reports behavior-preservation evidence.
- Audit prompt: `Review the recent auth changes for security issues.`
  - Expected behavior:
    - findings-first output,
    - severity ordering,
    - no workspace edits.
- Bugfix prompt: `Fix the failing clarification-resume flow in the engine.`
  - Expected behavior:
    - captures the repro or failing test,
    - states root cause,
    - applies minimal fix,
    - reruns verification.

### Invariants

- `Explore` never writes to the workspace.
- `Audit` never writes to the workspace.
- `Audit` never reports fixes as completed unless the user explicitly switched
  to a mutating workflow.
- `Feature`, `Refactor`, and `Bugfix` do not report success without a
  verification step or an explicit explanation of why validation could not be
  run.
- `Refactor` does not claim preserved behavior without identifying invariants,
  regression checks, or equivalent evidence.
- `Bugfix` does not stop at a symptom patch without stating the root cause.
- Clarification and approval pauses remain represented through the same
  session/run contract across all workflow families.

### Integration Checks

- Profile names, delegated worker names, and documentation examples use the same
  vocabulary.
- Coordinator routing guidance references the new workflow families.
- Direct profile sessions, delegated workers, and future API clients all use the
  same pause/resume and clarification semantics.
- Existing planner workflow behavior remains available to `feature`, `refactor`,
  and `bugfix` flows when needed.
- The embedded engine loop can prepare graph work, but graph execution remains a
  separate runtime path with separate responsibilities.

## Open Questions

- None for this revision. Audit is intentionally read-only without shell,
  feature/refactor escalation to spec/plan is intentionally situational, and
  worktree/isolation design is intentionally deferred to a separate effort.