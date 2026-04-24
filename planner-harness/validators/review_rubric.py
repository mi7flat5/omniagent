"""Deterministic review and audit quality checks."""

import re

from scoring import CheckResult, StageResult
from validators.eval_utils import line_matches_heading, matched_case_items, normalize_text


SEVERITY_PATTERN = re.compile(r"\b(critical|high|medium|low)\b", re.IGNORECASE)
NON_FINDING_PREFIXES = ("summary", "overview", "background", "context")


def validate_review(report_text: str, case_data: dict) -> StageResult:
    """Run all review rubric checks against a report and tracked case."""
    result = StageResult()
    result.rubric_checks = [
        check_findings_first(report_text),
        check_baseline_summary(report_text, case_data),
        check_required_findings(report_text, case_data),
        check_cluster_coverage(report_text, case_data),
        check_forbidden_patterns(report_text, case_data),
        check_forbidden_sections(report_text, case_data),
    ]
    return result


def _content_lines(report_text: str) -> list[str]:
    return [
        line.strip()
        for line in report_text.splitlines()
        if line.strip() and not line.strip().startswith("#")
    ]


def _has_severity_marker(line: str) -> bool:
    return bool(SEVERITY_PATTERN.search(line))


def check_findings_first(report_text: str) -> CheckResult:
    """Report should lead with findings instead of summary filler."""
    lines = _content_lines(report_text)[:8]
    if not lines:
        return CheckResult("Findings-first structure", 4, False, "Report is empty")

    first_finding_index = None
    for index, line in enumerate(lines):
        if _has_severity_marker(line):
            first_finding_index = index
            break

    if first_finding_index is None:
        return CheckResult(
            "Findings-first structure",
            4,
            False,
            "No severity-tagged finding appears near the start of the report",
        )

    leading_non_findings = [
        line for line in lines[:first_finding_index]
        if normalize_text(line).startswith(NON_FINDING_PREFIXES)
    ]
    if leading_non_findings:
        return CheckResult(
            "Findings-first structure",
            4,
            False,
            f"Leading non-finding sections before first finding: {leading_non_findings[0]}",
        )
    return CheckResult("Findings-first structure", 4, True)


def check_baseline_summary(report_text: str, case_data: dict) -> CheckResult:
    """When a case provides an explicit baseline, the report should reflect it."""
    baseline = case_data.get("baseline", {})
    required_terms = baseline.get("required_terms", [])
    if not required_terms:
        return CheckResult("Baseline reflected", 3, True, "No baseline terms required")

    missing = [term for term in required_terms if not normalize_text(term) in normalize_text(report_text)]
    if not missing:
        return CheckResult("Baseline reflected", 3, True)
    return CheckResult(
        "Baseline reflected",
        3,
        False,
        f"Missing baseline terms: {', '.join(missing)}",
    )


def check_required_findings(report_text: str, case_data: dict) -> CheckResult:
    """All required case findings should be covered by the report."""
    required_findings = case_data.get("required_findings", [])
    if not required_findings:
        return CheckResult("Required findings covered", 6, True, "No required findings")

    matched = matched_case_items(report_text, required_findings)
    matched_ids = {item["id"] for item in matched if "id" in item}
    missing = [item["id"] for item in required_findings if item.get("id") not in matched_ids]
    if not missing:
        return CheckResult("Required findings covered", 6, True)
    return CheckResult(
        "Required findings covered",
        6,
        False,
        f"Missing findings: {', '.join(missing)}",
    )


def check_cluster_coverage(report_text: str, case_data: dict) -> CheckResult:
    """Explicitly distinct failure clusters should remain distinct in the report."""
    required_findings = case_data.get("required_findings", [])
    min_clusters = int(case_data.get("min_required_clusters", 0) or 0)
    if not required_findings or min_clusters <= 0:
        return CheckResult("Distinct clusters preserved", 5, True, "No cluster minimum")

    matched = matched_case_items(report_text, required_findings)
    matched_clusters = sorted({item.get("cluster", "") for item in matched if item.get("cluster")})
    if len(matched_clusters) >= min_clusters:
        return CheckResult(
            "Distinct clusters preserved",
            5,
            True,
            f"{len(matched_clusters)}/{min_clusters} clusters: {', '.join(matched_clusters)}",
        )
    return CheckResult(
        "Distinct clusters preserved",
        5,
        False,
        f"Only {len(matched_clusters)}/{min_clusters} clusters covered: {', '.join(matched_clusters) or 'none'}",
    )


def check_forbidden_patterns(report_text: str, case_data: dict) -> CheckResult:
    """Reports should not contain explicitly forbidden invented or unsupported claims."""
    normalized_report = normalize_text(report_text)
    hits: list[str] = []
    for pattern_group in case_data.get("forbidden_patterns", []):
        pattern_id = pattern_group.get("id", "unknown")
        patterns = pattern_group.get("patterns", [])
        if any(normalize_text(pattern) in normalized_report for pattern in patterns):
            hits.append(pattern_id)
    if not hits:
        return CheckResult("No forbidden claims", 5, True)
    return CheckResult(
        "No forbidden claims",
        5,
        False,
        f"Forbidden claim groups present: {', '.join(hits)}",
    )


def check_forbidden_sections(report_text: str, case_data: dict) -> CheckResult:
    """Avoid generic filler sections that the case does not support."""
    forbidden_titles = case_data.get("forbidden_section_titles", [])
    if not forbidden_titles:
        return CheckResult("No filler sections", 4, True, "No forbidden sections")

    lines = [line.strip() for line in report_text.splitlines() if line.strip()]
    hits: list[str] = []
    for title in forbidden_titles:
        if any(line_matches_heading(line, title) for line in lines):
            hits.append(title)
    if not hits:
        return CheckResult("No filler sections", 4, True)
    return CheckResult(
        "No filler sections",
        4,
        False,
        f"Forbidden sections present: {', '.join(hits)}",
    )
