# Idea-To-Spec And Plan Workflow — Implementation Plan

Date: 2026-04-07

## Summary

This plan implements the new idea-to-spec-to-plan workflow as a bounded extension of the current planner architecture.

The chosen path is:

1. add one new public `planner-harness` workflow command,
2. add one new engine tool wrapper over that command,
3. expose it through the existing plan profile, delegated plan-agent, and CLI approval path,
4. validate it with focused bridge and engine tests,
5. write artifacts and docs in a way that a later session can resume from `SPEC.md` and `PLAN.json` alone.

## Chosen Defaults

- Bridge command: `build-from-idea`
- Engine tool: `planner_build_from_idea`
- Default outputs: `SPEC.md`, `planner-prompt.md`, `PLAN.json`
- Maximum spec attempts: 2
- Maximum plan repair attempts: reuse current limit of 2
- Default `skip_adversary`: true
- Default tool timeout: 900000 ms
- Default overwrite policy: false

## Delivery Sequence

### Phase 0: Contract Freeze

Goal:

- lock the command name, tool name, input schema, output JSON shape, and failure contract before editing runtime code.

Files:

- `docs/idea-to-spec-and-plan-workflow-spec-2026-04-07.md`
- `omniagent-engine/src/tools/planner_tools.h`
- `planner-harness/bridge.py`

Tasks:

- define the public bridge command name `build-from-idea`.
- define the engine tool name `planner_build_from_idea`.
- define the required arguments:
  - `idea` or `idea_path`
  - `spec_output_path`
  - `prompt_output_path`
  - `plan_output_path`
  - optional `context_paths`
  - optional `overwrite`, `model`, `skip_adversary`, `timeout`
- define the success JSON shape and the failed banner contract.

Done when:

- the spec and file-level implementation targets are explicit enough that the remaining phases can proceed without reopening naming or payload design.

### Phase 1: Planner-Harness Spec Generation Workflow

Goal:

- implement the bridge-side workflow that starts from an idea and produces a validated spec before the existing plan flow runs.

Files:

- `planner-harness/bridge.py`
- `planner-harness/prompts/spec_generator.md` (new)
- `planner-harness/tests/test_bridge.py`

Tasks:

- add a new prompt template `spec_generator.md` that produces planner-compatible specs with explicit sections for summary, requirements, file structure, testing, and validation criteria.
- add bridge helpers to:
  - load idea text from inline input or file,
  - load optional `context_paths`,
  - generate `SPEC.md`,
  - build structured spec-validation feedback,
  - retry spec generation once when validation fails,
  - stop the workflow before plan generation if spec validation still fails.
- add a new `build-from-idea` parser entry to `bridge.py`.
- extend timing output so the new workflow reports:
  - spec generation time,
  - spec validation time,
  - prompt generation time,
  - plan generation time,
  - plan validation time,
  - any repair attempt timings.
- enforce overwrite and path-behavior rules at the bridge layer.

Done when:

- direct bridge invocation writes `SPEC.md`, planner prompt, and `PLAN.json` for a sample idea,
- failed spec validation blocks plan generation,
- bridge tests cover both success and bounded-failure cases.

### Phase 2: Engine Tool Wrapper

Goal:

- expose the new bridge workflow as a first-class engine tool with the same machine-readable and failure-banner behavior as the existing planner tools.

Files:

- `omniagent-engine/src/tools/planner_tools.h`
- `omniagent-engine/src/tools/planner_tools.cpp`
- `omniagent-engine/src/tools/workspace_tools.cpp`
- `omniagent-engine/tests/test_planner_tools.cpp`

Tasks:

- add `PlannerBuildFromIdeaTool` declarations and implementation.
- define the tool input schema with:
  - inline idea or idea path,
  - output paths,
  - optional context paths,
  - overwrite flag,
  - model override,
  - timeout,
  - skip_adversary.
- reuse current bridge invocation, relative-path annotation, graph validation attachment, and failed-banner formatting.
- register the new tool in `make_default_workspace_tools()`.
- set the new tool's default timeout to 900000 ms.
- add planner-tool tests for:
  - success payload shape,
  - spec-failure banner behavior,
  - relative artifact paths,
  - graph-validation integration on generated plan output.

Done when:

- the engine test suite recognizes the new tool,
- the tool returns structured JSON on success,
- the tool returns `PLANNER_BUILD_FROM_IDEA STATUS: FAILED` on spec or plan failure.

### Phase 3: Plan Profile, Delegated Agents, And CLI Approval

Goal:

- make the new tool usable in the same real workflows as the current planner tools.

Files:

- `omniagent-engine/src/project_runtime_internal.h`
- `omniagent-engine/src/agents/agent_manager.cpp`
- `omniagent-engine/src/cli/repl_internal.h`
- `omniagent-engine/tests/test_cli_repl.cpp`
- `omniagent-engine/tests/test_project_host.cpp`

Tasks:

- add `planner_build_from_idea` to the `plan` profile explicit allow list.
- add `planner_build_from_idea` to delegated Plan-agent tool filtering.
- update the delegated Plan-agent system prompt so it knows when to use the new idea-to-spec-to-plan workflow tool.
- add `planner_build_from_idea` to CLI auto-read-only planner-tool auto-approval.
- add or extend tests for:
  - plan profile visibility,
  - CLI approval policy inclusion,
  - delegated planning prompts mentioning the new tool.

Done when:

- a plan-profile session can see the tool,
- CLI auto-read-only policy does not reintroduce the earlier planner approval problem,
- delegated plan agents can call the tool without manual wrapper knowledge.

### Phase 4: Documentation And Operator Guidance

Goal:

- document the new workflow so future sessions and users can invoke it correctly without reconstructing the contract from code.

Files:

- `omniagent-engine/README.md`
- optionally `omniagent-core/FEATURES.md` if cross-module discoverability matters
- this docs directory

Tasks:

- document the new engine tool and bridge command.
- add one CLI example and one direct bridge example.
- note the overwrite behavior, default timeout, and expected artifact set.
- note that the workflow preserves `SPEC.md` and `PLAN.json` even when later stages fail.

Done when:

- a fresh session can discover the workflow name, arguments, and artifact expectations by reading repo docs alone.

## Test Plan

### Planner-Harness Tests

- Add bridge tests for successful `build-from-idea` execution using monkeypatched generation and validation helpers.
- Add bridge tests for spec-validation failure that halts before plan generation.
- Add bridge tests for timing payload aggregation and attempt summaries.

### Engine Tests

- Extend `PlannerToolsTest` with a stub bridge implementation that returns a `build-from-idea` payload.
- Add a failure-case engine test where spec validation fails and the tool returns the expected failed banner.
- Extend `CliReplInternal` approval tests so `planner_build_from_idea` is included under auto-read-only policies and excluded under prompt mode.
- Extend plan-profile visibility coverage if needed to verify the new tool appears in that profile.

### Manual Validation

- Run a direct bridge call with a medium-difficulty idea prompt.
- Run the engine tool via `omni-engine-cli run` under the `plan` profile.
- Confirm the workflow writes `SPEC.md`, `planner-prompt.md`, and `PLAN.json`.
- Confirm failure cases preserve artifact paths and report exact blockers.

## Risks

### Risk 1: Spec Quality Is Too Generic Without Enough Repo Context

Why it matters:

- the bridge operates outside the engine's richer research toolset,
- monorepo projects often need architecture-specific context to write useful specs.

Mitigation:

- require optional `context_paths`,
- auto-include conservative root-level architecture files when present,
- stop after failed spec validation instead of generating a misleading plan,
- treat engine Spec-agent composition as the P1 fallback if output quality is still weak.

### Risk 2: Existing Artifact Overwrite Surprises Users

Why it matters:

- this workflow targets canonical filenames like `SPEC.md` and `PLAN.json`.

Mitigation:

- default `overwrite=false`,
- fail before generation if outputs already exist,
- allow explicit overwrite only when requested.

### Risk 3: Runtime Latency Becomes Too High For One Tool Call

Why it matters:

- the new workflow adds a spec-generation stage on top of the existing plan-generation and repair flow.

Mitigation:

- raise the engine tool timeout to 900000 ms,
- preserve per-stage timing metrics,
- stop early on spec failure instead of wasting the full plan budget.

### Risk 4: Delegated Agents Still Use The Old Two-Step Assumption

Why it matters:

- current Plan-agent guidance assumes `SPEC.md` already exists.

Mitigation:

- update delegated Plan-agent prompt guidance,
- add tests that keep the tool visible and auto-approved in plan workflows.

## Exit Criteria

This effort is complete when all of the following are true:

- `planner-harness/bridge.py` exposes `build-from-idea` and writes all expected artifacts.
- `planner_build_from_idea` exists and is registered in default workspace tools.
- plan-profile sessions and delegated Plan agents can use the new tool.
- CLI auto-read-only mode auto-approves the new tool.
- focused bridge and engine regression tests pass.
- a sample end-to-end run from idea to `SPEC.md` and `PLAN.json` succeeds or returns exact blockers without false success.

## Recommended Next-Session Start Point

Start in this order:

1. Implement Phase 1 in `planner-harness` first.
2. Add the engine wrapper in Phase 2 immediately after the bridge contract is stable.
3. Update plan-profile visibility and CLI approval in Phase 3.
4. Run the focused bridge and engine tests before attempting another live medium-project workflow.

If only one session is available, do not split the work by file type. Split it by contract boundary:

- first finish the bridge command and its tests,
- then finish the engine wrapper and approval/profile wiring,
- then run one real idea-to-spec-to-plan sample with timing output enabled.
