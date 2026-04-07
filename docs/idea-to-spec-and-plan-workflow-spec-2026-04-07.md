# Idea-To-Spec And Plan Workflow Spec

Date: 2026-04-07

## Summary

This spec defines a new end-to-end workflow that starts from a project idea and produces both `SPEC.md` and `PLAN.json` in one machine-readable flow.

The feature should add exactly one new public planner bridge command and one new engine tool. The bridge command should take an idea, generate a planner-compatible spec, validate it, generate a plan from that spec, validate and repair the plan when needed, and emit structured JSON with artifact paths, timings, and failure details. The engine tool should expose that workflow to the standalone CLI and delegated planning agents without requiring users to hand-author `SPEC.md` first.

## What The User Is Asking For

Core intent:

- make the current planner responsible for producing the initial spec instead of assuming a human already wrote `SPEC.md`,
- support a one-shot workflow from project idea to validated implementation plan,
- keep the result written to disk so a later session can continue from concrete artifacts rather than conversational context.

Explicit requirements:

- add a new engine tool,
- add a new planner bridge command,
- go from idea to `SPEC.md` to `PLAN.json` in one workflow,
- write the resulting artifacts down so another session can resume from them.

Implicit requirements:

- the new workflow must preserve the current project/workspace safety boundaries,
- the workflow must not silently overwrite existing artifacts without an explicit policy,
- failures must surface exact blocking checks rather than vague success-looking JSON,
- delegated plan execution must be able to use the new tool under the current plan-profile and CLI approval model,
- the generated spec must be concrete enough for the existing planner and validation pipeline to use immediately.

## Prior Art Research

Sources consulted:

- [Aider chat modes](https://aider.chat/docs/usage/modes.html) — separates ask/code/architect stages, uses an architect proposal before file-edit instructions, and treats planning as a distinct phase before execution.
- [Roo Code modes](https://docs.roocode.com/basic-usage/using-modes) — exposes architect and orchestrator modes, emphasizes task specialization, restricted edit scopes, and planning-specific interactions.
- [Claude Code skills](https://code.claude.com/docs/en/slash-commands) — supports slash-invoked reusable workflows, forked subagent execution, argumentized commands, and workflow packaging around artifacts and scripts.
- [Claude Code subagents](https://code.claude.com/docs/en/sub-agents) — shows built-in Explore and Plan agents, foreground/background execution, permission scoping, memory, and resume patterns for specialist workflows.
- [OpenHands CLI terminal mode](https://docs.openhands.dev/openhands/usage/run-openhands/cli-mode) — supports starting from task text or task files, explicit confirmation modes, pause controls, and conversation resume in a CLI workflow.

### What Prior Art Suggests

Common patterns across mature coding-agent tools:

1. Starting from a freeform task or idea is table stakes.
2. Planning or architecting is separated from code execution, either by a mode switch or a workflow command.
3. Reusable command-style workflows outperform ad hoc prompts when the task has multiple stages and artifacts.
4. Permissions and mode-specific tool restrictions are expected for planning workflows.
5. Resume-friendly artifacts matter as much as resume-friendly chat history.
6. Structured workflows need bounded loops and explicit failure states, not raw model output alone.

### Comparison Matrix

| Capability | Aider | Roo Code | Claude Code | OpenHands CLI | This Project Target |
|---|---|---|---|---|---|
| Start from freeform task text | Yes | Yes | Yes | Yes | P0 |
| Dedicated planning or architect stage | Yes | Yes | Yes | Partial | P0 |
| Reusable command or workflow entrypoint | Partial | Partial | Yes | Partial | P0 |
| Specialist workflow isolation | Partial | Yes | Yes | Partial | P1 |
| Permission and approval controls | Partial | Yes | Yes | Yes | P0 |
| Repository-written planning artifacts | Indirect | Markdown-oriented | Yes | Task-file input only | P0 |
| Bounded validation and retry loop | Partial | Partial | Prompt-driven | Partial | P0 |
| Structured machine-readable output | No | No | Limited | Limited | P0 |
| Resume from persisted artifacts | Indirect | Session mode persistence | Yes | Yes | P0 |

### Gaps Surfaced By Prior Art

The current project already has strong plan validation and repair, but it still lacks the workflow entrypoint that competitors expose in different forms:

- there is no single command that starts from a project idea,
- there is no spec-generation stage in `planner-harness`,
- there is no machine-readable workflow contract that stops after spec failure before wasting time on plan generation,
- there is no single artifact set that can be picked up by a later session without replaying the earlier reasoning.

## User Stories

- As a CLI user, I want to provide a project idea and get `SPEC.md` and `PLAN.json` so I can move directly into implementation without manually drafting the spec first.
- As a delegated planning agent, I want one workflow tool that returns exact artifact paths and validation states so I can continue autonomously after failures.
- As an automation script, I want machine-readable JSON with timings and artifact paths so I can chain follow-up steps without scraping prose.
- As a cautious user, I want overwrite controls and exact failure banners so the workflow never silently clobbers existing specs or falsely reports success.
- As a maintainer, I want the feature to fit the existing engine/bridge boundary instead of bypassing it with an ad hoc shell workflow.

## Recommended Defaults

These defaults should be implemented unless a later revision changes them explicitly.

- Public bridge command name: `build-from-idea`
- Public engine tool name: `planner_build_from_idea`
- Default outputs: `SPEC.md`, `planner-prompt.md`, `PLAN.json`
- Idea input: exactly one of inline `idea` text or `idea_path`
- Context input: optional repeated `context_paths` list, plus conservative auto-inclusion of workspace-root `AGENTS.md`, `CLAUDE.md`, `README.md`, and `API_CONTRACT.md` when present
- Spec attempts: maximum 2 generation attempts total
- Plan repair attempts: reuse the existing maximum of 2 attempts
- Validation defaults: `skip_adversary=true` for both spec and plan in the new workflow
- Default engine-tool timeout: `900000` ms
- Overwrite policy: default `overwrite=false`; if any target output exists and overwrite is not enabled, fail before generation starts

## Requirements

### Must Have (P0)

- Add a new public bridge command `build-from-idea` to `planner-harness/bridge.py`.
- Add a new public engine tool `planner_build_from_idea` to the engine planner tool surface.
- The new workflow must accept exactly one idea source:
  - inline idea text, or
  - an idea file path inside the workspace.
- The workflow must accept explicit output paths for `SPEC.md`, planner prompt, and `PLAN.json`.
- The workflow must accept optional `context_paths` so repo-specific architecture and API files can be injected into spec generation.
- The workflow must generate a planner-compatible `SPEC.md` using a new prompt template in `planner-harness/prompts/spec_generator.md`.
- The generated spec must be validated using the existing `validate-spec` bridge path before any plan generation starts.
- If spec validation fails, the workflow must perform at most one additional spec-generation attempt using structured rubric feedback from the failed validation.
- If spec validation still fails after the maximum attempts, the workflow must stop and return exact blocking checks without attempting plan generation.
- If spec validation passes, the workflow must generate `planner-prompt.md`, then generate `PLAN.json`, then validate the plan, and then run the existing plan-repair loop when needed.
- The workflow must emit structured JSON with all completed artifact paths, relative artifact paths when running through the engine tool, timings for each phase, validation objects, and attempt summaries.
- The engine tool must use the existing planner failure-banner contract and return a failed result when spec validation, plan validation, or graph validation fails.
- The engine tool must be registered in `make_default_workspace_tools()` and visible in the default workspace tool list.
- The new tool must be explicitly allowed in the engine's `plan` profile policy and in delegated Plan-agent tool filtering.
- CLI auto-read-only policies must auto-approve the new first-party workflow tool in the same way they now auto-approve the other planner workflow tools.
- All input and output paths must remain within the active workspace root.
- Partial artifacts must remain on disk for completed stages even when a later stage fails.

### Should Have (P1)

- Add a standalone `generate-spec` bridge command after the integrated workflow is stable, so spec generation can be used independently from plan generation.
- Preserve structured summaries of failed spec-generation attempts rather than only the final spec file.
- Allow an optional seed spec or requirements file to be merged into the generated `SPEC.md`.
- Add a dedicated spec-repair helper prompt rather than relying only on regeneration with rubric feedback.

### Nice To Have (P2)

- Allow the workflow to invoke the engine's higher-level spec agent for prior-art research before calling the planner bridge, while preserving a machine-readable workflow output.
- Extend `validate-spec` with architecture-spec-specific checks beyond the current code-centric rubric.
- Cache local context summaries so repeated idea-to-spec runs do not reread the same architecture files every time.

## Interactions

### Engine Tool

The primary engine interaction should look like this:

- `planner_build_from_idea` with `idea` or `idea_path`
- optional `context_paths`
- `spec_output_path`, `prompt_output_path`, `plan_output_path`
- optional `overwrite`, `model`, `skip_adversary`, and `timeout`

Expected success behavior:

- writes `SPEC.md`, `planner-prompt.md`, and `PLAN.json`
- returns JSON with validation states and timings
- includes relative artifact paths in engine output

Expected failure behavior:

- returns `PLANNER_BUILD_FROM_IDEA STATUS: FAILED`
- includes `spec_validation_passed`, `plan_validation_passed`, and `graph_validation_passed` booleans when available
- includes exact blocking checks and raw JSON payload

### Bridge CLI

The direct bridge command should support a command shape equivalent to:

```bash
python3 planner-harness/bridge.py build-from-idea \
  --idea-text "Build a FastAPI webhook relay service with retries and replay support" \
  --spec-output SPEC.md \
  --prompt-output planner-prompt.md \
  --plan-output PLAN.json \
  --skip-adversary
```

### Artifact Continuation

The resulting `SPEC.md` and `PLAN.json` must be sufficient for a later session to continue with:

- `planner_validate_spec`
- `planner_build_plan` or `planner_validate_plan`
- `planner_repair_plan`

No prior chat history should be required to understand the generated artifacts.

## Edge Cases

- Both `idea` and `idea_path` provided: fail with a clear argument error.
- Neither `idea` nor `idea_path` provided: fail with a clear argument error.
- Any `context_path` escapes the workspace root: fail before generation starts.
- Output files already exist and `overwrite=false`: fail before generation starts.
- Spec generation succeeds but spec validation fails twice: keep the generated `SPEC.md`, return blocker details, and do not generate a plan.
- Spec passes but prompt generation or plan generation times out: keep completed artifacts and return a failed workflow payload.
- Plan generation succeeds but validation fails after repairs: keep `SPEC.md`, planner prompt, and `PLAN.json`, and return the remaining blocking checks.
- A very short or vague project idea must not be reported as a successful workflow if the generated spec fails validation.
- A monorepo workspace with no root-level `README.md` or `CLAUDE.md` must still work when the caller supplies explicit `context_paths`.

## Architecture Notes

### Modules And Layers Affected

- `planner-harness` gains the new public workflow command and prompt template.
- `omniagent-engine` gains a new planner tool wrapper over that bridge command.
- Default tool registration, plan-profile explicit allow lists, delegated plan-agent filtering, and CLI auto-approval must all be updated to recognize the new tool.
- Existing run persistence, timing, and failure-banner behavior must be reused rather than replaced.

### Architectural Boundaries To Respect

- Keep the workflow machine-readable and inside `planner-harness` rather than reimplementing the planner contract with ad hoc shell commands.
- Preserve the current standalone separation of `omniagent-engine` from `omniagent-core`.
- Do not bypass current path resolution and workspace containment checks in the engine tool wrapper.
- Do not introduce GUI-specific dependencies into the engine runtime.
- Reuse existing validation and repair helpers instead of creating parallel plan-validation logic.

### Files Likely Affected

- `planner-harness/bridge.py`
- `planner-harness/prompts/spec_generator.md` (new)
- `planner-harness/tests/test_bridge.py`
- `omniagent-engine/src/tools/planner_tools.h`
- `omniagent-engine/src/tools/planner_tools.cpp`
- `omniagent-engine/src/tools/workspace_tools.cpp`
- `omniagent-engine/src/project_runtime_internal.h`
- `omniagent-engine/src/agents/agent_manager.cpp`
- `omniagent-engine/src/cli/repl_internal.h`
- `omniagent-engine/tests/test_planner_tools.cpp`
- `omniagent-engine/tests/test_cli_repl.cpp`
- `omniagent-engine/README.md`

## Validation Criteria

The following assertions must pass before this feature is considered complete.

### Concrete Assertions

- Running `python3 planner-harness/bridge.py build-from-idea --idea-text "Build a FastAPI webhook relay service with retries" --spec-output SPEC.md --prompt-output planner-prompt.md --plan-output PLAN.json --skip-adversary` returns `ok: true`, writes all three artifacts, and includes a top-level `timing.elapsed_ms` field greater than or equal to zero.
- When spec validation fails on the first attempt and passes on the second attempt, the payload includes a `spec_attempts` history showing the first failed validation and a final `spec_validation.passed == true`.
- When spec validation still fails after the maximum attempts, the payload includes `spec_validation.passed == false`, omits plan-generation success, and does not claim `workflow_passed == true`.
- `PlannerBuildFromIdeaTool` returns relative artifact paths for the generated spec, prompt, and plan when called inside an engine workspace context.
- Under the CLI auto-read-only approval policy, `planner_build_from_idea` is auto-approved when the tool is visible in the current session.

### Golden Dataset

Use at least two representative idea prompts as structural acceptance fixtures.

- Idea A: `Build a FastAPI webhook relay service with signature verification, retries, and replay support.`
  - Expected structural outcomes:
    - generated `SPEC.md` contains Summary, Requirements, File Structure, and Validation Criteria sections,
    - generated `PLAN.json` uses the phases/tasks schema,
    - plan graph validation passes.
- Idea B: `Build a CLI markdown link checker that scans a directory, validates HTTP links, and prints a summary report.`
  - Expected structural outcomes:
    - generated `SPEC.md` contains usage examples and explicit validation criteria,
    - generated `PLAN.json` covers source and test files,
    - plan validation either passes or returns exact blocking checks without false success.

### Invariants

- The workflow must never report overall success unless spec validation, plan validation, and graph validation all pass.
- If spec validation fails, plan generation must not start.
- Every written artifact path must remain within the workspace root.
- Failure payloads must preserve the artifact paths for all stages that completed.
- The engine wrapper must preserve the existing `PLANNER_* STATUS: FAILED` contract so delegated agents treat failures as failures.

### Integration Checks

- `make_default_workspace_tools()` includes `planner_build_from_idea`.
- The `plan` profile explicit allow list includes `planner_build_from_idea`.
- Delegated Plan-agent tool filtering and system guidance mention the new workflow tool.
- `pytest planner-harness/tests/test_bridge.py` passes with coverage for `build-from-idea`.
- `ctest --output-on-failure -R 'PlannerToolsTest\.|CliReplInternal|ProjectHostTest.PlanProfileAllowsPlannerToolsWithoutGenericNetworkAccess'` passes after implementation.

## Open Questions

- P0 recommendation: keep spec generation inside `planner-harness` using a new prompt template and local context files. If output quality is insufficient, P1 should evaluate composing the engine's existing Spec agent ahead of the bridge instead of widening the bridge command further.
- P1 recommendation: add a standalone `generate-spec` public command and engine tool only after `build-from-idea` is stable and well-tested.
