# OmniAgent Engine

Standalone engine module extracted from the old chat-oriented integration path.

Current product direction is a split architecture:

- `omniagent-engine` stays independently buildable and testable,
- `omniagent-core` can embed its project-scoped host/session/run APIs for ad
	hoc repository workflows,
- graph execution remains a separate runtime path.

The current standalone CLI defaults to a coordinator profile that delegates
repository exploration, feature work, refactors, audits, bugfixes, research,
spec work, and planning to specialized workers. Planner workers now use
structured planner tools backed by `planner-harness/bridge.py` instead of
relying on ad hoc shell prompts.

## What This Module Is

- A self-contained C++ engine library with its own source tree.
- Its own GoogleTest suite.
- Its own build entrypoint.
- Its own test harness script.
- A project-scoped host/session/run API for embedding in other applications.
- A standalone CLI for direct interactive and one-shot engine testing.

## What This Module Is Not

- It is not the graph runtime.
- It is not a replacement for graph execution.
- Standalone build and test of this module do not require an `omniagent-core`
	build.

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

## CLI

```bash
./build/omni-engine-cli --help
./build/omni-engine-cli repl --project-id demo --workspace-root /path/to/repo --base-url http://localhost:11434/v1 --model qwen3.5:32b
./build/omni-engine-cli run --project-id demo --workspace-root /path/to/repo --base-url http://localhost:11434/v1 --model qwen3.5:32b --prompt "summarize this repo"
```

### Global Launcher

If you want a reusable shell command, symlink the launcher into a directory already on your `PATH`:

```bash
ln -sf /home/mi7fl/omniagent/omniagent-engine/run_project_cli.sh ~/.local/bin/omni
```

After that, you can start it from any directory:

```bash
cd /path/to/project
omni
```

When launched this way, the current directory becomes the default:

- workspace root,
- working directory,
- project id source (`basename "$PWD"`),
- and session storage root (`$PWD/.omniagent/engine-cli`).

You can still override any of those with `ENGINE_WORKSPACE_ROOT`, `ENGINE_WORKING_DIR`, `ENGINE_PROJECT_ID`, or `ENGINE_STORAGE_DIR`.

The CLI is a thin wrapper over the public host/session/run API in `include/omni/host.h`,
`include/omni/project_session.h`, and `include/omni/run.h`.

### REPL Session Controls

The REPL now supports shared session controls intended for both terminal usage and
future UI bindings:

- `/rewind [count]` removes one (or `count`) most recent messages from the current session.
- `/fork [session-id]` clones current session context into a new session and switches to it.
- `/stop [run-id]` stops the current or named run.

Function-key aliases are available in terminals that emit standard escape sequences:

- `F6` => `/rewind 1`
- `F7` => `/fork`
- `F8` => `/stop`

If function keys are not captured by your terminal, use the slash commands directly.

## Workflow Families

The default `coordinator` profile routes work to more specific workflow
families. Switch profiles directly when you already know the task shape.

| Profile | Use When | Tool Posture |
|---|---|---|
| `coordinator` | Orchestrating specialist workers | Delegate-only |
| `explore` | Understanding an existing codebase | Read-only local tools |
| `feature` | Adding or extending behavior | Local edits, verification, optional planner escalation |
| `refactor` | Behavior-preserving structural cleanup | Local edits, verification, optional planner escalation |
| `audit` | Reviewing code or changes for findings | Read-only local tools |
| `bugfix` | Fixing failing behavior | Local edits and targeted verification |
| `research` | Gathering outside evidence or docs | Read-only local plus network tools |
| `spec` | Writing or revising `SPEC.md` | Local edits plus validation and research |
| `plan` | Building, repairing, or validating `PLAN.json` | Read-only local plus planner tools |
| `general` | Tasks that do not fit a narrower workflow | Full runtime |

Representative prompts:

- `explore`: `Trace the login flow and identify where session state is persisted.`
- `feature`: `Add tenant-scoped webhook retry history to the existing API.`
- `refactor`: `Split the auth utility into smaller units without changing behavior.`
- `audit`: `Review the recent auth changes for correctness and security regressions.`
- `bugfix`: `Fix the failing clarification resume flow and rerun the focused test.`

Current first-pass workflow limits:

- `audit` stays read-only and local-only.
- Larger feature or refactor work may escalate into `spec` plus `plan` artifacts
	and then hand off to the separate graph path.
- Background edit-capable workers and worktree isolation are intentionally
	deferred.

## Planner Integration

- Default workspace tools now include `planner_validate_spec`, `planner_validate_plan`, `planner_validate_review`, `planner_validate_bugfix`, `planner_repair_plan`, `planner_build_plan`, and `planner_build_from_idea`.
- These tools call `planner-harness/bridge.py`, which exposes machine-readable JSON for spec validation, review validation, bugfix validation, prompt generation, plan generation, and plan validation.
- `planner_validate_plan`, `planner_repair_plan`, `planner_build_plan`, and `planner_build_from_idea` also enforce the current `PLAN.json` graph parser contract used in `omniagent-core`.
- `planner_validate_review` accepts inline `report_text` so audit agents can score a draft report against tracked review cases without writing a workspace file first.

### Build Spec And Plan From Inside REPL

The REPL does not expose a dedicated `/tool` command. Instead, enter a normal
message and ask the agent to run the planner tools.

Recommended setup for planning workflows:

1. Switch profile to planning tools:
	- `/profile plan`
2. Confirm planner tools are visible:
	- `/tools`

#### Idea -> SPEC.md + PLAN.json (one shot)

Paste this prompt inside REPL and replace the idea text:

```text
Use planner_build_from_idea to generate SPEC.md and PLAN.json from this idea.

Idea:
<your idea text>

Arguments:
- spec_output_path: SPEC.md
- prompt_output_path: planner-prompt.md
- plan_output_path: PLAN.json
- clarification_mode: required
- overwrite: true

After generation, run planner_validate_plan and report any blocking checks.
```

If clarification questions are returned, answer them conversationally in one or
more turns. You can answer:

- one question at a time,
- multiple question IDs in one message,
- or say "you decide for me" to accept recommended defaults.

#### Existing SPEC.md -> PLAN.json

```text
Use planner_build_plan with:
- spec_path: SPEC.md
- prompt_output_path: planner-prompt.md
- plan_output_path: PLAN.json
- clarification_mode: required

Then run planner_validate_plan and summarize blockers if validation fails.
```

#### Spec-only workflow

Switch to spec profile first:

- `/profile spec`

Then use:

```text
Draft SPEC.md for this project idea, run planner_validate_spec,
and if validation fails, revise SPEC.md and validate again.
```

Useful REPL commands during planner workflows:

- `/runs`
- `/inspect run [id]`
- `/resume [approve|deny|always]`
- `/stop [run-id]`

To use the planner bridge, install the Python dependencies used by `planner-harness`:

```bash
python3 -m pip install -r ../planner-harness/requirements.txt
```

## Harness

```bash
./run_harness.sh
```

The harness configures, builds, and runs the module test suite in isolation.
When this module is used inside the current workspace, it automatically reuses
the core project's `CMAKE_PREFIX_PATH` from `omniagent-core/build/CMakeCache.txt`
unless you provide one explicitly.