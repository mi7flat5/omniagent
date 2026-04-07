import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent.parent))

import pytest
from validators.spec_rubric import (
    check_function_signatures,
    check_constants_exact,
    check_ambiguous_language,
    check_edge_cases,
    validate_spec,
)


def test_signatures_pass():
    spec = '```python\ndef add(a: int, b: int) -> int:\n    pass\n```'
    result = check_function_signatures(spec)
    assert result.passed


def test_signatures_missing_return():
    spec = '```python\ndef add(a: int, b: int):\n    pass\n```'
    result = check_function_signatures(spec)
    assert not result.passed
    assert "missing return" in result.detail


def test_constants_pass():
    spec = "The value of pi is 3.141592653589793."
    result = check_constants_exact(spec)
    assert result.passed


def test_constants_fail():
    spec = "The value is approximately 3.14."
    result = check_constants_exact(spec)
    assert not result.passed


def test_ambiguous_pass():
    spec = "The function returns an integer."
    result = check_ambiguous_language(spec)
    assert result.passed


def test_ambiguous_fail():
    spec = "The function should return an integer. It may also return None."
    result = check_ambiguous_language(spec)
    assert not result.passed
    assert "'should'" in result.detail


def test_ambiguous_ignores_code_blocks():
    spec = "Clean text.\n```python\n# should not flag this\n```"
    result = check_ambiguous_language(spec)
    assert result.passed


def test_full_validate():
    spec = '```python\ndef add(a: int, b: int) -> int:\n    pass\n```\nReturns sum. Raises ValueError on invalid input.'
    result = validate_spec(spec)
    assert len(result.rubric_checks) == 7
    assert result.rubric_score > 0
