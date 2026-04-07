"""Spec & Plan Quality Harness -- CLI entry point."""

import argparse
from pathlib import Path

from models import load_config
from pipeline import (
    resolve_model,
    generate_prompt,
    validate_spec_pipeline,
    generate_plan,
    validate_plan_pipeline,
    full_pipeline,
    compare_pipeline,
)


def cmd_generate_prompt(args, config):
    model = resolve_model(config, args.model, purpose="generation")
    generate_prompt(args.spec, config, model, args.output)


def cmd_validate_spec(args, config):
    model = resolve_model(config, args.model, purpose="adversary")
    validate_spec_pipeline(args.spec, config, model)


def cmd_generate_plan(args, config):
    model = resolve_model(config, args.model, purpose="generation")
    generate_plan(args.spec, args.prompt, config, model, args.output)


def cmd_validate_plan(args, config):
    model = resolve_model(config, args.model, purpose="adversary")
    validate_plan_pipeline(args.spec, args.plan, config, model)


def cmd_run(args, config):
    model = resolve_model(config, args.model, purpose="generation")
    full_pipeline(args.spec, args.prompt, config, model)


def cmd_compare(args, config):
    prompts = [p.strip() for p in args.prompts.split(",")]
    models = [m.strip() for m in args.models.split(",")]
    compare_pipeline(args.spec, prompts, models, config)


def main():
    parser = argparse.ArgumentParser(description="Spec & Plan Quality Harness")
    parser.add_argument("--config", default=None, help="Path to config.yaml")
    sub = parser.add_subparsers(dest="command", required=True)

    # generate-prompt
    p = sub.add_parser("generate-prompt", help="Generate project-specific planner prompt from spec")
    p.add_argument("spec", help="Path to SPEC.md")
    p.add_argument("--model", default=None)
    p.add_argument("-o", "--output", default="planner-prompt.md", help="Output prompt path")
    p.set_defaults(func=cmd_generate_prompt)

    # validate-spec
    p = sub.add_parser("validate-spec", help="Validate a spec file")
    p.add_argument("spec", help="Path to SPEC.md")
    p.add_argument("--model", default=None)
    p.set_defaults(func=cmd_validate_spec)

    # generate-plan
    p = sub.add_parser("generate-plan", help="Generate PLAN.json from spec")
    p.add_argument("spec", help="Path to SPEC.md")
    p.add_argument("--prompt", required=True)
    p.add_argument("--model", default=None)
    p.add_argument("-o", "--output", default="PLAN.json")
    p.set_defaults(func=cmd_generate_plan)

    # validate-plan
    p = sub.add_parser("validate-plan", help="Validate PLAN.json against spec")
    p.add_argument("spec", help="Path to SPEC.md")
    p.add_argument("plan", help="Path to PLAN.json")
    p.add_argument("--model", default=None)
    p.set_defaults(func=cmd_validate_plan)

    # run
    p = sub.add_parser("run", help="Full pipeline")
    p.add_argument("spec", help="Path to SPEC.md")
    p.add_argument("--prompt", required=True)
    p.add_argument("--model", default=None)
    p.set_defaults(func=cmd_run)

    # compare
    p = sub.add_parser("compare", help="Compare prompts/models")
    p.add_argument("spec", help="Path to SPEC.md")
    p.add_argument("--prompts", required=True)
    p.add_argument("--models", required=True)
    p.set_defaults(func=cmd_compare)

    args = parser.parse_args()
    config = load_config(args.config)
    args.func(args, config)


if __name__ == "__main__":
    main()
