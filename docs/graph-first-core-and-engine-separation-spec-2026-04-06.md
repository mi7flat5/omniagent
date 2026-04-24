# Feature: Graph-First Core And Standalone Engine Separation (Historical)

## Summary

The main OmniAgent product should be graph-first for now. The legacy engine path,
originally intended to replace chat-centric behavior, should be isolated into its
own standalone module with an independent build, test suite, and test harness so
core feature work can proceed without integration drag.

## Status Update (2026-04-08)

This document is retained as historical context for the extraction work.

Current direction:

- Keep `omniagent-engine` as an independently buildable/testable top-level module.
- Integrate engine host/session APIs back into `omniagent-core` for project-scoped
  agent loops that run outside graph execution.
- Use the integrated loop first for spec->plan and developer debug/remediation
  workflows, while keeping graph execution as a separate runtime path.

## Prior Art Research

Sources consulted:

- https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html — target-oriented separation, explicit binary targets, transitive dependency boundaries, and independent test executables.
- https://bazel.build/extending/rules — guidance on isolated executable and test targets, validation outputs, and keeping tool-specific validation outside the main dependency path.
- https://martinfowler.com/articles/modularizing-react-apps.html — layering and separation of view from non-view logic so the active product surface stays thin and maintainable.

### Comparison Matrix

| Capability | CMake Guidance | Bazel Guidance | Fowler Layering | This Project |
|---|---|---|---|---|
| Separate binary target per subsystem | Yes | Yes | Indirectly yes | P0 |
| Independent test target per subsystem | Yes | Yes | Yes | P0 |
| Keep validation off critical runtime path | Indirectly yes | Yes | Yes | P0 |
| Thin active UI surface | N/A | N/A | Yes | P0 |
| Remove dead integration shells | Indirectly yes | Indirectly yes | Yes | P0 |
| Preserve optional experimentation outside core | Yes | Yes | Yes | P1 |

## User Stories

- As a maintainer, I want the graph-first core to build without the legacy engine so I can add features strategically.
- As a maintainer, I want the engine to have its own module boundary so regressions there do not destabilize `agentcore`.
- As a maintainer, I want dead graph-unrelated web shell code removed so the live app tree reflects the product that actually ships.
- As a maintainer, I want the engine to have a single harness command for configure, build, and test so it can still evolve independently.

## Requirements

### Must Have (P0)

- [ ] `omniagent-core` must no longer link against the extracted engine module.
- [ ] The engine must live in its own top-level module directory with its own `CMakeLists.txt` entrypoint.
- [ ] The engine module must provide its own isolated test executable and support `ctest` independently of `omniagent-core`.
- [ ] The engine module must provide a one-command local test harness script.
- [ ] The active web app must remain graph-first and must not carry unmounted shell components for chat-era or app-shell work that is not currently shipped.
- [ ] Empty or unused chat-era frontend folders must be removed.
- [ ] Dead frontend shell stores and components that are not mounted from the live app root must be removed.

### Should Have (P1)

- [ ] The standalone engine module should carry its own dependency manifest.
- [ ] Documentation should explicitly state that the engine is not part of the graph-first runtime.
- [ ] The architecture review should remain aligned with the new module split.

### Nice to Have (P2)

- [ ] Add CI jobs that build and test `omniagent-core` and `omniagent-engine` independently.
- [ ] Add a dead-code inventory check for unmounted frontend components and uncompiled backend sources.

## Interactions

- **Core build**: `omniagent-core` builds without `omni-engine` in its transitive link graph.
- **Engine build**: `omniagent-engine` configures and builds as an isolated CMake project.
- **Engine validation**: `run_harness.sh` configures, builds, and runs `ctest` for the engine only.
- **Frontend runtime**: the root app remains focused on project, graph, activity, and drilldown surfaces.

## Edge Cases

- If stale build directories still reference the old integrated engine path, they must be regenerated rather than trusted.
- If any `omniagent-core` source still implicitly depends on engine headers or symbols, separation is incomplete and the build must fail loudly.
- If removed frontend shell code is later needed, it should come back behind an explicit product decision rather than remain dormant.

## Architecture Notes

- Modules affected:
  - `omniagent-core/src/CMakeLists.txt`
  - `omniagent-engine/*`
  - `omniagent-web/src/components/*`
  - `omniagent-web/src/stores/*`
- Architectural boundaries to respect:
  - Graph-first runtime remains the canonical product surface.
  - Engine experimentation must not leak back into `agentcore` through transitive linking.
  - Frontend should only retain code reachable from the live app root.

## Validation Criteria

### Concrete Assertions

- [ ] `grep_search("omni-engine", includePattern="omniagent-core/src/**/*")` shows no remaining active source dependency from `agentcore` onto the engine module.
- [ ] Configuring and building `omniagent-engine` from its own root succeeds.
- [ ] Running `ctest --test-dir <engine-build-dir>` from the engine module runs the engine-only suite.
- [ ] `npm run build` in `omniagent-web` succeeds after dead shell removal.

### Invariants

- [ ] The live app root only contains graph-first runtime surfaces.
- [ ] The engine module remains buildable without `omniagent-core` acting as its host build.
- [ ] No deleted frontend file was reachable from `src/app.tsx` at the time of removal.

### Integration Checks

- [ ] `omniagent-core` reconfigures successfully after the engine link removal.
- [ ] `omniagent-web` has no imports of removed shell components or shell-only stores.
- [ ] The engine harness script can be executed from the engine module root without referencing `omniagent-core` paths.

## Open Questions

- Should the engine eventually become its own repository instead of a top-level sibling module in this workspace?
- Should stubbed KPI and cost-comparison code also be removed now, or staged behind a dedicated future feature effort?