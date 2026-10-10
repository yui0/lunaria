#!/bin/sh
#
# Copyright © 2026 Yuichiro Nakada / Project Lunaria
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Stages a run-anywhere copy of the emulator and packs it.
#
# What makes a build portable is not the executable: it is everything the
# executable expects to find on the machine it was built on.  Three kinds of
# thing have to travel with it, and each was a reason a copied build did not
# start somewhere else:
#
#   runtime/    the Android shim libraries.  They are loaded by name, and the
#               launcher puts the bundle's own directory first.
#   lib/        the host libraries the executable and the shims were linked
#               against — GLFW, EGL/GLESv2, OpenSSL, ALSA and so on.  The
#               C library itself is deliberately *not* bundled: a libc from
#               the build machine cannot be mixed with the loader on the
#               running one, so the bundle asks for a distribution no older
#               than the one it was built on and uses its libc.
#   fonts/      luna-ui's own fonts.  It looks for them relative to the
#               working directory, which for a launcher is wherever the user
#               happened to be standing.
#
# Usage: make-dist.sh <stage-dir> <os> [arch] [binary] [syslib-dir] [decoder]

set -eu

argv0="$0"
msg() { printf -- '%s: %s\n' "${argv0##*/}" "$@" 1>&2; }
err() { msg "$@"; exit 1; }

[ $# -ge 2 ] || err 'usage: make-dist.sh <stage-dir> <os> [arch]'
stage="$1"
os="$2"
arch="${3:-$(uname -m)}"

here=$(CDPATH= cd -- "$(dirname -- "$argv0")/.." && pwd) || err 'cannot find the tree'
cd "$here"

case "$os" in
    windows) executable=lunaria.exe ;;
    linux|macos) executable=lunaria ;;
    *) err "unknown os \"$os\"" ;;
esac
binary="${4:-./$executable}"
syslib="${5:-syslib-arm64}"
decoder="${6:-}"
[ -f "$binary" ] || err "build the emulator first ($binary missing)"

rm -rf "$stage"
mkdir -p "$stage" "$stage/runtime" "$stage/lib" "$stage/fonts"

cp -p "$binary" "$stage/$executable"
cp -R ./keymaps "$stage/"
[ -f lunaria.conf.sample ] && cp -p lunaria.conf.sample "$stage/"
[ -f README.md ] && cp -p README.md "$stage/"
[ -f README.ja.md ] && cp -p README.ja.md "$stage/"
[ -f LICENSE ] && cp -p LICENSE "$stage/"
mkdir -p "$stage/licenses"
cp -p src/lib/musl/COPYRIGHT "$stage/licenses/musl.txt"
cp -p LICENSE.unicode "$stage/licenses/unicode.txt"
if [ "$os" = windows ]; then
    cp -p runtime/*.dll "$stage/runtime/" 2>/dev/null || true
else
    cp -p runtime/*.so "$stage/runtime/" 2>/dev/null || true
    cp -p runtime/*.dylib "$stage/runtime/" 2>/dev/null || true
fi
[ -z "$decoder" ] || cp -p "$decoder" "$stage/runtime/"

# The guest-side AArch64 system libraries, when they have been built.  Without
# them every libm call leaves the JIT for an SVC; with them the launcher finds
# the directory by name next to itself.
if [ -d "$syslib" ]; then
    mkdir -p "$stage/syslib-arm64"
    cp -p "$syslib"/*.so "$stage/syslib-arm64/" 2>/dev/null || true
fi

# --- fonts ----------------------------------------------------------------
for f in ../luna-shell/skins/fonts/web/Inter-Regular.ttf \
         ../luna-shell/skins/fonts/web/Inter-Bold.ttf \
         ../luna-shell/skins/fonts/LunaSymbols-Solid.otf \
         ../luna-shell/skins/fonts/LunaSymbols-Brands.otf; do
    [ -f "$f" ] && cp -p "$f" "$stage/fonts/"
done
# A CJK face, so Japanese, Korean and Chinese text in the emulator's own UI is
# not a row of boxes on a machine that has no system font for it.
for f in /usr/share/fonts/ja/TrueType/NotoSansCJKjp-Regular.otf \
         /usr/share/fonts/opentype/noto/NotoSansCJKjp-Regular.otf \
         /usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc; do
    if [ -f "$f" ]; then cp -p "$f" "$stage/fonts/"; break; fi
done

# --- host libraries -------------------------------------------------------
#
# Everything the executable and the shims name, minus the set that has to come
# from the running system: the C library and its companions are matched to the
# kernel and to the dynamic loader that is already running this process.
bundle_linux() {
    _keep="$1"
    for _bin in "$stage/lunaria" "$stage"/runtime/*.so; do
        [ -f "$_bin" ] || continue
        ldd "$_bin" 2>/dev/null | while read -r _name _arrow _path _rest; do
            [ "$_arrow" = '=>' ] || continue
            [ -f "$_path" ] || continue
            case "$_name" in
                libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*| \
                ld-linux*|libgcc_s.so.*|libresolv.so.*)
                    continue ;;
                # The shims in runtime/ resolve to the bundle's own copies.
                libjvm.so|libdl.so|libpthread.so|libc.so|libEGL.so|libz.so)
                    continue ;;
            esac
            [ -f "$_keep/$_name" ] && continue
            cp -Lp "$_path" "$_keep/$_name"
        done
    done
}


# A cross-built bundle cannot be asked what it needs: ldd runs the dynamic
# loader, and the loader on this machine is for another architecture.  Read
# the NEEDED entries with the target's objdump instead and look each name up
# in the target's library directories (DIST_LIB_DIRS, colon separated),
# repeating until nothing new turns up so that the libraries of the libraries
# travel too.  The same system-owned set as above stays behind.
bundle_linux_cross() {
    _keep="$1"
    _objdump="${OBJDUMP:-objdump}"
    [ -n "${DIST_LIB_DIRS:-}" ] || err 'cross bundle: set DIST_LIB_DIRS to the target library directories'
    _again=1
    while [ "$_again" = 1 ]; do
        _again=0
        for _bin in "$stage/lunaria" "$stage"/runtime/*.so "$_keep"/*.so*; do
            [ -f "$_bin" ] || continue
            for _name in $("$_objdump" -p "$_bin" 2>/dev/null | awk '/NEEDED/ {print $2}'); do
                case "$_name" in
                    libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*| \
                    ld-linux*|libgcc_s.so.*|libresolv.so.*)
                        continue ;;
                    libjvm.so|libdl.so|libpthread.so|libc.so|libEGL.so|libz.so)
                        continue ;;
                esac
                [ -f "$_keep/$_name" ] && continue
                [ -f "$stage/runtime/$_name" ] && continue
                _found=
                _oldifs=$IFS; IFS=:
                for _dir in $DIST_LIB_DIRS; do
                    if [ -e "$_dir/$_name" ]; then _found="$_dir/$_name"; break; fi
                done
                IFS=$_oldifs
                if [ -z "$_found" ]; then
                    msg "warning: $_name (needed by ${_bin##*/}) not found in DIST_LIB_DIRS"
                    continue
                fi
                cp -Lp "$_found" "$_keep/$_name"
                _again=1
            done
        done
    done
}

case "$os" in
    linux)
        if [ "$arch" != "$(uname -m)" ] && [ -n "${DIST_LIB_DIRS:-}" ]; then
            bundle_linux_cross "$stage/lib"
        else
            bundle_linux "$stage/lib"
        fi ;;
    macos) python3 scripts/bundle-macos.py "$stage" ;;
    windows) python3 scripts/bundle-windows.py "$stage" ;;
    *)     err "unknown os \"$os\"" ;;
esac

# An empty lib/ would make the launcher put a directory that holds nothing on
# the search path; drop it instead, so "this build carries its libraries" and
# "this build uses the system's" are visible from the tree.
rmdir "$stage/lib" 2>/dev/null || true

msg "staged $(du -sh "$stage" | cut -f1) in $stage"
