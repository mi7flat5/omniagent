#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ENGINE_BUILD_DIR:-${ROOT_DIR}/build}"
CORE_CACHE="${ROOT_DIR}/../omniagent-core/build/CMakeCache.txt"

cmake_args=()
if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
	cmake_args+=("-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}")
elif [[ -f "${CORE_CACHE}" ]]; then
	core_prefix="$(grep '^CMAKE_PREFIX_PATH:' "${CORE_CACHE}" | cut -d= -f2- || true)"
	if [[ -n "${core_prefix}" ]]; then
		cmake_args+=("-DCMAKE_PREFIX_PATH=${core_prefix}")
	fi
fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" "${cmake_args[@]}" "$@"
cmake --build "${BUILD_DIR}"
ctest --test-dir "${BUILD_DIR}" --output-on-failure