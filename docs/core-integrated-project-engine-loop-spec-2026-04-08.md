# Core-Integrated Project Engine Loop Spec

Date: 2026-04-08

## Summary

This spec defines the intended architecture for engine usage in this repository:

- `omniagent-engine` remains an independently buildable and testable module.
- `omniagent-core` embeds engine host/session APIs for project-scoped agent loops.
- Embedded engine loops run outside graph execution and are used for:
  - ad hoc repository work such as exploration, audits, bugfixes, refactors,
    and simpler feature work,
  - producing spec/plan artifacts and setting up new graph-targeted work when a
    larger feature warrants handoff to the separate graph runtime.

This supersedes any interpretation that core/engine integration is permanently forbidden.

## Product Position

The graph runtime and the embedded engine runtime are complementary:

- Graph runtime: deterministic execution of well-defined speced and planned
  tasks, using the graph-specific agent and context workflow already defined for
  that path.
- Embedded engine runtime: iterative specialist-agent loop for ad hoc repository
  issues, remediation, refactoring, auditing, and graph-setup handoff work.

They may share project identity and workspace policy, but they are separate execution paths.

## Requirements

### Must Have (P0)

- `omniagent-core` can create one engine host per project and open/resume sessions for that project.
- Embedded engine execution is bound to project workspace root and working directory constraints.
- Engine events exposed to core include project/session/run identifiers.
- Core integration path must not require REPL-only code.
- `omniagent-engine` remains buildable/testable directly from its own module root.
- Embedded engine workflows cover ad hoc repository tasks directly and can also
  produce spec/plan artifacts for new graph work when feature scope warrants it.
- Graph execution remains a separate runtime path and does not get folded into
  the embedded ad hoc loop.
- Embedded planning and run-control interactions must use a transport-agnostic,
  turn-native contract consumable by both terminal REPL and future chat UI.

### Should Have (P1)

- Core maps user/project policy to engine permission modes/rules with explicit defaults.
- Core and engine use a dedicated event namespace/channel for embedded engine runs.
- Embedded engine state can be inspected independently of graph run state.

### Nice To Have (P2)

- Shared operator dashboards for graph runs and embedded engine runs.
- Per-project quotas/limits for engine run concurrency and tool budgets.

## Architecture Boundaries

- Keep `omniagent-engine` runtime generic and reusable.
- Keep core-specific auth, REST, and WS contract mapping in core adapter layers.
- Do not mix graph executor internals into engine session/run internals.
- Do not turn the embedded engine loop into a second graph executor; it may set
  up graph work, but graph execution remains separate.
- Maintain isolated engine harness validation (`run_harness.sh`) even when core embeds the engine.
- Keep interaction semantics UI-agnostic: feature behavior must not depend on
  slash-command-only or terminal-only affordances.

## Validation Criteria

### Concrete Assertions

- Core can start an engine host for a project and run at least one ad hoc
  repository turn.
- Engine tool execution stays inside project workspace boundaries when launched through core.
- Core can run an ad hoc bugfix or refactor turn without routing through the
  graph runtime.
- Idea-to-spec-to-plan can run from core through engine tools and produce
  `SPEC.md` + `PLAN.json` artifacts when the loop is setting up graph work.
- Engine module builds/tests from `omniagent-engine/` without requiring core build steps.
- Clarification and run-control states can be represented in a common turn/event
  envelope without REPL-specific coupling.

### Invariants

- One engine session belongs to exactly one project host.
- Embedded engine loops do not execute through the graph runtime path.
- The graph runtime continues to execute well-defined planned tasks separately
  from the embedded ad hoc loop.
- Core can be upgraded without removing standalone engine harness viability.
- Interaction contracts remain stable across clients (REPL, API, and chat UI
  adapters).

## Notes

- This spec aligns with `docs/engine-consumption-and-integration-spec-2026-04-06.md` and clarifies repository-level direction after extraction work.
- Historical extraction context remains in `docs/graph-first-core-and-engine-separation-spec-2026-04-06.md`.
