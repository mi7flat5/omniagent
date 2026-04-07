"""Deterministic plan quality checks."""

import json
import re
from scoring import CheckResult, StageResult


def validate_plan(spec_text: str, plan_json: dict) -> StageResult:
    """Run all rubric checks against a plan."""
    files = _extract_files(plan_json)
    result = StageResult()
    result.rubric_checks = [
        check_spec_sections_present(files),
        check_test_coverage(files, spec_text),
        check_test_depends_on(files),
        check_no_circular_deps(files),
        check_phase_ordering(plan_json),
        check_depends_on_references(plan_json),
        check_spec_section_consistency(files),
        check_no_duplicate_paths(files),
        check_spec_section_nontrivial(files),
        check_test_spec_matches_source(files),
        check_import_completeness(files),
    ]
    return result


def _extract_files(plan_json: dict) -> list[dict]:
    """Extract flat list of file entries from plan JSON (handles both formats)."""
    files = []
    for phase in plan_json.get("phases", []):
        phase_idx = phase.get("phase", 0)
        for f in phase.get("files", phase.get("tasks", [])):
            entry = {
                "path": f.get("path", f.get("file", "")),
                "spec_section": f.get("spec_section", ""),
                "depends_on": f.get("depends_on", []),
                "description": f.get("description", ""),
                "test": f.get("test", False),
                "phase": phase_idx,
            }
            # Auto-detect test files
            if not entry["test"]:
                p = entry["path"]
                entry["test"] = ("test_" in p or "_test" in p or p.startswith("tests/"))
            files.append(entry)
    return files


def check_spec_sections_present(files: list[dict]) -> CheckResult:
    """Every file has a non-empty spec_section."""
    missing = [f["path"] for f in files if not f["spec_section"]]
    if not missing:
        return CheckResult("Every file has spec_section", 5, True)
    return CheckResult("Every file has spec_section", 5, False,
                       f"{len(missing)} files missing: {', '.join(missing[:5])}")


def _test_stems_for_source(source_path: str) -> list[str]:
    """Generate likely test filename stems for a source path."""
    filename = source_path.split("/")[-1].rsplit(".", 1)[0]
    if filename == "__init__":
        parts = source_path.replace("\\", "/").split("/")
        parent = parts[-2] if len(parts) >= 2 else "__init__"
        return [f"{parent}_init", f"test_{parent}_init", f"{parent}___init__"]

    stems = [filename]
    parts = source_path.replace("\\", "/").split("/")
    if len(parts) >= 2:
        stems.append(f"{parts[-2]}_{filename}")
    return stems


def _direct_test_coverage(files: list[dict]) -> set[str]:
    """Find sources covered by direct test filename matching."""
    test_files = [f["path"] for f in files if f["test"]]
    covered = set()
    for source in files:
        if source["test"]:
            continue
        stems = _test_stems_for_source(source["path"])
        if any(
            any(f"test_{stem}" in test_path or f"{stem}_test" in test_path for stem in stems)
            for test_path in test_files
        ):
            covered.add(source["path"])
    return covered


def _transitive_test_coverage(files: list[dict]) -> set[str]:
    """Find sources covered through transitive test dependencies."""
    path_to_entry = {f["path"]: f for f in files}
    covered = set()
    visited = set()
    stack = [f["path"] for f in files if f["test"]]

    while stack:
        current = stack.pop()
        if current in visited:
            continue
        visited.add(current)
        entry = path_to_entry.get(current)
        if not entry:
            continue
        for dep in entry["depends_on"]:
            dep_entry = path_to_entry.get(dep)
            if not dep_entry:
                continue
            if not dep_entry["test"]:
                covered.add(dep)
            stack.append(dep)

    return covered


def _is_coverage_exempt(source_path: str, spec_text: str) -> bool:
    """Honor explicit spec directives that scope test coverage."""
    normalized = spec_text.lower()
    if source_path.startswith("frontend/"):
        if "no test files specified for frontend" in normalized:
            return True
        if "focus on backend tests" in normalized:
            return True
        if "backend-only test coverage" in normalized:
            return True
    return False


def check_test_coverage(files: list[dict], spec_text: str = "") -> CheckResult:
    """Every required source file is covered by a direct or transitive test path."""
    covered = _direct_test_coverage(files) | _transitive_test_coverage(files)
    uncovered = []
    for source in files:
        if source["test"]:
            continue
        if _is_coverage_exempt(source["path"], spec_text):
            continue
        if source["path"] not in covered:
            uncovered.append(source["path"])
    if not uncovered:
        return CheckResult("Test coverage", 5, True)
    return CheckResult("Test coverage", 5, False,
                       f"{len(uncovered)} untested: {', '.join(uncovered[:5])}")


def check_test_depends_on(files: list[dict]) -> CheckResult:
    """Every test file's depends_on includes the source it tests."""
    issues = []
    for f in files:
        if not f["test"]:
            continue
        if not f["depends_on"]:
            issues.append(f"{f['path']}: empty depends_on")
    if not issues:
        return CheckResult("Test depends_on correct", 4, True)
    return CheckResult("Test depends_on correct", 4, False,
                       f"{len(issues)} tests missing deps: {', '.join(issues[:3])}")


def check_no_circular_deps(files: list[dict]) -> CheckResult:
    """Topological sort succeeds."""
    path_set = {f["path"] for f in files}
    adj: dict[str, list[str]] = {f["path"]: [] for f in files}
    for f in files:
        for dep in f["depends_on"]:
            if dep in path_set:
                adj[dep].append(f["path"])

    visited = set()
    rec_stack = set()
    cycle_node = None

    def dfs(node):
        nonlocal cycle_node
        visited.add(node)
        rec_stack.add(node)
        for neighbor in adj.get(node, []):
            if neighbor not in visited:
                if dfs(neighbor):
                    return True
            elif neighbor in rec_stack:
                cycle_node = neighbor
                return True
        rec_stack.discard(node)
        return False

    for node in adj:
        if node not in visited:
            if dfs(node):
                return CheckResult("No circular dependencies", 5, False,
                                   f"Cycle involving: {cycle_node}")
    return CheckResult("No circular dependencies", 5, True)


def check_phase_ordering(plan_json: dict) -> CheckResult:
    """No file depends on a file in a later phase."""
    path_to_phase = {}
    for phase in plan_json.get("phases", []):
        phase_idx = phase.get("phase", 0)
        for f in phase.get("files", phase.get("tasks", [])):
            path = f.get("path", f.get("file", ""))
            path_to_phase[path] = phase_idx

    violations = []
    for phase in plan_json.get("phases", []):
        phase_idx = phase.get("phase", 0)
        for f in phase.get("files", phase.get("tasks", [])):
            path = f.get("path", f.get("file", ""))
            for dep in f.get("depends_on", []):
                dep_phase = path_to_phase.get(dep)
                if dep_phase is not None and dep_phase > phase_idx:
                    violations.append(f"{path} (phase {phase_idx}) depends on {dep} (phase {dep_phase})")
    if not violations:
        return CheckResult("Phase ordering valid", 5, True)
    return CheckResult("Phase ordering valid", 5, False,
                       f"{len(violations)} violations: {violations[0]}")


def check_depends_on_references(plan_json: dict) -> CheckResult:
    """Every depends_on entry resolves to a known file path or phase/group name."""
    known_files = set()
    known_phase_names = set()

    for phase in plan_json.get("phases", []):
        phase_name = phase.get("name")
        if isinstance(phase_name, str) and phase_name:
            known_phase_names.add(phase_name)
        for entry in phase.get("files", phase.get("tasks", [])):
            path = entry.get("path", entry.get("file", ""))
            if isinstance(path, str) and path:
                known_files.add(path)

    issues = []
    for phase in plan_json.get("phases", []):
        for entry in phase.get("files", phase.get("tasks", [])):
            path = entry.get("path", entry.get("file", ""))
            for dep in entry.get("depends_on", []):
                if not isinstance(dep, str) or not dep:
                    continue
                if dep not in known_files and dep not in known_phase_names:
                    issues.append(f"{path} depends_on unknown file or group '{dep}'")

    if not issues:
        return CheckResult("Dependency references valid", 5, True)
    return CheckResult("Dependency references valid", 5, False,
                       f"{len(issues)} invalid references: {issues[0]}")


def check_spec_section_consistency(files: list[dict]) -> CheckResult:
    """Cross-file: if file A defines class Foo and file B imports Foo, signatures match."""
    all_defs: dict[str, list[tuple[str, str]]] = {}
    for f in files:
        ss = f["spec_section"]
        for match in re.finditer(r"(class|def)\s+(\w+)\s*[\(:]", ss):
            name = match.group(2)
            line = ss[match.start():ss.find("\n", match.start())]
            all_defs.setdefault(name, []).append((f["path"], line.strip()))

    mismatches = []
    for name, occurrences in all_defs.items():
        if len(occurrences) < 2:
            continue
        # Skip dunder names — __init__, __repr__, etc. are expected to differ across files
        if name.startswith("__") and name.endswith("__"):
            continue
        # Only check cross-file mismatches (same name in different files)
        by_file: dict[str, set[str]] = {}
        for path, sig in occurrences:
            by_file.setdefault(path, set()).add(sig)
        if len(by_file) < 2:
            continue  # All in one file — subclass overrides, not a cross-file issue
        # Compare signatures across files (use first signature per file as representative)
        cross_sigs = set()
        for path, sigs in by_file.items():
            cross_sigs.update(sigs)
        if len(cross_sigs) > 1:
            mismatches.append(f"{name}: {', '.join(by_file.keys())}")

    if not mismatches:
        return CheckResult("Spec_section consistency", 4, True)
    return CheckResult("Spec_section consistency", 4, False,
                       f"{len(mismatches)} mismatches: {mismatches[0]}")


def check_no_duplicate_paths(files: list[dict]) -> CheckResult:
    """Every file path is unique."""
    paths = [f["path"] for f in files]
    dupes = [p for p in paths if paths.count(p) > 1]
    if not dupes:
        return CheckResult("No duplicate paths", 3, True)
    return CheckResult("No duplicate paths", 3, False, f"Duplicates: {', '.join(set(dupes))}")


def check_spec_section_nontrivial(files: list[dict]) -> CheckResult:
    """spec_section is >100 chars."""
    trivial = [f["path"] for f in files if f["spec_section"] and len(f["spec_section"]) < 100]
    if not trivial:
        return CheckResult("Spec_section non-trivial", 3, True)
    return CheckResult("Spec_section non-trivial", 3, False,
                       f"{len(trivial)} too short: {', '.join(trivial[:3])}")


def _primary_dep(test_path: str, depends_on: list[str]) -> str | None:
    """Find the primary source file a test is testing (by name match)."""
    test_name = test_path.split("/")[-1].replace(".py", "")
    for dep in depends_on:
        dep_name = dep.split("/")[-1].replace(".py", "")
        if f"test_{dep_name}" == test_name or dep_name in test_name:
            return dep
    return depends_on[0] if depends_on else None


def check_test_spec_matches_source(files: list[dict]) -> CheckResult:
    """Test file's spec_section covers functions from its primary source."""
    source_map = {f["path"]: f for f in files if not f["test"]}
    issues = []
    for f in files:
        if not f["test"] or not f["depends_on"]:
            continue
        primary = _primary_dep(f["path"], f["depends_on"])
        if not primary:
            continue
        src = source_map.get(primary)
        if not src:
            continue
        src_funcs = set(re.findall(r"def\s+(\w+)", src["spec_section"]))
        test_funcs = set(re.findall(r"def\s+(\w+)", f["spec_section"]))
        src_names_in_test = any(name in f["spec_section"] for name in src_funcs if not name.startswith("_"))
        if src_funcs and not src_names_in_test and not test_funcs:
            issues.append(f"{f['path']} doesn't reference {primary} functions")
    if not issues:
        return CheckResult("Test spec matches source spec", 4, True)
    return CheckResult("Test spec matches source spec", 4, False,
                       f"{len(issues)} issues: {issues[0]}")


def _parent_module_paths(file_path: str) -> list[str]:
    """Generate possible parent package import paths for a file.

    E.g., "sci_calc/utils/history.py" yields:
    - "sci_calc.utils.history"
    - "sci_calc.utils"
    - "sci_calc"
    """
    parts = file_path.replace(".py", "").replace("/", ".")
    result = [parts]
    while "." in parts:
        parts = parts.rsplit(".", 1)[0]
        result.append(parts)
    return result


def _top_level_defined_classes(spec_section: str) -> set[str]:
    """Collect top-level class names and ignore nested helper classes.

    Nested classes such as Pydantic ``class Config`` blocks are implementation
    details inside another class body and should not be treated as exported
    cross-file types.
    """
    classes = set()
    for line in spec_section.splitlines():
        if not line or line[0].isspace():
            continue
        match = re.match(r"class\s+(\w+)", line)
        if match and not match.group(1).startswith("_"):
            classes.add(match.group(1))
    return classes


def check_import_completeness(files: list[dict]) -> CheckResult:
    """Spec_sections that reference types from other files must include import statements.

    For each non-test file, collect class/type names defined in its spec_section.
    Then for every other non-test file, if its spec_section uses one of those names
    (in a signature, type hint, or reference) but doesn't have an import/from statement
    mentioning the defining module, flag it.
    """
    # Build map: type_name -> defining file path
    type_to_file: dict[str, str] = {}
    for f in files:
        if f["test"]:
            continue
        ss = f["spec_section"]
        for name in _top_level_defined_classes(ss):
            type_to_file[name] = f["path"]
        # Find top-level type aliases / enum-like names (e.g., TaskStatus)
        # These are often dataclasses or enums referenced as type hints elsewhere

    if not type_to_file:
        return CheckResult("Import completeness", 5, True, "No cross-file types found")

    issues = []
    for f in files:
        if f["test"]:
            continue
        ss = f["spec_section"]
        # Find all type names referenced in this file's spec_section
        # Look in type hints: param: TypeName, -> TypeName, list[TypeName], dict[str, TypeName]
        referenced_types = set(re.findall(r'\b([A-Z]\w+)\b', ss))

        for type_name in referenced_types:
            defining_file = type_to_file.get(type_name)
            if not defining_file or defining_file == f["path"]:
                continue  # Defined locally or not in our type map

            # Check if there's an import statement for the defining module
            # Extract module name from path: "taskpipe/models.py" -> "models" or "taskpipe.models"
            module_parts = defining_file.replace(".py", "").replace("/", ".")
            module_name = defining_file.replace(".py", "").split("/")[-1]

            # Look for any import statement that brings this type name into scope.
            # Covers: from X import TypeName, from X.Y import TypeName,
            # from X.parent import TypeName (package re-export), import X
            has_import = (
                # Direct: "import TypeName" appears (e.g., "from foo import TypeName")
                f"import {type_name}" in ss
                # Module path: "from taskpipe.models" or "from models"
                or f"from {module_parts}" in ss
                or f"from {module_name}" in ss
                # Parent package re-export: "from sci_calc.utils import HistoryManager"
                # when HistoryManager is defined in sci_calc/utils/history.py
                or any(
                    f"from {parent_path}" in ss
                    for parent_path in _parent_module_paths(defining_file)
                )
            )

            if not has_import:
                issues.append(f"{f['path']} uses {type_name} (from {defining_file}) without import")

    if not issues:
        return CheckResult("Import completeness", 5, True)
    return CheckResult("Import completeness", 5, False,
                       f"{len(issues)} missing imports: {issues[0]}")
