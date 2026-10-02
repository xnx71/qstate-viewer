#!/usr/bin/env bash
# dev.sh webview|browser|serve [options] [-- extra qstate arguments]
#
# Development runner for the UI.
#
#   dev.sh webview   Vite dev server (hot reload) + the desktop app pointed at it:
#                      qstate-viewer --dev-url http://localhost:5173
#                    The page uses the real webview bridge (window.__qstate_invoke), exactly like production.
#   dev.sh browser   Vite dev server + the window-less backend `qstate-devserver` (needs no GTK/WebKit);
#                    open  http://localhost:5173/?api=http://127.0.0.1:8787  in any browser (HTTP + SSE transport).
#   dev.sh serve     The desktop app with the embedded / built UI AND the HTTP transport:
#                      qstate-viewer --serve 8787     (a browser can use ?api=http://127.0.0.1:8787 at the same time)
#
# Options (before the optional `--`):
#   --build-dir <dir>   CMake build directory with the binaries (default: <repo>/build)
#   --vite-port <n>     default 5173
#   --api-port <n>      default 8787
#   --no-vite           do not start Vite (it is already running)
# Everything after `--` is passed to the qstate binary, e.g.
#   scripts/dev.sh webview -- --core ~/qubic/core --state ~/states --epoch 199
#
# Build first:  cmake -S . -B build [-DQSTATE_WEBVIEW_SYSROOT=$PWD/.sysroot] && cmake --build build -j
# Linux without GTK/WebKit dev packages: scripts/bootstrap-sysroot.sh .sysroot (see docs/HOST.md).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
MODE="${1:-}"
[ -n "$MODE" ] || { sed -n '2,/^set -euo/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//' >&2; exit 2; }
shift

BUILD_DIR="$ROOT/build"
VITE_PORT=5173
API_PORT=8787
START_VITE=1
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift ;;
        --vite-port) VITE_PORT="$2"; shift ;;
        --api-port) API_PORT="$2"; shift ;;
        --no-vite) START_VITE=0 ;;
        --) shift; break ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1 (use -- before arguments for qstate)" >&2; exit 2 ;;
    esac
    shift
done
EXTRA=("$@")

find_bin() { find "$BUILD_DIR" -name "$1" -type f -perm -u+x 2>/dev/null | head -1; }

PIDS=()
cleanup() {
    for pid in "${PIDS[@]:-}"; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

start_vite() {
    [ "$START_VITE" -eq 1 ] || return 0
    command -v pnpm >/dev/null || { echo "pnpm not found" >&2; exit 1; }
    [ -d "$ROOT/ui/node_modules" ] || (cd "$ROOT/ui" && pnpm install)
    echo "[dev] starting Vite on port $VITE_PORT"
    (cd "$ROOT/ui" && exec pnpm exec vite --port "$VITE_PORT" --strictPort) &
    PIDS+=("$!")
    for _ in $(seq 1 100); do
        if curl -fs "http://localhost:$VITE_PORT/" >/dev/null 2>&1; then return 0; fi
        sleep 0.2
    done
    echo "Vite did not come up on port $VITE_PORT" >&2
    exit 1
}

case "$MODE" in
    webview)
        BIN="$(find_bin qstate-viewer)"
        [ -n "$BIN" ] || { echo "qstate-viewer not built under $BUILD_DIR (GUI deps missing? see docs/HOST.md)" >&2; exit 1; }
        start_vite
        echo "[dev] $BIN --dev-url http://localhost:$VITE_PORT ${EXTRA[*]:-}"
        "$BIN" --dev-url "http://localhost:$VITE_PORT" "${EXTRA[@]}"
        ;;
    browser)
        BIN="$(find_bin qstate-devserver)"
        [ -n "$BIN" ] || { echo "qstate-devserver not built under $BUILD_DIR (build the gui module)" >&2; exit 1; }
        start_vite
        echo "[dev] open  http://localhost:$VITE_PORT/?api=http://127.0.0.1:$API_PORT"
        "$BIN" --serve "$API_PORT" "${EXTRA[@]}"
        ;;
    serve)
        BIN="$(find_bin qstate-viewer)"
        [ -n "$BIN" ] || { echo "qstate-viewer not built under $BUILD_DIR" >&2; exit 1; }
        echo "[dev] browser: any UI with ?api=http://127.0.0.1:$API_PORT"
        "$BIN" --serve "$API_PORT" "${EXTRA[@]}"
        ;;
    *)
        echo "unknown mode: $MODE (webview | browser | serve)" >&2
        exit 2
        ;;
esac
