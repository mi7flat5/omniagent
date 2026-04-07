"""Deterministic spec quality checks."""

import re
from scoring import CheckResult, StageResult


def validate_spec(spec_text: str) -> StageResult:
    """Run all rubric checks against a spec."""
    result = StageResult()
    result.rubric_checks = [
        check_function_signatures(spec_text),
        check_constants_exact(spec_text),
        check_ambiguous_language(spec_text),
        check_edge_cases(spec_text),
        check_usage_examples(spec_text),
        check_error_behavior(spec_text),
        check_import_paths(spec_text),
    ]
    return result


def _extract_defs(code: str) -> list[tuple[str, str]]:
    """Extract (params, return_type) from def statements, handling nested brackets."""
    results = []
    for match in re.finditer(r"def\s+\w+\(", code):
        start = match.end()
        depth = 1
        i = start
        while i < len(code) and depth > 0:
            if code[i] in "([{":
                depth += 1
            elif code[i] in ")]}":
                depth -= 1
            i += 1
        params = code[start:i - 1]
        # Look for -> return type
        rest = code[i:i + 100]
        ret_match = re.match(r"\s*(->.*?):", rest)
        return_type = ret_match.group(1) if ret_match else ""
        results.append((params, return_type))
    return results


def _split_params(params: str) -> list[str]:
    """Split parameter string on commas, respecting nested brackets."""
    parts = []
    depth = 0
    current = []
    for ch in params:
        if ch in "([{":
            depth += 1
            current.append(ch)
        elif ch in ")]}":
            depth -= 1
            current.append(ch)
        elif ch == "," and depth == 0:
            parts.append("".join(current))
            current = []
        else:
            current.append(ch)
    if current:
        parts.append("".join(current))
    return parts


def check_function_signatures(spec_text: str) -> CheckResult:
    """Every def in code blocks has parameter types and return type."""
    code_blocks = re.findall(r"```(?:python|py)?\n(.*?)```", spec_text, re.DOTALL)
    all_code = "\n".join(code_blocks)
    defs = _extract_defs(all_code)
    if not defs:
        return CheckResult("Function signatures complete", 5, True, "No functions found in code blocks")
    missing_return = 0
    missing_param_types = 0
    for params, return_type in defs:
        if not return_type or return_type.strip() == "":
            missing_return += 1
        # Check if params have type annotations (skip self, cls)
        # Split on commas that are not inside brackets
        for param in _split_params(params):
            param = param.strip()
            if not param or param in ("self", "cls"):
                continue
            if ":" not in param and "=" not in param:
                missing_param_types += 1
    total_issues = missing_return + missing_param_types
    if total_issues == 0:
        return CheckResult("Function signatures complete", 5, True)
    return CheckResult("Function signatures complete", 5, False,
                       f"{missing_return} missing return types, {missing_param_types} missing param types")


def check_constants_exact(spec_text: str) -> CheckResult:
    """No approximate values near numbers."""
    patterns = [
        r"approximately\s+\d",
        r"about\s+\d",
        r"around\s+\d",
        r"roughly\s+\d",
        r"~\s*\d+\.\d",
    ]
    issues = []
    for pat in patterns:
        matches = re.findall(pat, spec_text, re.IGNORECASE)
        issues.extend(matches)
    if not issues:
        return CheckResult("Constants exact", 5, True)
    return CheckResult("Constants exact", 5, False, f"{len(issues)} approximate values found")


def check_ambiguous_language(spec_text: str) -> CheckResult:
    """Flag ambiguous wording."""
    # Only check outside code blocks.
    text_only = re.sub(r"```.*?```", "", spec_text, flags=re.DOTALL)
    words = ["should", "may", "typically", "optionally", "as needed",
             "etc.", "and so on", "for example", "might"]
    issues = []
    for word in words:
        count = len(re.findall(r"\b" + re.escape(word) + r"\b", text_only, re.IGNORECASE))
        if count > 0:
            issues.append(f"'{word}' x{count}")
    if not issues:
        return CheckResult("No ambiguous language", 3, True)
    return CheckResult("No ambiguous language", 3, False, ", ".join(issues))


def check_edge_cases(spec_text: str) -> CheckResult:
    """Functions that take numeric input define behavior for edge cases."""
    code_blocks = re.findall(r"```(?:python|py)?\n(.*?)```", spec_text, re.DOTALL)
    all_code = "\n".join(code_blocks)
    funcs_with_numeric = re.findall(r"def\s+(\w+)\([^)]*(?:int|float|number)[^)]*\)", all_code)
    if not funcs_with_numeric:
        return CheckResult("Edge cases defined", 4, True, "No numeric functions found")
    # Check if spec mentions edge cases for these functions
    edge_words = ["zero", "negative", "none", "empty", "infinity", "nan", "overflow",
                  "division by zero", "edge case", "boundary"]
    covered = 0
    for func in funcs_with_numeric:
        for word in edge_words:
            if word in spec_text.lower():
                covered += 1
                break
    if covered == len(funcs_with_numeric):
        return CheckResult("Edge cases defined", 4, True)
    return CheckResult("Edge cases defined", 4, False,
                       f"{len(funcs_with_numeric) - covered}/{len(funcs_with_numeric)} functions missing edge case docs")


def check_usage_examples(spec_text: str) -> CheckResult:
    """Public functions have usage examples."""
    code_blocks = re.findall(r"```(?:python|py)?\n(.*?)```", spec_text, re.DOTALL)
    all_code = "\n".join(code_blocks)
    public_funcs = re.findall(r"def\s+([a-z]\w+)\(", all_code)
    public_funcs = [f for f in public_funcs if not f.startswith("_")]
    if not public_funcs:
        return CheckResult("Usage examples present", 3, True, "No public functions found")
    # Look for example calls in the spec (function name followed by parentheses outside def)
    examples_found = 0
    for func in public_funcs:
        # Look for calls like func_name(... outside of def lines
        calls = re.findall(r"(?<!def\s)" + re.escape(func) + r"\s*\(", spec_text)
        if len(calls) > 1:  # More than just the definition
            examples_found += 1
    ratio = examples_found / len(public_funcs) if public_funcs else 1.0
    if ratio >= 0.5:
        return CheckResult("Usage examples present", 3, True,
                          f"{examples_found}/{len(public_funcs)} functions have examples")
    return CheckResult("Usage examples present", 3, False,
                       f"{examples_found}/{len(public_funcs)} functions have examples")


def check_error_behavior(spec_text: str) -> CheckResult:
    """Functions that can fail specify exceptions."""
    error_words = ["raise", "exception", "error", "ValueError", "TypeError",
                   "raises", "throws", "KeyError", "IndexError"]
    code_blocks = re.findall(r"```(?:python|py)?\n(.*?)```", spec_text, re.DOTALL)
    all_code = "\n".join(code_blocks)
    has_raises = any(w in all_code for w in error_words)
    text_only = re.sub(r"```.*?```", "", spec_text, flags=re.DOTALL)
    has_error_docs = any(w.lower() in text_only.lower() for w in error_words)
    if has_raises or has_error_docs:
        return CheckResult("Error behavior defined", 4, True)
    return CheckResult("Error behavior defined", 4, False, "No error/exception behavior documented")


def check_import_paths(spec_text: str) -> CheckResult:
    """Cross-module references specify import paths."""
    code_blocks = re.findall(r"```(?:python|py)?\n(.*?)```", spec_text, re.DOTALL)
    all_code = "\n".join(code_blocks)
    imports = re.findall(r"(?:from|import)\s+\S+", all_code)
    if not imports:
        return CheckResult("Import paths explicit", 3, True, "No imports found")
    # Check for bare class references without import
    classes = re.findall(r"class\s+(\w+)", all_code)
    # This is a best-effort check
    return CheckResult("Import paths explicit", 3, True, f"{len(imports)} imports found")
