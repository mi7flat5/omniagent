# Core-Integrated Project Engine Loop Implementation Plan

Date: 2026-04-08

## Summary

This plan implements the integrated architecture where `omniagent-core` embeds
project-scoped engine hosts for ad hoc repository loops and graph-setup
handoffs, while keeping `omniagent-engine` independently testable and the graph
runtime separate.

## Phase Sequence

### Phase 0: Documentation Alignment

Goal:

- Remove contradictory statements that claim core/engine integration is disallowed.
- Add explicit architecture guidance for embedded project-scoped loops.

Done when:

- Architecture docs consistently describe "independently buildable engine" plus "core embedded host/session usage".

### Phase 1: Core Adapter Surface Stabilization

Goal:

- Confirm and stabilize the host/session/run adapter contracts used by core.

Tasks:

- Define/lock project ownership and lifecycle mapping.
- Define WS/REST event translation boundaries.
- Keep REPL integration separate from core adapter code.

Done when:

- Core can create/reuse per-project engine hosts and stream run events.

### Phase 2: Workflow Prioritization (Ad Hoc Loop + Graph Setup Handoff)

Goal:

- Ensure integrated engine path supports repository-priority workflows first.

Tasks:

- Validate ad hoc repository workflows such as exploration, audit, bugfix, and
	refactor in project-scoped sessions.
- Keep idea-to-spec-to-plan tools available when the embedded loop needs to set
	up larger graph-targeted feature work.
- Validate generated artifacts are resumable (`SPEC.md`, `PLAN.json`) and
	suitable for handoff into the separate graph path.

Done when:

- Core-launched ad hoc loops work cleanly, and larger feature requests can
	produce explicit graph-setup artifacts and handoff points when needed.

### Phase 3: Permission Semantics Completion

Goal:

- Implement non-placeholder `PermissionMode::Plan` behavior.

Tasks:

- Auto-allow safe read-only tool execution under plan mode.
- Require explicit delegate approval for non-read-only operations.
- Add regression tests covering read-only auto-allow and write-path delegate checks.

Done when:

- Plan mode behavior is test-covered and no longer documented as fallback to default mode.

### Phase 4: Regression Validation

Goal:

- Confirm no architecture regressions while integrating.

Tasks:

- Run focused engine tests (permission checker, planner tools, CLI policy, project host).
- Run targeted core integration checks where adapter usage is present.

Done when:

- Focused suites pass and no doc/code contradictions remain for this architecture direction.

### Phase 5: Interaction Contract Alignment

Goal:

- Ensure integrated planner and run-control features follow a shared,
	UI-agnostic conversational turn contract suitable for future chat UI reuse.

Tasks:

- Verify clarification-required, approval-required, paused, resumed, stopped,
	and validation-failed states all map to one consistent run-turn envelope.
- Verify REPL slash commands remain convenience wrappers over the same contract,
	not separate feature paths.
- Add/update docs that define adapter expectations for chat UI and API clients.

Done when:

- All core-embedded engine interactions can be consumed without terminal-specific
	assumptions and without protocol forks per feature.

## Risks

- Mixing graph and embedded-engine event contracts can create ambiguous runtime state.
- Permission defaults can drift across CLI, delegated agents, and adapter usage if not tested together.
- Documentation can regress again unless architecture status updates accompany major refactors.

## Exit Criteria

This effort is complete when all of the following are true:

- Documentation consistently states integrated-core + standalone-engine architecture.
- Plan mode permissions are implemented and test-verified.
- Core can use project-scoped engine loops for ad hoc repository workflows
	without graph-path coupling.
- Core can use the embedded engine path to prepare spec/plan artifacts for graph
	work without collapsing graph execution into the ad hoc loop.
- Engine standalone build/test harness remains healthy.
- Interaction surfaces for planning and run control are specified as
	conversation-native and UI-agnostic.
