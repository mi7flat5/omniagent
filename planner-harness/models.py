"""OpenAI-compatible API client for ollama/llama.cpp/lmstudio."""

import contextlib
import json
import signal
import threading
import yaml
from pathlib import Path
from openai import OpenAI


class ModelRequestTimeoutError(TimeoutError):
    """Raised when a model call exceeds the configured wall-clock timeout."""


def load_config(config_path: str = None) -> dict:
    """Load config from yaml file."""
    if config_path is None:
        config_path = Path(__file__).parent / "config.yaml"
    with open(config_path) as f:
        return yaml.safe_load(f)


def get_client(config: dict, model_name: str) -> tuple[OpenAI, str]:
    """Return (OpenAI client, model_id) for the named model config."""
    model_cfg = config["models"][model_name]
    defaults = config.get("defaults", {})
    client = OpenAI(
        base_url=model_cfg["endpoint"],
        api_key=model_cfg.get("api_key", "none"),
        max_retries=model_cfg.get("max_retries", defaults.get("max_retries", 0)),
    )
    return client, model_cfg["model"]


def request_timeout_seconds(config: dict, model_name: str) -> float:
    """Resolve the request timeout for a model call."""
    defaults = config.get("defaults", {})
    model_cfg = config["models"][model_name]
    value = model_cfg.get("timeout_seconds", defaults.get("request_timeout_seconds", 180))
    try:
        timeout = float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"invalid timeout_seconds for model '{model_name}': {value!r}") from exc
    if timeout <= 0:
        raise ValueError(f"timeout_seconds must be > 0 for model '{model_name}'")
    return timeout


@contextlib.contextmanager
def hard_timeout(seconds: float, operation: str):
    """Apply a wall-clock timeout to a blocking model call on Unix main-thread callers."""
    if seconds <= 0:
        yield
        return
    if threading.current_thread() is not threading.main_thread():
        yield
        return
    if not hasattr(signal, "SIGALRM") or not hasattr(signal, "setitimer"):
        yield
        return

    previous_handler = signal.getsignal(signal.SIGALRM)
    previous_timer = signal.setitimer(signal.ITIMER_REAL, 0)

    def _handle_timeout(signum, frame):
        raise ModelRequestTimeoutError(f"{operation} exceeded {seconds:g}s")

    signal.signal(signal.SIGALRM, _handle_timeout)
    signal.setitimer(signal.ITIMER_REAL, seconds)
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous_handler)
        if previous_timer != (0.0, 0.0):
            signal.setitimer(signal.ITIMER_REAL, *previous_timer)


def call_llm(config: dict, model_name: str, system: str, user: str) -> str:
    """Send a system+user prompt to the model, return the text response."""
    client, model_id = get_client(config, model_name)
    model_cfg = config["models"][model_name]
    timeout_seconds = request_timeout_seconds(config, model_name)
    with hard_timeout(timeout_seconds, f"model '{model_name}' request"):
        response = client.chat.completions.create(
            model=model_id,
            messages=[
                {"role": "system", "content": system},
                {"role": "user", "content": user},
            ],
            max_tokens=model_cfg.get("max_tokens", 4096),
            temperature=model_cfg.get("temperature", 0.0),
            timeout=timeout_seconds,
        )
    content = response.choices[0].message.content
    if content is None or not str(content).strip():
        raise ValueError(f"model '{model_name}' returned empty content")
    return content


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
