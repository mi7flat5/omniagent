# Audit, Review, And Bugfix Rubric System Spec

Date: 2026-04-08

## Summary

This spec defines a case-driven evaluation system for evidence-based audits,
code reviews, and bugfix writeups in `planner-harness`.

The immediate problem is that repository audits can look plausible while still
containing invented findings, filler sections, or collapsed root-cause stories
that are not supported by the actual code or test output. The existing harness
already validates `SPEC.md` and `PLAN.json` with deterministic rubric checks plus
an adversary pass. This feature extends that pattern to review-style outputs and
bugfix reports.

The intended shape is:

- tracked source-of-truth cases stored in the repository,
- deterministic validators that check coverage and contract compliance,
- optional adversary prompts that try to find unsupported claims,
- machine-readable bridge commands for validation,
- initial golden data seeded from the direct `confctl` audit.

## What The User Is Asking For

Core intent:

- make audits, code reviews, and bugfix writeups more trustworthy,
- stop rewarding fluent but weak outputs,
- reuse the same planner-harness validation pattern already used for specs and
  plans.

Explicit requirements:

- add a rubric system for audits,
- add a rubric system for code reviews,
- add a rubric system for bugfix writeups,
- ground the first golden case in a direct source-of-truth audit rather than in
  model-generated summaries.

Implicit requirements:

- review-style validation must be able to preserve distinct failure clusters
  instead of collapsing them into one guessed root cause,
- validators must penalize unsupported claims and generic filler,
- tracked golden datasets must live in versioned repository data, not in ignored
  report folders,
- initial implementation should fit the current bridge/validator architecture
  instead of creating a parallel evaluator stack.

## Prior Art Research

Sources consulted earlier in this work:

- Google engineering practices for code review quality bars
- GitHub review states and required-review gating
- SonarQube quality gates
- Aider lint/test verification loops
- SWE-bench evaluation patterns for bugfix validation

### What Prior Art Suggests

1. Good review systems are gate-oriented: they do not just score style, they
   block on missing evidence or unresolved failures.
2. Review outputs need both deterministic checks and an adversarial pass. The
   deterministic layer catches format and coverage drift; the adversarial layer
   catches unsupported reasoning.
3. Bugfix evaluation is stronger when it preserves the chain:
   repro -> root cause -> fix -> verification.
4. Source-of-truth datasets matter. Without tracked cases, the evaluator drifts
   into taste rather than validation.

## Recommended Architecture

### Case Families

Add two initial case families:

- `review`
  - covers repository audits and code reviews
  - checks findings-first structure, required finding coverage, failure-cluster
    coverage, evidence anchors, and forbidden filler/claims
- `bugfix`
  - covers remediation writeups
  - checks repro context, root cause, concrete fix description, verification,
    and forbidden unsupported success claims

### Storage Model

Tracked evaluation cases live under `planner-harness/tests/data/`.

- review data under `planner-harness/tests/data/review/`
- bugfix data under `planner-harness/tests/data/bugfix/`

This keeps cases versioned and reusable in tests, unlike `reports/`, which is
ignored.

### Validation Surface

Add machine-readable bridge commands:

- `validate-review <case.json> <report.md>`
- `validate-bugfix <case.json> <report.md>`

Each command should emit the same stage-style payload shape already used for
`validate-spec` and `validate-plan`.

### Review Case Shape

Review cases must support:

- baseline command/result summary,
- required findings with cluster IDs and phrase groups,
- minimum distinct cluster coverage,
- forbidden claim patterns,
- forbidden filler section titles.

### Bugfix Case Shape

Bugfix cases must support:

- repro phrase groups,
- root-cause phrase groups,
- fix phrase groups,
- verification phrase groups,
- forbidden unsupported-claim patterns.

## Requirements

### Must Have (P0)

- Add deterministic validators for `review` and `bugfix` case families.
- Add adversary prompt support for both families.
- Add bridge commands for both families.
- Keep validation outputs machine-readable and stage-shaped.
- Seed at least one tracked review case from a direct source-of-truth audit.
- Penalize unsupported claims such as invented syntax errors, test-blame, or
  filler review sections not backed by evidence.
- Preserve distinct failure clusters when the case baseline explicitly shows
  them.
- Allow focused tests to validate both good and bad sample outputs.

### Should Have (P1)

- Add more review cases from real repository audits.
- Add bugfix cases grounded in real repo regressions instead of synthetic
  samples.
- Add top-level comparison/report helpers once more than one family is commonly
  used.

### Nice To Have (P2)

- Add multi-artifact cases where the evaluator inspects both an audit report and
  structured evidence attachments.
- Add regression dashboards across case families.

## Initial Golden Dataset

The first tracked review case is the `confctl` audit generated from direct code
and test evidence on 2026-04-08.

That case captures:

- the fresh `pytest -q` baseline: `42 failed, 72 passed in 1.19s`,
- required coverage for schema, store, CLI, and env-loader failure clusters,
- explicit forbidden claims such as invented syntax errors and blaming the
  tests without evidence,
- forbidden filler headings such as `Security Considerations` and `Production
  Readiness` when the case evidence does not support them.

## Validation Criteria

### Concrete Assertions

- `validate-review` passes for the tracked `confctl` good sample report.
- `validate-review` fails for a report that invents a syntax error, misses core
  failure clusters, or adds filler sections.
- `validate-bugfix` passes for a report that states repro, root cause, fix, and
  verification.
- `validate-bugfix` fails for a vague report that claims success without
  verification.
- Bridge payloads for both commands include `rubric_checks`, scores, and the
  standard adversary block.

### Invariants

- Review validators must not reward unsupported findings just because the prose
  sounds plausible.
- Bugfix validators must not reward a claimed fix unless verification is stated.
- Golden data must stay in tracked repository paths rather than in ignored
  output folders.

## Open Questions

- None for this slice. The first implementation intentionally prioritizes a
  strong review/audit case and a lightweight bugfix case format over a broader
  reporting UI.