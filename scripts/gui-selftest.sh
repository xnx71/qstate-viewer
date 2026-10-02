#!/usr/bin/env bash
# gui-selftest.sh [--build-dir <dir>] [--screenshot <out.png>]
#
# Runs the bridge self test of the desktop app (QSTATE_SELFTEST=1, see docs/HOST.md "Testing") on a virtual X display
# (xvfb-run) and exits with its status. With --screenshot the window is kept open for a few seconds and the virtual
# screen is saved as PNG (xwd + scripts/xwd2png.py), which shows that the built-in test page really rendered.
#
# WebKitGTK environment (see docs/HOST.md): nothing is required on a normal desktop or on this machine (webview does
# not enable WebKit's bubblewrap sandbox). LIBGL_ALWAYS_SOFTWARE=1 silences the DRI3 warnings of Xvfb; the COMPOSITING /
# DMABUF switches are a precaution for odd GPU stacks.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
BUILD_DIR="$ROOT/build"
SHOT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift ;;
        --screenshot) SHOT="$2"; shift ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

EXE="$(find "$BUILD_DIR" -name qstate-viewer -type f -perm -u+x 2>/dev/null | head -1)"
[ -n "$EXE" ] || { echo "qstate-viewer not found under $BUILD_DIR (configure with -DQSTATE_WEBVIEW_SYSROOT=... and build)" >&2; exit 2; }
command -v xvfb-run >/dev/null || { echo "xvfb-run not installed" >&2; exit 2; }

export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"
export WEBKIT_DISABLE_COMPOSITING_MODE="${WEBKIT_DISABLE_COMPOSITING_MODE:-1}"
export WEBKIT_DISABLE_DMABUF_RENDERER="${WEBKIT_DISABLE_DMABUF_RENDERER:-1}"
export GDK_BACKEND=x11
export QSTATE_SELFTEST=1

if [ -z "$SHOT" ]; then
    exec xvfb-run -a -s "-screen 0 1280x900x24" "$EXE"
fi

command -v xwd >/dev/null || { echo "xwd not installed (x11-apps)" >&2; exit 2; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
SHOT_DELAY=3
export EXE SHOT TMP HERE SHOT_DELAY QSTATE_SELFTEST_HOLD_MS=$(( (SHOT_DELAY + 3) * 1000 ))
xvfb-run -a -s "-screen 0 1280x900x24" bash -c '
    "$EXE" &
    pid=$!
    sleep "$SHOT_DELAY"
    xwd -root -silent -display "$DISPLAY" > "$TMP/screen.xwd" || true
    wait $pid
'
python3 "$HERE/xwd2png.py" "$TMP/screen.xwd" "$SHOT"
echo "screenshot: $SHOT"
