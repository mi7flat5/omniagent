#!/usr/bin/env bash
set -euo pipefail

# ---------------------------------------------------------------------------
# OmniAgent Reset — rebuild frontend + C++, restart headless server
#
# Layout:
#   /home/mi7fl/omniagent/           (parent repo)
#     omniagent-core/                (C++ submodule)
#       build/                       (cmake build dir)
#         app/omniagent              (binary)
#     omniagent-web/                 (frontend submodule)
#       dist/                        (vite build output)
#     tools/omni_reset.sh            (this script)
# ---------------------------------------------------------------------------

OMNI_DIR="/home/mi7fl/omniagent"
CORE_DIR="$OMNI_DIR/omniagent-core"
WEB_DIR="$OMNI_DIR/omniagent-web"
BUILD_DIR="$CORE_DIR/build"
BINARY="$BUILD_DIR/app/omniagent"
PORT=8765

echo "=== OmniAgent Reset ==="

# ── 1. Kill running instance ──────────────────────────────────────────────
echo "[1/4] Killing omniagent..."
pkill -f "omniagent.*--headless" 2>/dev/null && echo "  Killed." || echo "  Not running."
sleep 1

# ── 2. Build web frontend ─────────────────────────────────────────────────
echo "[2/4] Building web frontend..."
cd "$WEB_DIR"
npm run build 2>&1 | tail -5
echo "  Done → $WEB_DIR/dist"

# ── 3. Build C++ binary ──────────────────────────────────────────────────
echo "[3/4] Building C++ binary..."
echo "  Refreshing CMake build files..."
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cmake -B "$BUILD_DIR" -S "$CORE_DIR" 2>&1 | tail -5
else
    if [ -z "${VCPKG_ROOT:-}" ]; then
        echo "  FAILED: VCPKG_ROOT is not set and $BUILD_DIR/CMakeCache.txt does not exist."
        echo "  Set VCPKG_ROOT or configure omniagent-core once before using this script."
        exit 1
    fi
    cmake -B "$BUILD_DIR" -S "$CORE_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -5
fi
cmake --build "$BUILD_DIR" --target omniagent -j"$(nproc)" 2>&1 | tail -10
if [ ! -x "$BINARY" ]; then
    echo "  FAILED: expected executable not found at $BINARY"
    exit 1
fi
echo "  Done → $BINARY"

# ── 4. Restart ────────────────────────────────────────────────────────────
echo "[4/4] Starting omniagent on port $PORT..."
cd "$OMNI_DIR"
OMNIAGENT_WEB_DIST="$WEB_DIR/dist" \
OMNIAGENT_DUMP_CONTEXT=1 \
    nohup "$BINARY" --headless --port "$PORT" > /tmp/omniagent.log 2>&1 &
disown
sleep 2

if pgrep -f "omniagent.*--headless" > /dev/null; then
    PID=$(pgrep -f 'omniagent.*--headless')
    echo "  Running (PID $PID)"
    echo "  Log: /tmp/omniagent.log"
    echo "=== Done. UI at http://localhost:$PORT ==="
else
    echo "  FAILED to start. Last 20 lines:"
    tail -20 /tmp/omniagent.log 2>/dev/null
    exit 1
fi
