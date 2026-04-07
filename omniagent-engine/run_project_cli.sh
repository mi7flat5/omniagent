#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ENGINE_BUILD_DIR:-${ROOT_DIR}/build}"
CLI_BIN="${ENGINE_CLI_BIN:-${BUILD_DIR}/omni-engine-cli}"
CORE_CACHE="${ROOT_DIR}/../omniagent-core/build/CMakeCache.txt"

MODE="${1:-repl}"
if [[ $# -gt 0 ]]; then
    shift
fi

WORKSPACE_ROOT="${ENGINE_WORKSPACE_ROOT:-/home/mi7fl/projects/engine-repl-project}"
WORKING_DIR="${ENGINE_WORKING_DIR:-${WORKSPACE_ROOT}}"
PROJECT_ID="${ENGINE_PROJECT_ID:-$(basename "${WORKSPACE_ROOT}")}"
STORAGE_DIR="${ENGINE_STORAGE_DIR:-${WORKSPACE_ROOT}/.omniagent/engine-cli}"
BASE_URL="${ENGINE_BASE_URL:-http://192.168.1.84:8999/v1}"
MODEL="${ENGINE_MODEL:-opus5_5.gguf}"
PROFILE="${ENGINE_PROFILE:-coordinator}"
API_KEY="${ENGINE_API_KEY:-}"
APPROVAL_POLICY="${ENGINE_APPROVAL_POLICY:-}"

if [[ -z "${APPROVAL_POLICY}" && "${MODE}" == "run" ]]; then
    APPROVAL_POLICY="auto-read-only-prompt"
fi

if [[ ! -x "${CLI_BIN}" ]]; then
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
        echo "usage: $0 [repl|run|inspect|resume] [args...]" >&2
        echo "env overrides: ENGINE_WORKSPACE_ROOT ENGINE_WORKING_DIR ENGINE_PROJECT_ID ENGINE_STORAGE_DIR ENGINE_BASE_URL ENGINE_MODEL ENGINE_API_KEY ENGINE_PROFILE ENGINE_PROMPT ENGINE_APPROVAL_POLICY BRAVE_SEARCH_KEY" >&2
        exit 1
        ;;
esac