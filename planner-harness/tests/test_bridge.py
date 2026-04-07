import json
import importlib.util
import subprocess
import sys
from pathlib import Path


BRIDGE = Path(__file__).parent.parent / "bridge.py"
BRIDGE_SPEC = importlib.util.spec_from_file_location("planner_bridge", BRIDGE)
bridge = importlib.util.module_from_spec(BRIDGE_SPEC)
assert BRIDGE_SPEC.loader is not None
BRIDGE_SPEC.loader.exec_module(bridge)


def _run_bridge(*args: str) -> dict:
    result = subprocess.run(
        [sys.executable, str(BRIDGE), *args],
        capture_output=True,
        text=True,
        check=True,
    )
    return json.loads(result.stdout)


def test_validate_spec_skip_adversary_returns_json(tmp_path):
    spec_path = tmp_path / "SPEC.md"
    spec_path.write_text(
        "# Example\n\n"
        "```python\n"
        "def add(a: int, b: int) -> int:\n"
        "    pass\n"
        "```\n\n"
        "Example: add(1, 2) returns 3.\n"
        "Raises ValueError on invalid input.\n",
        encoding="utf-8",
    )

    payload = _run_bridge("validate-spec", str(spec_path), "--skip-adversary")

    assert payload["ok"] is True
    assert payload["command"] == "validate-spec"
    assert payload["stage"]["adversary"]["skipped"] is True
    assert payload["timing"]["elapsed_ms"] >= 0
    assert payload["timing"]["rubric_ms"] >= 0
    assert payload["timing"]["adversary_ms"] == 0
    assert isinstance(payload["stage"]["rubric_checks"], list)
    assert payload["stage"]["rubric_score"] >= 0


def test_validate_plan_skip_adversary_returns_json(tmp_path):
    spec_path = tmp_path / "SPEC.md"
    spec_path.write_text("# Spec\n", encoding="utf-8")

    plan_path = tmp_path / "PLAN.json"
    plan_path.write_text(
        json.dumps(
            {
                "phases": [
                    {
                        "phase": 0,
                        "name": "Foundation",
                        "tasks": [
                            {
                                "file": "src/main.py",
                                "description": "Create entrypoint",
                                "spec_section": "def main() -> None:\n    pass\n" * 10,
                                "depends_on": [],
                            },
                            {
                                "file": "tests/test_main.py",
                                "description": "Test entrypoint",
                                "spec_section": "def test_main() -> None:\n    pass\n" * 10,
                                "depends_on": ["src/main.py"],
                            },
                        ],
                    }
                ]
            }
        ),
        encoding="utf-8",
    )

    payload = _run_bridge("validate-plan", str(spec_path), str(plan_path), "--skip-adversary")

    assert payload["ok"] is True
    assert payload["command"] == "validate-plan"
    assert payload["stage"]["adversary"]["skipped"] is True
    assert payload["timing"]["elapsed_ms"] >= 0
    assert payload["timing"]["rubric_ms"] >= 0
    assert payload["timing"]["adversary_ms"] == 0
    assert isinstance(payload["stage"]["rubric_checks"], list)
    assert payload["stage"]["combined_score"] >= 0


def test_augment_generated_prompt_enforces_init_pairs():
    spec_text = """
    File Structure:
    - `backend/__init__.py`
    - `backend/api/__init__.py`
    - `backend/domain/__init__.py`
    """
    prompt_text = """
    REQUIRED FILE PAIRS
    - `backend/__init__.py` -> no test required (package marker)
    - `backend/api/__init__.py` -> no test required (package marker)
    """

    augmented = bridge._augment_generated_prompt(prompt_text, spec_text)

    assert "no test required" not in augmented.lower()
    assert "`backend/__init__.py` -> `tests/test_backend_init.py`" in augmented
    assert "`backend/api/__init__.py` -> `tests/test_api_init.py`" in augmented
    assert "`backend/domain/__init__.py` -> `tests/test_domain_init.py`" in augmented


def test_build_plan_repair_feedback_includes_failed_checks_and_init_rule():
    stage = {
        "rubric_checks": [
            {
                "name": "Test coverage",
                "passed": False,
                "detail": "4 untested: backend/__init__.py",
            },
            {
                "name": "Import completeness",
                "passed": False,
                "detail": "backend/app/api/auth.py uses UserOut without import",
            }
        ],
        "adversary": {
            "error": "",
            "blocking_gaps": 0,
            "blocking_guesses": 0,
            "contradiction_count": 0,
        },
    }

    feedback = bridge._build_plan_repair_feedback(stage)

    assert "Test coverage: 4 untested: backend/__init__.py" in feedback
    assert "tests/test_<parent>_init.py" in feedback
    assert "explicit `from ... import Symbol` lines" in feedback


def test_build_plan_repair_feedback_adds_targeted_hints_for_failed_checks():
    stage = {
        "rubric_checks": [
            {
                "name": "Spec_section non-trivial",
                "passed": False,
                "detail": "backend/app/__init__.py too short",
            },
            {
                "name": "Test spec matches source spec",
                "passed": False,
                "detail": "tests/test_api_auth.py doesn't reference backend/app/api/auth.py functions",
            },
            {
                "name": "Import completeness",
                "passed": False,
                "detail": "backend/app/api/auth.py uses Token without import",
            },
        ],
        "adversary": {
            "error": "",
            "blocking_gaps": 0,
            "blocking_guesses": 0,
            "contradiction_count": 0,
        },
    }

    feedback = bridge._build_plan_repair_feedback(stage)

    assert "Keep package __init__.py spec_section blocks concrete and non-trivial" in feedback
    assert "the test spec_section must explicitly name the source functions" in feedback
    assert "Fix missing imports by adding explicit `from ... import Symbol` lines" in feedback


def test_apply_plan_patch_moves_replaces_adds_and_removes_tasks():
    plan = {
        "phases": [
            {
                "phase": 0,
                "name": "Foundation",
                "tasks": [
                    {
                        "file": "app/utils/pdf.py",
                        "description": "PDF utility",
                        "spec_section": "def generate_pdf() -> bytes:\n    return b''",
                        "depends_on": ["app/models/recipe.py"],
                        "agent_type": "coder",
                        "test_strategy": "unit",
                    },
                    {
                        "file": "tests/test_old.py",
                        "description": "Obsolete test",
                        "spec_section": "def test_old():\n    pass",
                        "depends_on": ["app/utils/pdf.py"],
                        "agent_type": "coder",
                        "test_strategy": "unit",
                    },
                ],
            },
            {
                "phase": 1,
                "name": "Models",
                "tasks": [
                    {
                        "file": "app/models/recipe.py",
                        "description": "Recipe model",
                        "spec_section": "class Recipe:\n    pass",
                        "depends_on": [],
                        "agent_type": "coder",
                        "test_strategy": "unit",
                    }
                ],
            },
        ]
    }
    patch = {
        "operations": [
            {
                "op": "move_task",
                "file": "app/utils/pdf.py",
                "target_phase": 1,
            },
            {
                "op": "replace_task",
                "file": "app/models/recipe.py",
                "task": {
                    "file": "app/models/recipe.py",
                    "description": "Recipe model updated",
                    "spec_section": "class Recipe:\n    id: str",
                    "depends_on": [],
                    "agent_type": "coder",
                    "test_strategy": "unit",
                },
            },
            {
                "op": "add_task",
                "phase": 1,
                "task": {
                    "file": "tests/test_utils_pdf.py",
                    "description": "PDF tests",
                    "spec_section": "def test_pdf():\n    pass",
                    "depends_on": ["app/utils/pdf.py"],
                    "agent_type": "coder",
                    "test_strategy": "unit",
                },
            },
            {
                "op": "remove_task",
                "file": "tests/test_old.py",
            },
        ]
    }

    repaired = bridge._apply_plan_patch(plan, patch)
    phase_zero_files = [task["file"] for task in repaired["phases"][0]["tasks"]]
    phase_one_files = [task["file"] for task in repaired["phases"][1]["tasks"]]

    assert phase_zero_files == []
    assert "app/utils/pdf.py" in phase_one_files
    assert "tests/test_utils_pdf.py" in phase_one_files
    assert "tests/test_old.py" not in phase_one_files
    recipe_task = next(task for task in repaired["phases"][1]["tasks"] if task["file"] == "app/models/recipe.py")
    assert recipe_task["description"] == "Recipe model updated"


def test_apply_plan_patch_upserts_existing_add_task():
    plan = {
        "phases": [
            {
                "phase": 0,
                "name": "Foundation",
                "tasks": [
                    {
                        "file": "tests/test_models_init.py",
                        "description": "Old init test",
                        "spec_section": "def test_old():\n    pass",
                        "depends_on": ["app/models/__init__.py"],
                        "agent_type": "coder",
                        "test_strategy": "unit",
                    }
                ],
            },
            {
                "phase": 1,
                "name": "Models",
                "tasks": [],
            },
        ]
    }
    patch = {
        "operations": [
            {
                "op": "add_task",
                "phase": 1,
                "task": {
                    "file": "tests/test_models_init.py",
                    "description": "Updated init test",
                    "spec_section": "def test_models_init_exports():\n    assert True",
                    "depends_on": ["app/models/__init__.py"],
                    "agent_type": "coder",
                    "test_strategy": "unit",
                },
            }
        ]
    }

    repaired = bridge._apply_plan_patch(plan, patch)
    phase_zero_files = [task["file"] for task in repaired["phases"][0]["tasks"]]
    phase_one_tasks = repaired["phases"][1]["tasks"]

    assert phase_zero_files == []
    assert len(phase_one_tasks) == 1
    assert phase_one_tasks[0]["file"] == "tests/test_models_init.py"
    assert phase_one_tasks[0]["description"] == "Updated init test"


def test_run_workflow_uses_patch_repair(monkeypatch):
    def fake_generate_prompt(*args, **kwargs):
        return {
            "ok": True,
            "command": "generate-prompt",
            "model": "opus5-5",
            "spec_path": "/tmp/SPEC.md",
            "output_path": "/tmp/planner-prompt.md",
            "characters": 42,
            "timing": {"elapsed_ms": 11},
        }

    def fake_generate_plan(*args, **kwargs):
        return {
            "ok": True,
            "command": "generate-plan",
            "model": "opus5-5",
            "spec_path": "/tmp/SPEC.md",
            "prompt_path": "/tmp/planner-prompt.md",
            "output_path": "/tmp/PLAN.json",
            "summary": {"format": "phases", "phase_count": 1, "task_count": 2},
            "timing": {"elapsed_ms": 22},
        }

    def fake_validate_spec(*args, **kwargs):
        return {
            "ok": True,
            "command": "validate-spec",
            "stage": {"passed": True},
            "timing": {"elapsed_ms": 33},
        }

    validate_results = [
        {
            "ok": True,
            "command": "validate-plan",
            "stage": {
                "passed": False,
                "rubric_checks": [
                    {
                        "name": "Phase ordering valid",
                        "passed": False,
                        "detail": "1 violations: app/utils/pdf.py (phase 0) depends on app/models/recipe.py (phase 1)",
                    }
                ],
                "adversary": {
                    "error": "",
                    "blocking_gaps": 0,
                    "blocking_guesses": 0,
                    "contradiction_count": 0,
                },
            },
            "timing": {"elapsed_ms": 44},
        },
        {
            "ok": True,
            "command": "validate-plan",
            "stage": {
                "passed": True,
                "rubric_checks": [],
                "adversary": {
                    "error": "",
                    "blocking_gaps": 0,
                    "blocking_guesses": 0,
                    "contradiction_count": 0,
                },
            },
            "timing": {"elapsed_ms": 55},
        },
    ]

    repair_calls = []

    def fake_validate_plan(*args, **kwargs):
        return validate_results.pop(0)

    def fake_repair_plan_with_patch(*args, **kwargs):
        repair_calls.append(kwargs["repair_feedback"])
        return {
            "ok": True,
            "command": "repair-plan",
            "model": "opus5-5",
            "spec_path": "/tmp/SPEC.md",
            "plan_path": "/tmp/PLAN.json",
            "output_path": "/tmp/PLAN.json",
            "repair_strategy": "patch",
            "patch_operation_count": 1,
            "summary": {
                "format": "phases",
                "phase_count": 1,
                "task_count": 2,
                "repair_strategy": "patch",
            },
            "timing": {"elapsed_ms": 66},
        }

    monkeypatch.setattr(bridge, "_generate_prompt", fake_generate_prompt)
    monkeypatch.setattr(bridge, "_generate_plan", fake_generate_plan)
    monkeypatch.setattr(bridge, "_validate_spec", fake_validate_spec)
    monkeypatch.setattr(bridge, "_validate_plan", fake_validate_plan)
    monkeypatch.setattr(bridge, "_repair_plan_with_patch", fake_repair_plan_with_patch)

    payload = bridge._run_workflow(
        "SPEC.md",
        "planner-prompt.md",
        "PLAN.json",
        config_path=None,
        model_name="opus5-5",
        skip_adversary=True,
    )

    assert payload["plan_validation"]["passed"] is True
    assert len(repair_calls) == 1
    assert payload["repair_attempts"][0]["strategy"] == "patch"
    assert payload["repair_attempts"][0]["patch_operation_count"] == 1
    assert payload["timing"]["prompt_generation_ms"] == 11
    assert payload["timing"]["plan_generation_ms"] == 22
    assert payload["timing"]["spec_validation_ms"] == 33
    assert payload["timing"]["plan_validation_ms"] == 55
    assert payload["repair_attempts"][0]["timing"]["repair_ms"] == 66
    assert payload["repair_attempts"][0]["timing"]["validation_ms"] == 55


def test_resolve_model_uses_planner_default_for_generation():
    config = {
        "models": {
            "qwen3.5": {"model": "qwen3.5:cloud"},
            "qwen3-coder-next": {"model": "qwen3-coder-next:cloud"},
        },
        "defaults": {
            "adversary_model": "qwen3.5",
            "planner_model": "qwen3-coder-next",
        },
    }

    assert bridge._resolve_model(config, None, purpose="generation") == "qwen3-coder-next"
    assert bridge._resolve_model(config, None, purpose="adversary") == "qwen3.5"


def test_resolve_model_accepts_provider_model_id():
    config = {
        "models": {
            "qwen3-coder-next": {"model": "qwen3-coder-next:cloud"},
        }
    }

    assert bridge._resolve_model(
        config,
        "qwen3-coder-next:cloud",
        purpose="generation",
    ) == "qwen3-coder-next"


def test_build_from_idea_workflow_success(monkeypatch, tmp_path):
    workspace_root = tmp_path.resolve()
    monkeypatch.setattr(bridge, "_workspace_root", lambda: workspace_root)

    def fake_generate_spec_from_idea(*args, **kwargs):
        return {
            "ok": True,
            "command": "generate-spec",
            "output_path": str(workspace_root / "SPEC.md"),
            "timing": {"elapsed_ms": 11},
        }

    def fake_validate_spec(*args, **kwargs):
        return {
            "ok": True,
            "command": "validate-spec",
            "stage": {
                "passed": True,
                "rubric_checks": [],
                "adversary": {
                    "error": "",
                    "blocking_gaps": 0,
                    "blocking_guesses": 0,
                    "contradiction_count": 0,
                },
            },
            "timing": {"elapsed_ms": 22},
        }

    def fake_generate_prompt(*args, **kwargs):
        return {
            "ok": True,
            "command": "generate-prompt",
            "output_path": str(workspace_root / "planner-prompt.md"),
            "timing": {"elapsed_ms": 33},
        }

    def fake_generate_plan(*args, **kwargs):
        return {
            "ok": True,
            "command": "generate-plan",
            "output_path": str(workspace_root / "PLAN.json"),
            "summary": {"format": "phases", "phase_count": 1, "task_count": 2},
            "timing": {"elapsed_ms": 44},
        }

    def fake_validate_plan(*args, **kwargs):
        return {
            "ok": True,
            "command": "validate-plan",
            "stage": {
                "passed": True,
                "rubric_checks": [],
                "adversary": {
                    "error": "",
                    "blocking_gaps": 0,
                    "blocking_guesses": 0,
                    "contradiction_count": 0,
                },
            },
            "timing": {"elapsed_ms": 55},
        }

    monkeypatch.setattr(bridge, "_generate_spec_from_idea", fake_generate_spec_from_idea)
    monkeypatch.setattr(bridge, "_validate_spec", fake_validate_spec)
    monkeypatch.setattr(bridge, "_generate_prompt", fake_generate_prompt)
    monkeypatch.setattr(bridge, "_generate_plan", fake_generate_plan)
    monkeypatch.setattr(bridge, "_validate_plan", fake_validate_plan)

    payload = bridge._build_from_idea_workflow(
        idea="Build a webhook relay service",
        idea_path=None,
        spec_output_path="SPEC.md",
        prompt_output_path="planner-prompt.md",
        plan_output_path="PLAN.json",
        context_paths=[],
        overwrite=False,
        config_path=None,
        model_name="opus5-5",
        skip_adversary=True,
    )

    assert payload["command"] == "build-from-idea"
    assert payload["workflow_passed"] is True
    assert payload["artifacts"]["spec_path"].endswith("SPEC.md")
    assert payload["artifacts"]["prompt_path"].endswith("planner-prompt.md")
    assert payload["artifacts"]["plan_path"].endswith("PLAN.json")
    assert payload["timing"]["spec_generation_ms"] == 11
    assert payload["timing"]["spec_validation_ms"] == 22
    assert payload["timing"]["prompt_generation_ms"] == 33
    assert payload["timing"]["plan_generation_ms"] == 44
    assert payload["timing"]["plan_validation_ms"] == 55


def test_build_from_idea_workflow_stops_after_failed_spec_attempts(monkeypatch, tmp_path):
    workspace_root = tmp_path.resolve()
    monkeypatch.setattr(bridge, "_workspace_root", lambda: workspace_root)

    validation_results = [
        {
            "ok": True,
            "command": "validate-spec",
            "stage": {
                "passed": False,
                "rubric_checks": [
                    {
                        "name": "No ambiguous language",
                        "passed": False,
                        "detail": "'should' x1",
                    }
                ],
                "adversary": {
                    "error": "",
                    "blocking_gaps": 0,
                    "blocking_guesses": 0,
                    "contradiction_count": 0,
                },
            },
            "timing": {"elapsed_ms": 20},
        },
        {
            "ok": True,
            "command": "validate-spec",
            "stage": {
                "passed": False,
                "rubric_checks": [
                    {
                        "name": "Error behavior defined",
                        "passed": False,
                        "detail": "No error/exception behavior documented",
                    }
                ],
                "adversary": {
                    "error": "",
                    "blocking_gaps": 0,
                    "blocking_guesses": 0,
                    "contradiction_count": 0,
                },
            },
            "timing": {"elapsed_ms": 21},
        },
    ]

    generation_attempts = []

    def fake_generate_spec_from_idea(*args, **kwargs):
        generation_attempts.append(kwargs.get("validation_feedback"))
        return {
            "ok": True,
            "command": "generate-spec",
            "output_path": str(workspace_root / "SPEC.md"),
            "timing": {"elapsed_ms": 10},
        }

    def fake_validate_spec(*args, **kwargs):
        return validation_results.pop(0)

    monkeypatch.setattr(bridge, "_generate_spec_from_idea", fake_generate_spec_from_idea)
    monkeypatch.setattr(bridge, "_validate_spec", fake_validate_spec)
    monkeypatch.setattr(
        bridge,
        "_generate_prompt",
        lambda *args, **kwargs: (_ for _ in ()).throw(AssertionError("prompt generation should not run")),
    )
    monkeypatch.setattr(
        bridge,
        "_generate_plan",
        lambda *args, **kwargs: (_ for _ in ()).throw(AssertionError("plan generation should not run")),
    )

    payload = bridge._build_from_idea_workflow(
        idea="Build a markdown linter",
        idea_path=None,
        spec_output_path="SPEC.md",
        prompt_output_path="planner-prompt.md",
        plan_output_path="PLAN.json",
        context_paths=[],
        overwrite=False,
        config_path=None,
        model_name="opus5-5",
        skip_adversary=True,
    )

    assert payload["workflow_passed"] is False
    assert payload["spec_validation"]["passed"] is False
    assert len(payload["spec_attempts"]) == bridge.MAX_SPEC_GENERATION_ATTEMPTS
    assert "plan_validation" not in payload
    assert payload["timing"]["prompt_generation_ms"] == 0
    assert payload["timing"]["plan_generation_ms"] == 0
    assert payload["timing"]["plan_validation_ms"] == 0
    assert generation_attempts[0] is None
    assert isinstance(generation_attempts[1], str)