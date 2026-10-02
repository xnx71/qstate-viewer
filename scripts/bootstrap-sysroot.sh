#!/usr/bin/env bash
# bootstrap-sysroot.sh <dest_dir> [--force] [--packages "pkg ..."]
#
# Builds a LOCAL (no root needed) development sysroot with the GTK3 + WebKitGTK headers and linker
# symlinks needed to build the qstate-viewer desktop app on Debian / Ubuntu machines where the
# `libgtk-3-dev` / `libwebkit2gtk-4.1-dev` packages are not installed but the runtime libraries
# (libgtk-3-0, libwebkit2gtk-4.1-0) are.
#
# How it works
#   1. `apt-get install --print-uris` (needs no root) lists the .deb files apt would install for the
#      -dev packages, i.e. exactly the closure that is missing on this machine.
#   2. Each .deb is downloaded (curl, sha256 verified against apt's data; apt-get download as a fallback)
#      into <dest>/.debs and extracted with `dpkg -x` into <dest>.
#   3. Dangling lib*.so development symlinks (they point to lib*.so.N that only exist in the system lib
#      directory) are repointed to the installed runtime libraries.
#   4. The pkg-config files are rewritten so that prefix / libdir / includedir are absolute paths inside
#      <dest> (so no PKG_CONFIG_SYSROOT_DIR is needed).
#
# Idempotent and resumable: finished downloads / extractions are skipped (stamp files). After the first
# successful run it works OFFLINE. If you move <dest> re-run the script to rewrite the .pc files.
# Not for production: a sysroot like this is only meant for building and testing on a dev machine.
#
# Use it:   cmake -S . -B build -DQSTATE_WEBVIEW_SYSROOT=<dest>      (cmake/FindWebviewDeps.cmake does the rest)
# Or:       source <dest>/sysroot-env.sh      (PKG_CONFIG_PATH for manual pkg-config use)
set -euo pipefail

usage() { sed -n '2,/^set -euo/p' "$0" | sed -e '$d' -e 's/^# \{0,1\}//' >&2; exit "${1:-2}"; }

DEST=""
FORCE=0
PKGS=(libgtk-3-dev libwebkit2gtk-4.1-dev)
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage 0 ;;
        --force) FORCE=1 ;;
        --packages) shift; read -r -a PKGS <<<"${1:?--packages needs a value}" ;;
        -*) echo "unknown option: $1" >&2; usage ;;
        *) if [ -z "$DEST" ]; then DEST="$1"; else echo "unexpected argument: $1" >&2; usage; fi ;;
    esac
    shift
done
[ -n "$DEST" ] || usage

log() { printf '[sysroot] %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

for tool in apt-get dpkg curl pkg-config awk sed find sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || die "required tool not found: $tool (this script is for Debian / Ubuntu machines)"
done

if [ "$FORCE" -eq 0 ] && pkg-config --exists gtk+-3.0 'webkit2gtk-4.1' 2>/dev/null; then
    log "gtk+-3.0 and webkit2gtk-4.1 development files are already installed system-wide: nothing to do."
    log "(use --force to build the sysroot anyway)"
    exit 0
fi

MULTIARCH="$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo x86_64-linux-gnu)"
SYSLIB="/usr/lib/${MULTIARCH}"

mkdir -p "$DEST"
DEST="$(cd "$DEST" && pwd -P)"
DEBS="$DEST/.debs"
STAMPS="$DEST/.stamps"
mkdir -p "$DEBS" "$STAMPS"

# ---------------------------------------------------------------------------
# 1. Resolve the package closure (falls back to the cached list when apt cannot resolve, e.g. offline).
# ---------------------------------------------------------------------------
URIS="$DEST/.uris.txt"
log "resolving the closure of: ${PKGS[*]}"
if apt-get install --print-uris -qq --no-install-recommends "${PKGS[@]}" > "$URIS.tmp" 2>"$URIS.err"; then
    grep -E "^'[^']+\.deb'" "$URIS.tmp" > "$URIS" || true
elif [ -s "$URIS" ]; then
    log "apt could not resolve the packages ($(head -c 200 "$URIS.err" | tr '\n' ' ')); using the cached list"
else
    cat "$URIS.err" >&2
    die "cannot resolve ${PKGS[*]} (are the apt lists present? is the distribution supported?)"
fi
rm -f "$URIS.tmp" "$URIS.err"
[ -s "$URIS" ] || { log "apt reports nothing to install: the dev packages are present system-wide"; exit 0; }
log "$(wc -l < "$URIS") packages in the closure"

# ---------------------------------------------------------------------------
# 2. Download. Lines: 'URI' FILE SIZE SHA256:HASH
# ---------------------------------------------------------------------------
while IFS= read -r line; do
    uri="$(printf '%s' "$line" | awk -F"'" '{print $2}')"
    file="$(printf '%s' "$line" | awk '{print $2}')"
    size="$(printf '%s' "$line" | awk '{print $3}')"
    hash="$(printf '%s' "$line" | awk '{print $4}' | sed -n 's/^SHA256://p')"
    target="$DEBS/$file"
    if [ -s "$target" ] && { [ -z "$hash" ] || [ "$(sha256sum "$target" | awk '{print $1}')" = "$hash" ]; }; then
        continue
    fi
    rm -f "$target"
    log "downloading $file ($size bytes)"
    if ! curl -fsSL --retry 3 -o "$target.part" "$uri"; then
        rm -f "$target.part"
        log "curl failed, trying apt-get download"
        pkg="${file%%_*}"
        (cd "$DEBS" && apt-get download "$pkg" >/dev/null 2>&1) || die "cannot download $file"
        [ -s "$target" ] || die "apt-get download did not produce $file"
    else
        mv "$target.part" "$target"
    fi
    if [ -n "$hash" ] && [ "$(sha256sum "$target" | awk '{print $1}')" != "$hash" ]; then
        rm -f "$target"
        die "sha256 mismatch for $file"
    fi
done < "$URIS"

# ---------------------------------------------------------------------------
# 3. Extract (dpkg -x needs no root).
# ---------------------------------------------------------------------------
while IFS= read -r line; do
    file="$(printf '%s' "$line" | awk '{print $2}')"
    [ -e "$STAMPS/$file.extracted" ] && continue
    log "extracting $file"
    dpkg -x "$DEBS/$file" "$DEST"
    : > "$STAMPS/$file.extracted"
done < "$URIS"

# ---------------------------------------------------------------------------
# 4. Repair dangling development symlinks: lib/<arch>/libfoo.so -> libfoo.so.N, where libfoo.so.N lives in
#    the system library directory (installed runtime package), not in the sysroot.
# ---------------------------------------------------------------------------
UNRESOLVED=()
for libdir in "$DEST$SYSLIB" "$DEST/usr/lib"; do
    [ -d "$libdir" ] || continue
    while IFS= read -r -d '' lnk; do
        [ -e "$lnk" ] && continue
        target="$(readlink "$lnk")"
        base="$(basename "$target")"
        fixed=0
        for cand in "$SYSLIB/$base" "/usr/lib/$base" "/lib/$MULTIARCH/$base"; do
            if [ -e "$cand" ]; then ln -sfn "$cand" "$lnk"; fixed=1; break; fi
        done
        [ "$fixed" -eq 1 ] || UNRESOLVED+=("$(basename "$lnk") -> $target")
    done < <(find "$libdir" -maxdepth 1 -type l -name 'lib*.so*' -print0)
done
if [ ${#UNRESOLVED[@]} -gt 0 ]; then
    log "WARNING: ${#UNRESOLVED[@]} development symlinks have no installed runtime library (only a problem if linked):"
    printf '[sysroot]    %s\n' "${UNRESOLVED[@]}" >&2
fi

# ---------------------------------------------------------------------------
# 5. Rewrite the pkg-config files: absolute prefix / libdir / includedir inside the sysroot. The pristine
#    file is kept as *.pc.orig, which makes the rewrite repeatable (also after moving the directory).
# ---------------------------------------------------------------------------
for pcdir in "$DEST$SYSLIB/pkgconfig" "$DEST/usr/share/pkgconfig" "$DEST/usr/lib/pkgconfig"; do
    [ -d "$pcdir" ] || continue
    for pc in "$pcdir"/*.pc; do
        [ -f "$pc" ] || continue
        [ -f "$pc.orig" ] || cp -p "$pc" "$pc.orig"
        sed -E \
            -e "s#(=|-I|-L|-isystem ?|-F|-rpath[ =]|,)/usr(/|\$|[[:space:]])#\1$DEST/usr\2#g" \
            -e "s#(=|-I|-L)/lib(/|\$|[[:space:]])#\1$DEST/lib\2#g" \
            "$pc.orig" > "$pc"
    done
done

# ---------------------------------------------------------------------------
# 6. Environment file for manual use.
# ---------------------------------------------------------------------------
PC_PATH="$DEST$SYSLIB/pkgconfig:$DEST/usr/share/pkgconfig:$DEST/usr/lib/pkgconfig"
cat > "$DEST/sysroot-env.sh" <<ENVEOF
# source this file before running pkg-config / cmake by hand (cmake -DQSTATE_WEBVIEW_SYSROOT=... does it itself)
export QSTATE_WEBVIEW_SYSROOT="$DEST"
export PKG_CONFIG_PATH="$PC_PATH\${PKG_CONFIG_PATH:+:\$PKG_CONFIG_PATH}"
ENVEOF

# ---------------------------------------------------------------------------
# 7. Self-check.
# ---------------------------------------------------------------------------
export PKG_CONFIG_PATH="$PC_PATH${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
if pkg-config --exists gtk+-3.0 webkit2gtk-4.1; then
    log "self-check ok: gtk+-3.0 $(pkg-config --modversion gtk+-3.0), webkit2gtk-4.1 $(pkg-config --modversion webkit2gtk-4.1)"
else
    pkg-config --print-errors --exists gtk+-3.0 webkit2gtk-4.1 || true
    die "self-check failed: pkg-config cannot find gtk+-3.0 / webkit2gtk-4.1 in $DEST"
fi
log "done. Use: cmake -S <repo> -B <build> -DQSTATE_WEBVIEW_SYSROOT=$DEST"
