"""Orchestrates validation stages."""

import json
from pathlib import Path
from rich.console import Console
from rich.table import Table

from models import load_config, call_llm
from scoring import Report, StageResult
from validators.spec_rubric import validate_spec as spec_rubric
from validators.spec_adversary import run_adversary as spec_adversary
from validators.plan_rubric import validate_plan as plan_rubric
from validators.plan_adversary import run_adversary as plan_adversary

console = Console()


def resolve_model(config: dict, model_name: str | None) -> str:
    """Resolve model name from args or config defaults."""
    if model_name:
        return model_name
    return config.get("defaults", {}).get("adversary_model", list(config["models"].keys())[0])


PROMPT_GENERATOR = Path(__file__).parent / "prompts" / "prompt_generator.md"


def generate_prompt(spec_path: str, config: dict, model_name: str,
                    output_path: str) -> str:
    """Generate a project-specific planner prompt from a spec."""
    spec_text = Path(spec_path).read_text()
    meta_prompt = PROMPT_GENERATOR.read_text()

    console.print(f"\n[bold]Generating planner prompt:[/bold] {spec_path} + {model_name}")

    raw = call_llm(config, model_name, meta_prompt, spec_text)

    # Strip thinking tags, fences, and leaked special tokens
    import re
    text = raw.strip()
    text = re.sub(r'<think>.*?</think>', '', text, flags=re.DOTALL).strip()
    text = re.sub(r'<\|im_end\|>|<\|endoftext\|>|<\|end\|>', '', text).strip()

    Path(output_path).parent.mkdir(parents=True, exist_ok=True)
    Path(output_path).write_text(text)
    console.print(f"  Prompt saved to {output_path} ({len(text)} chars)")

    return output_path


def validate_spec_pipeline(spec_path: str, config: dict, model_name: str) -> StageResult:
    """Run spec validation: rubric + adversary."""
    spec_text = Path(spec_path).read_text()
    console.print(f"\n[bold]Validating spec:[/bold] {spec_path}")

    # Rubric
    console.print("  Running rubric checks...", style="dim")
    rubric_result = spec_rubric(spec_text)

    # Adversary
    console.print(f"  Running adversary ({model_name})...", style="dim")
    adv_result = spec_adversary(config, model_name, spec_text)

    # Merge
    combined = StageResult(
        rubric_checks=rubric_result.rubric_checks,
        adversary_gaps=adv_result.adversary_gaps,
    )

    _print_stage("Spec", combined)
    return combined


def generate_plan(spec_path: str, prompt_path: str, config: dict,
                  model_name: str, output_path: str) -> dict:
    """Generate PLAN.json from spec using planner prompt."""
    spec_text = Path(spec_path).read_text()
    prompt_text = Path(prompt_path).read_text()

    console.print(f"\n[bold]Generating plan:[/bold] {prompt_path} + {model_name}")

    full_prompt = prompt_text + "\n\n---\n\n" + spec_text
    raw = call_llm(config, model_name, "You are a project planner.", full_prompt)

    # Parse JSON from response
    text = raw.strip()
    if text.startswith("```"):
        lines = text.split("\n")
        lines = [l for l in lines if not l.strip().startswith("```")]
        text = "\n".join(lines)
    plan = json.loads(text)

    Path(output_path).parent.mkdir(parents=True, exist_ok=True)
    Path(output_path).write_text(json.dumps(plan, indent=2))
    console.print(f"  Plan saved to {output_path}")

    phases = plan.get("phases", [])
    total_files = sum(len(p.get("files", p.get("tasks", []))) for p in phases)
    console.print(f"  {len(phases)} phases, {total_files} files")

    return plan


def validate_plan_pipeline(spec_path: str, plan_path: str,
                           config: dict, model_name: str) -> StageResult:
    """Run plan validation: rubric + adversary."""
    spec_text = Path(spec_path).read_text()
    with open(plan_path) as f:
        plan_json = json.load(f)

    console.print(f"\n[bold]Validating plan:[/bold] {plan_path}")

    # Rubric
    console.print("  Running rubric checks...", style="dim")
    rubric_result = plan_rubric(spec_text, plan_json)

    # Adversary
    console.print(f"  Running adversary ({model_name})...", style="dim")
    adv_result = plan_adversary(config, model_name, plan_json)

    # Merge
    combined = StageResult(
        rubric_checks=rubric_result.rubric_checks,
        adversary_guesses=adv_result.adversary_guesses,
        contradictions=adv_result.contradictions,
    )

    _print_stage("Plan", combined)
    return combined


def full_pipeline(spec_path: str, prompt_path: str, config: dict,
                  model_name: str) -> Report:
    """Full pipeline: validate spec -> generate plan -> validate plan."""
    report = Report(model=model_name, prompt=prompt_path)

    # Stage 1: Spec validation
    report.spec_result = validate_spec_pipeline(spec_path, config, model_name)

    # Stage 2: Generate plan
    output_path = f"reports/plan-{model_name}-{Path(prompt_path).stem}.json"
    Path("reports").mkdir(exist_ok=True)
    plan = generate_plan(spec_path, prompt_path, config, model_name, output_path)

    # Stage 3: Plan validation
    report.plan_result = validate_plan_pipeline(spec_path, output_path, config, model_name)

    # Save report
    report_path = report.save()
    console.print(f"\n[bold green]Report saved:[/bold green] {report_path}")

    return report


def compare_pipeline(spec_path: str, prompt_names: list[str],
                     model_names: list[str], config: dict) -> list[Report]:
    """Run full pipeline across multiple prompts and models."""
    reports = []
    for model in model_names:
        for prompt in prompt_names:
            console.print(f"\n{'='*60}")
            console.print(f"[bold]Run: model={model} prompt={prompt}[/bold]")
            console.print(f"{'='*60}")
            report = full_pipeline(spec_path, prompt, config, model)
            reports.append(report)

    # Print comparison table
    if len(reports) > 1:
        _print_comparison(reports)

    return reports


def _print_stage(label: str, result: StageResult):
    """Print stage results to console."""
    table = Table(title=f"{label} Score: {result.combined_score:.0f}/100")
    table.add_column("Check", style="cyan")
    table.add_column("Status")
    table.add_column("Detail", style="dim")

    for c in result.rubric_checks:
        status = "[green]PASS[/green]" if c.passed else "[red]FAIL[/red]"
        table.add_row(c.name, status, c.detail or "")

    console.print(table)

    if result.adversary_gaps:
        blocking = [g for g in result.adversary_gaps if g.severity == "BLOCKING"]
        console.print(f"  Adversary: {len(blocking)} BLOCKING, "
                      f"{len(result.adversary_gaps) - len(blocking)} COSMETIC")
    if result.adversary_guesses:
        blocking = [g for g in result.adversary_guesses if g.severity == "BLOCKING"]
        console.print(f"  Adversary: {len(blocking)} BLOCKING guesses, "
                      f"{len(result.adversary_guesses) - len(blocking)} COSMETIC")
    if result.contradictions:
        console.print(f"  [red]{len(result.contradictions)} cross-file contradictions[/red]")


def _print_comparison(reports: list[Report]):
    """Print comparison table across runs."""
    table = Table(title="Comparison")
    table.add_column("Model")
    table.add_column("Prompt")
    table.add_column("Spec Score")
    table.add_column("Plan Score")
    table.add_column("Combined")

    for r in reports:
        spec = r.spec_result.combined_score if r.spec_result else 0
        plan = r.plan_result.combined_score if r.plan_result else 0
        combined = (spec + plan) / 2
        table.add_row(
            r.model, Path(r.prompt).stem if r.prompt else "",
            f"{spec:.0f}", f"{plan:.0f}", f"[bold]{combined:.0f}[/bold]",
        )

    console.print(table)
