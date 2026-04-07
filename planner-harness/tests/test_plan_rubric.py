import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent.parent))

import pytest
from validators.plan_rubric import (
    validate_plan,
    check_spec_sections_present,
    check_test_coverage,
    check_no_circular_deps,
    check_phase_ordering,
    check_depends_on_references,
    check_no_duplicate_paths,
    check_import_completeness,
    _extract_files,
)


GOOD_PLAN = {
    "phases": [
        {
            "phase": 0,
            "name": "Foundation",
            "files": [
                {"path": "src/schema.py", "spec_section": "class Config:\n    name: str\n    value: int\n    def to_dict(self) -> dict: ...", "depends_on": [], "description": "Schema"},
            ]
        },
        {
            "phase": 0,
            "name": "Foundation",
            "files": [
                {"path": "tests/test_schema.py", "spec_section": "class Config:\n    name: str\n    value: int\n    def to_dict(self) -> dict: ...", "depends_on": ["src/schema.py"], "description": "Schema tests", "test": True},
            ]
        },
        {
            "phase": 1,
            "name": "Core",
            "files": [
                {"path": "src/engine.py", "spec_section": "class Engine:\n    def run(self, config: Config) -> Result: ...", "depends_on": ["src/schema.py"], "description": "Engine"},
            ]
        },
    ]
}


def test_extract_files():
    files = _extract_files(GOOD_PLAN)
    assert len(files) == 3
    assert files[0]["path"] == "src/schema.py"


def test_spec_sections_pass():
    files = _extract_files(GOOD_PLAN)
    result = check_spec_sections_present(files)
    assert result.passed


def test_spec_sections_fail():
    plan = {"phases": [{"phase": 0, "files": [{"path": "a.py", "spec_section": "", "depends_on": []}]}]}
    files = _extract_files(plan)
    result = check_spec_sections_present(files)
    assert not result.passed


def test_test_coverage_allows_transitive_backend_coverage():
    plan = {
        "phases": [
            {
                "phase": 0,
                "files": [
                    {"path": "backend/domain/models.py", "spec_section": "class UserOut:\n    id: str", "depends_on": []},
                ],
            },
            {
                "phase": 1,
                "files": [
                    {
                        "path": "backend/domain/auth_service.py",
                        "spec_section": "from backend.domain.models import UserOut\n\ndef register_user() -> UserOut: ...",
                        "depends_on": ["backend/domain/models.py"],
                    },
                ],
            },
            {
                "phase": 2,
                "files": [
                    {
                        "path": "tests/test_auth_api.py",
                        "spec_section": "Test register_user through API.",
                        "depends_on": ["backend/domain/auth_service.py"],
                        "test": True,
                    },
                ],
            },
        ]
    }
    files = _extract_files(plan)
    result = check_test_coverage(files, "backend-only test coverage")
    assert result.passed


def test_test_coverage_honors_frontend_exemption():
    plan = {
        "phases": [
            {
                "phase": 0,
                "files": [
                    {"path": "frontend/src/lib/api.ts", "spec_section": "export function apiFetch(): Promise<Response> { return fetch(''); }", "depends_on": []},
                ],
            },
        ]
    }
    files = _extract_files(plan)
    result = check_test_coverage(files, "No test files specified for frontend. Focus on backend tests.")
    assert result.passed


def test_import_completeness_ignores_nested_config_classes():
    plan = {
        "phases": [
            {
                "phase": 0,
                "files": [
                    {
                        "path": "app/schemas/user.py",
                        "spec_section": (
                            "from pydantic import BaseModel\n\n"
                            "class UserOut(BaseModel):\n"
                            "    id: str\n"
                            "    class Config:\n"
                            "        from_attributes = True\n"
                        ),
                        "depends_on": [],
                    },
                    {
                        "path": "app/schemas/meal_plan.py",
                        "spec_section": (
                            "from pydantic import BaseModel\n\n"
                            "class MealPlanOut(BaseModel):\n"
                            "    id: str\n"
                            "    class Config:\n"
                            "        from_attributes = True\n"
                        ),
                        "depends_on": [],
                    },
                ],
            }
        ]
    }

    files = _extract_files(plan)
    result = check_import_completeness(files)
    assert result.passed


def test_no_circular_deps_pass():
    files = _extract_files(GOOD_PLAN)
    result = check_no_circular_deps(files)
    assert result.passed


def test_circular_deps_detected():
    plan = {"phases": [{"phase": 0, "files": [
        {"path": "a.py", "spec_section": "x", "depends_on": ["b.py"]},
        {"path": "b.py", "spec_section": "x", "depends_on": ["a.py"]},
    ]}]}
    files = _extract_files(plan)
    result = check_no_circular_deps(files)
    assert not result.passed


def test_phase_ordering_pass():
    result = check_phase_ordering(GOOD_PLAN)
    assert result.passed


def test_phase_ordering_fail():
    plan = {"phases": [
        {"phase": 0, "files": [{"path": "a.py", "depends_on": ["b.py"]}]},
        {"phase": 1, "files": [{"path": "b.py", "depends_on": []}]},
    ]}
    result = check_phase_ordering(plan)
    assert not result.passed


def test_depends_on_references_pass_for_files_and_phase_names():
    plan = {"phases": [
        {
            "phase": 0,
            "name": "Foundation",
            "files": [{"path": "a.py", "spec_section": "def a(): pass", "depends_on": []}],
        },
        {
            "phase": 1,
            "name": "API",
            "files": [{"path": "b.py", "spec_section": "def b(): pass", "depends_on": ["a.py", "Foundation"]}],
        },
    ]}

    result = check_depends_on_references(plan)
    assert result.passed


def test_depends_on_references_fail_for_unknown_entries():
    plan = {"phases": [
        {
            "phase": 0,
            "name": "Foundation",
            "files": [{"path": "a.py", "spec_section": "def a(): pass", "depends_on": ["missing.py"]}],
        },
    ]}

    result = check_depends_on_references(plan)
    assert not result.passed
    assert "missing.py" in result.detail


def test_no_duplicates():
    files = _extract_files(GOOD_PLAN)
    result = check_no_duplicate_paths(files)
    assert result.passed


def test_import_completeness_pass():
    """File that imports the type it uses passes."""
    plan = {"phases": [{"phase": 0, "files": [
        {"path": "pkg/models.py", "spec_section": "class TaskDef:\n    name: str\n    command: str", "depends_on": []},
        {"path": "pkg/executor.py", "spec_section": "from pkg.models import TaskDef\n\ndef run(t: TaskDef) -> None: ...", "depends_on": ["pkg/models.py"]},
    ]}]}
    files = _extract_files(plan)
    result = check_import_completeness(files)
    assert result.passed


def test_import_completeness_fail():
    """File that references a type without importing it fails."""
    plan = {"phases": [{"phase": 0, "files": [
        {"path": "pkg/models.py", "spec_section": "class TaskDef:\n    name: str\n    command: str", "depends_on": []},
        {"path": "pkg/executor.py", "spec_section": "def run(t: TaskDef) -> None: ...", "depends_on": ["pkg/models.py"]},
    ]}]}
    files = _extract_files(plan)
    result = check_import_completeness(files)
    assert not result.passed
    assert "TaskDef" in result.detail


def test_full_validate():
    result = validate_plan("spec text", GOOD_PLAN)
    assert len(result.rubric_checks) == 11
