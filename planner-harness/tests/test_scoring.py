import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent.parent))

import pytest
from scoring import CheckResult, StageResult, AdversaryGap, AdversaryGuess, Contradiction, Report


def test_rubric_score_all_pass():
    result = StageResult(rubric_checks=[
        CheckResult("check1", 5, True),
        CheckResult("check2", 3, True),
    ])
    assert result.rubric_score == 100.0


def test_rubric_score_partial():
    result = StageResult(rubric_checks=[
        CheckResult("check1", 5, True),
        CheckResult("check2", 5, False),
    ])
    assert result.rubric_score == 50.0


def test_rubric_score_empty():
    result = StageResult()
    assert result.rubric_score == 100.0


def test_adversary_score_no_gaps():
    result = StageResult()
    assert result.adversary_score == 100.0


def test_adversary_score_with_gaps():
    result = StageResult(adversary_gaps=[
        AdversaryGap("text", "what?", "BLOCKING"),
        AdversaryGap("text", "hm?", "COSMETIC"),
    ])
    assert result.adversary_score == 94.0  # 100 - 5 - 1


def test_adversary_score_with_contradictions():
    result = StageResult(contradictions=[
        Contradiction("mismatch", "a.py", "int", "b.py", "str"),
    ])
    assert result.adversary_score == 95.0  # 100 - 5


def test_combined_score():
    result = StageResult(
        rubric_checks=[CheckResult("c1", 10, True)],
        adversary_gaps=[AdversaryGap("t", "q", "BLOCKING")],
    )
    # rubric = 100, adversary = 95
    # combined = 100*0.4 + 95*0.6 = 40 + 57 = 97
    assert result.combined_score == 97.0


def test_report_markdown():
    report = Report(model="test-model", prompt="test-prompt")
    report.spec_result = StageResult(rubric_checks=[
        CheckResult("sigs", 5, True),
        CheckResult("ambig", 3, False, "found 3 issues"),
    ])
    md = report.to_markdown()
    assert "Spec Score:" in md
    assert "[PASS] sigs" in md
    assert "[FAIL] ambig" in md
