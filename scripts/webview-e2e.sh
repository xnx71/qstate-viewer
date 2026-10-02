#!/usr/bin/env bash
# webview-e2e.sh --build-dir <dir> --core <dir> --state <dir> [--ref auto] [--shots <dir>] [--size WxH]
#
# Runs the REAL desktop app (WebKitGTK webview, embedded UI) under xvfb against real state files and drives the UI with
# ui/scripts/webview-e2e.js (injected through `qstate-viewer --selftest-script`). The script logs "SHOT <name>"; this
# runner captures the virtual screen at that moment (xwd -> scripts/xwd2png.py) into --shots. Exit status = verdict.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
BUILD_DIR="$ROOT/build"
CORE=""; STATE=""; REF="auto"; SHOTS="/tmp/webview-e2e-shots"; SIZE="1500x900"
SCRIPT="$ROOT/ui/scripts/webview-e2e.js"
TIMEOUT=240
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift ;;
        --core) CORE="$2"; shift ;;
        --state) STATE="$2"; shift ;;
        --ref) REF="$2"; shift ;;
        --shots) SHOTS="$2"; shift ;;
        --size) SIZE="$2"; shift ;;
        --script) SCRIPT="$2"; shift ;;
        --timeout) TIMEOUT="$2"; shift ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$CORE" ] && [ -n "$STATE" ] || { echo "--core and --state are required" >&2; exit 2; }
EXE="$(find "$BUILD_DIR" -name qstate-viewer -type f -perm -u+x 2>/dev/null | head -1)"
[ -n "$EXE" ] || { echo "qstate-viewer not found under $BUILD_DIR" >&2; exit 2; }
command -v xvfb-run >/dev/null && command -v xwd >/dev/null || { echo "xvfb-run and xwd are required" >&2; exit 2; }

export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"
export WEBKIT_DISABLE_COMPOSITING_MODE="${WEBKIT_DISABLE_COMPOSITING_MODE:-1}"
export WEBKIT_DISABLE_DMABUF_RENDERER="${WEBKIT_DISABLE_DMABUF_RENDERER:-1}"
export GDK_BACKEND=x11
mkdir -p "$SHOTS"
rm -f "$SHOTS"/*.xwd "$SHOTS"/*.png
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export EXE CORE STATE REF SHOTS SIZE SCRIPT TMP TIMEOUT
# settings (theme, panel sizes) of the app must not leak into / from the user's configuration
export XDG_CONFIG_HOME="$TMP/config"

set +e
xvfb-run -a -s "-screen 0 1600x1000x24" bash -c '
    LOG="$TMP/app.log"
    : > "$LOG"
    timeout "$TIMEOUT" "$EXE" --core "$CORE" --ref "$REF" --state "$STATE" --size "$SIZE" --selftest-script "$SCRIPT" 2> "$LOG" &
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
