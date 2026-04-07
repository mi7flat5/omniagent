"""LLM-powered spec gap finder."""

from pathlib import Path
from models import call_llm_json
from scoring import StageResult, AdversaryGap


PROMPT_PATH = Path(__file__).parent.parent / "prompts" / "spec_adversary.md"


def run_adversary(config: dict, model_name: str, spec_text: str) -> StageResult:
    """Ask the LLM to find ambiguities in the spec."""
    system_prompt = PROMPT_PATH.read_text()
    result_json = call_llm_json(config, model_name, system_prompt, spec_text)

    result = StageResult()
    for gap in result_json.get("gaps", []):
        result.adversary_gaps.append(AdversaryGap(
            quote=gap.get("quote", ""),
            question=gap.get("question", ""),
            severity=gap.get("severity", "COSMETIC").upper(),
        ))
    return result
