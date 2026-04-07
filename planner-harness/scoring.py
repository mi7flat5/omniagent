"""Weighted scoring and report generation."""

from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path


@dataclass
class CheckResult:
    name: str
    weight: int
    passed: bool
    detail: str = ""


@dataclass
class AdversaryGap:
    quote: str
    question: str
    severity: str  # "BLOCKING" or "COSMETIC"


@dataclass
class AdversaryGuess:
    what: str
    why: str
    severity: str
    file_path: str = ""


@dataclass
class Contradiction:
    description: str
    file_a: str
    file_a_says: str
    file_b: str
    file_b_says: str


@dataclass
class StageResult:
    rubric_checks: list[CheckResult] = field(default_factory=list)
    adversary_gaps: list[AdversaryGap] = field(default_factory=list)
    adversary_guesses: list[AdversaryGuess] = field(default_factory=list)
    contradictions: list[Contradiction] = field(default_factory=list)

    @property
    def rubric_score(self) -> float:
        total_weight = sum(c.weight for c in self.rubric_checks)
        if total_weight == 0:
            return 100.0
        passed_weight = sum(c.weight for c in self.rubric_checks if c.passed)
        return (passed_weight / total_weight) * 100

    @property
    def adversary_score(self) -> float:
        score = 100.0
        for g in self.adversary_gaps:
            score -= 5.0 if g.severity == "BLOCKING" else 1.0
        for g in self.adversary_guesses:
            score -= 3.0 if g.severity == "BLOCKING" else 1.0
        score -= len(self.contradictions) * 5.0
        return max(0.0, score)

    @property
    def combined_score(self) -> float:
        return (self.rubric_score * 0.4) + (self.adversary_score * 0.6)


@dataclass
class Report:
    model: str
    prompt: str
    spec_result: StageResult | None = None
    plan_result: StageResult | None = None

    def to_markdown(self) -> str:
        lines = [
            "# Spec & Plan Quality Report",
            f"Model: {self.model} | Prompt: {self.prompt} | Date: {datetime.now().strftime('%Y-%m-%d %H:%M')}",
            "",
        ]
        if self.spec_result:
            s = self.spec_result
            lines.append(f"## Spec Score: {s.combined_score:.0f}/100")
            lines.append(f"### Rubric: {s.rubric_score:.0f}/100")
            for c in s.rubric_checks:
                status = "PASS" if c.passed else "FAIL"
                lines.append(f"- [{status}] {c.name} ({c.weight}pts){': ' + c.detail if c.detail else ''}")
            lines.append(f"### Adversary: {s.adversary_score:.0f}/100")
            blocking = [g for g in s.adversary_gaps if g.severity == "BLOCKING"]
            cosmetic = [g for g in s.adversary_gaps if g.severity == "COSMETIC"]
            lines.append(f"- {len(blocking)} BLOCKING gaps")
            lines.append(f"- {len(cosmetic)} COSMETIC gaps")
            if blocking:
                lines.append("\n**Blocking gaps:**")
                for g in blocking:
                    lines.append(f"- {g.question}")
            lines.append("")

        if self.plan_result:
            p = self.plan_result
            lines.append(f"## Plan Score: {p.combined_score:.0f}/100")
            lines.append(f"### Rubric: {p.rubric_score:.0f}/100")
            for c in p.rubric_checks:
                status = "PASS" if c.passed else "FAIL"
                lines.append(f"- [{status}] {c.name} ({c.weight}pts){': ' + c.detail if c.detail else ''}")
            lines.append(f"### Adversary: {p.adversary_score:.0f}/100")
            blocking_g = [g for g in p.adversary_guesses if g.severity == "BLOCKING"]
            cosmetic_g = [g for g in p.adversary_guesses if g.severity == "COSMETIC"]
            lines.append(f"- {len(blocking_g)} BLOCKING guesses across {len(set(g.file_path for g in p.adversary_guesses))} files")
            lines.append(f"- {len(cosmetic_g)} COSMETIC guesses")
            lines.append(f"- {len(p.contradictions)} cross-file contradictions")
            if p.contradictions:
                lines.append("\n**Contradictions:**")
                for c in p.contradictions:
                    lines.append(f"- {c.description} ({c.file_a} vs {c.file_b})")
            lines.append("")

        return "\n".join(lines)

    def save(self, output_dir: str = "reports") -> str:
        Path(output_dir).mkdir(parents=True, exist_ok=True)
        ts = datetime.now().strftime("%Y-%m-%d-%H-%M")
        prompt_slug = Path(self.prompt).stem if self.prompt else "none"
        filename = f"{ts}-{self.model}-{prompt_slug}.md"
        path = Path(output_dir) / filename
        path.write_text(self.to_markdown())
        return str(path)
