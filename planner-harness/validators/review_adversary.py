"""LLM-powered review gap finder and unsupported-claim detector."""

import json
from pathlib import Path

from models import call_llm_json
from scoring import AdversaryGap, AdversaryGuess, StageResult


PROMPT_PATH = Path(__file__).parent.parent / "prompts" / "review_adversary.md"


def run_adversary(config: dict, model_name: str, case_data: dict, report_text: str) -> StageResult:
    """Ask the model to find unsupported or missing review/report content."""
    system_prompt = PROMPT_PATH.read_text()
    user_prompt = "\n\n".join(
        [
            "Case JSON:",
            json.dumps(case_data, indent=2),
            "Report:",
            report_text,
        ]
    )
    result_json = call_llm_json(config, model_name, system_prompt, user_prompt)

    result = StageResult()
    for gap in result_json.get("gaps", []):
        result.adversary_gaps.append(AdversaryGap(
            quote=gap.get("quote", ""),
            question=gap.get("question", ""),
            severity=gap.get("severity", "COSMETIC").upper(),
        ))
    for guess in result_json.get("guesses", []):
        result.adversary_guesses.append(AdversaryGuess(
            what=guess.get("what", ""),
            why=guess.get("why", ""),
            severity=guess.get("severity", "COSMETIC").upper(),
            file_path=guess.get("file_path", ""),
        ))
    return result
