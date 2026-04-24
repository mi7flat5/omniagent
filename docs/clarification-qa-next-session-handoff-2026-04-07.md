# Clarification Q&A — Next Session Handoff

Date: 2026-04-07

## Current State

Implemented in this session:

- Bridge clarification contract and gating logic are implemented.
- Planner tool wrappers now support clarification-required status.
- Defaults for build workflows were switched to robust mode:
  - adversary enabled by default,
  - clarification mode required by default.
- Tests updated and passing for bridge and planner tool wrappers.

Not yet completed:

- REPL first-class clarification interaction loop (plain conversational answer turns plus helper commands).
- Cross-feature interaction envelope alignment for all run-control states.

## Next Session Goal

Complete seamless conversational clarification UX in REPL and finish cross-feature interaction alignment without protocol forks.

## Execution Plan (In Order)

1. REPL clarification state machine
- Add active pending clarification state bound to run/session.
- Ensure run can pause in clarification-required state and resume with answers.

2. Conversational answer handling in REPL
- Accept plain text answers as normal turns.
- Support one-by-one and batched answers.
- Support one-sentence multi-answer mapping.
- Support delegation phrase: you decide for me.
- Keep slash commands only as optional convenience wrappers.

3. REPL helper commands (optional but useful)
- Add /clarifications
- Add /answer <id> <value>
- Add /answers
- Add /continue

4. Envelope parity and client-agnostic behavior
- Ensure clarification-required, approval-required, paused, resumed, failed, and stopped states all map to one shared turn/event envelope.
- Ensure REPL-specific commands do not introduce separate protocol semantics.

5. Tests and regressions
- Add/extend REPL tests for:
  - sequential Q&A,
  - batch Q&A,
  - one-sentence multi-answer,
  - delegation default handling,
  - disambiguation follow-up behavior.
- Re-run planner and agent manager suites.

## Files To Touch Next

- omniagent-engine/src/cli/repl.cpp
- omniagent-engine/src/cli/repl_internal.h
- omniagent-engine/tests/test_cli_repl.cpp
- optional: omniagent-engine/src/tools/planner_tools.cpp (only if envelope tweaks are needed)

## Validation Commands

Run these after implementation:

```bash
cd /home/mi7fl/omniagent
python3 -m pytest -q planner-harness/tests/test_bridge.py

cd /home/mi7fl/omniagent/omniagent-engine
cmake --build build -j4
./build/omni-engine-tests --gtest_filter='PlannerToolsTest.*:CliReplInternal.*:AgentManager.*'
```

## Acceptance Checklist

- Clarification interaction in REPL feels like normal conversation turns.
- User can answer one question at a time naturally.
- User can answer multiple questions in one turn.
- User can say you decide for me and unresolved questions use recommended defaults.
- If answer mapping is ambiguous, REPL asks a focused disambiguation follow-up.
- No regressions in planner bridge and planner tool behavior.
- Turn/event contract remains UI-agnostic for future chat UI integration.

## Existing Reference Docs

- docs/spec-and-plan-clarification-qa-spec-2026-04-07.md
- docs/spec-and-plan-clarification-qa-implementation-plan-2026-04-07.md
- docs/core-integrated-project-engine-loop-spec-2026-04-08.md
- docs/core-integrated-project-engine-loop-implementation-plan-2026-04-08.md
