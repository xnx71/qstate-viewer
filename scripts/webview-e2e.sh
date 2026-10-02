#!/usr/bin/env bash
# webview-e2e.sh --build-dir <dir> --repo <url-or-local-path> --ref <ref> --state <path> [--shots <dir>] [--cache <dir>]
#                [--script <file.js>] [--timeout <s>]
#
# Runs the REAL desktop app (WebKitGTK webview, embedded UI) under xvfb and drives the UI with ui/scripts/webview-e2e.js.
# The app takes no arguments; the runner uses its test hooks (docs/HOST.md "Testing"):
#   QSTATE_SELFTEST_SCRIPT   a temporary script = one line `window.__QSTATE_E2E = {repoUrl, ref, statePath};` followed by
#                            ui/scripts/webview-e2e.js, injected before the page's own scripts
#   QSTATE_CONFIG_DIR / QSTATE_CACHE_DIR   a throw-away settings file and git mirror cache (so nothing leaks from or
#                            into the user's configuration; --cache reuses a mirror cache between runs)
#
#   --repo   repository URL or a LOCAL git clone path (a path needs no network), e.g. ~/qubic/core
#   --ref    tag, branch, commit sha or "auto"
#   --state  directory with contractNNNN.EEE files, or one such file
#
# The page script logs "SHOT <name>"; the runner captures the virtual screen at that moment (xwd -> scripts/xwd2png.py)
# into --shots. Exit status = verdict of the script.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
BUILD_DIR="$ROOT/build"
REPO=""; STATE=""; REF="auto"; SHOTS="/tmp/webview-e2e-shots"; CACHE=""
SCRIPT="$ROOT/ui/scripts/webview-e2e.js"
TIMEOUT=240
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift ;;
        --repo) REPO="$2"; shift ;;
        --ref) REF="$2"; shift ;;
        --state) STATE="$2"; shift ;;
        --shots) SHOTS="$2"; shift ;;
        --cache) CACHE="$2"; shift ;;
        --script) SCRIPT="$2"; shift ;;
        --timeout) TIMEOUT="$2"; shift ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$REPO" ] && [ -n "$STATE" ] || { echo "--repo and --state are required" >&2; exit 2; }
[ -f "$SCRIPT" ] || { echo "script not found: $SCRIPT" >&2; exit 2; }
EXE="$(find "$BUILD_DIR" -name qstate-viewer -type f -perm -u+x 2>/dev/null | head -1)"
[ -n "$EXE" ] || { echo "qstate-viewer not found under $BUILD_DIR" >&2; exit 2; }
command -v xvfb-run >/dev/null && command -v xwd >/dev/null || { echo "xvfb-run and xwd are required" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 2; }

# A local path must reach the page as an absolute path (the app resolves it against its own working directory).
if [ -d "$REPO" ]; then REPO="$(cd "$REPO" && pwd)"; fi
STATE="$(realpath -- "$STATE")"

export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"
export WEBKIT_DISABLE_COMPOSITING_MODE="${WEBKIT_DISABLE_COMPOSITING_MODE:-1}"
export WEBKIT_DISABLE_DMABUF_RENDERER="${WEBKIT_DISABLE_DMABUF_RENDERER:-1}"
export GDK_BACKEND=x11
mkdir -p "$SHOTS"
rm -f "$SHOTS"/*.xwd "$SHOTS"/*.png
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# window.__QSTATE_E2E first (JSON strings, so every character of a path survives), then the real script.
INJECTED="$TMP/e2e-injected.js"
python3 - "$INJECTED" "$SCRIPT" "$REPO" "$REF" "$STATE" <<'PY'
import json, sys
out, script, repo, ref, state = sys.argv[1:6]
with open(out, "w", encoding="utf-8") as f:
    f.write("window.__QSTATE_E2E = " + json.dumps({"repoUrl": repo, "ref": ref, "statePath": state}) + ";\n")
    with open(script, encoding="utf-8") as s:
        f.write(s.read())
PY

export EXE SHOTS INJECTED TMP TIMEOUT
export QSTATE_SELFTEST_SCRIPT="$INJECTED"
export QSTATE_CONFIG_DIR="$TMP/config"
export QSTATE_CACHE_DIR="${CACHE:-$TMP/cache}"

set +e
xvfb-run -a -s "-screen 0 1600x1000x24" bash -c '
    LOG="$TMP/app.log"
    : > "$LOG"
    timeout "$TIMEOUT" "$EXE" 2> "$LOG" &
    app=$!
    # screen capture on every "[page] SHOT <name>" line (polling: no pipes that could outlive the app)
    seen=0
    while kill -0 "$app" 2>/dev/null; do
        total=$(wc -l < "$LOG")
        while [ "$seen" -lt "$total" ]; do
            seen=$((seen + 1))
            line=$(sed -n "${seen}p" "$LOG")
            case "$line" in
                "[page] SHOT "*) xwd -root -silent -display "$DISPLAY" > "$SHOTS/${line#\[page\] SHOT }.xwd" ;;
            esac
        done
        sleep 0.1
    done
    wait "$app"
    rc=$?
    cat "$LOG"
    exit $rc
'
rc=$?
set -e
for f in "$SHOTS"/*.xwd; do
    [ -e "$f" ] || continue
    python3 "$HERE/xwd2png.py" "$f" "${f%.xwd}.png" && rm -f "$f"
done
echo "screenshots: $SHOTS"
exit $rc
