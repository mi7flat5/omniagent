from pathlib import Path

from validators.review_rubric import validate_review


DATA_DIR = Path(__file__).parent / "data" / "review"


def _load_case() -> dict:
    import json

    return json.loads((DATA_DIR / "confctl_source_of_truth_case.json").read_text(encoding="utf-8"))


def test_review_rubric_passes_good_confctl_report():
    case_data = _load_case()
    report_text = (DATA_DIR / "confctl_good_report.md").read_text(encoding="utf-8")

    result = validate_review(report_text, case_data)

    assert all(check.passed for check in result.rubric_checks)


def test_review_rubric_fails_bad_confctl_report():
    case_data = _load_case()
    report_text = (DATA_DIR / "confctl_bad_report.md").read_text(encoding="utf-8")

    result = validate_review(report_text, case_data)
    checks = {check.name: check for check in result.rubric_checks}

    assert checks["Findings-first structure"].passed is False
    assert checks["Required findings covered"].passed is False
    assert checks["No forbidden claims"].passed is False
    assert checks["No filler sections"].passed is False
