#!/usr/bin/env python3
"""Benchmark planner-harness build-from-idea across models and repeats."""

from __future__ import annotations

import argparse
import json
import re
import statistics
import subprocess
import sys
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any


DEFAULT_IDEA = (
    "Build a production-grade multi-tenant workflow automation platform with FastAPI. "
    "Requirements: JWT auth with RBAC, organization-level tenant isolation, webhook ingestion "
    "with HMAC signature verification and idempotency keys, DAG-based workflow engine with step "
    "dependencies, retries and compensating actions, PostgreSQL persistence with migrations, "
    "Redis-backed queues and rate limiting, outbox/inbox reliability pattern, audit log and "
    "immutable event history, admin and operator CLIs (migrate/seed/replay/reconcile), "
    "OpenTelemetry tracing plus Prometheus metrics, and pytest unit/integration tests with "
    "docker-compose local stack. PLAN CONSTRAINTS (STRICT): (1) every file path must be unique "
    "globally, (2) if any test file depends_on src/<pkg>/__init__.py then that exact "
    "src/<pkg>/__init__.py file must exist as a source entry in the plan, (3) do not create "
    "tests/test_<pkg>_init.py unless the matching src/<pkg>/__init__.py source entry exists, "
    "(4) every spec_section must be non-trivial with concrete imports + callable/class signatures "
    "+ behavior details, including init-related files, (5) every test spec_section must name exact "
    "source APIs/endpoints/classes under test, (6) keep strict forward-only dependency order "
    "across phases."
)

DEFAULT_CONTEXT_PATHS = [
    "omniagent-core/AGENTS.md",
    "omniagent-core/API_CONTRACT.md",
    "docs/architecture-and-remediation-review-2026-04-06.md",
]


@dataclass
class RunResult:
    model: str
    run_index: int
    run_dir: Path
    command: list[str]
    return_code: int
    stdout: str
    stderr: str
    payload: dict[str, Any] | None
    timed_out: bool

    @property
    def workflow_passed(self) -> bool:
        if not self.payload:
            return False
        return bool(self.payload.get("workflow_passed", False))

    @property
    def spec_passed(self) -> bool:
        if not self.payload:
            return False
        return bool(self.payload.get("spec_validation", {}).get("passed", False))

    @property
    def plan_passed(self) -> bool:
        if not self.payload:
            return False
        return bool(self.payload.get("plan_validation", {}).get("passed", False))

    @property
    def elapsed_ms(self) -> int | None:
        if not self.payload:
            return None
        value = self.payload.get("timing", {}).get("elapsed_ms")
        if isinstance(value, int):
            return value
        return None

    @property
    def repair_attempts(self) -> int:
        if not self.payload:
            return 0
        attempts = self.payload.get("repair_attempts", [])
        return len(attempts) if isinstance(attempts, list) else 0


def _slugify(text: str) -> str:
    return re.sub(r"[^a-zA-Z0-9_.-]+", "-", text).strip("-") or "model"


def _extract_json_payload(stdout: str) -> dict[str, Any] | None:
    start = stdout.find("{")
    end = stdout.rfind("}")
    if start == -1 or end == -1 or end <= start:
        return None
    blob = stdout[start : end + 1]
    try:
        parsed = json.loads(blob)
    except json.JSONDecodeError:
        return None
    return parsed if isinstance(parsed, dict) else None


def _build_command(
    model: str,
    bridge_path: Path,
    idea_text: str,
    context_paths: list[str],
    run_dir: Path,
    skip_adversary: bool,
    overwrite: bool,
    config_path: str | None,
) -> list[str]:
    spec_path = run_dir / "SPEC.md"
    prompt_path = run_dir / "planner-prompt.md"
    plan_path = run_dir / "PLAN.json"

    cmd: list[str] = [
        "python3",
        str(bridge_path),
        "build-from-idea",
        "--model",
        model,
        "--idea",
        idea_text,
        "--spec-output",
        str(spec_path),
        "--prompt-output",
        str(prompt_path),
        "--plan-output",
        str(plan_path),
    ]
    for context_path in context_paths:
        cmd.extend(["--context-path", context_path])
    if skip_adversary:
        cmd.append("--skip-adversary")
    if overwrite:
        cmd.append("--overwrite")
    if config_path:
        cmd.extend(["--config", config_path])
    return cmd


def _run_single(
    model: str,
    run_index: int,
    bridge_path: Path,
    idea_text: str,
    context_paths: list[str],
    root_output: Path,
    workspace_root: Path,
    skip_adversary: bool,
    overwrite: bool,
    config_path: str | None,
    timeout_seconds: int,
    dry_run: bool,
) -> RunResult:
    model_slug = _slugify(model)
    run_dir = root_output / f"{model_slug}-run-{run_index:02d}"
    run_dir.mkdir(parents=True, exist_ok=True)
    command = _build_command(
        model,
        bridge_path,
        idea_text,
        context_paths,
        run_dir,
        skip_adversary,
        overwrite,
        config_path,
    )

    if dry_run:
        return RunResult(
            model=model,
            run_index=run_index,
            run_dir=run_dir,
            command=command,
            return_code=0,
            stdout="",
            stderr="",
            payload=None,
            timed_out=False,
        )

    try:
        completed = subprocess.run(
            command,
            cwd=str(workspace_root),
            capture_output=True,
            text=True,
            timeout=timeout_seconds if timeout_seconds > 0 else None,
            check=False,
        )
        stdout = completed.stdout or ""
        stderr = completed.stderr or ""
        payload = _extract_json_payload(stdout)
        return RunResult(
            model=model,
            run_index=run_index,
            run_dir=run_dir,
            command=command,
            return_code=completed.returncode,
            stdout=stdout,
            stderr=stderr,
            payload=payload,
            timed_out=False,
        )
    except subprocess.TimeoutExpired as exc:
        return RunResult(
            model=model,
            run_index=run_index,
            run_dir=run_dir,
            command=command,
            return_code=124,
            stdout=exc.stdout or "",
            stderr=exc.stderr or "",
            payload=None,
            timed_out=True,
        )


def _write_run_artifacts(result: RunResult) -> None:
    command_text = " ".join(result.command)
    (result.run_dir / "command.txt").write_text(command_text + "\n", encoding="utf-8")
    (result.run_dir / "stdout.txt").write_text(result.stdout, encoding="utf-8")
    (result.run_dir / "stderr.txt").write_text(result.stderr, encoding="utf-8")
    if result.payload is not None:
        (result.run_dir / "result.json").write_text(
            json.dumps(result.payload, indent=2) + "\n",
            encoding="utf-8",
        )


def _median_elapsed_ms(results: list[RunResult]) -> float | None:
    values = [value for value in (r.elapsed_ms for r in results) if isinstance(value, int)]
    if not values:
        return None
    return statistics.median(values)


def _build_summary(results: list[RunResult]) -> dict[str, Any]:
    by_model: dict[str, list[RunResult]] = {}
    for result in results:
        by_model.setdefault(result.model, []).append(result)

    model_rows: list[dict[str, Any]] = []
    for model, model_results in by_model.items():
        total = len(model_results)
        pass_count = sum(1 for r in model_results if r.workflow_passed)
        spec_pass_count = sum(1 for r in model_results if r.spec_passed)
        plan_pass_count = sum(1 for r in model_results if r.plan_passed)
        repair_attempts = [r.repair_attempts for r in model_results if r.payload is not None]
        median_elapsed_ms = _median_elapsed_ms(model_results)

        model_rows.append(
            {
                "model": model,
                "runs": total,
                "workflow_passes": pass_count,
                "workflow_pass_rate": pass_count / total if total else 0.0,
                "spec_passes": spec_pass_count,
                "plan_passes": plan_pass_count,
                "median_elapsed_ms": median_elapsed_ms,
                "average_repair_attempts": (
                    sum(repair_attempts) / len(repair_attempts) if repair_attempts else None
                ),
                "timeouts": sum(1 for r in model_results if r.timed_out),
                "nonzero_exit_codes": sum(1 for r in model_results if r.return_code != 0),
            }
        )

    model_rows.sort(
        key=lambda row: (
            -row["workflow_pass_rate"],
            row["median_elapsed_ms"] if row["median_elapsed_ms"] is not None else float("inf"),
        )
    )

    return {
        "generated_at": datetime.now(UTC).isoformat(),
        "models": model_rows,
        "runs": [
            {
                "model": r.model,
                "run_index": r.run_index,
                "run_dir": str(r.run_dir),
                "return_code": r.return_code,
                "timed_out": r.timed_out,
                "workflow_passed": r.workflow_passed,
                "spec_passed": r.spec_passed,
                "plan_passed": r.plan_passed,
                "repair_attempts": r.repair_attempts,
                "elapsed_ms": r.elapsed_ms,
            }
            for r in results
        ],
    }


def _render_markdown(summary: dict[str, Any]) -> str:
    lines = [
        "# build-from-idea benchmark leaderboard",
        "",
        f"Generated: {summary['generated_at']}",
        "",
        "| Model | Runs | Workflow Pass Rate | Spec Passes | Plan Passes | Median Elapsed (ms) | Avg Repair Attempts |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for row in summary["models"]:
        median_elapsed = row["median_elapsed_ms"]
        avg_repair = row["average_repair_attempts"]
        lines.append(
            "| {model} | {runs} | {pass_rate:.0%} | {spec_passes} | {plan_passes} | {median} | {repair} |".format(
                model=row["model"],
                runs=row["runs"],
                pass_rate=row["workflow_pass_rate"],
                spec_passes=row["spec_passes"],
                plan_passes=row["plan_passes"],
                median="-" if median_elapsed is None else int(median_elapsed),
                repair="-" if avg_repair is None else f"{avg_repair:.2f}",
            )
        )
    lines.append("")
    return "\n".join(lines)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Benchmark planner-harness build-from-idea runs")
    parser.add_argument(
        "--model",
        dest="models",
        action="append",
        required=True,
        help="Model name to benchmark. Repeat --model for multiple models.",
    )
    parser.add_argument("--repeats", type=int, default=3, help="Runs per model")
    parser.add_argument(
        "--bridge-path",
        default="planner-harness/bridge.py",
        help="Path to bridge.py from workspace root",
    )
    parser.add_argument(
        "--workspace-root",
        default=".",
        help="Workspace root directory where the benchmark command executes",
    )
    parser.add_argument(
        "--output-root",
        default="tmp/e2e-benchmarks",
        help="Output directory under workspace root",
    )
    parser.add_argument(
        "--idea-file",
        default=None,
        help="Optional file containing the idea text",
    )
    parser.add_argument(
        "--context-path",
        dest="context_paths",
        action="append",
        default=[],
        help="Context path to add. If omitted, uses default hard-scenario context files.",
    )
    parser.add_argument("--config", default=None, help="Optional path to config.yaml")
    parser.add_argument(
        "--timeout-seconds",
        type=int,
        default=0,
        help="Per-run timeout. 0 disables timeout.",
    )
    parser.add_argument("--with-adversary", action="store_true", help="Do not skip adversary checks")
    parser.add_argument("--overwrite", action="store_true", help="Pass --overwrite to build-from-idea")
    parser.add_argument("--dry-run", action="store_true", help="Print commands without executing")
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    workspace_root = Path(args.workspace_root).resolve()
    bridge_path = (workspace_root / args.bridge_path).resolve()
    output_root = (workspace_root / args.output_root).resolve()
    timestamp = datetime.now(UTC).strftime("%Y%m%d-%H%M%S")
    session_root = output_root / f"build-from-idea-{timestamp}"
    session_root.mkdir(parents=True, exist_ok=True)

    if args.idea_file:
        idea_text = Path(args.idea_file).read_text(encoding="utf-8")
    else:
        idea_text = DEFAULT_IDEA

    context_paths = args.context_paths or DEFAULT_CONTEXT_PATHS
    skip_adversary = not args.with_adversary

    all_results: list[RunResult] = []
    total_runs = len(args.models) * args.repeats
    run_number = 0
    for model in args.models:
        for i in range(1, args.repeats + 1):
            run_number += 1
            print(f"[{run_number}/{total_runs}] model={model} run={i}", flush=True)
            result = _run_single(
                model=model,
                run_index=i,
                bridge_path=bridge_path,
                idea_text=idea_text,
                context_paths=context_paths,
                root_output=session_root,
                workspace_root=workspace_root,
                skip_adversary=skip_adversary,
                overwrite=args.overwrite,
                config_path=args.config,
                timeout_seconds=args.timeout_seconds,
                dry_run=args.dry_run,
            )
            _write_run_artifacts(result)
            all_results.append(result)

            if args.dry_run:
                status = "DRY-RUN"
            else:
                status = "PASS" if result.workflow_passed else "FAIL"
            elapsed = result.elapsed_ms
            elapsed_text = "-" if elapsed is None else f"{elapsed} ms"
            print(
                f"  => {status} spec={result.spec_passed} plan={result.plan_passed} "
                f"repairs={result.repair_attempts} elapsed={elapsed_text} rc={result.return_code}",
                flush=True,
            )

    summary = _build_summary(all_results)
    summary_path = session_root / "summary.json"
    leaderboard_path = session_root / "leaderboard.md"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    leaderboard_path.write_text(_render_markdown(summary), encoding="utf-8")

    print(f"\nSummary JSON: {summary_path}")
    print(f"Leaderboard: {leaderboard_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())