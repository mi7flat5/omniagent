import json
from pathlib import Path

from validators.bugfix_rubric import validate_bugfix


DATA_DIR = Path(__file__).parent / "data" / "bugfix"


def _case_stems() -> list[str]:
    return sorted(path.name.removesuffix("_case.json") for path in DATA_DIR.glob("*_case.json"))


def test_bugfix_rubric_passes_good_reports():
    for stem in _case_stems():
        case_data = json.loads((DATA_DIR / f"{stem}_case.json").read_text(encoding="utf-8"))
        report_text = (DATA_DIR / f"{stem}_good_report.md").read_text(encoding="utf-8")

        result = validate_bugfix(report_text, case_data)

        assert all(check.passed for check in result.rubric_checks), stem


def test_bugfix_rubric_fails_vague_reports():
    for stem in _case_stems():
        case_data = json.loads((DATA_DIR / f"{stem}_case.json").read_text(encoding="utf-8"))
        report_text = (DATA_DIR / f"{stem}_bad_report.md").read_text(encoding="utf-8")

        result = validate_bugfix(report_text, case_data)
        checks = {check.name: check for check in result.rubric_checks}

        assert checks["Repro context stated"].passed is False, stem
        assert checks["Root cause stated"].passed is False, stem
        assert checks["Concrete fix described"].passed is False, stem
        assert checks["Verification reported"].passed is False, stem
        assert checks["No unsupported success claims"].passed is False, stem
