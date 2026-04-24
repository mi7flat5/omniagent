#!/usr/bin/env bash
set -euo pipefail

SOURCE_PATH="${BASH_SOURCE[0]}"
while [[ -L "${SOURCE_PATH}" ]]; do
    SOURCE_DIR="$(cd -P "$(dirname "${SOURCE_PATH}")" && pwd)"
    LINK_TARGET="$(readlink "${SOURCE_PATH}")"
    if [[ "${LINK_TARGET}" != /* ]]; then
        SOURCE_PATH="${SOURCE_DIR}/${LINK_TARGET}"
    else
        SOURCE_PATH="${LINK_TARGET}"
    fi
done

ROOT_DIR="$(cd -P "$(dirname "${SOURCE_PATH}")" && pwd)"
REPO_ROOT="$(cd -P "${ROOT_DIR}/.." && pwd)"
BUILD_DIR="${ENGINE_BUILD_DIR:-${ROOT_DIR}/build}"
CLI_BIN="${ENGINE_CLI_BIN:-${BUILD_DIR}/omni-engine-cli}"
CORE_CACHE="${ROOT_DIR}/../omniagent-core/build/CMakeCache.txt"
LAUNCH_DIR="${PWD}"

export OMNIAGENT_ENGINE_ROOT="${OMNIAGENT_ENGINE_ROOT:-${ROOT_DIR}}"
export OMNIAGENT_REPO_ROOT="${OMNIAGENT_REPO_ROOT:-${REPO_ROOT}}"
if [[ -z "${OMNIAGENT_PLANNER_BRIDGE:-}" && -f "${OMNIAGENT_REPO_ROOT}/planner-harness/bridge.py" ]]; then
    export OMNIAGENT_PLANNER_BRIDGE="${OMNIAGENT_REPO_ROOT}/planner-harness/bridge.py"
fi

USE_CLOUD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        -cloud|--cloud)
            USE_CLOUD=1
            shift
            ;;
        --)
            shift
            break
            ;;
        *)
            break
            ;;
    esac
done

MODE="${1:-repl}"
if [[ $# -gt 0 ]]; then
    shift
fi

WORKSPACE_ROOT="${ENGINE_WORKSPACE_ROOT:-${LAUNCH_DIR}}"
WORKING_DIR="${ENGINE_WORKING_DIR:-${LAUNCH_DIR}}"
PROJECT_ID="${ENGINE_PROJECT_ID:-$(basename "${WORKSPACE_ROOT}")}"
STORAGE_DIR="${ENGINE_STORAGE_DIR:-${WORKSPACE_ROOT}/.omniagent/engine-cli}"
PROFILE="${ENGINE_PROFILE:-coordinator}"
APPROVAL_POLICY="${ENGINE_APPROVAL_POLICY:-}"

settings_max_context_tokens=""
settings_temperature=""
settings_top_p=""
settings_top_k=""
settings_min_p=""
settings_presence_penalty=""
settings_frequency_penalty=""
settings_max_tokens=""
if command -v python3 >/dev/null 2>&1; then
    while IFS=$'\t' read -r key value; do
        case "${key}" in
            max_context_tokens)
                settings_max_context_tokens="${value}"
                ;;
            temperature)
                settings_temperature="${value}"
                ;;
            top_p)
                settings_top_p="${value}"
                ;;
            top_k)
                settings_top_k="${value}"
                ;;
            min_p)
                settings_min_p="${value}"
                ;;
            presence_penalty)
                settings_presence_penalty="${value}"
                ;;
            frequency_penalty)
                settings_frequency_penalty="${value}"
                ;;
            max_tokens)
                settings_max_tokens="${value}"
                ;;
        esac
    done < <(python3 - "${HOME:-}" "${WORKSPACE_ROOT}" "${PROFILE}" <<'PY'
import json
import pathlib
import sys


def load_json(path_str):
    path = pathlib.Path(path_str)
    if not path.is_file():
        return {}
    try:
        with path.open("r", encoding="utf-8") as handle:
            data = json.load(handle)
    except Exception:
        return {}
    return data if isinstance(data, dict) else {}


def deep_merge(base, override):
    if not isinstance(base, dict) or not isinstance(override, dict):
        return override
    merged = dict(base)
    for key, value in override.items():
        if key in merged and isinstance(merged[key], dict) and isinstance(value, dict):
            merged[key] = deep_merge(merged[key], value)
        else:
            merged[key] = value
    return merged


home_dir, workspace_root, profile = sys.argv[1:4]
files = []
if home_dir:
    files.append(pathlib.Path(home_dir) / ".omniagent" / "settings.json")
if workspace_root:
    project_dir = pathlib.Path(workspace_root) / ".omniagent"
    files.append(project_dir / "settings.json")
    files.append(project_dir / "settings.local.json")

merged = {}
for path in files:
    merged = deep_merge(merged, load_json(str(path)))

agent = merged.get("agent")
if isinstance(agent, dict):
    value = agent.get("max_context_tokens")
    if value is not None:
        print(f"max_context_tokens\t{value}")

section = merged.get("llm")
section = section if isinstance(section, dict) else {}

if profile == "coordinator":
    orchestrator = merged.get("orchestrator_llm")
    if isinstance(orchestrator, dict):
        section = deep_merge(section, orchestrator)

for key in (
    "temperature",
    "top_p",
    "top_k",
    "min_p",
    "presence_penalty",
    "frequency_penalty",
    "max_tokens",
):
    value = section.get(key)
    if value is None:
        continue
    print(f"{key}\t{value}")
PY
)
fi

provider_hint=""
if [[ ${USE_CLOUD} -eq 1 ]]; then
    provider_hint="cloud"
elif [[ -n "${OLLAMA_BASE_URL:-}${OLLAMA_MODEL:-}${OLLAMA_API_KEY:-}" ]]; then
    provider_hint="ollama"
elif [[ -n "${OPENAI_API_KEY:-}${OPENAI_BASE_URL:-}${OPENAI_MODEL:-}" ]]; then
    provider_hint="openai"
elif [[ -n "${OPENROUTER_API_KEY:-}${OPENROUTER_BASE_URL:-}${OPENROUTER_MODEL:-}" ]]; then
    provider_hint="openrouter"
fi

case "${provider_hint}" in
    cloud)
        default_base_url="${ENGINE_CLOUD_BASE_URL:-${OLLAMA_BASE_URL:-http://localhost:11434/v1}}"
        default_model="${ENGINE_CLOUD_MODEL:-gemma4-code-audit}"
        default_api_key="${ENGINE_CLOUD_API_KEY:-${OLLAMA_API_KEY:-}}"
        ;;
    ollama)
        default_base_url="${OLLAMA_BASE_URL:-http://localhost:11434/v1}"
        default_model="${OLLAMA_MODEL:-gemma4-code-audit}"
        default_api_key="${OLLAMA_API_KEY:-}"
        ;;
    openai)
        default_base_url="${OPENAI_BASE_URL:-https://api.openai.com/v1}"
        default_model="${OPENAI_MODEL:-gpt-4.1}"
        default_api_key="${OPENAI_API_KEY:-}"
        ;;
    openrouter)
        default_base_url="${OPENROUTER_BASE_URL:-https://openrouter.ai/api/v1}"
        default_model="${OPENROUTER_MODEL:-openai/gpt-4.1}"
        default_api_key="${OPENROUTER_API_KEY:-}"
        ;;
    *)
        default_base_url="http://localhost:11434/v1"
        default_model="gemma4-code-audit"
        default_api_key=""
        ;;
esac

BASE_URL="${ENGINE_BASE_URL:-${default_base_url}}"
MODEL="${ENGINE_MODEL:-${default_model}}"
API_KEY="${ENGINE_API_KEY:-${default_api_key}}"
MAX_CONTEXT_TOKENS="${ENGINE_MAX_CONTEXT_TOKENS:-${settings_max_context_tokens:-}}"
TEMPERATURE="${ENGINE_TEMPERATURE:-${settings_temperature:-}}"
TOP_P="${ENGINE_TOP_P:-${settings_top_p:-}}"
TOP_K="${ENGINE_TOP_K:-${settings_top_k:-}}"
MIN_P="${ENGINE_MIN_P:-${settings_min_p:-}}"
PRESENCE_PENALTY="${ENGINE_PRESENCE_PENALTY:-${settings_presence_penalty:-}}"
FREQUENCY_PENALTY="${ENGINE_FREQUENCY_PENALTY:-${settings_frequency_penalty:-}}"
MAX_TOKENS="${ENGINE_MAX_TOKENS:-${settings_max_tokens:-}}"

if [[ -z "${APPROVAL_POLICY}" && "${MODE}" == "run" ]]; then
    APPROVAL_POLICY="auto-read-only-prompt"
fi

needs_build=0
if [[ ! -x "${CLI_BIN}" || ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    needs_build=1
elif [[ "${ROOT_DIR}/CMakeLists.txt" -nt "${CLI_BIN}" ]]; then
    needs_build=1
elif find "${ROOT_DIR}/src" "${ROOT_DIR}/include" -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' -o -name '*.h' -o -name '*.hpp' \) -newer "${CLI_BIN}" -print -quit | grep -q .; then
    needs_build=1
fi

if [[ ${needs_build} -eq 1 ]]; then
    cmake_args=()
    if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
        cmake_args+=("-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}")
    elif [[ -f "${CORE_CACHE}" ]]; then
        core_prefix="$(grep '^CMAKE_PREFIX_PATH:' "${CORE_CACHE}" | cut -d= -f2- || true)"
        if [[ -n "${core_prefix}" ]]; then
            cmake_args+=("-DCMAKE_PREFIX_PATH=${core_prefix}")
        fi
    fi

    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" "${cmake_args[@]}"
    cmake --build "${BUILD_DIR}" --target omni-engine-cli -j2
fi

mkdir -p "${STORAGE_DIR}"

common_args=(
    --project-id "${PROJECT_ID}"
    --workspace-root "${WORKSPACE_ROOT}"
    --cwd "${WORKING_DIR}"
    --storage-dir "${STORAGE_DIR}"
    --profile "${PROFILE}"
    --base-url "${BASE_URL}"
    --model "${MODEL}"
)

if [[ -n "${API_KEY}" ]]; then
    common_args+=(--api-key "${API_KEY}")
fi

if [[ -n "${MAX_CONTEXT_TOKENS}" ]]; then
    common_args+=(--max-context-tokens "${MAX_CONTEXT_TOKENS}")
fi

if [[ -n "${TEMPERATURE}" ]]; then
    common_args+=(--temperature "${TEMPERATURE}")
fi

if [[ -n "${TOP_P}" ]]; then
    common_args+=(--top-p "${TOP_P}")
fi

if [[ -n "${TOP_K}" ]]; then
    common_args+=(--top-k "${TOP_K}")
fi

if [[ -n "${MIN_P}" ]]; then
    common_args+=(--min-p "${MIN_P}")
fi

if [[ -n "${PRESENCE_PENALTY}" ]]; then
    common_args+=(--presence-penalty "${PRESENCE_PENALTY}")
fi

if [[ -n "${FREQUENCY_PENALTY}" ]]; then
    common_args+=(--frequency-penalty "${FREQUENCY_PENALTY}")
fi

if [[ -n "${MAX_TOKENS}" ]]; then
    common_args+=(--max-tokens "${MAX_TOKENS}")
fi

if [[ -n "${APPROVAL_POLICY}" ]]; then
    common_args+=(--approval-policy "${APPROVAL_POLICY}")
fi

case "${MODE}" in
    repl)
        exec "${CLI_BIN}" repl "${common_args[@]}" "$@"
        ;;
    run)
        if [[ $# -eq 0 && -z "${ENGINE_PROMPT:-}" ]]; then
            echo "run mode requires a prompt as remaining arguments or ENGINE_PROMPT" >&2
            exit 1
        fi
        if [[ $# -gt 0 ]]; then
            prompt="$*"
        else
            prompt="${ENGINE_PROMPT}"
        fi
        exec "${CLI_BIN}" run "${common_args[@]}" --prompt "${prompt}"
        ;;
    inspect)
        exec "${CLI_BIN}" inspect "${common_args[@]}" "$@"
        ;;
    resume)
        exec "${CLI_BIN}" resume "${common_args[@]}" "$@"
        ;;
    *)
        echo "usage: $0 [-cloud] [repl|run|inspect|resume] [args...]" >&2
        echo "env overrides: ENGINE_WORKSPACE_ROOT ENGINE_WORKING_DIR ENGINE_PROJECT_ID ENGINE_STORAGE_DIR ENGINE_BASE_URL ENGINE_MODEL ENGINE_API_KEY ENGINE_MAX_CONTEXT_TOKENS ENGINE_TEMPERATURE ENGINE_TOP_P ENGINE_TOP_K ENGINE_MIN_P ENGINE_PRESENCE_PENALTY ENGINE_FREQUENCY_PENALTY ENGINE_MAX_TOKENS ENGINE_PROFILE ENGINE_PROMPT ENGINE_APPROVAL_POLICY ENGINE_CLOUD_BASE_URL ENGINE_CLOUD_MODEL ENGINE_CLOUD_API_KEY OLLAMA_BASE_URL OLLAMA_MODEL OLLAMA_API_KEY OPENAI_BASE_URL OPENAI_MODEL OPENAI_API_KEY OPENROUTER_BASE_URL OPENROUTER_MODEL OPENROUTER_API_KEY BRAVE_SEARCH_KEY" >&2
        exit 1
        ;;
esac