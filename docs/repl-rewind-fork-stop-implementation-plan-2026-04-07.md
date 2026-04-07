# REPL Rewind, Fork, and Stop Controls Implementation Plan

Date: 2026-04-07

## Summary

This plan delivers rewind, fork, and stop controls as shared session actions that can be called by REPL today and by UI buttons later.

Execution strategy:

1. add core session/host APIs first,
2. wire REPL slash commands,
3. add optional function-key aliases,
4. harden with host and REPL tests.

## Baseline Facts

- `/stop` already exists in [omniagent-engine/src/cli/repl.cpp](../omniagent-engine/src/cli/repl.cpp) for paused runs and explicit run ids.
- session history reset already uses `resume({})` in [omniagent-engine/src/project_session_impl.cpp](../omniagent-engine/src/project_session_impl.cpp).
- session persistence and resume paths already exist in [omniagent-engine/src/host_impl.cpp](../omniagent-engine/src/host_impl.cpp) and [omniagent-engine/src/services/session_persistence.cpp](../omniagent-engine/src/services/session_persistence.cpp).

This means rewind and fork can be implemented as incremental API additions, not a new subsystem.

## Defaults

- Rewind default count: 1.
- Rewind allowed only when no active run exists.
- Fork default behavior: clone current snapshot, create new generated session id, and switch REPL to forked session.
- Function-key aliases:
  - F6 -> rewind
  - F7 -> fork
  - F8 -> stop
- Slash commands are canonical behavior and remain supported in all environments.

## Phase 0: API Contract Changes

Goal:

- define minimal public APIs needed by REPL and future UI.

Files:

- [omniagent-engine/include/omni/project_session.h](../omniagent-engine/include/omni/project_session.h)
- [omniagent-engine/include/omni/host.h](../omniagent-engine/include/omni/host.h)

Tasks:

- add session rewind API, for example:
  - `std::size_t rewind_messages(std::size_t count = 1);`
- add host fork API, for example:
  - `std::unique_ptr<ProjectSession> fork_session(const std::string& session_id, SessionOptions options = {});`

Exit criteria:

- headers compile and expose required operations without affecting existing callers.

## Phase 1: Core Session and Host Implementation

Goal:

- implement rewind and fork semantics with persistence safety.

Files:

- [omniagent-engine/src/project_session_impl.cpp](../omniagent-engine/src/project_session_impl.cpp)
- [omniagent-engine/src/host_impl.cpp](../omniagent-engine/src/host_impl.cpp)

Tasks:

- implement rewind:
  - guard against active run,
  - remove tail messages by count,
  - persist updated session.
- implement fork:
  - resolve source session snapshot from live or persisted source id,
  - open new session with same profile and working directory,
  - load copied messages into new session,
  - persist and return new session handle.

Implementation notes:

- use existing snapshot and resume patterns to avoid duplicate persistence logic.
- do not copy active run state, pending approvals, or run ids.

Exit criteria:

- rewind and fork operations succeed in unit tests,
- persisted sessions remain resumable after both operations.

## Phase 2: REPL Command Wiring

Goal:

- expose rewind and fork controls in terminal workflow with clear UX.

Files:

- [omniagent-engine/src/cli/repl.cpp](../omniagent-engine/src/cli/repl.cpp)

Tasks:

- add `/rewind [count]` command handler.
- add `/fork [new-session-id]` command handler.
- update help text and usage output.
- keep `/stop` behavior and ensure it resolves active run id where available.

Exit criteria:

- REPL command loop supports rewind/fork/stop controls without regressions to existing commands.

## Phase 3: Function-Key Alias Support

Goal:

- provide REPL key shortcuts for rewind/fork/stop where terminal input path supports it.

Files:

- [omniagent-engine/src/cli/repl.cpp](../omniagent-engine/src/cli/repl.cpp)
- optionally a new small input helper module under [omniagent-engine/src/cli](../omniagent-engine/src/cli)

Tasks:

- add key-dispatch path for F6/F7/F8 or terminal escape-sequence equivalent.
- map each key to the same command handlers used by slash commands.
- print one-time fallback guidance when keys are not detectable in current terminal mode.

Exit criteria:

- key aliases work in supported terminals,
- slash commands remain fallback on all terminals.

## Phase 4: Test Coverage

Goal:

- lock behavior with focused host and REPL tests.

Files:

- [omniagent-engine/tests/test_project_host.cpp](../omniagent-engine/tests/test_project_host.cpp)
- [omniagent-engine/tests/test_cli_repl.cpp](../omniagent-engine/tests/test_cli_repl.cpp)

Tasks:

- host tests:
  - rewind removes expected message count,
  - rewind rejects active-run state,
  - fork clones messages and preserves source independence.
- REPL tests:
  - help text includes new commands,
  - command parsing for `/rewind` and `/fork` works,
  - stop path remains valid.
- optional terminal/key tests:
  - function key alias dispatch routes to same handlers.

Exit criteria:

- targeted test suites pass with no regressions in existing stop/cancel behavior.

## Phase 5: Documentation and Operator Notes

Goal:

- make controls discoverable for both terminal and future UI integration.

Files:

- [omniagent-engine/README.md](../omniagent-engine/README.md)
- [docs/repl-rewind-fork-stop-spec-2026-04-07.md](repl-rewind-fork-stop-spec-2026-04-07.md)

Tasks:

- add command examples for rewind/fork/stop.
- document function-key aliases and slash fallback.
- document expected behavior when active run exists.

Exit criteria:

- operators can use the feature without reading source code.

## Risks and Mitigations

### Risk 1: Function-key capture is unreliable across terminals

Mitigation:

- treat slash commands as source of truth,
- implement key aliases as optional adapter layer,
- print fallback guidance when key capture is unavailable.

### Risk 2: Rewind removes unintended message types

Mitigation:

- define explicit removable-message policy in API contract,
- add tests for mixed message lists and boundary conditions.

### Risk 3: Fork introduces session persistence inconsistencies

Mitigation:

- reuse existing snapshot/resume persistence path,
- test fork from both live and persisted source sessions.

### Risk 4: Stop semantics become confusing between paused and active runs

Mitigation:

- keep one stop command contract,
- resolve run id deterministically,
- print clear status output after stop attempt.

## Test Commands

Suggested targeted test execution after implementation:

- `cd omniagent-engine && ctest --test-dir build -R test_project_host --output-on-failure`
- `cd omniagent-engine && ctest --test-dir build -R test_cli_repl --output-on-failure`

## Exit Criteria

Feature is complete when:

- rewind works and persists,
- fork creates independent cloned sessions,
- stop is available as documented control,
- REPL exposes slash commands and key aliases (where supported),
- host and REPL tests pass.