# Spec And Plan Clarification Q&A Gate Implementation Plan

Date: 2026-04-07

## Summary

This plan adds a default-on clarification gate to spec/planning workflows while
preserving current functionality and machine-readable contracts.

The delivery strategy is incremental:

1. add bridge-level clarification payloads and gating,
2. wire engine planner tool wrappers and status mapping,
3. add REPL question/answer controls,
4. switch defaults to required mode,
5. verify with focused regression suites.

Interaction quality objective:

- clarification turns must feel like normal conversation turns,
- sequential and batch answering must both be first-class,
- one-sentence multi-answer and "you decide for me" must be supported without
  clunky mode switching.

## Phase 0: Contract Freeze

Goal:

- lock JSON schema, status names, and mode flags before behavior changes.

Files:

- `docs/spec-and-plan-clarification-qa-spec-2026-04-07.md`
- `planner-harness/bridge.py`
- `omniagent-engine/src/tools/planner_tools.cpp`

Tasks:

- freeze `clarification_mode` values: `required`, `assume`, `off`.
- freeze output fields: `clarification_required`, `clarifications`,
  `workflow_passed`.
- freeze question schema fields and ID format.
- define clarification-required banner contract for engine planner tools.
- freeze turn-native request/answer envelopes that are UI-agnostic.
- freeze answer interpretation rules for:
  - by-id answers,
  - single-sentence multi-answer mapping,
  - delegation phrase handling ("you decide for me").

Done when:

- all downstream phases can implement without renaming contracts.

## Phase 1: Bridge Clarification Extraction And Gate

Goal:

- derive structured clarification questions from existing validation outputs and
  enforce gating in required mode.

Files:

- `planner-harness/bridge.py`
- `planner-harness/scoring.py`
- `planner-harness/tests/test_bridge.py`

Tasks:

- add normalization function that converts rubric/adversary blockers into
  clarification question objects.
- add dedupe logic and stable ID assignment.
- add `clarification_mode` argument handling to `build-from-idea` and run
  workflows.
- in required mode, stop progression when unresolved blocking clarifications
  exist.
- add support for optional `answers_json` input and answer validation.
- inject resolved answers into next generation/repair prompts.
- add an answer interpreter that supports:
  - strict by-id structured answers,
  - conversational freeform answer turns,
  - one-sentence multi-answer parsing.
- add confidence-based disambiguation fallback when freeform mapping is
  uncertain.
- add delegation handling so unresolved questions can accept recommended
  defaults when user explicitly delegates.

Done when:

- bridge returns clarification-required payloads for ambiguous inputs,
- reruns with answers resume progression,
- existing success path remains unchanged for clear inputs.

## Phase 2: Engine Planner Tool Wrapper Integration

Goal:

- propagate clarification contracts through engine planner tools and preserve
  existing failure-banner behavior.

Files:

- `omniagent-engine/src/tools/planner_tools.cpp`
- `omniagent-engine/tests/test_planner_tools.cpp`

Tasks:

- add pass-through inputs for `clarification_mode`, `answers_json_path`, and
  optional clarification output path.
- add a distinct `STATUS: CLARIFICATION_REQUIRED` banner path with summary
  fields.
- ensure raw JSON in tool output includes full clarification array and
  unresolved IDs.
- maintain graph validation attachment where plan artifact exists.
- keep status formatting clean and concise for conversational clients while
  preserving machine-readable detail in raw JSON.

Done when:

- planner tools return structured clarification-required responses,
- existing failed/success banners still function for non-clarification cases.

## Phase 3: REPL Q&A Interaction Surface

Goal:

- make clarification handling first-class in interactive CLI sessions.

Files:

- `omniagent-engine/src/cli/repl.cpp`
- `omniagent-engine/src/cli/repl_internal.h` (if helpers needed)
- `omniagent-engine/tests/test_cli_repl.cpp`

Tasks:

- add `/clarifications` command to list pending questions.
- add `/answer <id> <value>` command to store one answer.
- add `/answers` command to inspect staged answers.
- add `/continue` behavior for clarification-paused runs.
- ensure compatibility with existing `/resume`, `/stop`, `/inspect run`.
- accept plain conversational answers in REPL without requiring `/answer`.
- support one-turn batch answers and global delegation phrases.

Done when:

- user can complete full clarification loop without leaving REPL.

## Phase 4: Default-On Rollout And Compatibility Controls

Goal:

- make robust clarification behavior default while preserving explicit opt-out.

Files:

- `omniagent-engine/src/tools/planner_tools.cpp`
- `planner-harness/bridge.py`
- `omniagent-engine/README.md`
- docs in `/docs`

Tasks:

- set planner workflow defaults to:
  - adversary enabled,
  - `clarification_mode=required`.
- preserve explicit legacy path via `clarification_mode=off`.
- document migration guidance for automation scripts.
- ensure default prompt copy is short, clear, and conversation-native.

## Phase 4.5: Cross-Feature Interaction Alignment

Goal:

- ensure related run controls and planner features follow the same UI-agnostic
  turn contract and do not introduce clunky side-path interactions.

Files:

- `omniagent-engine/src/cli/repl.cpp`
- `omniagent-engine/src/tools/planner_tools.cpp`
- integration docs under `/docs`

Tasks:

- verify pause/resume, inspect, stop, and clarification-required states can be
  represented in a shared run-turn event model.
- verify planner failure, clarification-required, and approval-required states
  use consistent envelope shape and status semantics.
- document client guidance for REPL and chat UI adapters using the same
  transport contract.

Done when:

- all planning and run-control surfaces can be consumed by future chat UI
  without feature-specific protocol forks.

Done when:

- default interactive and tool-driven planning runs request clarification on
  blockers without extra flags,
- automated callers can still force legacy behavior explicitly.

## Phase 5: Test And Validation Hardening

Goal:

- lock behavior with deterministic tests and end-to-end scenarios.

Files:

- `planner-harness/tests/test_bridge.py`
- `omniagent-engine/tests/test_planner_tools.cpp`
- `omniagent-engine/tests/test_cli_repl.cpp`
- optional integration fixtures under `tmp/` for local e2e checks

Tasks:

- add bridge tests:
  - required mode blocks and emits questions,
  - answers resume progression,
  - assume mode records assumptions,
  - off mode preserves legacy progression.
- add bridge tests for:
  - sequential answer progression,
  - batch by-id answer progression,
  - one-sentence multi-answer parsing,
  - explicit delegation phrase behavior.
- add tool tests:
  - clarification-required banner shape,
  - raw JSON carries question payloads,
  - success path unchanged for clear input.
- add tool tests for clean clarification-required summary and unchanged
  machine-readable payloads.
- add REPL tests for question display, answer capture, and continue behavior.
- add REPL tests for plain-language answer turns and batch answer turns.

Done when:

- focused planner and REPL suites pass,
- no regressions in existing planner workflow tests.

## Risks

### Risk 1: Question Explosion From Adversary Output

Why it matters:

- large specs can produce too many low-value questions.

Mitigation:

- prioritize blocking questions,
- dedupe aggressively,
- cap cosmetic display by default.

### Risk 2: Breaking Existing Automation

Why it matters:

- default-on gating could stall scripts expecting one-shot behavior.

Mitigation:

- provide explicit `clarification_mode=off` and `assume`,
- document compatibility mode and migration path.

### Risk 3: Inconsistent Behavior Across Bridge, Tool Wrappers, And REPL

Why it matters:

- users may get different semantics depending on entrypoint.

Mitigation:

- centralize gating logic in bridge,
- keep wrappers thin,
- test all entrypoints against shared fixtures.

## Exit Criteria

This effort is complete when all of the following are true:

- clarification Q&A is default-on for spec/planning workflows,
- blocking ambiguities are surfaced as structured questions,
- required mode blocks progression until answered,
- REPL supports inspect/answer/continue flows,
- explicit opt-out modes preserve non-interactive compatibility,
- focused bridge, planner tool, and REPL tests pass.

## Recommended Execution Order

1. Phase 1 bridge extraction and gating.
2. Phase 2 planner tool wrapper wiring.
3. Phase 3 REPL commands.
4. Phase 4 default switch and documentation.
5. Phase 5 focused regression and e2e validation.
