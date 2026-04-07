"""OpenAI-compatible API client for ollama/llama.cpp/lmstudio."""

import json
import yaml
from pathlib import Path
from openai import OpenAI


def load_config(config_path: str = None) -> dict:
    """Load config from yaml file."""
    if config_path is None:
        config_path = Path(__file__).parent / "config.yaml"
    with open(config_path) as f:
        return yaml.safe_load(f)


def get_client(config: dict, model_name: str) -> tuple[OpenAI, str]:
    """Return (OpenAI client, model_id) for the named model config."""
    model_cfg = config["models"][model_name]
    client = OpenAI(
        base_url=model_cfg["endpoint"],
        api_key=model_cfg.get("api_key", "none"),
    )
    return client, model_cfg["model"]


def call_llm(config: dict, model_name: str, system: str, user: str) -> str:
    """Send a system+user prompt to the model, return the text response."""
    client, model_id = get_client(config, model_name)
    model_cfg = config["models"][model_name]
    response = client.chat.completions.create(
        model=model_id,
        messages=[
            {"role": "system", "content": system},
            {"role": "user", "content": user},
        ],
        max_tokens=model_cfg.get("max_tokens", 4096),
        temperature=model_cfg.get("temperature", 0.0),
    )
    return response.choices[0].message.content


def call_llm_json(config: dict, model_name: str, system: str, user: str) -> dict:
    """Send a prompt and parse the response as JSON."""
    raw = call_llm(config, model_name, system, user)
    # Strip markdown code fences if present.
    text = raw.strip()
    if text.startswith("```"):
        lines = text.split("\n")
        lines = [l for l in lines if not l.strip().startswith("```")]
        text = "\n".join(lines)
    return json.loads(text)
