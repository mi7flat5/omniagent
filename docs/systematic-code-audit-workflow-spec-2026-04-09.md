# Systematic Code Audit Workflow Spec

Date: 2026-04-09

## Summary

This spec defines a systematic, codebase-agnostic audit workflow for
`omniagent-engine`.

The current audit runtime already enforces evidence-backed final answers and can
validate reports against tracked review cases. That is necessary but not
sufficient. In practice, the audit loop still behaves like opportunistic
exploration: it may inspect a few implementation files, stop early, and only
learn that coverage was insufficient after deterministic validation rejects the
draft.

The goal of this feature is to make audits deliberate instead of ad hoc. Audit
workers should gather evidence in a stable order, cross-check code against at
least one contract or validation surface when available, preserve distinct
failure clusters, and recover from deterministic validation failure by gathering
the missing evidence instead of only rewriting the same incomplete draft.

## What The User Is Asking For

Core intent:

- make Omni produce good audits for any repository, not just tracked benchmark
  cases,
- give audits a systematic method instead of a model-specific exploration style,
- keep findings trustworthy and grounded in gathered evidence.

Explicit requirements:

- audits must work for arbitrary codebases,
- audits must proceed in a systematic way,
- the system should produce better audits, not just better fallback behavior.

Implicit requirements:

- the workflow must remain read-only,
- the design should reuse the current profile/session/query-engine stack rather
  than add a parallel audit subsystem,
- tracked review validation should shape evidence gathering earlier and more
  effectively,
- the workflow should prefer targeted evidence collection over broad sweeps,
- distinct failure clusters must be preserved when the evidence shows them.

## Prior Art Research

Sources consulted:

- [Aider chat modes](https://aider.chat/docs/usage/modes.html) - separates ask,
  code, and architect flows and recommends planning before editing.
- [Aider repository map](https://aider.chat/docs/repomap.html) - emphasizes
  whole-repository structure mapping before narrowing.
- [Aider linting and testing](https://aider.chat/docs/usage/lint-test.html) -
  treats verification loops as normal rather than optional polish.
- [GitHub code review guidance](https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/getting-started/helping-others-review-your-changes) -
  emphasizes scoped review, context, and explicit reviewer expectations.
- [SonarQube quality gates](https://docs.sonarsource.com/sonarqube-server/latest/quality-standards-administration/managing-quality-gates/introduction-to-quality-gates/) -
  treats pass/fail gates as first-class quality controls.
- [Semgrep false-positive reduction guidance](https://semgrep.dev/docs/kb/semgrep-code/reduce-false-positives) -
  emphasizes high-confidence findings, interfile context, and false-positive
  control.
- [Existing codebase workflow patterns](./existing-codebase-development-workflow-patterns-spec-2026-04-08.md) -
  establishes audit as a first-class workflow family in this repository.

### What Prior Art Suggests

1. Good audits start with repository mapping, not random deep reads.
2. Trustworthy findings pair implementation evidence with a contract, test,
   interface, or command-output surface.
3. Gate-oriented validation is stronger than stylistic scoring alone.
4. Recovery after a failed quality gate should gather missing evidence, not just
   rewrite prose.
5. High-confidence audits minimize filler and unsupported claims.

### Comparison Matrix

| Capability | Aider | GitHub Review Guidance | SonarQube | Semgrep | This Project Target |
|---|---|---|---|---|---|
| Repository mapping before narrowing | Yes | Partial | No | Partial | P0 |
| Findings tied to concrete evidence | Partial | Yes | Yes | Yes | P0 |
| Validation / gate after draft | Partial | Partial | Yes | Partial | P0 |
| Distinct failure-cluster preservation | Partial | Partial | Partial | Partial | P0 |
| Targeted recovery after failed gate | Partial | No | Partial | Partial | P0 |
| Read-only audit workflow | Yes | Yes | Yes | Yes | P0 |
| Generic filler suppression | Partial | Partial | Yes | Yes | P0 |

## User Stories

- As a user auditing an unfamiliar repository, I want the agent to map the repo
  and identify relevant entrypoints before it claims findings.
- As a user auditing failing software, I want the agent to cross-check code
  against tests, CLI contracts, schemas, or other validation surfaces when they
  exist.
- As a maintainer, I want tracked review validation failures to trigger a
  targeted evidence-gathering recovery pass rather than a prose-only rewrite.
- As a maintainer, I want the audit workflow to remain generic and read-only so
  it works across arbitrary repositories.

## Requirements

### Must Have (P0)

- The audit profile prompt must encode a staged workflow instead of only a
  findings-style output format.
- The staged workflow must tell the model to:
  - map repository surface first,
  - identify entrypoints and contract surfaces,
  - inspect at least one validation surface when present,
  - pair each finding with both implementation evidence and the violated
    contract or observed behavior,
  - preserve distinct failure clusters,
  - stop once the evidence is sufficient.
- When tracked review validation fails and non-validator read tools are still
  available, QueryEngine must allow one targeted tools-enabled recovery loop
  before falling back to a no-tools rewrite.
- The recovery loop must be driven by validation feedback and must instruct the
  model to gather only missing evidence.
- The recovery loop must stay read-only and reuse the normal tool execution
  path.
- If recovery still fails, the existing evidence-verification and concise
  rewrite/failure behavior must remain intact.

### Should Have (P1)

- Audit prompts for delegated audit workers should use the same staged guidance
  as top-level audit profiles.
- Recovery instructions should explicitly bias the model toward contract and
  validation surfaces such as tests, CLI entrypoints, schemas, configs, and API
  contracts.
- Focused tests should cover both rewrite-only fallback and tools-enabled
  recovery.

### Nice To Have (P2)

- Add generic audit-coverage heuristics that can detect when no contract or
  validation surface has been inspected yet.
- Add persisted metadata describing which audit stages were completed for a run.

## Interactions

- **Read-only tools**: primary audit mechanism; used for repo mapping, focused
  file reads, and contract inspection.
- **Planner review validator**: used as a quality gate, not as a substitute for
  evidence gathering.
- **Recovery loop**: activated only after deterministic validation failure and
  only when non-validator tools remain available.

## Edge Cases

- No tracked review case exists: skip deterministic review recovery and rely on
  the systematic staged audit prompt plus evidence-based finalization.
- Validator is the only visible tool: keep the existing rewrite-only behavior,
  because there is no tool surface that can gather missing evidence.
- Repositories without tests: the workflow should inspect another contract
  surface such as CLI help, schema files, API contracts, config loaders, or
  documented interfaces.
- Sparse repositories with no explicit contract surfaces: findings must stay
  narrow and evidence-backed; missing contract evidence should reduce confidence
  or suppress the finding.

## Architecture Notes

- Modules affected:
  - `omniagent-engine/src/project_runtime_internal.h`
  - `omniagent-engine/src/agents/agent_manager.cpp`
  - `omniagent-engine/src/core/query_engine.h`
  - `omniagent-engine/src/core/query_engine.cpp`
  - `omniagent-engine/tests/test_query_engine.cpp`
  - `omniagent-engine/tests/test_agent_manager.cpp`
- Architectural boundaries to respect:
  - keep audit read-only,
  - reuse existing profile/session/query-engine plumbing,
  - reuse existing tool execution and validation surfaces,
  - do not add a separate audit runner.

## Validation Criteria

### Concrete Assertions

- `QueryEngine` still rewrites a failed audit report without tools when
  deterministic review validation fails and no evidence-gathering tool is
  available.
- `QueryEngine` performs one tools-enabled recovery loop when deterministic
  review validation fails and non-validator tools are available.
- Audit profile prompts mention staged audit behavior, including repo mapping
  and validation/contract inspection.
- Delegated audit worker prompts mention the same staged audit behavior.

### Integration Checks

- The top-level audit profile still exposes `planner_validate_review` and stays
  read-only.
- Delegated audit workers still expose `planner_validate_review` and stay
  read-only.
- Existing evidence-based final-answer tests continue to pass.

### Invariants

- Audits must not modify workspace files.
- Findings must remain evidence-backed.
- Deterministic validation failure must not silently pass through as a final
  answer without either recovery or explicit failure.
- The new recovery path must be bounded; it cannot create an unbounded nested
  audit loop.

## Open Questions

- None for this first revision. The initial implementation keeps shell access
  out of scope and limits recovery to one bounded tools-enabled pass.