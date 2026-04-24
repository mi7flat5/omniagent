"""Deterministic bugfix writeup quality checks."""

from scoring import CheckResult, StageResult
from validators.eval_utils import normalize_text, phrase_groups_covered


def validate_bugfix(report_text: str, case_data: dict) -> StageResult:
    """Run all bugfix rubric checks against a report and tracked case."""
    result = StageResult()
    result.rubric_checks = [
        check_repro_context(report_text, case_data),
        check_root_cause(report_text, case_data),
        check_fix_description(report_text, case_data),
        check_verification(report_text, case_data),
        check_forbidden_patterns(report_text, case_data),
    ]
    return result


def _check_groups(report_text: str,
                  groups: list[list[str]],
                  *,
                  name: str,
                  weight: int,
                  failure_detail: str) -> CheckResult:
    if not groups:
        return CheckResult(name, weight, True, "No phrase groups required")
    if phrase_groups_covered(report_text, groups):
        return CheckResult(name, weight, True)
    return CheckResult(name, weight, False, failure_detail)


def check_repro_context(report_text: str, case_data: dict) -> CheckResult:
    return _check_groups(
        report_text,
        case_data.get("repro_groups", []),
        name="Repro context stated",
        weight=4,
        failure_detail="Missing repro/problem statement",
    )


def check_root_cause(report_text: str, case_data: dict) -> CheckResult:
    return _check_groups(
        report_text,
        case_data.get("root_cause_groups", []),
        name="Root cause stated",
        weight=5,
        failure_detail="Missing root-cause explanation",
    )


def check_fix_description(report_text: str, case_data: dict) -> CheckResult:
    return _check_groups(
        report_text,
        case_data.get("fix_groups", []),
        name="Concrete fix described",
        weight=5,
        failure_detail="Missing concrete fix description",
    )


def check_verification(report_text: str, case_data: dict) -> CheckResult:
    return _check_groups(
        report_text,
        case_data.get("verification_groups", []),
        name="Verification reported",
        weight=6,
        failure_detail="Missing verification evidence",
    )


def check_forbidden_patterns(report_text: str, case_data: dict) -> CheckResult:
    normalized_report = normalize_text(report_text)
    hits: list[str] = []
    for pattern_group in case_data.get("forbidden_patterns", []):
        pattern_id = pattern_group.get("id", "unknown")
        if any(normalize_text(pattern) in normalized_report for pattern in pattern_group.get("patterns", [])):
            hits.append(pattern_id)
    if not hits:
        return CheckResult("No unsupported success claims", 4, True)
    return CheckResult(
        "No unsupported success claims",
        4,
        False,
        f"Forbidden claim groups present: {', '.join(hits)}",
    )
