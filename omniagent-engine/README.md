# OmniAgent Engine

Standalone engine module extracted from the old chat-oriented integration path.

Current product direction in the main app is graph-first. This module is kept
separate so it can evolve, break, and be tested independently without coupling
those risks back into `omniagent-core`.

The current standalone CLI defaults to a coordinator profile that delegates
research, spec work, and planning to specialized workers. Planner workers now
use structured planner tools backed by `planner-harness/bridge.py` instead of
relying on ad hoc shell prompts.

## What This Module Is

- A self-contained C++ engine library with its own source tree.
- Its own GoogleTest suite.
- Its own build entrypoint.
- Its own test harness script.
- A project-scoped host/session/run API for embedding in other applications.
- A standalone CLI for direct interactive and one-shot engine testing.

## What This Module Is Not

- It is not linked into `omniagent-core`.
- It is not part of the graph-first web app runtime.
- It is not a dependency of the active `agentcore` build.

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

## Planner Integration

- Default workspace tools now include `planner_validate_spec`, `planner_validate_plan`, and `planner_build_plan`.
- These tools call `planner-harness/bridge.py`, which exposes machine-readable JSON for spec validation, prompt generation, plan generation, and plan validation.
- `planner_validate_plan` and `planner_build_plan` also enforce the current `PLAN.json` graph parser contract used in `omniagent-core`.

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