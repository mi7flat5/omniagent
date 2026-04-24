"""Machine-readable entrypoint for planner-harness integrations."""

import argparse
import json
import re
import sys
import time
from pathlib import Path


PROMPT_GENERATOR = Path(__file__).parent / "prompts" / "prompt_generator.md"
SPEC_GENERATOR = Path(__file__).parent / "prompts" / "spec_generator.md"
MAX_SPEC_GENERATION_ATTEMPTS = 2
MAX_PLAN_REPAIR_ATTEMPTS = 2
DEFAULT_ROOT_CONTEXT_FILES = (
    "AGENTS.md",
    "CLAUDE.md",
    "README.md",
    "API_CONTRACT.md",
)
CLARIFICATION_MODES = {"required", "assume", "off"}
DELEGATION_PHRASES = (
    "you decide for me",
    "decide for me",
    "your call",
    "use the recommended default",
    "use recommended defaults",
)


def _read_text(path: str) -> str:
    return Path(path).read_text(encoding="utf-8")


def _write_text(path: str, content: str) -> str:
    output_path = Path(path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(content, encoding="utf-8")
    return str(output_path.resolve())


def _workspace_root() -> Path:
    return Path.cwd().resolve()


def _resolve_workspace_path(path: str,
                            *,
                            workspace_root: Path | None = None,
                            allow_missing_leaf: bool = False) -> Path:
    root = (workspace_root or _workspace_root()).resolve()
    candidate = Path(path)
    if not candidate.is_absolute():
        candidate = root / candidate
    resolved = candidate.resolve(strict=False)
    try:
        resolved.relative_to(root)
    except ValueError as exc:
        raise ValueError(f"path escapes workspace root: {path}") from exc
    if not allow_missing_leaf and not resolved.exists():
        raise FileNotFoundError(f"path does not exist: {path}")
    return resolved


def _resolve_workspace_file(path: str,
                            *,
                            workspace_root: Path | None = None) -> Path:
    resolved = _resolve_workspace_path(path, workspace_root=workspace_root)
    if not resolved.is_file():
        raise ValueError(f"path is not a file: {path}")
    return resolved


def _resolve_workspace_output_path(path: str,
                                   *,
                                   workspace_root: Path | None = None) -> Path:
    root = (workspace_root or _workspace_root()).resolve()
    resolved = _resolve_workspace_path(
        path,
        workspace_root=root,
        allow_missing_leaf=True,
    )
    parent = resolved.parent.resolve(strict=False)
    try:
        parent.relative_to(root)
    except ValueError as exc:
        raise ValueError(f"output path escapes workspace root: {path}") from exc
    return resolved


def _relative_path(path: Path, workspace_root: Path) -> str:
    return path.resolve(strict=False).relative_to(workspace_root).as_posix()


def _elapsed_ms(started_at: float) -> int:
    return max(0, int((time.perf_counter() - started_at) * 1000))


def _attach_timing(payload: dict, started_at: float, **fields: object) -> dict:
    timing = dict(payload.get("timing", {}))
    timing["elapsed_ms"] = _elapsed_ms(started_at)
    for key, value in fields.items():
        if value is not None:
            timing[key] = value
    payload["timing"] = timing
    return payload


def _resolve_model(config: dict,
                   model_name: str | None,
                   *,
                   purpose: str) -> str:
    models = config.get("models", {})
    if not models:
        raise ValueError("config does not define any models")

    if model_name:
        if model_name in models:
            return model_name
        for key, model_cfg in models.items():
            if model_cfg.get("model") == model_name:
                return key
        raise KeyError(model_name)

    defaults = config.get("defaults", {})
    default_key = f"{purpose}_model"
    if defaults.get(default_key):
        return defaults[default_key]
    if defaults.get("planner_model") and purpose == "generation":
        return defaults["planner_model"]
    if defaults.get("adversary_model"):
        return defaults["adversary_model"]
    return next(iter(models.keys()))


def _strip_generated_text(raw: str) -> str:
    text = raw.strip()
    text = re.sub(r"<think>.*?</think>", "", text, flags=re.DOTALL).strip()
    text = re.sub(r"<\|im_end\|>|<\|endoftext\|>|<\|end\|>", "", text).strip()
    if text.startswith("```"):
        lines = text.splitlines()
        lines = [line for line in lines if not line.strip().startswith("```")]
        text = "\n".join(lines).strip()
    return text


def _dedupe_preserving_order(values: list[str]) -> list[str]:
    seen: set[str] = set()
    ordered: list[str] = []
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        ordered.append(value)
    return ordered


def _extract_python_package_inits(spec_text: str) -> list[str]:
    matches = re.findall(r"`?([A-Za-z0-9_./-]+/__init__\.py)`?", spec_text)
    normalized = [match.replace("\\", "/") for match in matches]
    return _dedupe_preserving_order(normalized)


def _required_init_test_pairs(spec_text: str) -> list[tuple[str, str]]:
    pairs: list[tuple[str, str]] = []
    for init_path in _extract_python_package_inits(spec_text):
        parent_dir = Path(init_path).parent.name
        if not parent_dir:
            continue
        pairs.append((init_path, f"tests/test_{parent_dir}_init.py"))
    return pairs


def _remove_contradictory_init_exemptions(prompt_text: str) -> str:
    filtered_lines: list[str] = []
    for line in prompt_text.splitlines():
        lower = line.lower()
        if "__init__.py" in line and (
            "no test required" in lower or "package marker" in lower
        ):
            continue
        filtered_lines.append(line)
    return "\n".join(filtered_lines).strip()


def _augment_generated_prompt(prompt_text: str, spec_text: str) -> str:
    prompt_text = _remove_contradictory_init_exemptions(prompt_text)
    init_pairs = _required_init_test_pairs(spec_text)
    if not init_pairs:
        return prompt_text

    section_lines = [
        "MANDATORY PACKAGE INIT TEST RULES",
        "These package __init__.py files DO require tests. Do not mark package init files as test-exempt.",
        "For every pair below, include both the source entry and the matching test entry in the plan:",
    ]
    for init_path, test_path in init_pairs:
        section_lines.append(f"- `{init_path}` -> `{test_path}`")
    section_lines.append(
        'Each init test file must have `"test": true` and `depends_on` including the corresponding `__init__.py` file.'
    )

    augmented = prompt_text.strip()
    if "MANDATORY PACKAGE INIT TEST RULES" not in augmented:
        augmented += "\n\n" + "\n".join(section_lines)
    return augmented


def _failed_plan_checks(stage: dict) -> list[str]:
    failures: list[str] = []
    for check in stage.get("rubric_checks", []):
        if check.get("passed", False):
            continue
        detail = check.get("detail", "").strip()
        if detail:
            failures.append(f"{check['name']}: {detail}")
        else:
            failures.append(check["name"])

    adversary = stage.get("adversary", {})
    if adversary.get("error"):
        failures.append(f"Adversary error: {adversary['error']}")
    if adversary.get("contradiction_count", 0):
        failures.append(
            f"Adversary contradictions: {adversary['contradiction_count']}"
        )
    if adversary.get("blocking_gaps", 0):
        failures.append(f"Blocking adversary gaps: {adversary['blocking_gaps']}")
    if adversary.get("blocking_guesses", 0):
        failures.append(
            f"Blocking adversary guesses: {adversary['blocking_guesses']}"
        )

    return failures


def _failed_check_names(stage: dict) -> set[str]:
    names: set[str] = set()
    for check in stage.get("rubric_checks", []):
        if check.get("passed", False):
            continue
        name = check.get("name")
        if isinstance(name, str) and name:
            names.add(name)
    return names


def _build_plan_repair_feedback(stage: dict) -> str:
    failures = _failed_plan_checks(stage)
    failed_names = _failed_check_names(stage)
    lines = [
        "The previous PLAN.json failed validation. Fix every issue below in the next plan:",
    ]
    if failures:
        lines.extend(f"- {failure}" for failure in failures)
    else:
        lines.append(
            "- The previous plan did not pass validation. Re-check file coverage, depends_on, and output schema."
        )
    lines.extend(
        [
            "- For every package __init__.py source file, include tests/test_<parent>_init.py and make that test depend_on the matching __init__.py file.",
            "- Keep package __init__.py spec_section blocks concrete and non-trivial: show exported symbols, imports, and the package surface instead of a one-line placeholder.",
            "- When a test file depends_on a source file, the test spec_section must explicitly name the source functions, endpoints, classes, or behaviors it verifies.",
            "- When a spec_section references a type or schema from another file, add an explicit import for that symbol in the same spec_section.",
            "- Preserve the exact phases/tasks/file schema when you replace or add plan entries.",
        ]
    )

    if "Spec_section non-trivial" in failed_names:
        lines.append(
            "- Fix short spec_section blocks by adding concrete code structure: imports, function/class signatures, returned values, and exported symbols."
        )
    if "Test spec matches source spec" in failed_names:
        lines.append(
            "- Fix test/source mismatches by naming the exact source APIs in the test spec_section, for example `test_login_returns_token` for auth login handlers."
        )
    if "Import completeness" in failed_names:
        lines.append(
            "- Fix missing imports by adding explicit `from ... import Symbol` lines for every cross-file class, schema, or alias used in a spec_section."
        )

    return "\n".join(lines)


def _phase_task_key(phase: dict) -> str:
    if "tasks" in phase:
        return "tasks"
    if "files" in phase:
        return "files"
    phase["tasks"] = []
    return "tasks"


def _phase_tasks(phase: dict) -> list[dict]:
    key = _phase_task_key(phase)
    phase.setdefault(key, [])
    return phase[key]


def _phase_sort_key(phase: dict) -> int:
    try:
        return int(phase.get("phase", 0))
    except (TypeError, ValueError):
        return 0


def _coerce_phase_number(value, *, field_name: str) -> int:
    try:
        return int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"'{field_name}' must be an integer") from exc


def _task_file(task: dict) -> str:
    file_path = task.get("file", task.get("path"))
    if not isinstance(file_path, str) or not file_path.strip():
        raise ValueError("task is missing a non-empty 'file' field")
    return file_path


def _normalize_task(task: dict) -> dict:
    if not isinstance(task, dict):
        raise ValueError("task must be an object")
    normalized = dict(task)
    if "file" not in normalized and "path" in normalized:
        normalized["file"] = normalized.pop("path")
    _task_file(normalized)
    return normalized


def _build_task_index(plan_json: dict) -> dict[str, tuple[dict, list[dict], int]]:
    index: dict[str, tuple[dict, list[dict], int]] = {}
    for phase in plan_json.get("phases", []):
        tasks = _phase_tasks(phase)
        for task_index, task in enumerate(tasks):
            file_path = _task_file(task)
            if file_path in index:
                raise ValueError(f"PLAN.json contains duplicate task entries for '{file_path}'")
            index[file_path] = (phase, tasks, task_index)
    return index


def _get_or_create_phase(plan_json: dict,
                         phase_number: int,
                         *,
                         phase_name: str | None = None) -> dict:
    phases = plan_json.setdefault("phases", [])
    for phase in phases:
        if _phase_sort_key(phase) == phase_number:
            if phase_name and not phase.get("name"):
                phase["name"] = phase_name
            return phase

    new_phase = {
        "phase": phase_number,
        "name": phase_name or f"Phase {phase_number}",
        "tasks": [],
    }
    phases.append(new_phase)
    phases.sort(key=_phase_sort_key)
    return new_phase


def _plan_patch_operations(plan_patch) -> list[dict]:
    if isinstance(plan_patch, list):
        operations = plan_patch
    elif isinstance(plan_patch, dict):
        operations = plan_patch.get("operations")
    else:
        raise ValueError("plan patch must be an object or a list of operations")

    if not isinstance(operations, list):
        raise ValueError("plan patch must contain an 'operations' list")
    return operations


def _apply_plan_patch(plan_json: dict, plan_patch) -> dict:
    operations = _plan_patch_operations(plan_patch)

    for op_index, operation in enumerate(operations, start=1):
        if not isinstance(operation, dict):
            raise ValueError(f"patch operation {op_index} must be an object")

        op_name = operation.get("op")
        if not isinstance(op_name, str) or not op_name:
            raise ValueError(f"patch operation {op_index} is missing a non-empty 'op' field")

        task_index = _build_task_index(plan_json)

        if op_name == "move_task":
            file_path = operation.get("file")
            if not isinstance(file_path, str):
                raise ValueError(f"move_task operation {op_index} is missing 'file'")
            target_phase = _coerce_phase_number(operation.get("target_phase"), field_name="target_phase")
            if file_path not in task_index:
                raise ValueError(f"move_task operation {op_index} references unknown file '{file_path}'")

            _, source_tasks, task_position = task_index[file_path]
            task = source_tasks.pop(task_position)
            target = _get_or_create_phase(
                plan_json,
                target_phase,
                phase_name=operation.get("target_phase_name"),
            )
            _phase_tasks(target).append(task)
        elif op_name == "replace_task":
            file_path = operation.get("file")
            if not isinstance(file_path, str):
                raise ValueError(f"replace_task operation {op_index} is missing 'file'")
            if file_path not in task_index:
                raise ValueError(f"replace_task operation {op_index} references unknown file '{file_path}'")

            replacement = _normalize_task(operation.get("task"))
            replacement_file = _task_file(replacement)
            if replacement_file != file_path:
                raise ValueError(
                    f"replace_task operation {op_index} must keep the same file path: '{file_path}'"
                )

            _, tasks, task_position = task_index[file_path]
            tasks[task_position] = replacement
        elif op_name == "add_task":
            task = _normalize_task(operation.get("task"))
            file_path = _task_file(task)
            target_phase = _coerce_phase_number(operation.get("phase"), field_name="phase")
            if file_path in task_index:
                existing_phase, existing_tasks, task_position = task_index[file_path]
                existing_tasks.pop(task_position)
                target = _get_or_create_phase(
                    plan_json,
                    target_phase,
                    phase_name=operation.get("phase_name"),
                )
                _phase_tasks(target).append(task)
            else:
                target = _get_or_create_phase(
                    plan_json,
                    target_phase,
                    phase_name=operation.get("phase_name"),
                )
                _phase_tasks(target).append(task)
        elif op_name == "remove_task":
            file_path = operation.get("file")
            if not isinstance(file_path, str):
                raise ValueError(f"remove_task operation {op_index} is missing 'file'")
            if file_path not in task_index:
                raise ValueError(f"remove_task operation {op_index} references unknown file '{file_path}'")

            _, tasks, task_position = task_index[file_path]
            tasks.pop(task_position)
        else:
            raise ValueError(f"unsupported plan patch operation '{op_name}'")

    plan_json.setdefault("phases", []).sort(key=_phase_sort_key)
    return plan_json


def _build_plan_patch_request(spec_text: str,
                              plan_json: dict,
                              repair_feedback: str) -> str:
    patch_schema = {
        "operations": [
            {
                "op": "move_task",
                "file": "path/to/file.py",
                "target_phase": 1,
                "target_phase_name": "Existing or New Phase Name",
            },
            {
                "op": "replace_task",
                "file": "path/to/file.py",
                "task": {
                    "file": "path/to/file.py",
                    "description": "Updated description",
                    "spec_section": "Updated code snippet or implementation contract",
                    "depends_on": [],
                    "agent_type": "coder",
                    "test_strategy": "unit",
                },
            },
            {
                "op": "add_task",
                "phase": 1,
                "phase_name": "Existing or New Phase Name",
                "task": {
                    "file": "path/to/new_file.py",
                    "description": "New task description",
                    "spec_section": "New code snippet or implementation contract",
                    "depends_on": [],
                    "agent_type": "coder",
                    "test_strategy": "unit",
                },
            },
            {
                "op": "remove_task",
                "file": "path/to/file.py",
            },
        ]
    }

    instructions = [
        "Repair the current PLAN.json using the smallest possible set of patch operations.",
        "Return only JSON. Do not return the full corrected plan.",
        "Use move_task when only the phase placement changes.",
        "Use replace_task when an existing task's description, spec_section, or dependencies must change.",
        "Use add_task only for missing files, and include a complete task object when you add or replace tasks.",
        "Do not emit operations for files that do not need to change.",
        repair_feedback,
        "Patch schema:",
        json.dumps(patch_schema, indent=2),
        "SPEC.md:",
        spec_text,
        "Current PLAN.json:",
        json.dumps(plan_json, separators=(",", ":")),
    ]
    return "\n\n".join(instructions)


def _serialize_check(check) -> dict:
    return {
        "name": check.name,
        "weight": check.weight,
        "passed": check.passed,
        "detail": check.detail,
    }


def _serialize_gap(gap) -> dict:
    return {
        "quote": gap.quote,
        "question": gap.question,
        "severity": gap.severity,
    }


def _serialize_guess(guess) -> dict:
    return {
        "what": guess.what,
        "why": guess.why,
        "severity": guess.severity,
        "file_path": guess.file_path,
    }


def _serialize_contradiction(contradiction) -> dict:
    return {
        "description": contradiction.description,
        "file_a": contradiction.file_a,
        "file_a_says": contradiction.file_a_says,
        "file_b": contradiction.file_b,
        "file_b_says": contradiction.file_b_says,
    }


def _normalize_clarification_mode(value: str | None) -> str:
    mode = (value or "required").strip().lower()
    if mode not in CLARIFICATION_MODES:
        raise ValueError(
            f"invalid clarification_mode '{value}' (expected one of: required, assume, off)"
        )
    return mode


def _extract_stage_clarifications(stage: dict,
                                  *,
                                  stage_name: str) -> list[dict]:
    if not isinstance(stage, dict):
        return []

    adversary = stage.get("adversary")
    if not isinstance(adversary, dict):
        return []

    clarifications: list[dict] = []

    for index, gap in enumerate(adversary.get("gaps", []), start=1):
        if not isinstance(gap, dict):
            continue
        clarifications.append(
            {
                "id": f"{stage_name}-clar-gap-{index:03d}",
                "stage": stage_name,
                "kind": "gap",
                "severity": str(gap.get("severity", "COSMETIC")).upper(),
                "quote": str(gap.get("quote", "")),
                "question": str(gap.get("question", "")).strip(),
                "recommended_default": "Use the strictest deterministic interpretation and record it as an explicit assumption.",
                "answer_type": "text",
                "options": [],
            }
        )

    for index, guess in enumerate(adversary.get("guesses", []), start=1):
        if not isinstance(guess, dict):
            continue
        what = str(guess.get("what", "")).strip()
        why = str(guess.get("why", "")).strip()
        question = what if not why else f"{what} ({why})"
        clarifications.append(
            {
                "id": f"{stage_name}-clar-guess-{index:03d}",
                "stage": stage_name,
                "kind": "guess",
                "severity": str(guess.get("severity", "COSMETIC")).upper(),
                "quote": "",
                "question": question,
                "recommended_default": "Choose the lowest-risk implementation default and capture it in assumptions.",
                "answer_type": "text",
                "options": [],
            }
        )

    for index, contradiction in enumerate(adversary.get("contradictions", []), start=1):
        if not isinstance(contradiction, dict):
            continue
        description = str(contradiction.get("description", "")).strip()
        file_a = str(contradiction.get("file_a", "")).strip()
        file_b = str(contradiction.get("file_b", "")).strip()
        question = description or f"Resolve contradiction between {file_a} and {file_b}."
        clarifications.append(
            {
                "id": f"{stage_name}-clar-contradiction-{index:03d}",
                "stage": stage_name,
                "kind": "contradiction",
                "severity": "BLOCKING",
                "quote": question,
                "question": question,
                "recommended_default": "Prefer consistency with dependency order and explicit API/import contracts.",
                "answer_type": "text",
                "options": [],
            }
        )

    deduped: list[dict] = []
    seen: set[tuple[str, str, str, str]] = set()
    for item in clarifications:
        key = (
            item["stage"],
            item.get("kind", ""),
            item.get("question", "").strip().lower(),
            item.get("quote", "").strip().lower(),
        )
        if key in seen:
            continue
        seen.add(key)
        deduped.append(item)
    return deduped


def _normalize_answer_value(value) -> str:
    if value is None:
        return ""
    return str(value).strip()


def _normalize_answers_payload(raw_answers) -> dict[str, str]:
    if raw_answers is None:
        return {}

    if isinstance(raw_answers, dict):
        normalized: dict[str, str] = {}
        for key, value in raw_answers.items():
            answer = _normalize_answer_value(value)
            if not answer:
                continue
            normalized[str(key)] = answer
        return normalized

    if isinstance(raw_answers, list):
        normalized: dict[str, str] = {}
        for entry in raw_answers:
            if not isinstance(entry, dict):
                continue
            answer_id = str(entry.get("id", "")).strip()
            answer = _normalize_answer_value(entry.get("value"))
            if not answer_id or not answer:
                continue
            normalized[answer_id] = answer
        return normalized

    raise ValueError("clarification answers must be an object or array of {id, value}")


def _parse_answers_text(text: str | None,
                        clarifications: list[dict]) -> dict:
    response = {
        "answers": {},
        "delegate_all": False,
        "ambiguous": False,
    }
    if text is None:
        return response

    raw = text.strip()
    if not raw:
        return response

    lower = raw.lower()
    response["delegate_all"] = any(phrase in lower for phrase in DELEGATION_PHRASES)

    if not clarifications:
        return response

    ids = [item["id"] for item in clarifications]
    id_union = "|".join(re.escape(answer_id) for answer_id in ids)
    by_id_pattern = re.compile(
        rf"({id_union})\s*[:=]\s*(.*?)(?=(?:{id_union})\s*[:=]|$)",
        re.IGNORECASE | re.DOTALL,
    )
    for match in by_id_pattern.finditer(raw):
        answer_id = match.group(1)
        value = match.group(2).strip(" \t\n,;.")
        if value:
            response["answers"][answer_id] = value

    if response["answers"]:
        return response

    if len(clarifications) == 1 and not response["delegate_all"]:
        response["answers"][clarifications[0]["id"]] = raw
        return response

    if response["delegate_all"]:
        return response

    segments = [
        segment.strip(" \t\n,;.")
        for segment in re.split(r"\s*;\s*|\s*\|\s*|\s*\n\s*", raw)
        if segment.strip()
    ]
    if len(segments) == len(clarifications):
        for item, segment in zip(clarifications, segments):
            response["answers"][item["id"]] = segment
        return response

    response["ambiguous"] = True
    return response


def _resolve_clarification_answers(clarifications: list[dict],
                                   *,
                                   structured_answers: dict[str, str],
                                   answers_text: str | None,
                                   delegate_unanswered: bool,
                                   clarification_mode: str) -> dict:
    text_parse = _parse_answers_text(answers_text, clarifications)
    answers = dict(text_parse["answers"])
    answers.update(structured_answers)

    answered_ids: list[str] = []
    assumptions: list[dict] = []
    unresolved_blocking_ids: list[str] = []

    for item in clarifications:
        answer_id = item["id"]
        answer_value = answers.get(answer_id, "").strip()
        if answer_value:
            answered_ids.append(answer_id)
            continue

        should_delegate = (
            clarification_mode == "assume"
            or delegate_unanswered
            or text_parse["delegate_all"]
        )
        if should_delegate:
            assumptions.append(
                {
                    "id": answer_id,
                    "stage": item.get("stage"),
                    "question": item.get("question", ""),
                    "value": item.get("recommended_default", ""),
                    "source": "recommended_default",
                }
            )
            continue

        if str(item.get("severity", "COSMETIC")).upper() == "BLOCKING":
            unresolved_blocking_ids.append(answer_id)

    return {
        "answers": answers,
        "answered_ids": answered_ids,
        "assumptions": assumptions,
        "unresolved_blocking_ids": unresolved_blocking_ids,
        "ambiguous_freeform": text_parse["ambiguous"],
        "delegate_all": text_parse["delegate_all"],
    }


def _clarification_feedback_lines(clarifications: list[dict],
                                  resolution: dict) -> list[str]:
    id_to_question = {item["id"]: item.get("question", "") for item in clarifications}
    lines: list[str] = []
    for answer_id in resolution.get("answered_ids", []):
        answer_text = resolution["answers"].get(answer_id, "")
        question = id_to_question.get(answer_id, "")
        lines.append(f"- {answer_id}: {question} -> {answer_text}")

    for assumption in resolution.get("assumptions", []):
        lines.append(
            f"- {assumption['id']}: {assumption['question']} -> "
            f"{assumption['value']} (delegated default)"
        )
    return lines


def _build_clarification_result(*,
                                command: str,
                                clarifications: list[dict],
                                resolution: dict,
                                clarification_mode: str,
                                message: str = "") -> dict:
    unresolved_ids = resolution.get("unresolved_blocking_ids", [])
    clarification_required = bool(unresolved_ids)
    return {
        "command": command,
        "clarification_mode": clarification_mode,
        "clarification_required": clarification_required,
        "clarifications": clarifications,
        "clarification_answers": resolution.get("answers", {}),
        "clarification_answered_ids": resolution.get("answered_ids", []),
        "clarification_assumptions": resolution.get("assumptions", []),
        "pending_clarification_ids": unresolved_ids,
        "clarification_parse_ambiguous": resolution.get("ambiguous_freeform", False),
        "clarification_message": message,
    }


def _attach_clarification_fields(payload: dict, clarification: dict) -> dict:
    payload["clarification"] = clarification
    payload["clarification_required"] = clarification.get("clarification_required", False)
    payload["clarifications"] = clarification.get("clarifications", [])
    payload["pending_clarification_ids"] = clarification.get("pending_clarification_ids", [])
    return payload


def _serialize_stage(stage_result, *, adversary_skipped: bool, adversary_error: str = "") -> dict:
    blocking_gaps = sum(1 for gap in stage_result.adversary_gaps if gap.severity == "BLOCKING")
    blocking_guesses = sum(1 for guess in stage_result.adversary_guesses if guess.severity == "BLOCKING")
    passed = all(check.passed for check in stage_result.rubric_checks)
    if not adversary_skipped:
        passed = passed and not adversary_error and blocking_gaps == 0 and blocking_guesses == 0 and not stage_result.contradictions

    return {
        "passed": passed,
        "rubric_score": stage_result.rubric_score,
        "adversary_score": stage_result.adversary_score,
        "combined_score": stage_result.combined_score,
        "rubric_checks": [_serialize_check(check) for check in stage_result.rubric_checks],
        "adversary": {
            "skipped": adversary_skipped,
            "error": adversary_error,
            "blocking_gaps": blocking_gaps,
            "blocking_guesses": blocking_guesses,
            "contradiction_count": len(stage_result.contradictions),
            "gaps": [_serialize_gap(gap) for gap in stage_result.adversary_gaps],
            "guesses": [_serialize_guess(guess) for guess in stage_result.adversary_guesses],
            "contradictions": [_serialize_contradiction(c) for c in stage_result.contradictions],
        },
    }


def _load_case_json(case_path: str) -> dict:
    case_data = json.loads(_read_text(case_path))
    if not isinstance(case_data, dict):
        raise ValueError(f"case file must contain a JSON object: {case_path}")
    return case_data


def _load_config_and_model(config_path: str | None,
                           model_name: str | None,
                           *,
                           purpose: str) -> tuple[dict, str]:
    from models import load_config

    config = load_config(config_path)
    return config, _resolve_model(config, model_name, purpose=purpose)


def _resolve_idea_text(idea: str | None,
                       idea_path: str | None,
                       *,
                       workspace_root: Path) -> tuple[str, str | None]:
    has_idea_text = bool(idea and idea.strip())
    has_idea_path = bool(idea_path and idea_path.strip())
    if has_idea_text == has_idea_path:
        raise ValueError("exactly one of 'idea' or 'idea_path' is required")

    if has_idea_text:
        assert idea is not None
        return idea.strip(), None

    assert idea_path is not None
    resolved_idea_path = _resolve_workspace_file(idea_path, workspace_root=workspace_root)
    idea_text = _read_text(str(resolved_idea_path)).strip()
    if not idea_text:
        raise ValueError(f"idea file is empty: {idea_path}")
    return idea_text, str(resolved_idea_path)


def _resolve_context_paths(context_paths: list[str] | None,
                           *,
                           workspace_root: Path) -> list[Path]:
    requested = list(context_paths or [])
    for root_file in DEFAULT_ROOT_CONTEXT_FILES:
        candidate = workspace_root / root_file
        if candidate.is_file():
            requested.append(str(candidate))

    resolved: list[Path] = []
    seen: set[str] = set()
    for requested_path in requested:
        context_path = _resolve_workspace_file(requested_path, workspace_root=workspace_root)
        normalized = str(context_path)
        if normalized in seen:
            continue
        seen.add(normalized)
        resolved.append(context_path)
    return resolved


def _render_context_bundle(context_paths: list[Path], *, workspace_root: Path) -> str:
    if not context_paths:
        return "No additional workspace context files were provided."

    blocks: list[str] = []
    for context_path in context_paths:
        relative = _relative_path(context_path, workspace_root)
        blocks.append(
            f"## Context file: {relative}\n"
            f"```\n{_read_text(str(context_path)).strip()}\n```"
        )
    return "\n\n".join(blocks)


def _resolve_output_paths(spec_output_path: str,
                          prompt_output_path: str,
                          plan_output_path: str,
                          *,
                          workspace_root: Path,
                          overwrite: bool) -> tuple[Path, Path, Path]:
    spec_path = _resolve_workspace_output_path(spec_output_path, workspace_root=workspace_root)
    prompt_path = _resolve_workspace_output_path(prompt_output_path, workspace_root=workspace_root)
    plan_path = _resolve_workspace_output_path(plan_output_path, workspace_root=workspace_root)
    resolved_outputs = [spec_path, prompt_path, plan_path]

    normalized = {str(path) for path in resolved_outputs}
    if len(normalized) != len(resolved_outputs):
        raise ValueError("spec_output_path, prompt_output_path, and plan_output_path must be distinct")

    if not overwrite:
        for output_path in resolved_outputs:
            if output_path.exists():
                raise FileExistsError(
                    f"output path already exists and overwrite is false: {output_path}"
                )

    return spec_path, prompt_path, plan_path


def _spec_validation_failures(stage: dict) -> list[str]:
    failures: list[str] = []
    for check in stage.get("rubric_checks", []):
        if check.get("passed", False):
            continue
        detail = check.get("detail", "").strip()
        if detail:
            failures.append(f"{check['name']}: {detail}")
        else:
            failures.append(check["name"])

    adversary = stage.get("adversary", {})
    if adversary.get("error"):
        failures.append(f"Adversary error: {adversary['error']}")
    if adversary.get("blocking_gaps", 0):
        failures.append(f"Blocking adversary gaps: {adversary['blocking_gaps']}")
    if adversary.get("blocking_guesses", 0):
        failures.append(f"Blocking adversary guesses: {adversary['blocking_guesses']}")
    if adversary.get("contradiction_count", 0):
        failures.append(f"Adversary contradictions: {adversary['contradiction_count']}")
    return failures


def _build_spec_repair_feedback(stage: dict) -> str:
    failures = _spec_validation_failures(stage)
    lines = [
        "The previous SPEC.md failed validation. Fix every blocker below in the next attempt:",
    ]
    if failures:
        lines.extend(f"- {failure}" for failure in failures)
    else:
        lines.append("- Validation failed without a detailed blocker. Tighten specificity and rubric compliance.")

    lines.extend([
        "- Keep language strict and deterministic. Do not use words like should, may, typically, optionally, might, etc., or for example.",
        "- Include concrete typed Python signatures with return annotations in code blocks.",
        "- Add explicit usage examples that call public functions.",
        "- Document edge-case behavior and explicit exceptions for invalid inputs.",
        "- Keep import paths explicit for every cross-file symbol used in code examples.",
    ])
    return "\n".join(lines)


def _build_spec_generation_request(idea_text: str,
                                   context_bundle: str,
                                   validation_feedback: str | None = None) -> str:
    sections = [
        "Project idea:",
        idea_text,
        "Workspace context:",
        context_bundle,
    ]
    if validation_feedback:
        sections.extend([
            "Validation feedback from the previous failed SPEC.md attempt:",
            validation_feedback,
        ])
    return "\n\n".join(sections)


def _generate_spec_from_idea(idea_text: str,
                             output_path: str,
                             *,
                             context_paths: list[Path],
                             workspace_root: Path,
                             config_path: str | None,
                             model_name: str | None,
                             validation_feedback: str | None = None) -> dict:
    from models import call_llm, request_timeout_seconds

    started_at = time.perf_counter()
    config, model = _load_config_and_model(config_path, model_name, purpose="generation")
    timeout_seconds = request_timeout_seconds(config, model)
    system_prompt = _read_text(str(SPEC_GENERATOR))
    context_bundle = _render_context_bundle(context_paths, workspace_root=workspace_root)
    request_text = _build_spec_generation_request(
        idea_text,
        context_bundle,
        validation_feedback=validation_feedback,
    )

    llm_started_at = time.perf_counter()
    raw = call_llm(config, model, system_prompt, request_text)
    llm_elapsed_ms = _elapsed_ms(llm_started_at)
    spec_text = _strip_generated_text(raw)
    resolved_output = _write_text(output_path, spec_text)

    return _attach_timing(
        {
            "ok": True,
            "command": "generate-spec",
            "model": model,
            "output_path": resolved_output,
            "idea_characters": len(idea_text),
            "context_paths": [
                _relative_path(context_path, workspace_root)
                for context_path in context_paths
            ],
            "characters": len(spec_text),
        },
        started_at,
        llm_ms=llm_elapsed_ms,
        model_timeout_seconds=timeout_seconds,
    )


def _validate_spec(spec_path: str,
                   *,
                   config_path: str | None,
                   model_name: str | None,
                   skip_adversary: bool) -> dict:
    from scoring import StageResult
    from models import request_timeout_seconds
    from validators.spec_adversary import run_adversary as spec_adversary
    from validators.spec_rubric import validate_spec as spec_rubric

    started_at = time.perf_counter()
    spec_text = _read_text(spec_path)
    rubric_started_at = time.perf_counter()
    rubric_result = spec_rubric(spec_text)
    rubric_elapsed_ms = _elapsed_ms(rubric_started_at)
    adversary_error = ""
    adversary_skipped = skip_adversary
    adversary_result = StageResult()
    adversary_elapsed_ms = 0
    adversary_model = None
    adversary_timeout_seconds = None

    if not skip_adversary:
        try:
            config, model = _load_config_and_model(config_path, model_name, purpose="adversary")
            adversary_model = model
            adversary_timeout_seconds = request_timeout_seconds(config, model)
            adversary_started_at = time.perf_counter()
            adversary_result = spec_adversary(config, model, spec_text)
            adversary_elapsed_ms = _elapsed_ms(adversary_started_at)
        except Exception as exc:  # pragma: no cover - exercised via JSON result path
            adversary_error = str(exc)

    combined = StageResult(
        rubric_checks=rubric_result.rubric_checks,
        adversary_gaps=adversary_result.adversary_gaps,
    )
    return _attach_timing({
        "ok": True,
        "command": "validate-spec",
        "spec_path": str(Path(spec_path).resolve()),
        "stage": _serialize_stage(
            combined,
            adversary_skipped=adversary_skipped,
            adversary_error=adversary_error,
        ),
    },
        started_at,
        rubric_ms=rubric_elapsed_ms,
        adversary_ms=adversary_elapsed_ms,
        adversary_model=adversary_model,
        adversary_timeout_seconds=adversary_timeout_seconds,
    )


def _validate_plan(spec_path: str,
                   plan_path: str,
                   *,
                   config_path: str | None,
                   model_name: str | None,
                   skip_adversary: bool) -> dict:
    from scoring import StageResult
    from models import request_timeout_seconds
    from validators.plan_adversary import run_adversary as plan_adversary
    from validators.plan_rubric import validate_plan as plan_rubric

    started_at = time.perf_counter()
    spec_text = _read_text(spec_path)
    plan_json = json.loads(_read_text(plan_path))
    rubric_started_at = time.perf_counter()
    rubric_result = plan_rubric(spec_text, plan_json)
    rubric_elapsed_ms = _elapsed_ms(rubric_started_at)
    adversary_error = ""
    adversary_skipped = skip_adversary
    adversary_result = StageResult()
    adversary_elapsed_ms = 0
    adversary_model = None
    adversary_timeout_seconds = None

    if not skip_adversary:
        try:
            config, model = _load_config_and_model(config_path, model_name, purpose="adversary")
            adversary_model = model
            adversary_timeout_seconds = request_timeout_seconds(config, model)
            adversary_started_at = time.perf_counter()
            adversary_result = plan_adversary(config, model, plan_json)
            adversary_elapsed_ms = _elapsed_ms(adversary_started_at)
        except Exception as exc:  # pragma: no cover - exercised via JSON result path
            adversary_error = str(exc)

    combined = StageResult(
        rubric_checks=rubric_result.rubric_checks,
        adversary_guesses=adversary_result.adversary_guesses,
        contradictions=adversary_result.contradictions,
    )
    return _attach_timing({
        "ok": True,
        "command": "validate-plan",
        "spec_path": str(Path(spec_path).resolve()),
        "plan_path": str(Path(plan_path).resolve()),
        "stage": _serialize_stage(
            combined,
            adversary_skipped=adversary_skipped,
            adversary_error=adversary_error,
        ),
    },
        started_at,
        rubric_ms=rubric_elapsed_ms,
        adversary_ms=adversary_elapsed_ms,
        adversary_model=adversary_model,
        adversary_timeout_seconds=adversary_timeout_seconds,
    )


def _validate_review(case_path: str,
                     report_path: str,
                     *,
                     config_path: str | None,
                     model_name: str | None,
                     skip_adversary: bool) -> dict:
    from scoring import StageResult
    from models import request_timeout_seconds
    from validators.review_adversary import run_adversary as review_adversary
    from validators.review_rubric import validate_review as review_rubric

    started_at = time.perf_counter()
    case_data = _load_case_json(case_path)
    report_text = _read_text(report_path)
    rubric_started_at = time.perf_counter()
    rubric_result = review_rubric(report_text, case_data)
    rubric_elapsed_ms = _elapsed_ms(rubric_started_at)
    adversary_error = ""
    adversary_skipped = skip_adversary
    adversary_result = StageResult()
    adversary_elapsed_ms = 0
    adversary_model = None
    adversary_timeout_seconds = None

    if not skip_adversary:
        try:
            config, model = _load_config_and_model(config_path, model_name, purpose="adversary")
            adversary_model = model
            adversary_timeout_seconds = request_timeout_seconds(config, model)
            adversary_started_at = time.perf_counter()
            adversary_result = review_adversary(config, model, case_data, report_text)
            adversary_elapsed_ms = _elapsed_ms(adversary_started_at)
        except Exception as exc:  # pragma: no cover - exercised via JSON result path
            adversary_error = str(exc)

    combined = StageResult(
        rubric_checks=rubric_result.rubric_checks,
        adversary_gaps=adversary_result.adversary_gaps,
        adversary_guesses=adversary_result.adversary_guesses,
    )
    return _attach_timing({
        "ok": True,
        "command": "validate-review",
        "case_id": case_data.get("id", ""),
        "case_kind": case_data.get("kind", "review"),
        "case_path": str(Path(case_path).resolve()),
        "report_path": str(Path(report_path).resolve()),
        "stage": _serialize_stage(
            combined,
            adversary_skipped=adversary_skipped,
            adversary_error=adversary_error,
        ),
    },
        started_at,
        rubric_ms=rubric_elapsed_ms,
        adversary_ms=adversary_elapsed_ms,
        adversary_model=adversary_model,
        adversary_timeout_seconds=adversary_timeout_seconds,
    )


def _validate_bugfix(case_path: str,
                     report_path: str,
                     *,
                     config_path: str | None,
                     model_name: str | None,
                     skip_adversary: bool) -> dict:
    from scoring import StageResult
    from models import request_timeout_seconds
    from validators.bugfix_adversary import run_adversary as bugfix_adversary
    from validators.bugfix_rubric import validate_bugfix as bugfix_rubric

    started_at = time.perf_counter()
    case_data = _load_case_json(case_path)
    report_text = _read_text(report_path)
    rubric_started_at = time.perf_counter()
    rubric_result = bugfix_rubric(report_text, case_data)
    rubric_elapsed_ms = _elapsed_ms(rubric_started_at)
    adversary_error = ""
    adversary_skipped = skip_adversary
    adversary_result = StageResult()
    adversary_elapsed_ms = 0
    adversary_model = None
    adversary_timeout_seconds = None

    if not skip_adversary:
        try:
            config, model = _load_config_and_model(config_path, model_name, purpose="adversary")
            adversary_model = model
            adversary_timeout_seconds = request_timeout_seconds(config, model)
            adversary_started_at = time.perf_counter()
            adversary_result = bugfix_adversary(config, model, case_data, report_text)
            adversary_elapsed_ms = _elapsed_ms(adversary_started_at)
        except Exception as exc:  # pragma: no cover - exercised via JSON result path
            adversary_error = str(exc)

    combined = StageResult(
        rubric_checks=rubric_result.rubric_checks,
        adversary_gaps=adversary_result.adversary_gaps,
        adversary_guesses=adversary_result.adversary_guesses,
    )
    return _attach_timing({
        "ok": True,
        "command": "validate-bugfix",
        "case_id": case_data.get("id", ""),
        "case_kind": case_data.get("kind", "bugfix"),
        "case_path": str(Path(case_path).resolve()),
        "report_path": str(Path(report_path).resolve()),
        "stage": _serialize_stage(
            combined,
            adversary_skipped=adversary_skipped,
            adversary_error=adversary_error,
        ),
    },
        started_at,
        rubric_ms=rubric_elapsed_ms,
        adversary_ms=adversary_elapsed_ms,
        adversary_model=adversary_model,
        adversary_timeout_seconds=adversary_timeout_seconds,
    )


def _generate_prompt(spec_path: str,
                     output_path: str,
                     *,
                     config_path: str | None,
                     model_name: str | None) -> dict:
    from models import call_llm, request_timeout_seconds

    started_at = time.perf_counter()
    config, model = _load_config_and_model(config_path, model_name, purpose="generation")
    timeout_seconds = request_timeout_seconds(config, model)
    spec_text = _read_text(spec_path)
    meta_prompt = _read_text(str(PROMPT_GENERATOR))
    llm_started_at = time.perf_counter()
    raw = call_llm(config, model, meta_prompt, spec_text)
    llm_elapsed_ms = _elapsed_ms(llm_started_at)
    prompt_text = _augment_generated_prompt(_strip_generated_text(raw), spec_text)
    resolved_output = _write_text(output_path, prompt_text)

    return _attach_timing({
        "ok": True,
        "command": "generate-prompt",
        "model": model,
        "spec_path": str(Path(spec_path).resolve()),
        "output_path": resolved_output,
        "characters": len(prompt_text),
    },
        started_at,
        llm_ms=llm_elapsed_ms,
        model_timeout_seconds=timeout_seconds,
    )


def _plan_summary(plan_json: dict) -> dict:
    if plan_json.get("phases") and isinstance(plan_json["phases"], list):
        phase_count = len(plan_json["phases"])
        task_count = sum(len(phase.get("files", phase.get("tasks", []))) for phase in plan_json["phases"])
        return {
            "format": "phases",
            "phase_count": phase_count,
            "task_count": task_count,
        }
    if plan_json.get("nodes") and isinstance(plan_json["nodes"], list):
        return {
            "format": "nodes",
            "phase_count": len(plan_json["nodes"]),
            "task_count": len(plan_json["nodes"]),
        }
    return {
        "format": "unknown",
        "phase_count": 0,
        "task_count": 0,
    }


def _generate_plan(spec_path: str,
                   prompt_path: str,
                   output_path: str,
                   *,
                   config_path: str | None,
                   model_name: str | None,
                   repair_feedback: str | None = None) -> dict:
    from models import call_llm, request_timeout_seconds

    started_at = time.perf_counter()
    config, model = _load_config_and_model(config_path, model_name, purpose="generation")
    timeout_seconds = request_timeout_seconds(config, model)
    spec_text = _read_text(spec_path)
    prompt_text = _read_text(prompt_path)
    system_prompt = "You are a project planner."
    request_parts = [prompt_text]
    if repair_feedback:
        system_prompt = "You are a project planner repairing an invalid implementation plan."
        request_parts.append(repair_feedback)
    request_parts.extend(["---", spec_text])
    llm_started_at = time.perf_counter()
    raw = call_llm(config, model, system_prompt, "\n\n".join(request_parts))
    llm_elapsed_ms = _elapsed_ms(llm_started_at)
    plan_text = _strip_generated_text(raw)
    plan_json = json.loads(plan_text)
    resolved_output = _write_text(output_path, json.dumps(plan_json, indent=2) + "\n")

    return _attach_timing({
        "ok": True,
        "command": "generate-plan",
        "model": model,
        "spec_path": str(Path(spec_path).resolve()),
        "prompt_path": str(Path(prompt_path).resolve()),
        "output_path": resolved_output,
        "summary": _plan_summary(plan_json),
    },
        started_at,
        llm_ms=llm_elapsed_ms,
        model_timeout_seconds=timeout_seconds,
    )


def _repair_plan_with_patch(spec_path: str,
                            plan_path: str,
                            output_path: str,
                            *,
                            config_path: str | None,
                            model_name: str | None,
                            repair_feedback: str) -> dict:
    from models import call_llm, request_timeout_seconds

    started_at = time.perf_counter()
    config, model = _load_config_and_model(config_path, model_name, purpose="generation")
    timeout_seconds = request_timeout_seconds(config, model)
    spec_text = _read_text(spec_path)
    plan_json = json.loads(_read_text(plan_path))
    request_text = _build_plan_patch_request(spec_text, plan_json, repair_feedback)
    llm_started_at = time.perf_counter()
    raw = call_llm(
        config,
        model,
        "You are a project planner repairing an invalid implementation plan with minimal patch operations.",
        request_text,
    )
    llm_elapsed_ms = _elapsed_ms(llm_started_at)
    plan_patch = json.loads(_strip_generated_text(raw))
    repaired_plan = _apply_plan_patch(plan_json, plan_patch)
    resolved_output = _write_text(output_path, json.dumps(repaired_plan, indent=2) + "\n")
    operation_count = len(_plan_patch_operations(plan_patch))
    summary = _plan_summary(repaired_plan)
    summary["repair_strategy"] = "patch"

    return _attach_timing({
        "ok": True,
        "command": "repair-plan",
        "model": model,
        "spec_path": str(Path(spec_path).resolve()),
        "plan_path": str(Path(plan_path).resolve()),
        "output_path": resolved_output,
        "repair_strategy": "patch",
        "patch_operation_count": operation_count,
        "summary": summary,
    },
        started_at,
        llm_ms=llm_elapsed_ms,
        model_timeout_seconds=timeout_seconds,
    )


def _repair_plan(spec_path: str,
                 plan_path: str,
                 output_path: str,
                 *,
                 config_path: str | None,
                 model_name: str | None,
                 validation_json_path: str | None,
                 repair_feedback: str | None,
                 skip_adversary: bool) -> dict:
    if not repair_feedback:
        if validation_json_path:
            validation_payload = json.loads(_read_text(validation_json_path))
            stage = validation_payload.get("stage") or validation_payload.get("plan_validation")
            if not isinstance(stage, dict):
                raise ValueError(
                    f"validation payload '{validation_json_path}' does not contain a plan stage"
                )
        else:
            validation_payload = _validate_plan(
                spec_path,
                plan_path,
                config_path=config_path,
                model_name=model_name,
                skip_adversary=skip_adversary,
            )
            stage = validation_payload["stage"]
        repair_feedback = _build_plan_repair_feedback(stage)

    return _repair_plan_with_patch(
        spec_path,
        plan_path,
        output_path,
        config_path=config_path,
        model_name=model_name,
        repair_feedback=repair_feedback,
    )


def _run_workflow(spec_path: str,
                  prompt_output: str,
                  plan_output: str,
                  *,
                  config_path: str | None,
                  model_name: str | None,
                  skip_adversary: bool,
                  clarification_mode: str,
                  clarification_answers,
                  clarification_answers_text: str | None,
                  delegate_unanswered: bool) -> dict:
    started_at = time.perf_counter()
    clarification_mode = _normalize_clarification_mode(clarification_mode)
    parsed_answers = _normalize_answers_payload(clarification_answers)

    prompt_result = _generate_prompt(
        spec_path,
        prompt_output,
        config_path=config_path,
        model_name=model_name,
    )
    plan_result = _generate_plan(
        spec_path,
        prompt_result["output_path"],
        plan_output,
        config_path=config_path,
        model_name=model_name,
    )
    initial_plan_generation_ms = plan_result.get("timing", {}).get("elapsed_ms", 0)
    spec_validation = _validate_spec(
        spec_path,
        config_path=config_path,
        model_name=model_name,
        skip_adversary=skip_adversary,
    )

    spec_clarifications = _extract_stage_clarifications(spec_validation["stage"], stage_name="spec")
    spec_resolution = _resolve_clarification_answers(
        spec_clarifications,
        structured_answers=parsed_answers,
        answers_text=clarification_answers_text,
        delegate_unanswered=delegate_unanswered,
        clarification_mode=clarification_mode,
    )
    spec_clarification = _build_clarification_result(
        command="run",
        clarifications=spec_clarifications,
        resolution=spec_resolution,
        clarification_mode=clarification_mode,
        message=(
            "One or more clarification answers could not be confidently mapped. "
            "Please answer by question id or in clearly separated segments."
            if spec_resolution["ambiguous_freeform"]
            else ""
        ),
    )

    if clarification_mode == "required" and spec_clarification["clarification_required"]:
        payload = {
            "ok": True,
            "command": "run",
            "workflow_passed": False,
            "spec_path": str(Path(spec_path).resolve()),
            "artifacts": {
                "prompt_path": prompt_result["output_path"],
                "plan_path": plan_result["output_path"],
            },
            "spec_validation": spec_validation["stage"],
            "plan_generation": plan_result["summary"],
            "timing": {
                "prompt_generation_ms": prompt_result.get("timing", {}).get("elapsed_ms", 0),
                "plan_generation_ms": initial_plan_generation_ms,
                "spec_validation_ms": spec_validation.get("timing", {}).get("elapsed_ms", 0),
                "plan_validation_ms": 0,
            },
        }
        payload = _attach_clarification_fields(payload, spec_clarification)
        return _attach_timing(payload, started_at)

    spec_feedback_lines = _clarification_feedback_lines(spec_clarifications, spec_resolution)

    plan_validation = _validate_plan(
        spec_path,
        plan_result["output_path"],
        config_path=config_path,
        model_name=model_name,
        skip_adversary=skip_adversary,
    )

    repair_attempts: list[dict] = []
    for attempt in range(1, MAX_PLAN_REPAIR_ATTEMPTS + 1):
        if plan_validation["stage"]["passed"]:
            break

        plan_clarifications = _extract_stage_clarifications(plan_validation["stage"], stage_name="plan")
        plan_resolution = _resolve_clarification_answers(
            plan_clarifications,
            structured_answers=parsed_answers,
            answers_text=clarification_answers_text,
            delegate_unanswered=delegate_unanswered,
            clarification_mode=clarification_mode,
        )
        plan_clarification = _build_clarification_result(
            command="run",
            clarifications=plan_clarifications,
            resolution=plan_resolution,
            clarification_mode=clarification_mode,
            message=(
                "One or more clarification answers could not be confidently mapped. "
                "Please answer by question id or in clearly separated segments."
                if plan_resolution["ambiguous_freeform"]
                else ""
            ),
        )

        if clarification_mode == "required" and plan_clarification["clarification_required"]:
            payload = {
                "ok": True,
                "command": "run",
                "workflow_passed": False,
                "spec_path": str(Path(spec_path).resolve()),
                "artifacts": {
                    "prompt_path": prompt_result["output_path"],
                    "plan_path": plan_result["output_path"],
                },
                "spec_validation": spec_validation["stage"],
                "plan_generation": plan_result["summary"],
                "plan_validation": plan_validation["stage"],
                "timing": {
                    "prompt_generation_ms": prompt_result.get("timing", {}).get("elapsed_ms", 0),
                    "plan_generation_ms": initial_plan_generation_ms,
                    "spec_validation_ms": spec_validation.get("timing", {}).get("elapsed_ms", 0),
                    "plan_validation_ms": plan_validation.get("timing", {}).get("elapsed_ms", 0),
                },
            }
            if repair_attempts:
                payload["repair_attempts"] = repair_attempts
            payload = _attach_clarification_fields(payload, plan_clarification)
            return _attach_timing(payload, started_at)

        feedback = _build_plan_repair_feedback(plan_validation["stage"])
        plan_feedback_lines = _clarification_feedback_lines(plan_clarifications, plan_resolution)
        all_feedback_lines = spec_feedback_lines + plan_feedback_lines
        if all_feedback_lines:
            feedback += "\n\nUser clarifications and delegated defaults:\n" + "\n".join(all_feedback_lines)
        try:
            plan_result = _repair_plan_with_patch(
                spec_path,
                plan_result["output_path"],
                plan_output,
                config_path=config_path,
                model_name=model_name,
                repair_feedback=feedback,
            )
        except Exception as exc:
            repair_attempts.append(
                {
                    "attempt": attempt,
                    "strategy": "patch",
                    "feedback": feedback,
                    "error": str(exc),
                }
            )
            break

        plan_validation = _validate_plan(
            spec_path,
            plan_result["output_path"],
            config_path=config_path,
            model_name=model_name,
            skip_adversary=skip_adversary,
        )
        repair_attempts.append(
            {
                "attempt": attempt,
                "strategy": "patch",
                "feedback": feedback,
                "patch_operation_count": plan_result.get("patch_operation_count", 0),
                "timing": {
                    "repair_ms": plan_result.get("timing", {}).get("elapsed_ms", 0),
                    "validation_ms": plan_validation.get("timing", {}).get("elapsed_ms", 0),
                },
                "plan_validation": plan_validation["stage"],
            }
        )

    default_clarification = {
        "command": "run",
        "clarification_mode": clarification_mode,
        "clarification_required": False,
        "clarifications": [],
        "clarification_answers": parsed_answers,
        "clarification_answered_ids": list(parsed_answers.keys()),
        "clarification_assumptions": [],
        "pending_clarification_ids": [],
        "clarification_parse_ambiguous": False,
        "clarification_message": "",
    }

    payload = {
        "ok": True,
        "command": "run",
        "spec_path": str(Path(spec_path).resolve()),
        "artifacts": {
            "prompt_path": prompt_result["output_path"],
            "plan_path": plan_result["output_path"],
        },
        "spec_validation": spec_validation["stage"],
        "plan_generation": plan_result["summary"],
        "plan_validation": plan_validation["stage"],
        "timing": {
            "prompt_generation_ms": prompt_result.get("timing", {}).get("elapsed_ms", 0),
            "plan_generation_ms": initial_plan_generation_ms,
            "spec_validation_ms": spec_validation.get("timing", {}).get("elapsed_ms", 0),
            "plan_validation_ms": plan_validation.get("timing", {}).get("elapsed_ms", 0),
        },
    }
    if repair_attempts:
        payload["repair_attempts"] = repair_attempts
    payload = _attach_clarification_fields(payload, default_clarification)
    return _attach_timing(payload, started_at)


def _build_from_idea_workflow(*,
                              idea: str | None,
                              idea_path: str | None,
                              spec_output_path: str,
                              prompt_output_path: str,
                              plan_output_path: str,
                              context_paths: list[str] | None,
                              overwrite: bool,
                              config_path: str | None,
                              model_name: str | None,
                              skip_adversary: bool,
                              clarification_mode: str,
                              clarification_answers,
                              clarification_answers_text: str | None,
                              delegate_unanswered: bool) -> dict:
    started_at = time.perf_counter()
    clarification_mode = _normalize_clarification_mode(clarification_mode)
    parsed_answers = _normalize_answers_payload(clarification_answers)

    workspace_root = _workspace_root()
    resolved_idea_text, resolved_idea_path = _resolve_idea_text(
        idea,
        idea_path,
        workspace_root=workspace_root,
    )
    resolved_context_paths = _resolve_context_paths(
        context_paths,
        workspace_root=workspace_root,
    )
    spec_output, prompt_output, plan_output = _resolve_output_paths(
        spec_output_path,
        prompt_output_path,
        plan_output_path,
        workspace_root=workspace_root,
        overwrite=overwrite,
    )

    spec_attempts: list[dict] = []
    last_spec_validation: dict | None = None
    retry_feedback: str | None = None
    spec_result: dict | None = None
    for attempt in range(1, MAX_SPEC_GENERATION_ATTEMPTS + 1):
        spec_result = _generate_spec_from_idea(
            resolved_idea_text,
            str(spec_output),
            context_paths=resolved_context_paths,
            workspace_root=workspace_root,
            config_path=config_path,
            model_name=model_name,
            validation_feedback=retry_feedback,
        )
        spec_validation_payload = _validate_spec(
            spec_result["output_path"],
            config_path=config_path,
            model_name=model_name,
            skip_adversary=skip_adversary,
        )
        last_spec_validation = spec_validation_payload["stage"]

        attempt_entry = {
            "attempt": attempt,
            "spec_path": spec_result["output_path"],
            "timing": {
                "spec_generation_ms": spec_result.get("timing", {}).get("elapsed_ms", 0),
                "spec_validation_ms": spec_validation_payload.get("timing", {}).get("elapsed_ms", 0),
            },
            "spec_validation": last_spec_validation,
        }
        if retry_feedback:
            attempt_entry["retry_feedback"] = retry_feedback
        spec_attempts.append(attempt_entry)

        spec_clarifications = _extract_stage_clarifications(last_spec_validation, stage_name="spec")
        spec_resolution = _resolve_clarification_answers(
            spec_clarifications,
            structured_answers=parsed_answers,
            answers_text=clarification_answers_text,
            delegate_unanswered=delegate_unanswered,
            clarification_mode=clarification_mode,
        )
        spec_clarification = _build_clarification_result(
            command="build-from-idea",
            clarifications=spec_clarifications,
            resolution=spec_resolution,
            clarification_mode=clarification_mode,
            message=(
                "One or more clarification answers could not be confidently mapped. "
                "Please answer by question id or in clearly separated segments."
                if spec_resolution["ambiguous_freeform"]
                else ""
            ),
        )

        if clarification_mode == "required" and spec_clarification["clarification_required"]:
            payload = {
                "ok": True,
                "command": "build-from-idea",
                "workflow_passed": False,
                "idea": {
                    "source": "idea_path" if resolved_idea_path else "idea",
                    "idea_path": resolved_idea_path,
                    "characters": len(resolved_idea_text),
                },
                "artifacts": {
                    "spec_path": spec_result["output_path"],
                },
                "context_paths": [
                    _relative_path(context_path, workspace_root)
                    for context_path in resolved_context_paths
                ],
                "spec_attempts": spec_attempts,
                "spec_validation": last_spec_validation,
                "timing": {
                    "spec_generation_ms": sum(
                        item.get("timing", {}).get("spec_generation_ms", 0)
                        for item in spec_attempts
                    ),
                    "spec_validation_ms": sum(
                        item.get("timing", {}).get("spec_validation_ms", 0)
                        for item in spec_attempts
                    ),
                    "prompt_generation_ms": 0,
                    "plan_generation_ms": 0,
                    "plan_validation_ms": 0,
                },
            }
            payload = _attach_clarification_fields(payload, spec_clarification)
            return _attach_timing(payload, started_at)

        if last_spec_validation["passed"]:
            break
        retry_feedback = _build_spec_repair_feedback(last_spec_validation)
        spec_feedback_lines = _clarification_feedback_lines(spec_clarifications, spec_resolution)
        if spec_feedback_lines:
            retry_feedback += "\n\nUser clarifications and delegated defaults:\n" + "\n".join(spec_feedback_lines)
        spec_attempts[-1]["validation_feedback"] = retry_feedback

    if not spec_result or not last_spec_validation:
        raise RuntimeError("spec generation did not produce a result")

    spec_generation_total_ms = sum(
        attempt.get("timing", {}).get("spec_generation_ms", 0)
        for attempt in spec_attempts
    )
    spec_validation_total_ms = sum(
        attempt.get("timing", {}).get("spec_validation_ms", 0)
        for attempt in spec_attempts
    )

    if not last_spec_validation["passed"]:
        default_clarification = {
            "command": "build-from-idea",
            "clarification_mode": clarification_mode,
            "clarification_required": False,
            "clarifications": [],
            "clarification_answers": parsed_answers,
            "clarification_answered_ids": list(parsed_answers.keys()),
            "clarification_assumptions": [],
            "pending_clarification_ids": [],
            "clarification_parse_ambiguous": False,
            "clarification_message": "",
        }
        failure_payload = {
            "ok": True,
            "command": "build-from-idea",
            "workflow_passed": False,
            "idea": {
                "source": "idea_path" if resolved_idea_path else "idea",
                "idea_path": resolved_idea_path,
                "characters": len(resolved_idea_text),
            },
            "artifacts": {
                "spec_path": spec_result["output_path"],
            },
            "context_paths": [
                _relative_path(context_path, workspace_root)
                for context_path in resolved_context_paths
            ],
            "spec_attempts": spec_attempts,
            "spec_validation": last_spec_validation,
            "timing": {
                "spec_generation_ms": spec_generation_total_ms,
                "spec_validation_ms": spec_validation_total_ms,
                "prompt_generation_ms": 0,
                "plan_generation_ms": 0,
                "plan_validation_ms": 0,
            },
        }
        failure_payload = _attach_clarification_fields(failure_payload, default_clarification)
        return _attach_timing(failure_payload, started_at)

    prompt_result = _generate_prompt(
        spec_result["output_path"],
        str(prompt_output),
        config_path=config_path,
        model_name=model_name,
    )
    plan_result = _generate_plan(
        spec_result["output_path"],
        prompt_result["output_path"],
        str(plan_output),
        config_path=config_path,
        model_name=model_name,
    )
    initial_plan_generation_ms = plan_result.get("timing", {}).get("elapsed_ms", 0)
    plan_validation = _validate_plan(
        spec_result["output_path"],
        plan_result["output_path"],
        config_path=config_path,
        model_name=model_name,
        skip_adversary=skip_adversary,
    )

    repair_attempts: list[dict] = []
    for attempt in range(1, MAX_PLAN_REPAIR_ATTEMPTS + 1):
        if plan_validation["stage"]["passed"]:
            break

        plan_clarifications = _extract_stage_clarifications(plan_validation["stage"], stage_name="plan")
        plan_resolution = _resolve_clarification_answers(
            plan_clarifications,
            structured_answers=parsed_answers,
            answers_text=clarification_answers_text,
            delegate_unanswered=delegate_unanswered,
            clarification_mode=clarification_mode,
        )
        plan_clarification = _build_clarification_result(
            command="build-from-idea",
            clarifications=plan_clarifications,
            resolution=plan_resolution,
            clarification_mode=clarification_mode,
            message=(
                "One or more clarification answers could not be confidently mapped. "
                "Please answer by question id or in clearly separated segments."
                if plan_resolution["ambiguous_freeform"]
                else ""
            ),
        )

        if clarification_mode == "required" and plan_clarification["clarification_required"]:
            payload = {
                "ok": True,
                "command": "build-from-idea",
                "workflow_passed": False,
                "idea": {
                    "source": "idea_path" if resolved_idea_path else "idea",
                    "idea_path": resolved_idea_path,
                    "characters": len(resolved_idea_text),
                },
                "artifacts": {
                    "spec_path": spec_result["output_path"],
                    "prompt_path": prompt_result["output_path"],
                    "plan_path": plan_result["output_path"],
                },
                "context_paths": [
                    _relative_path(context_path, workspace_root)
                    for context_path in resolved_context_paths
                ],
                "spec_attempts": spec_attempts,
                "spec_validation": last_spec_validation,
                "plan_generation": plan_result["summary"],
                "plan_validation": plan_validation["stage"],
                "timing": {
                    "spec_generation_ms": spec_generation_total_ms,
                    "spec_validation_ms": spec_validation_total_ms,
                    "prompt_generation_ms": prompt_result.get("timing", {}).get("elapsed_ms", 0),
                    "plan_generation_ms": initial_plan_generation_ms,
                    "plan_validation_ms": plan_validation.get("timing", {}).get("elapsed_ms", 0),
                },
            }
            if repair_attempts:
                payload["repair_attempts"] = repair_attempts
            payload = _attach_clarification_fields(payload, plan_clarification)
            return _attach_timing(payload, started_at)

        feedback = _build_plan_repair_feedback(plan_validation["stage"])
        plan_feedback_lines = _clarification_feedback_lines(plan_clarifications, plan_resolution)
        if plan_feedback_lines:
            feedback += "\n\nUser clarifications and delegated defaults:\n" + "\n".join(plan_feedback_lines)
        try:
            plan_result = _repair_plan_with_patch(
                spec_result["output_path"],
                plan_result["output_path"],
                str(plan_output),
                config_path=config_path,
                model_name=model_name,
                repair_feedback=feedback,
            )
        except Exception as exc:
            repair_attempts.append(
                {
                    "attempt": attempt,
                    "strategy": "patch",
                    "feedback": feedback,
                    "error": str(exc),
                }
            )
            break

        plan_validation = _validate_plan(
            spec_result["output_path"],
            plan_result["output_path"],
            config_path=config_path,
            model_name=model_name,
            skip_adversary=skip_adversary,
        )
        repair_attempts.append(
            {
                "attempt": attempt,
                "strategy": "patch",
                "feedback": feedback,
                "patch_operation_count": plan_result.get("patch_operation_count", 0),
                "timing": {
                    "repair_ms": plan_result.get("timing", {}).get("elapsed_ms", 0),
                    "validation_ms": plan_validation.get("timing", {}).get("elapsed_ms", 0),
                },
                "plan_validation": plan_validation["stage"],
            }
        )

    default_clarification = {
        "command": "build-from-idea",
        "clarification_mode": clarification_mode,
        "clarification_required": False,
        "clarifications": [],
        "clarification_answers": parsed_answers,
        "clarification_answered_ids": list(parsed_answers.keys()),
        "clarification_assumptions": [],
        "pending_clarification_ids": [],
        "clarification_parse_ambiguous": False,
        "clarification_message": "",
    }

    payload = {
        "ok": True,
        "command": "build-from-idea",
        "workflow_passed": last_spec_validation["passed"] and plan_validation["stage"]["passed"],
        "idea": {
            "source": "idea_path" if resolved_idea_path else "idea",
            "idea_path": resolved_idea_path,
            "characters": len(resolved_idea_text),
        },
        "artifacts": {
            "spec_path": spec_result["output_path"],
            "prompt_path": prompt_result["output_path"],
            "plan_path": plan_result["output_path"],
        },
        "context_paths": [
            _relative_path(context_path, workspace_root)
            for context_path in resolved_context_paths
        ],
        "spec_attempts": spec_attempts,
        "spec_validation": last_spec_validation,
        "plan_generation": plan_result["summary"],
        "plan_validation": plan_validation["stage"],
        "timing": {
            "spec_generation_ms": spec_generation_total_ms,
            "spec_validation_ms": spec_validation_total_ms,
            "prompt_generation_ms": prompt_result.get("timing", {}).get("elapsed_ms", 0),
            "plan_generation_ms": initial_plan_generation_ms,
            "plan_validation_ms": plan_validation.get("timing", {}).get("elapsed_ms", 0),
        },
    }
    if repair_attempts:
        payload["repair_attempts"] = repair_attempts
    payload = _attach_clarification_fields(payload, default_clarification)
    return _attach_timing(payload, started_at)


def _emit(payload: dict, exit_code: int) -> None:
    sys.stdout.write(json.dumps(payload, indent=2))
    sys.stdout.write("\n")
    raise SystemExit(exit_code)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Machine-readable planner harness bridge")
    parser.add_argument("--config", default=None, help="Path to config.yaml")
    sub = parser.add_subparsers(dest="command", required=True)

    validate_spec = sub.add_parser("validate-spec", help="Validate SPEC.md and emit JSON")
    validate_spec.add_argument("spec", help="Path to SPEC.md")
    validate_spec.add_argument("--model", default=None)
    validate_spec.add_argument("--skip-adversary", action="store_true")

    generate_prompt = sub.add_parser("generate-prompt", help="Generate planner prompt and emit JSON")
    generate_prompt.add_argument("spec", help="Path to SPEC.md")
    generate_prompt.add_argument("--model", default=None)
    generate_prompt.add_argument("-o", "--output", default="planner-prompt.md")

    generate_plan = sub.add_parser("generate-plan", help="Generate PLAN.json and emit JSON")
    generate_plan.add_argument("spec", help="Path to SPEC.md")
    generate_plan.add_argument("--prompt", required=True)
    generate_plan.add_argument("--model", default=None)
    generate_plan.add_argument("-o", "--output", default="PLAN.json")

    repair_plan = sub.add_parser("repair-plan", help="Repair PLAN.json with minimal patch operations and emit JSON")
    repair_plan.add_argument("spec", help="Path to SPEC.md")
    repair_plan.add_argument("plan", help="Path to PLAN.json")
    repair_plan.add_argument("--model", default=None)
    repair_plan.add_argument("--validation-json", default=None)
    repair_plan.add_argument("--repair-feedback", default=None)
    repair_plan.add_argument("--skip-adversary", action="store_true")
    repair_plan.add_argument("-o", "--output", default=None)

    validate_plan = sub.add_parser("validate-plan", help="Validate PLAN.json and emit JSON")
    validate_plan.add_argument("spec", help="Path to SPEC.md")
    validate_plan.add_argument("plan", help="Path to PLAN.json")
    validate_plan.add_argument("--model", default=None)
    validate_plan.add_argument("--skip-adversary", action="store_true")

    validate_review = sub.add_parser("validate-review", help="Validate a review or audit report and emit JSON")
    validate_review.add_argument("case", help="Path to tracked review case JSON")
    validate_review.add_argument("report", help="Path to review report markdown/text")
    validate_review.add_argument("--model", default=None)
    validate_review.add_argument("--skip-adversary", action="store_true")

    validate_bugfix = sub.add_parser("validate-bugfix", help="Validate a bugfix writeup and emit JSON")
    validate_bugfix.add_argument("case", help="Path to tracked bugfix case JSON")
    validate_bugfix.add_argument("report", help="Path to bugfix report markdown/text")
    validate_bugfix.add_argument("--model", default=None)
    validate_bugfix.add_argument("--skip-adversary", action="store_true")

    run = sub.add_parser("run", help="Run prompt generation, plan generation, and validation")
    run.add_argument("spec", help="Path to SPEC.md")
    run.add_argument("--model", default=None)
    run.add_argument("--skip-adversary", action="store_true")
    run.add_argument("--clarification-mode", default="required")
    run.add_argument("--answers-json-text", default=None)
    run.add_argument("--answers-text", default=None)
    run.add_argument("--delegate-unanswered", action="store_true")
    run.add_argument("--prompt-output", default="planner-prompt.md")
    run.add_argument("--plan-output", default="PLAN.json")

    build_from_idea = sub.add_parser(
        "build-from-idea",
        help="Generate SPEC.md, planner-prompt.md, and PLAN.json from a project idea",
    )
    build_from_idea.add_argument("--idea", "--idea-text", dest="idea", default=None)
    build_from_idea.add_argument("--idea-path", default=None)
    build_from_idea.add_argument("--context-path", dest="context_paths", action="append", default=[])
    build_from_idea.add_argument("--model", default=None)
    build_from_idea.add_argument("--skip-adversary", action="store_true")
    build_from_idea.add_argument("--clarification-mode", default="required")
    build_from_idea.add_argument("--answers-json-text", default=None)
    build_from_idea.add_argument("--answers-text", default=None)
    build_from_idea.add_argument("--delegate-unanswered", action="store_true")
    build_from_idea.add_argument("--overwrite", action="store_true")
    build_from_idea.add_argument("--spec-output", default="SPEC.md")
    build_from_idea.add_argument("--prompt-output", default="planner-prompt.md")
    build_from_idea.add_argument("--plan-output", default="PLAN.json")

    return parser


def main() -> None:
    parser = _build_parser()
    args = parser.parse_args()

    parsed_answers = None
    if getattr(args, "answers_json_text", None):
        try:
            parsed_answers = json.loads(args.answers_json_text)
        except json.JSONDecodeError as exc:
            _emit(
                {
                    "ok": False,
                    "command": args.command,
                    "error": f"invalid --answers-json-text payload: {exc}",
                    "error_type": type(exc).__name__,
                },
                1,
            )

    try:
        if args.command == "validate-spec":
            payload = _validate_spec(
                args.spec,
                config_path=args.config,
                model_name=args.model,
                skip_adversary=args.skip_adversary,
            )
        elif args.command == "generate-prompt":
            payload = _generate_prompt(
                args.spec,
                args.output,
                config_path=args.config,
                model_name=args.model,
            )
        elif args.command == "generate-plan":
            payload = _generate_plan(
                args.spec,
                args.prompt,
                args.output,
                config_path=args.config,
                model_name=args.model,
            )
        elif args.command == "repair-plan":
            payload = _repair_plan(
                args.spec,
                args.plan,
                args.output or args.plan,
                config_path=args.config,
                model_name=args.model,
                validation_json_path=args.validation_json,
                repair_feedback=args.repair_feedback,
                skip_adversary=args.skip_adversary,
            )
        elif args.command == "validate-plan":
            payload = _validate_plan(
                args.spec,
                args.plan,
                config_path=args.config,
                model_name=args.model,
                skip_adversary=args.skip_adversary,
            )
        elif args.command == "validate-review":
            payload = _validate_review(
                args.case,
                args.report,
                config_path=args.config,
                model_name=args.model,
                skip_adversary=args.skip_adversary,
            )
        elif args.command == "validate-bugfix":
            payload = _validate_bugfix(
                args.case,
                args.report,
                config_path=args.config,
                model_name=args.model,
                skip_adversary=args.skip_adversary,
            )
        elif args.command == "build-from-idea":
            payload = _build_from_idea_workflow(
                idea=args.idea,
                idea_path=args.idea_path,
                spec_output_path=args.spec_output,
                prompt_output_path=args.prompt_output,
                plan_output_path=args.plan_output,
                context_paths=args.context_paths,
                overwrite=args.overwrite,
                config_path=args.config,
                model_name=args.model,
                skip_adversary=args.skip_adversary,
                clarification_mode=args.clarification_mode,
                clarification_answers=parsed_answers,
                clarification_answers_text=args.answers_text,
                delegate_unanswered=args.delegate_unanswered,
            )
        else:
            payload = _run_workflow(
                args.spec,
                args.prompt_output,
                args.plan_output,
                config_path=args.config,
                model_name=args.model,
                skip_adversary=args.skip_adversary,
                clarification_mode=args.clarification_mode,
                clarification_answers=parsed_answers,
                clarification_answers_text=args.answers_text,
                delegate_unanswered=args.delegate_unanswered,
            )
    except Exception as exc:
        _emit(
            {
                "ok": False,
                "command": args.command,
                "error": str(exc),
                "error_type": type(exc).__name__,
            },
            1,
        )

    _emit(payload, 0)


if __name__ == "__main__":
    main()