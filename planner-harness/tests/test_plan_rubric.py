import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent.parent))

import pytest
from validators.plan_rubric import (
    validate_plan,
    check_spec_sections_present,
    check_no_circular_deps,
    check_phase_ordering,
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
    assert len(result.rubric_checks) == 10
