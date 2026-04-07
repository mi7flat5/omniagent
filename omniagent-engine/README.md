# OmniAgent Engine

Standalone engine module extracted from the old chat-oriented integration path.

Current product direction in the main app is graph-first. This module is kept
separate so it can evolve, break, and be tested independently without coupling
those risks back into `omniagent-core`.

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

## Harness

```bash
./run_harness.sh
```

The harness configures, builds, and runs the module test suite in isolation.
When this module is used inside the current workspace, it automatically reuses
the core project's `CMAKE_PREFIX_PATH` from `omniagent-core/build/CMakeCache.txt`
unless you provide one explicitly.