"""LLM-powered plan gap finder and cross-file consistency checker."""

import re
from pathlib import Path
from models import call_llm_json
from scoring import StageResult, AdversaryGuess, Contradiction


ADVERSARY_PROMPT = Path(__file__).parent.parent / "prompts" / "plan_adversary.md"
CROSS_CHECK_PROMPT = Path(__file__).parent.parent / "prompts" / "plan_cross_check.md"


def _extract_files(plan_json: dict) -> list[dict]:
    """Extract flat list of file entries."""
    files = []
    for phase in plan_json.get("phases", []):
        for f in phase.get("files", phase.get("tasks", [])):
            files.append({
                "path": f.get("path", f.get("file", "")),
                "spec_section": f.get("spec_section", ""),
                "depends_on": f.get("depends_on", []),
            })
    return files


def run_adversary(config: dict, model_name: str, plan_json: dict) -> StageResult:
    """Run adversary checks on each file, then cross-file consistency."""
    files = _extract_files(plan_json)
    result = StageResult()

    adversary_template = ADVERSARY_PROMPT.read_text()
    cross_template = CROSS_CHECK_PROMPT.read_text()

    # Per-file: ask model what it would have to guess
    for f in files:
        if not f["spec_section"]:
            result.adversary_guesses.append(AdversaryGuess(
                what="Entire implementation",
                why="No spec_section provided",
                severity="BLOCKING",
                file_path=f["path"],
            ))
            continue

        prompt = adversary_template.replace("{{spec_section}}", f["spec_section"])
        prompt = prompt.replace("{{file_path}}", f["path"])

        try:
            resp = call_llm_json(config, model_name, "You are a code reviewer.", prompt)
            for g in resp.get("guesses", []):
                result.adversary_guesses.append(AdversaryGuess(
                    what=g.get("what", ""),
                    why=g.get("why", ""),
                    severity=g.get("severity", "COSMETIC").upper(),
                    file_path=f["path"],
                ))
        except Exception as e:
            result.adversary_guesses.append(AdversaryGuess(
                what=f"LLM call failed: {e}",
                why="Could not evaluate",
                severity="BLOCKING",
                file_path=f["path"],
            ))

    # Cross-file consistency: check pairs connected by depends_on
    path_to_spec = {f["path"]: f["spec_section"] for f in files}
    checked_pairs = set()
    for f in files:
        for dep in f["depends_on"]:
            pair = tuple(sorted([f["path"], dep]))
            if pair in checked_pairs:
                continue
            checked_pairs.add(pair)

            dep_spec = path_to_spec.get(dep, "")
            if not dep_spec or not f["spec_section"]:
                continue

            prompt = cross_template.replace("{{path_a}}", dep)
            prompt = prompt.replace("{{spec_section_a}}", dep_spec)
            prompt = prompt.replace("{{path_b}}", f["path"])
            prompt = prompt.replace("{{spec_section_b}}", f["spec_section"])

            try:
                resp = call_llm_json(config, model_name, "You are a code reviewer.", prompt)
                for c in resp.get("contradictions", []):
                    result.contradictions.append(Contradiction(
                        description=c.get("description", ""),
                        file_a=dep,
                        file_a_says=c.get("file_a_says", ""),
                        file_b=f["path"],
                        file_b_says=c.get("file_b_says", ""),
                    ))
            except Exception:
                pass  # Cross-check is best-effort

    return result
