#!/usr/bin/env bash
# e2e-real.sh --build-dir <dir> --core <dir> --state <dir> [--shots <dir>] [--ports 18787,18788] [-- extra args for e2e-real.mjs]
#
# End-to-end test of the real UI (headless Chrome) against the real backend (`qstate-cli serve`) and real state files.
# Starts two servers and stops them again: one WITHOUT startup arguments (the test drives the workspace dialog and
# the directory browser) and one on a temporary copy of contract0005.EEE that the test modifies on disk (live update
# events). Needs `pnpm build` in ui/ (the servers serve ui/dist) and /usr/bin/google-chrome (CHROME_BIN overrides).
# Exit status = verdict; screenshots (look at them!) and per-step latencies are the result. See ui/scripts/e2e-real.mjs.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
BUILD_DIR="$ROOT/build"; CORE=""; STATE=""; SHOTS="/tmp/e2e-shots"; PORTS="18787,18788"
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift ;;
        --core) CORE="$2"; shift ;;
        --state) STATE="$2"; shift ;;
        --shots) SHOTS="$2"; shift ;;
        --ports) PORTS="$2"; shift ;;
        --) shift; break ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$CORE" ] && [ -n "$STATE" ] || { echo "--core and --state are required" >&2; exit 2; }
CLI="$(find "$BUILD_DIR" -name qstate-cli -type f -perm -u+x 2>/dev/null | head -1)"
[ -n "$CLI" ] || { echo "qstate-cli not found under $BUILD_DIR" >&2; exit 2; }
[ -f "$ROOT/ui/dist/index.html" ] || { echo "ui/dist/index.html missing: run 'pnpm build' in ui/" >&2; exit 2; }
P1="${PORTS%,*}"; P2="${PORTS#*,}"
TMP="$(mktemp -d)"
pids=()
cleanup() { for p in "${pids[@]:-}"; do kill "$p" 2>/dev/null || true; done; wait 2>/dev/null || true; rm -rf "$TMP"; }
trap cleanup EXIT
# the epoch of the first contract file in the state directory decides which small file is copied
SMALL="$(ls "$STATE"/contract0005.* 2>/dev/null | head -1)"
[ -n "$SMALL" ] || { echo "no contract0005.* in $STATE" >&2; exit 2; }
mkdir -p "$TMP/live" "$TMP/cfg-main" "$TMP/cfg-live"
cp "$SMALL" "$TMP/live/"
XDG_CONFIG_HOME="$TMP/cfg-main" "$CLI" serve --port "$P1" --ui-dir "$ROOT/ui/dist" > "$TMP/main.log" 2>&1 & pids+=($!)
XDG_CONFIG_HOME="$TMP/cfg-live" "$CLI" serve --core "$CORE" --ref auto --state "$TMP/live" --port "$P2" --ui-dir "$ROOT/ui/dist" > "$TMP/live.log" 2>&1 & pids+=($!)
sleep 1.5
cd "$ROOT/ui"
node scripts/e2e-real.mjs --url="http://127.0.0.1:$P1/" --api="http://127.0.0.1:$P1" --core="$CORE" --state="$STATE" --shots="$SHOTS" \
    --live-url="http://127.0.0.1:$P2/" --live-api="http://127.0.0.1:$P2" --live-file="$TMP/live/$(basename "$SMALL")" "$@"
