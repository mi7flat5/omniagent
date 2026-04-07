import time

import pytest

from models import ModelRequestTimeoutError, hard_timeout, request_timeout_seconds
from pipeline import resolve_model


def test_request_timeout_seconds_uses_default_and_override():
    config = {
        "defaults": {"request_timeout_seconds": 180},
        "models": {
            "planner": {"model": "planner-model"},
            "slow": {"model": "slow-model", "timeout_seconds": 240},
        },
    }

    assert request_timeout_seconds(config, "planner") == 180
    assert request_timeout_seconds(config, "slow") == 240


def test_resolve_model_uses_planner_and_adversary_defaults_by_purpose():
    config = {
        "defaults": {
            "planner_model": "planner",
            "adversary_model": "adversary",
        },
        "models": {
            "planner": {"model": "planner-model"},
            "adversary": {"model": "adversary-model"},
        },
    }

    assert resolve_model(config, None, purpose="generation") == "planner"
    assert resolve_model(config, None, purpose="adversary") == "adversary"


def test_hard_timeout_raises_for_slow_call():
    with pytest.raises(ModelRequestTimeoutError):
        with hard_timeout(0.1, "slow test"):
            time.sleep(1)


def test_hard_timeout_allows_fast_call():
    with hard_timeout(1, "fast test"):
        time.sleep(0.01)