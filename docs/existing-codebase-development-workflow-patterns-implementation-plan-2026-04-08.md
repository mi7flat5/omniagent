# Existing-Codebase Development Workflow Patterns Implementation Plan

Date: 2026-04-08

## Summary

This plan implements first-class repository workflow patterns on top of the
existing engine runtime for the core-integrated ad hoc agent loop.

The chosen approach is:

1. extend the runtime profile manifest with the missing workflow families,
2. extend delegated agent types so the coordinator can route to those families,
3. replace blanket typed-agent bypass with workflow-appropriate permission
   defaults,
4. encode workflow-specific prompts, tool filters, and verification contracts,
5. document and test the resulting surface.

This effort is intentionally engine-first and REPL-first. It should not depend
on web or core UI work, but it must remain compatible with the current
project-scoped host/session/run architecture so later embedding work can reuse
the exact same semantics. The graph runtime stays separate: this loop handles ad
hoc repository work and may set up graph-targeted spec/plan artifacts when the
workload warrants it.

## Chosen Defaults

- Keep `coordinator` as the default top-level profile.
- Reuse `explore` for existing-codebase understanding.
- Add new runtime profiles:
  - `feature`
  - `refactor`
  - `audit`
- Add new delegated agent types:
  - `Feature`
  - `Refactor`
  - `Audit`
  - `Bugfix`
- Keep `research`, `spec`, and `plan` as supporting specialists.
- Permission defaults for delegated workflows:
  - `Explore`, `Research`, `Audit` -> `PermissionMode::Plan`
  - `Feature`, `Refactor`, `Bugfix` -> `PermissionMode::AcceptEdits`
  - keep current `Spec` and `Plan` behavior for now unless a focused follow-up
    proves a safer change does not regress planner workflows
- Keep generic network access off for `feature`, `refactor`, `audit`, and
  `bugfix`; route external research through `research`.
- Keep destructive operations off by default for `feature`, `refactor`, and
  `bugfix` profiles unless explicitly elevated later.
- Keep audit on read-only local tools only in the first implementation; do not
  add shell until a separate targeted design exists for safe read-only command
  execution.
- Do not make spec/plan mandatory for every feature or refactor request.
  Implement support for both direct ad hoc loop work and explicit escalation to
  spec/plan plus graph setup.
- Do not enable background edit-capable workers in this phase.
- Explicitly defer worktree and isolation design to a later dedicated spec.

## Delivery Sequence

### Phase 0: Contract Freeze

Goal:

- lock the workflow family names, delegated worker names, permission defaults,
  and routing model before changing runtime code.

Files:

- `docs/existing-codebase-development-workflow-patterns-spec-2026-04-08.md`
- `omniagent-engine/src/project_runtime_internal.h`
- `omniagent-engine/src/agents/agent_manager.h`
- `omniagent-engine/src/agents/agent_tool.cpp`

Tasks:

- freeze the user-visible workflow family names:
  - `explore`
  - `feature`
  - `refactor`
  - `audit`
  - `bugfix`
- freeze the new delegated worker names:
  - `Feature`
  - `Refactor`
  - `Audit`
  - `Bugfix`
- freeze the coordinator routing rules and workflow-family intent mapping.
- freeze the distinction between the ad hoc embedded loop and the separate graph
  execution path.
- freeze the initial permission defaults for each workflow family.

Done when:

- naming and surface shape are stable enough to implement without reopening the
  taxonomy.

### Phase 1: Runtime Profile Manifest Expansion

Goal:

- make the missing workflow families visible at the runtime-profile level.

Files:

- `omniagent-engine/src/project_runtime_internal.h`

Tasks:

- add `feature`, `refactor`, and `audit` to `default_profiles()`.
- review `bugfix` policy and remove default destructive access unless a specific
  regression proves it is required.
- encode workflow-appropriate tool policies for each new profile.
- keep `coordinator`, `explore`, `research`, `spec`, `plan`, and `general`
  intact unless changes are required for routing clarity.
- update coordinator system prompt guidance so it routes to the new workflow
  families rather than overusing `general`.

Done when:

- the runtime exposes all intended workflow profiles,
- profile manifests express the intended safety boundaries.

### Phase 2: Delegated Agent Type Expansion

Goal:

- let the coordinator and public agent tool reference the new workflow workers
  directly.

Files:

- `omniagent-engine/src/agents/agent_manager.h`
- `omniagent-engine/src/agents/agent_manager.cpp`
- `omniagent-engine/src/agents/agent_tool.cpp`

Tasks:

- add `Feature`, `Refactor`, `Audit`, and `Bugfix` to `enum class AgentType`.
- update the public `agent` tool string-to-type mapping.
- keep the existing `Explore`, `Research`, `Spec`, `Plan`, and
  `GeneralPurpose` mappings stable.
- ensure the agent-tool schema and help text reflect the expanded surface.

Done when:

- the coordinator can explicitly spawn the new worker families,
- direct tool callers can request them by name.

### Phase 3: Tool Filters And Permission Modes

Goal:

- align each workflow family with the correct tool access and approval posture.

Files:

- `omniagent-engine/src/agents/agent_manager.cpp`
- optionally `omniagent-engine/src/permissions/permission_checker.cpp` if a
  new read-only shell path is introduced

Tasks:

- extend `filter_tools_for_type(...)` with the new worker types.
- ensure `Audit` excludes write, edit, and shell tools in the initial phase.
- ensure `Feature` and `Refactor` include planner tools plus local edit and
  verification tools so they can either work directly or escalate into
  spec/plan and graph setup.
- ensure `Bugfix` includes edit and verification tools but not generic network
  tools, and keep spec/plan escalation routed through supporting `spec` and
  `plan` workers rather than broadening the default bugfix surface.
- replace blanket typed-agent `Bypass` defaults with workflow-specific
  permission modes.
- keep `Explore` and `Audit` on `PermissionMode::Plan` so safe read-only work is
  auto-allowed while unsafe escalation is not silently bypassed.

Done when:

- each worker family has a defensible tool filter,
- read-only workflows cannot mutate the repository,
- edit-capable workflows still have the tools needed to finish the job.

### Phase 4: Workflow Prompts And Success Contracts

Goal:

- make workflow behavior explicit in worker prompts rather than implied by the
  user request alone.

Files:

- `omniagent-engine/src/agents/agent_manager.cpp`
- `omniagent-engine/src/project_runtime_internal.h`

Tasks:

- add or update `system_prompt_for_type(...)` entries for:
  - `Feature`
  - `Refactor`
  - `Audit`
  - `Bugfix`
- tighten `Explore` wording so it clearly acts as an existing-codebase
  understanding worker.
- encode findings-first review expectations in `Audit`.
- encode invariant and regression expectations in `Refactor`.
- encode repro, root-cause, minimal-fix, and verification expectations in
  `Bugfix`.
- encode situational escalation guidance in `Feature` and `Refactor` so they can
  either stay in the ad hoc loop or route into spec/plan plus graph setup.
- update coordinator routing text in the profile manifest so it chooses the
  right worker family.

Done when:

- the behavior of each workflow family is defined by prompt contract,
- success criteria differ appropriately between audit, feature, refactor, and
  bugfix work.

### Phase 5: Documentation And Operator Guidance

Goal:

- make the workflow families discoverable to users without reading source code.

Files:

- `omniagent-engine/README.md`
- this `docs/` directory

Tasks:

- add a dedicated README section describing when to use each workflow family.
- add examples for:
  - existing-codebase exploration,
  - feature addition,
  - refactor,
  - audit,
  - bugfix.
- describe the coordinator default and how it routes specialized workers.
- document the initial limitation that background edit-capable workers remain
  out of scope until isolated parallel editing exists.

Done when:

- a new operator can discover the workflow families and pick the right one from
  docs alone.

### Phase 6: Test Coverage And Validation

Goal:

- lock the workflow-family surface with focused engine tests.

Files:

- `omniagent-engine/tests/test_agent_manager.cpp`
- optionally `omniagent-engine/tests/test_cli_repl.cpp` if profile-discovery or
  help text is updated in the CLI surface

Tasks:

- add `AgentType` parsing coverage for the new worker families.
- add tool-filter assertions for `Feature`, `Refactor`, `Audit`, and `Bugfix`.
- add permission-mode assertions for at least:
  - `Explore`
  - `Audit`
  - `Feature`
  - `Bugfix`
- add prompt-content assertions for:
  - findings-first review,
  - invariant preservation,
  - root cause and verification,
  - direct-or-escalate feature guidance.
- add coordinator-routing prompt assertions so new worker families are named in
  the system prompt.

Done when:

- focused tests catch profile/agent drift,
- the workflow-family contract is encoded in both code and tests.

## Concrete Code-Change Checklist

This section turns the phased plan into the exact edit surface expected for the
first implementation pass.

### `omniagent-engine/src/project_runtime_internal.h`

- [ ] Update the `coordinator` profile prompt in `default_profiles()` so it
  names `feature`, `refactor`, `audit`, and `bugfix` explicitly.
- [ ] Update the same coordinator prompt to state that larger feature work may
  escalate into `spec` plus `plan` and then graph setup, while graph execution
  itself remains outside this loop.
- [ ] Add `ToolCapabilityPolicy feature_policy` with:
  - `allow_write = true`
  - `allow_shell = true`
  - `allow_network = false`
  - `allow_destructive = false`
  - explicit planner-tool allowlist matching the delegated `Feature` worker
- [ ] Add `ToolCapabilityPolicy refactor_policy` with the same first-pass tool
  surface as `feature_policy`.
- [ ] Add `ToolCapabilityPolicy audit_policy` with read-only local inspection
  only and no shell, network, MCP, or write access.
- [ ] Tighten `bugfix_policy` by removing default destructive access while
  keeping local edit plus shell available.
- [ ] Add manifest entries for `feature`, `refactor`, and `audit` with
  permission defaults aligned to the chosen workflow defaults.
- [ ] Keep `general` unchanged as the escape-hatch profile for tasks that do
  not fit the specialized families.

### `omniagent-engine/src/agents/agent_manager.h`

- [ ] Extend `enum class AgentType` with `Feature`, `Refactor`, `Audit`, and
  `Bugfix`.
- [ ] Update the enum comments so each worker's intended scope is visible in the
  type declaration.
- [ ] Keep `AgentConfig` unchanged unless a narrow test seam is required for the
  new assertions.

### `omniagent-engine/src/agents/agent_tool.cpp`

- [ ] Extend `AgentTool::input_schema()` so the `type` enum includes `feature`,
  `refactor`, `audit`, and `bugfix`.
- [ ] Update `AgentTool::call(...)` so those strings map to the new
  `AgentType` values.
- [ ] Keep unknown `type` strings falling back to `GeneralPurpose` unless a
  stronger validation rule is introduced deliberately.

### `omniagent-engine/src/agents/agent_manager.cpp`

- [ ] Extend `filter_tools_for_type(...)` for `Feature`, `Refactor`, `Audit`,
  and `Bugfix`.
- [ ] `Audit` filter shape:
  - include read-only local tools
  - exclude network, MCP, `write_file`, `edit_file`, `bash`, and planner tools
- [ ] `Feature` filter shape:
  - include read-only local tools
  - include `write_file`, `edit_file`, and `bash`
  - include planner validation/build tools so the worker can escalate into
    `spec`/`plan` plus graph setup when needed
  - exclude generic network and MCP tools
- [ ] `Refactor` filter shape:
  - same first-pass tool surface as `Feature`
  - prompt contract, not tool shape, should carry the behavior-preserving
    distinction
- [ ] `Bugfix` filter shape:
  - include read-only local tools plus `write_file`, `edit_file`, and `bash`
  - exclude generic network and MCP tools
  - do not add direct planner-generation tools in the first pass; escalate via
    supporting `spec`/`plan` workers if the bugfix grows beyond ad hoc repair
- [ ] Update `profile_name_for_type(...)` so each new worker maps to the new
  runtime profile name.
- [ ] Update `permission_mode_for_type(...)` so:
  - `Explore`, `Research`, and `Audit` use `PermissionMode::Plan`
  - `Feature`, `Refactor`, and `Bugfix` use `PermissionMode::AcceptEdits`
  - `Spec` and `Plan` remain on their current path in this slice unless a
    separate hardening pass is taken on deliberately
- [ ] Extend `system_prompt_for_type(...)` with explicit contracts for:
  - `Feature`: direct implementation or escalate to `spec`/`plan` and graph
    setup when scope warrants it
  - `Refactor`: preserve behavior, state invariants, and verify regressions
  - `Audit`: findings first, no edits, no remediation unless explicitly
    requested later
  - `Bugfix`: reproduce, isolate root cause, make the smallest defensible fix,
    and verify the outcome
- [ ] Tighten the existing `Explore` prompt so it is clearly an
  existing-codebase understanding worker.
- [ ] Leave `spawn(...)` control flow intact unless the new test coverage shows
  a real need for a smaller extraction seam.

### `omniagent-engine/tests/test_agent_manager.cpp`

- [ ] Add tool-filter tests parallel to the existing explore/spec/plan/research
  cases:
  - `SpawnFeatureAgentToolFilter`
  - `SpawnRefactorAgentToolFilter`
  - `SpawnAuditAgentToolFilter`
  - `SpawnBugfixAgentToolFilter`
- [ ] In those tests, assert the expected inclusion and exclusion of:
  - `am_read_tool`
  - `am_network_tool`
  - `write_file`
  - `edit_file`
  - `bash`
  - planner tools where applicable
- [ ] Reuse the existing context-propagation pattern to confirm each new worker
  sets `ToolContext.profile` to the expected profile name.
- [ ] Add prompt-contract assertions for the new worker families. If the
  private helper boundary makes this awkward, add a narrow internal test seam
  instead of widening the public API.
- [ ] Add permission-mode assertions for the new worker families through the
  same narrow seam or equivalent session inspection hook.
- [ ] Extend existing parsing or spawn coverage so the new worker types are
  exercised end to end, not just by enum construction.

### `omniagent-engine/tests/test_cli_repl.cpp`

- [ ] Only change this file if CLI help, profile discovery, or workflow-family
  examples are surfaced directly in the REPL.
- [ ] If no REPL-visible text changes, leave this file untouched in the first
  implementation pass.

### `omniagent-engine/README.md`

- [ ] Add a workflow-family matrix describing `explore`, `feature`,
  `refactor`, `audit`, `bugfix`, `spec`, `plan`, and `research`.
- [ ] Add one short example per workflow family showing when the coordinator
  should stay in the ad hoc loop versus when it should escalate into
  `spec`/`plan` plus graph setup.
- [ ] Document that `audit` is read-only in the first pass and that worktree
  isolation is intentionally deferred.

### Intentionally Unchanged In This Slice

- [ ] Leave `planner-harness/` behavior unchanged.
- [ ] Leave run persistence, approval handling, and clarification resume logic
  unchanged.
- [ ] Leave graph execution behavior unchanged.
- [ ] Leave worktree or isolation mechanics for a separate follow-up effort.

## Test Plan

### Unit And Focused Engine Tests

- Extend `test_agent_manager.cpp` to cover new `AgentType` enum values,
  tool-filter results, permission modes, and workflow prompt contracts.
- If CLI profile help or examples are updated in code, extend
  `test_cli_repl.cpp` accordingly.

### Manual Validation

- Start the engine REPL and confirm the intended profiles are visible.
- Exercise one prompt per workflow family and confirm the expected behavior
  shape:
  - explore -> read-only summary
  - feature -> inspect, either edit directly or escalate to spec/plan and graph
    setup, then verify or hand off
  - refactor -> invariants, direct or escalated path, regression check
  - audit -> findings only
  - bugfix -> repro, fix, verify

### Regression Scope

- confirm planner tools remain usable for feature/refactor flows directly and
  remain reachable for larger bugfix escalations through supporting specialist
  workers,
- confirm existing `Explore`, `Research`, `Spec`, and `Plan` flows still work,
- confirm no accidental write path appears in `Audit`,
- confirm the embedded workflow docs still keep graph execution as a separate
  path rather than collapsing the two systems.

## Risks

### Risk 1: Too Many Workflow Families Create Routing Confusion

Why it matters:

- if the boundaries between feature, refactor, audit, and bugfix are vague, the
  coordinator may route inconsistently and users may not know which profile to
  choose.

Mitigation:

- freeze a clear intent map in Phase 0,
- encode those distinctions in prompts and docs,
- test for routing vocabulary explicitly.

### Risk 2: Feature And Refactor Workflows Drift Into The Same Behavior

Why it matters:

- both workflows edit code and may run tests, so without explicit prompt and
  policy differences they collapse into the same mode with two names.

Mitigation:

- make `Refactor` invariant-driven and behavior-preserving by default,
- make `Feature` explicitly requirement- and behavior-adding.

### Risk 3: Audit Accidentally Gains A Mutating Surface

Why it matters:

- audit loses its value if it can silently modify code or if its findings-first
  contract becomes optional.

Mitigation:

- exclude write, edit, and shell tools in the first phase,
- add prompt and tool-filter tests that fail if audit becomes mutating.

### Risk 4: Blanket Child-Agent Bypass Persists In Disguise

Why it matters:

- the current typed-agent bypass default is one of the main reasons workflow
  families are not meaningfully safer than a general worker.

Mitigation:

- explicitly change `permission_mode_for_type(...)` for the new workflow
  families,
- add tests that assert safer defaults for `Explore` and `Audit`.

### Risk 5: Workflows Promise Verification But Cannot Run Project-Specific Commands

Why it matters:

- verification is central to feature, refactor, and bugfix work, but the right
  command is project-specific.

Mitigation:

- keep the contract at the behavior level: run targeted validation or explain
  why it could not be run,
- avoid baking project-specific commands into prompts.

## Exit Criteria

This effort is complete when all of the following are true:

- the runtime exposes the intended workflow families,
- the coordinator and public agent tool can reference them directly,
- tool filters and permission modes reflect workflow intent,
- audit remains read-only and findings-first,
- feature/refactor/bugfix prompts require verification,
- feature and refactor support both direct loop work and explicit escalation to
  spec/plan plus graph setup,
- bugfix becomes a first-class delegated workflow,
- focused engine tests cover the new surface,
- README and design docs explain how to use the workflow families.