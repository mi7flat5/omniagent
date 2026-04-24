# Spec And Plan Clarification Q&A Gate Spec

Date: 2026-04-07

## Summary

This spec defines a robust clarification question-and-answer mechanism for spec
and plan workflows. The mechanism must be on by default, must preserve current
workflow capability, and must stop guess-heavy progression when blocking
ambiguities are detected.

The target behavior is:

- detect gaps and assumptions during spec/plan validation,
- surface explicit clarification questions to the user,
- pause progression until required answers are provided,
- resume generation/repair with the answers incorporated,
- preserve machine-readable output and artifact continuity.

The interaction must feel like normal conversational turns, not a separate
workflow mode with brittle command syntax.

## What The User Is Asking For

Core intent:

- keep current planner functionality,
- add a robust Q&A phase during spec/planning,
- enable it by default,
- ensure the system identifies gaps and assumptions and asks the user for
  clarification.

Explicit requirements:

- clarification mechanism must be default-on,
- must ask user questions when ambiguity is blocking,
- should preserve current spec/plan generation and repair flows.
- should support natural turn-based Q&A in both sequential and batch styles.

Implicit requirements:

- non-interactive automation must remain possible with explicit opt-out,
- question payloads must be structured and resumable,
- clarification behavior must be visible and consistent across bridge, engine
  tool wrappers, and REPL.
- answers should support "you decide for me" semantics for one or many
  unresolved questions.
- answering multiple questions in one sentence should be supported when intent
  can be resolved with high confidence.

## Prior Art Research

Sources consulted:

- https://openai.github.io/openai-agents-python/human_in_the_loop/ -
  run interruption, pending approval payloads, durable pause/resume state.
- https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/human-in-the-loop.html -
  user feedback during run and feedback between runs; explicit transfer of control.
- https://docs.openhands.dev/openhands/usage/run-openhands/cli-mode -
  CLI pause/continue interaction, confirmation defaults, and resume patterns.

### What Prior Art Suggests

1. Clarification/approval should be modeled as a first-class pause condition,
   not only as post-failure logs.
2. A paused state should be resumable and durable.
3. Interactive mode should default to safe confirmation behavior.
4. Structured interruption payloads should include all data needed for resume.
5. Non-interactive mode should be explicit and auditable.

### Comparison Matrix

| Capability | OpenAI Agents HITL | AutoGen HITL | OpenHands CLI | This Project Target |
|---|---|---|---|---|
| Pause run on clarification need | Yes | Yes | Yes | P0 |
| Structured question payloads | Yes | Partial | Partial | P0 |
| Resume after answers | Yes | Yes | Yes | P0 |
| Durable paused state | Yes | Partial | Yes | P1 |
| Default-on safe mode | Yes pattern | Configurable | Yes pattern | P0 |
| Explicit non-interactive override | Yes | Yes | Yes | P0 |
| Machine-readable failure/interrupt contract | Yes | Partial | Partial | P0 |

## User Stories

- As a user running idea-to-spec-to-plan, I want blocking ambiguities to be
  surfaced as concrete questions so I can answer before the system guesses.
- As a CLI user, I want the run to pause and resume cleanly after I provide
  clarifications.
- As an automation owner, I want explicit opt-out controls for non-interactive
  pipelines while still receiving structured assumptions and questions.
- As a maintainer, I want this behavior to reuse existing validation/adversary
  outputs rather than introducing parallel ambiguity detectors.

## Recommended Defaults

- Clarification mode default: `required`.
- Adversary checks default for spec/plan workflows: enabled.
- Blocking clarification threshold: any `BLOCKING` gap/guess/contradiction.
- Cosmetic clarification threshold: include in payload, do not block by default.
- Resume behavior: answers are injected into the next generation/repair attempt.
- Non-interactive override: explicit `clarification_mode=assume` or
  `clarification_mode=off`.

## Requirements

### Must Have (P0)

- Add a first-class clarification state for planner workflows.
- Clarification behavior must be enabled by default in interactive CLI and
  engine planner tool wrappers.
- The system must emit structured clarification questions with stable IDs.
- Question objects must include:
  - `id`
  - `stage` (`spec` or `plan`)
  - `severity` (`BLOCKING` or `COSMETIC`)
  - `quote`
  - `question`
  - `recommended_default`
  - `answer_type` and optional `options`
- If blocking clarification items exist and mode is `required`, workflow must
  stop before further generation/repair attempts.
- Bridge output must include:
  - `clarification_required`
  - `clarifications` array
  - `workflow_passed`
  - completed artifacts and validation payloads
- Engine planner tools must return a distinct failed/paused status banner for
  clarification-required outcomes.
- REPL must allow user to inspect pending clarifications and provide answers,
  then continue the paused run.
- Answers must be fed back into generation/repair prompts as explicit
  clarifications.
- Existing successful flows with fully specified inputs must still complete
  without additional manual steps.
- Clarification Q&A must support both interaction styles:
  - sequential: ask one question, accept one answer, proceed,
  - batch: ask multiple questions, accept multiple answers in one turn.
- Answer ingestion must support:
  - direct single-answer turns,
  - multi-answer turns using question IDs,
  - one-sentence multi-answer turns (intent-mapped),
  - "you decide for me" per-question and all-pending forms.
- If one-sentence mapping is ambiguous, the system must ask a focused
  disambiguation follow-up instead of silently guessing.
- The underlying contract must be UI-agnostic and transport-agnostic so REPL,
  chat UI, and API clients share identical semantics.
- Clarification prompts may include recommended choices, but wording must remain
  clean and concise for conversational UX.

### Should Have (P1)

- Persist clarifications and answers under project storage for run resume and
  postmortem traceability.
- Include an assumptions section in workflow output when defaults are applied in
  non-required modes.
- Support partial answering and show unresolved blocking questions explicitly.

### Nice To Have (P2)

- Add question deduplication and clustering for large ambiguity sets.
- Add optional prioritization tags (`scope`, `security`, `data_model`, etc.).
- Add telemetry for clarification frequency, answer latency, and override rate.

## Clarification Contract

### Clarification Mode

Introduce workflow mode:

- `required` (default): stop on blocking clarifications and request answers.
- `assume`: continue using recommended defaults while recording assumptions.
- `off`: bypass clarification gate (legacy-like behavior; explicit only).

### Clarification Object Schema

```json
{
  "id": "spec-clar-001",
  "stage": "spec",
  "severity": "BLOCKING",
  "quote": "...",
  "question": "Which tenant isolation model is required for background workers?",
  "recommended_default": "Enforce tenant_id at queue dequeue and DB access layers",
  "answer_type": "text",
  "options": []
}
```

### Turn-Native Interaction Contract

Clarification must be represented as turn payloads, not REPL-only command
syntax.

Question turn payload:

```json
{
  "type": "clarification_request",
  "run_id": "run-123",
  "mode": "required",
  "questions": [
    {
      "id": "spec-clar-001",
      "question": "Which tenant isolation model is required for background workers?",
      "recommended_default": "Enforce tenant_id checks at dequeue and data access",
      "severity": "BLOCKING"
    }
  ]
}
```

Answer turn payload:

```json
{
  "type": "clarification_answer",
  "run_id": "run-123",
  "answers": [
    {"id": "spec-clar-001", "value": "row-level tenant_id enforcement"}
  ],
  "delegate_unanswered": false
}
```

Natural-language answer support:

- parse one-sentence responses and map them to question IDs,
- if confidence is low for any mapping, return a short disambiguation turn.

Delegation support:

- if user says "you decide for me" (global or scoped), apply recommended
  defaults for unresolved questions and record explicit assumption metadata.

### Workflow State Machine

- `running` -> `clarification_required` when blocking items exist in required mode.
- `clarification_required` -> `running` when all blocking questions are answered
  or delegated.
- `clarification_required` -> `running_with_assumptions` in assume mode.
- `running` -> `completed` when validation passes.
- `running` -> `failed` for non-clarification hard errors.

## Interactions

### Bridge CLI

Add/extend arguments for `build-from-idea` and related workflow commands:

- `--clarification-mode required|assume|off`
- `--answers-json <path>` optional provided answers
- `--clarifications-output <path>` optional question output snapshot

Behavior:

- when clarification is required, return machine-readable payload and do not
  proceed to further retries until answers are supplied (in required mode).

### Engine Tool Wrappers

Planner tool wrappers must:

- pass clarification mode through to bridge,
- map clarification-required outcomes to explicit status banners,
- include the clarification array in raw JSON for the caller.

### REPL

Add REPL commands:

- `/clarifications` list pending clarification questions for active run
- `/answer <id> <value>` set answer for one question
- `/answers` show currently captured answers
- `/continue` resume clarification-paused run

REPL compatibility requirement:

- slash commands are convenience helpers only,
- plain conversational turns must also be accepted for answers,
- REPL must use the same underlying clarification request/answer contract as
  API and future chat UI.

## Edge Cases

- Duplicate questions generated across retries: dedupe by normalized
  `(stage, question, quote)` key.
- Missing answer for one blocking question: keep run paused.
- Invalid answer type for constrained option questions: reject and keep paused.
- User answers only cosmetic questions: still block until blocking set resolved.
- One sentence answers multiple questions but omits one blocking item: accept
  resolved subset and re-ask unresolved blocking items only.
- User sends "you decide for me" after partially answering: preserve supplied
  answers and apply defaults only to unresolved questions.
- User provides one-sentence answer with conflicting intents for two questions:
  request disambiguation before resume.
- Non-interactive mode with required clarifications: fail with
  `clarification_required=true` and complete question payload.
- Resumed run after process restart: pending clarifications and answers must load
  from persisted state (P1 durability requirement).

## Architecture Notes

Modules/layers affected:

- `planner-harness/bridge.py`
- `planner-harness/scoring.py`
- `planner-harness/validators/spec_adversary.py`
- `planner-harness/validators/plan_adversary.py`
- `omniagent-engine/src/tools/planner_tools.cpp`
- `omniagent-engine/src/cli/repl.cpp`
- `omniagent-engine/include/omni/run.h` and run persistence surfaces as needed

Architectural boundaries to respect:

- reuse existing rubric/adversary outputs as the source of ambiguity data,
- keep workflow machine-readable,
- keep bridge as the planner contract boundary,
- keep clarification contracts independent from REPL-specific parsing,
- do not regress workspace path containment or artifact persistence behavior.

## Validation Criteria

### Concrete Assertions

- `build-from-idea` with ambiguous input and
  `clarification_mode=required` returns:
  - `workflow_passed=false`
  - `clarification_required=true`
  - non-empty `clarifications` with at least one `BLOCKING` item.
- Providing complete `answers_json` for all blocking questions and rerunning the
  same workflow resumes and reaches either validated success or a different,
  newly surfaced blocker set.
- REPL can pause on clarification-required status, accept answers, and continue
  the same run ID.
- Sequential UX assertion: when three blocking questions exist, the system can
  ask one, accept one, then ask the next without leaving clarification state.
- Batch UX assertion: when three blocking questions exist, a single answer turn
  can resolve all three by ID and resume immediately.
- One-sentence UX assertion: a one-sentence answer that resolves multiple
  questions with high confidence is accepted in one turn.
- Delegation UX assertion: "you decide for me" resolves unresolved questions
  using recommended defaults and records assumptions.
- In `clarification_mode=assume`, workflow continues while recording the applied
  assumptions in output payload.
- In `clarification_mode=off`, behavior matches current non-gated progression.

### Invariants

- Blocking clarifications cannot be silently ignored in required mode.
- Every blocking question must be answerable via stable `id`.
- Clarification interactions must be representable as generic request/answer
  turns regardless of client UI.
- Clarification payload must survive status banner wrapping and remain available
  in raw JSON.
- Existing planner artifacts from completed stages remain on disk on both
  clarification pause and failure outcomes.

### Integration Checks

- Planner tool wrappers preserve failure-banner semantics while adding
  clarification-required status.
- REPL command set can inspect and resolve clarification queues without breaking
  existing `/resume`, `/stop`, `/inspect run` flows.
- Planner, REPL, and future chat UI clients all consume the same turn-native
  clarification payload contract.
- Existing planner test suites continue to pass, with new coverage for
  clarification-required behavior.

## Open Questions

- Should cosmetic-only question sets be shown by default or behind a verbosity
  flag?
- Should bridge auto-generate `recommended_default` values from model output or
  deterministic templates by category?
- Should clarification answers be copied into `SPEC.md`/`PLAN.json` comments or
  only stored in machine-readable metadata?
