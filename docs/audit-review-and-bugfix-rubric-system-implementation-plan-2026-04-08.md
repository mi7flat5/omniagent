# Audit, Review, And Bugfix Rubric System Implementation Plan

Date: 2026-04-08

## Summary

This plan extends `planner-harness` from spec/plan validation into two new
evaluation families:

- review validation for audits and code reviews,
- bugfix validation for remediation writeups.

The delivery strategy stays close to the current harness architecture:

1. define tracked case formats,
2. add deterministic rubric validators,
3. add adversary prompts,
4. expose bridge commands,
5. add golden data and focused tests.

## Phase 0: Contract Freeze

Goal:

- lock case formats and command names before wiring them into the bridge.

Tasks:

- freeze bridge command names:
  - `validate-review`
  - `validate-bugfix`
- freeze tracked data roots under `planner-harness/tests/data/`
- freeze the review case fields needed for required findings, clusters, and
  forbidden claims
- freeze the bugfix case fields needed for repro, root cause, fix, and
  verification checks

Done when:

- validators and tests can be written without renaming inputs midway.

## Phase 1: Deterministic Review Validator

Goal:

- add a strong first-pass review validator that rewards evidence-backed coverage
  and rejects unsupported filler.

Files:

- `planner-harness/validators/review_rubric.py`
- `planner-harness/tests/test_review_rubric.py`

Tasks:

- implement findings-first heuristics
- implement baseline-summary checks
- implement required-finding coverage using grouped phrase anchors
- implement distinct-cluster coverage
- implement forbidden-claim checks
- implement forbidden-section-title checks

Done when:

- the `confctl` good sample passes and the bad sample fails for the right
  reasons.

## Phase 2: Deterministic Bugfix Validator

Goal:

- add a bugfix validator that checks the repro -> root cause -> fix ->
  verification chain.

Files:

- `planner-harness/validators/bugfix_rubric.py`
- `planner-harness/tests/test_bugfix_rubric.py`

Tasks:

- check repro context coverage
- check root-cause explanation coverage
- check concrete fix coverage
- check verification coverage
- check forbidden unsupported completion claims

Done when:

- a good sample remediation report passes and a vague unsupported one fails.

## Phase 3: Adversary Prompts

Goal:

- preserve the existing harness pattern of deterministic rubric plus adversary.

Files:

- `planner-harness/prompts/review_adversary.md`
- `planner-harness/prompts/bugfix_adversary.md`
- `planner-harness/validators/review_adversary.py`
- `planner-harness/validators/bugfix_adversary.py`

Tasks:

- define JSON response shapes for unsupported claims and missing evidence
- keep adversary outputs compatible with existing `StageResult` fields

Done when:

- both commands can run with or without adversary mode using the same serialized
  stage shape as spec/plan.

## Phase 4: Bridge Integration

Goal:

- expose both validators through `bridge.py`.

Files:

- `planner-harness/bridge.py`
- `planner-harness/tests/test_bridge.py`

Tasks:

- add `_validate_review(...)`
- add `_validate_bugfix(...)`
- add parser entries and command dispatch
- serialize outputs using the existing stage serializer

Done when:

- `python bridge.py validate-review ...` and `validate-bugfix ...` emit stable
  JSON payloads.

## Phase 5: Golden Data And Focused Tests

Goal:

- lock the first real case and test the bridge end to end.

Files:

- `planner-harness/tests/data/review/confctl_source_of_truth_case.json`
- `planner-harness/tests/data/review/confctl_good_report.md`
- `planner-harness/tests/data/review/confctl_bad_report.md`
- `planner-harness/tests/data/bugfix/*.json|*.md`

Tasks:

- add the tracked `confctl` review case
- add a good sample that reflects the source-of-truth audit
- add a bad sample that invents findings and adds filler
- add a simple tracked bugfix case and matching good/bad samples

Done when:

- the focused review and bugfix tests pass without needing live model calls.

## Exit Criteria

- `planner-harness` exposes deterministic and adversarial validation for review
  and bugfix outputs.
- The tracked `confctl` case is reusable in tests.
- Focused unit and bridge tests pass.
- The first implementation improves audit trustworthiness without creating a new
  evaluator stack outside the harness.