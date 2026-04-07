# REPL Rewind, Fork, and Stop Controls Spec

Date: 2026-04-07

## Summary

This spec defines three session-control features for omniagent-engine:

1. rewind: remove the most recent conversation message from active session context each time it is called,
2. fork: create a new session from the current session state,
3. stop: stop an in-flight or paused run from the current session.

The same action contract should be usable by both future UI buttons and terminal REPL controls. REPL should support slash commands as canonical behavior and function-key shortcuts as aliases where terminal input supports them.

## User Intent

Core intent:

- add interactive controls to correct course quickly without resetting whole sessions,
- branch a conversation/session state to try alternative approaches,
- interrupt execution immediately from REPL now and UI later.

Explicit requirements:

- rewind removes the last message in context each call,
- fork creates a new session from current session,
- stop exists as a first-class control,
- UI buttons will eventually call the same controls,
- REPL access can be through function keys.

Implicit requirements:

- session persistence and resume behavior must remain valid,
- controls must not corrupt run state or message ordering,
- behavior must be deterministic and auditable in tests,
- slash-command equivalents must exist even when function keys are not available in terminal mode.

## Prior Art Research

Sources consulted:

- [Claude Code built-in commands](https://code.claude.com/docs/en/commands): includes `/rewind`, `/branch` (alias `/fork`), and other direct session controls.
- [Claude Code checkpointing](https://code.claude.com/docs/en/checkpointing): rewind and summarize behavior, targeted rollback semantics, and restore options.
- [Claude Code session lifecycle](https://code.claude.com/docs/en/how-claude-code-works#resume-or-fork-sessions): fork creates a new session id while preserving prior conversation history.
- [Aider in-chat commands](https://aider.chat/docs/usage/commands.html): slash-driven session controls such as `/clear` and safe interruption with Control-C.
- [prompt_toolkit key bindings](https://python-prompt-toolkit.readthedocs.io/en/stable/pages/advanced_topics/key_bindings.html): function key and control-key support across terminals.
- [git branch](https://git-scm.com/docs/git-branch): branch/fork mental model from a stable baseline while preserving original history.

### Comparison Matrix

| Capability | Claude Code | Aider | Prompt Toolkit | git branch model | This Project Target |
|---|---|---|---|---|---|
| Rewind session context | Yes | Partial (clear/reset) | N/A | N/A | P0 |
| Fork session from current point | Yes | No explicit session fork | N/A | Yes (branch from HEAD) | P0 |
| Stop/cancel active work | Yes (interrupt and steer) | Yes (Ctrl-C) | Yes (SIGINT/key handling) | N/A | P0 |
| Slash command controls | Yes | Yes | N/A | N/A | P0 |
| Function-key shortcuts | Terminal dependent | Terminal dependent | Yes | N/A | P1 |

### Prior-Art Defaults We Should Mirror

- Treat rewind and fork as session-state operations, not global host operations.
- Keep original source session untouched when forking.
- Ensure stop remains available for paused runs and active runs.
- Keep command-level behavior as canonical; keyboard shortcuts should be aliases.

## Scope

In scope:

- engine session and host APIs for rewind and fork,
- REPL command surface for rewind and fork,
- improved stop behavior in REPL command path,
- optional function-key mapping in REPL input path,
- tests and docs.

Out of scope for this iteration:

- web UI implementation,
- rewinding filesystem side effects from shell commands,
- multi-step checkpoint browsing UX,
- cross-process lock coordination beyond existing host/session constraints.

## Requirements

### Must Have (P0)

- Add session rewind operation that removes one most-recent message by default.
- Rewind must support an explicit count argument for repeated removal in one call.
- Rewind must refuse to run when the session has an active run.
- Rewind must persist updated session message history immediately.
- Add host-level fork operation that clones session conversation state into a new session id.
- Fork must preserve active profile and working directory in the new session.
- Fork must leave source session unchanged.
- Add REPL commands:
  - `/rewind [count]`
  - `/fork [new-session-id]`
  - existing `/stop [run-id]` remains and is documented as the stop control.
- `/stop` must work for paused run id and currently active run id when available.
- All failures must return explicit user-facing errors in REPL without crashing.

### Should Have (P1)

- Function-key aliases in REPL:
  - `F6` => `/rewind 1`
  - `F7` => `/fork`
  - `F8` => `/stop` for active run.
- If function keys are unavailable in the current terminal, print one-time fallback guidance to slash commands.
- Add host API to fork by session id for future UI/API integration.

### Nice To Have (P2)

- Optional rewind preview mode that shows which message will be removed.
- Optional fork name/label metadata for easier listing in `/sessions`.
- Optional compact-and-fork workflow for very long sessions.

## Interaction Design

### REPL Commands

- `/rewind`
  - Removes the last message from context.
  - Prints: removed role/type summary and resulting message count.
- `/rewind <count>`
  - Removes up to count messages from the tail.
  - If count exceeds available messages, remove all user/assistant/tool messages and keep empty conversation state.
- `/fork`
  - Creates new session with generated id, clones messages, switches current REPL attachment to the new session.
  - Prints source id and new id.
- `/fork <session-id>`
  - Uses provided id if valid and not already present.
- `/stop [run-id]`
  - Existing command; ensure docs describe it as stop control for this feature set.

### Function Key Aliases (REPL)

- `F6`: invoke rewind once.
- `F7`: invoke fork and switch to forked session.
- `F8`: invoke stop for active run id.

Slash commands remain canonical behavior for compatibility.

### Future UI Buttons

Future UI should call the same host/session operations, not separate logic:

- Rewind button -> session rewind API.
- Fork button -> host fork API.
- Stop button -> host stop_run for selected/active run.

## Edge Cases

- Rewind with no removable messages: no-op with explicit message.
- Rewind while run is active: fail with clear error.
- Fork while run is active: allowed, but clone only persisted conversation messages and no active run state.
- Fork with duplicate explicit session id: fail clearly.
- Stop with no active/paused run id: fail clearly.
- Stop for already finished run: return not found or already finished message without exception.
- Terminal without function-key support: slash commands still fully functional.

## Architecture Notes

Current behavior observed:

- REPL command loop in [omniagent-engine/src/cli/repl.cpp](../omniagent-engine/src/cli/repl.cpp) already supports `/stop` and `/cancel`.
- Session reset is implemented by replacing message history with empty vector in [omniagent-engine/src/project_session_impl.cpp](../omniagent-engine/src/project_session_impl.cpp).
- Session resume and persistence are implemented in [omniagent-engine/src/host_impl.cpp](../omniagent-engine/src/host_impl.cpp) and [omniagent-engine/src/services/session_persistence.cpp](../omniagent-engine/src/services/session_persistence.cpp).

Primary interfaces to extend:

- [omniagent-engine/include/omni/project_session.h](../omniagent-engine/include/omni/project_session.h)
  - add rewind API.
- [omniagent-engine/include/omni/host.h](../omniagent-engine/include/omni/host.h)
  - add fork_session API.
- [omniagent-engine/src/project_session_impl.cpp](../omniagent-engine/src/project_session_impl.cpp)
  - implement rewind with persist.
- [omniagent-engine/src/host_impl.cpp](../omniagent-engine/src/host_impl.cpp)
  - implement fork by cloning snapshot messages into a new session.
- [omniagent-engine/src/cli/repl.cpp](../omniagent-engine/src/cli/repl.cpp)
  - add `/rewind` and `/fork` command handlers and key aliases.

## Validation Criteria

### Concrete Assertions

- Rewind removes exactly one message by default:
  - open session,
  - submit two turns,
  - call rewind once,
  - snapshot message count decreases by one.
- Rewind count behavior:
  - rewind count N removes min(N, available_tail_messages).
- Fork creates independent session:
  - source snapshot messages equal fork snapshot messages at fork time,
  - adding a new message to fork does not change source message count.
- Stop behavior:
  - active run can be stopped and ends with status stopped,
  - paused run can be stopped using existing run-id path.

### Invariants

- Source session is immutable under fork operation.
- Rewind never mutates another session.
- Rewind and fork cannot bypass workspace or profile boundaries.
- Session persistence remains loadable after rewind/fork operations.

### Integration Checks

- REPL help text includes rewind and fork commands.
- Existing `/stop` and `/cancel` semantics remain intact.
- `ctest` targeted suites for REPL and host pass.

## Open Questions

- Should rewind remove only non-system messages or any tail message in the raw message list?
- Should fork be blocked while active run exists, or snapshot current conversation and proceed?
- Should REPL switch automatically to the forked session (recommended default: yes)?