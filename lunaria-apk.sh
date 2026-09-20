#!/bin/sh
#
# Copyright © 2026 Yuichiro Nakada / Project Lunaria
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#

argv0="$0"
msg() { printf -- '%s: %s\n' "${argv0##*/}" "$@" 1>&2; }
err() { msg "$@"; exit 1; }

script_dir=$(CDPATH= cd -- "$(dirname -- "$argv0")" && pwd) \
    || err 'cannot resolve launcher directory'

# lunaria.conf — the settings that are a property of this installation rather
# than of one run.  Which device the emulator reports itself as is the obvious
# one: it belongs with the machine, not on every command line.
#
# The file is `NAME=value` lines, `#` comments, one setting per line, and only
# LUNARIA_* names are honoured — it is read, not sourced, so a stray command in
# it cannot run.  A variable already set in the environment always wins, so
# `LUNARIA_DEVICE=pixel7 ./lunaria-apk.sh …` still overrides the file for one
# run.
#
# There is one place to look: `lunaria.conf` in the directory the launcher was
# started from.  A search path (the launcher's own directory, ~/.config, an
# environment override) means the settings actually in force are wherever the
# first hit happened to be, which is the wrong thing to have to work out when a
# run behaves differently than the file in front of you says it should.
lunaria_load_conf() {
    _conf="$PWD/lunaria.conf"
    [ -f "$_conf" ] || return 0
    msg "config: $_conf"
    while IFS= read -r _line || [ -n "$_line" ]; do
        case "$_line" in
            ''|'#'*) continue ;;
            LUNARIA_*=*) ;;
            *) continue ;;
        esac
        _name=${_line%%=*}
        _value=${_line#*=}
        # Strip one layer of surrounding quotes and trailing blanks.
        _value=${_value%"${_value##*[! ]}"}
        case "$_value" in
            \"*\") _value=${_value#\"}; _value=${_value%\"} ;;
            \'*\') _value=${_value#\'}; _value=${_value%\'} ;;
        esac
        # The environment wins: a setting made for this run is not overridden
        # by the installation's default.
        eval "_cur=\${$_name-}"
        [ -n "$_cur" ] && continue
        export "$_name=$_value"
    done < "$_conf"
}
lunaria_load_conf

[ -z "$1" ] && err 'usage: <apk-or-xapk>'
inputfile="$(realpath "$1")"
pkgfile="$inputfile"
xapk_dir=""
xapk_splits=""

# Installing a package is not part of launching it.  A device unpacks the APK,
# materialises its splits and stages the expansion once, at install time; every
# later launch starts from that unpacked tree.  This script used to redo all of
# it into a fresh mktemp dir on every run — for this title 46 s of unzip, move
# and pak staging before the emulator got a single instruction — which makes
# the emulator look far slower to start than it is.
#
# So keep the unpacked tree, keyed on the input file's identity (path, size,
# mtime): a different or rebuilt package gets a different key and is unpacked
# again.  The marker file is written last, so an interrupted unpack is never
# mistaken for a finished one.  LUNARIA_NO_CACHE=1 forces a fresh unpack;
# LUNARIA_CACHE_DIR moves the tree off tmpfs (it is several GB).
cache_dir=""
cache_hit=0
if [ -z "$LUNARIA_NO_CACHE" ]; then
    # Include the installed-tree format.  Launcher fixes that change how
    # split assets are materialised must never silently reuse an older tree.
    # Format 7 invalidates trees repaired before base entries were restored
    # first.  Overlay order is significant when base and a split contain the
    # same path (notably AndroidManifest.xml and resources.arsc).
    _cache_format=7
    # Python supplies one implementation on GNU/Linux and BSD/macOS.  The old
    # stat -c | sha1sum pipeline produced an empty key on macOS; that collapsed
    # every package into the cache root and made the following cleanup unsafe.
    _key="$(python3 - "$inputfile" "$_cache_format" <<'PYEOF'
import hashlib, os, sys
path, fmt = sys.argv[1:]
st = os.stat(path)
identity = f'{os.path.realpath(path)}|{fmt}|{st.st_size}|{st.st_mtime_ns}'
print(hashlib.sha1(identity.encode()).hexdigest()[:16])
PYEOF
)" || err 'cannot identify input package'
    [ -n "$_key" ] || err 'empty package cache key'
    cache_dir="${LUNARIA_CACHE_DIR:-${TMPDIR:-/tmp}/lunaria-cache}/$_key"
    if [ -f "$cache_dir/.ready" ]; then
        cache_hit=1
        msg "package cache hit: $cache_dir (LUNARIA_NO_CACHE=1 to rebuild)"
    else
        rm -rf "$cache_dir"
        mkdir -p "$cache_dir" || cache_dir=""
    fi
fi

# XAPK / APKS are split-APK install containers. Keep every contained APK
# byte-for-byte intact: use the base APK as nativeFile and overlay split
# contents only in the temporary installed-package view created below.
case "$inputfile" in
    *.xapk|*.XAPK)
        if [ -n "$cache_dir" ]; then
            xapk_dir="$cache_dir/pkg"
        else
            xapk_dir="$(mktemp -d)"
        fi
        # pkg/ is a cheap-to-rebuild staging copy of the xapk's own zip
        # contents; the expensive tree $cache_hit actually guards is inst/
        # (unpacked further down).  A host that reclaims disk space under
        # pressure has no reason to know that, and reaps whichever files in
        # the cache tree look least recently touched -- which, once a package
        # is cached, is always pkg/: inst/ is opened every run, pkg/ never is
        # again until the input xapk changes.  Losing just pkg/ used to fail
        # the whole run with "invalid xapk manifest" and offer only
        # LUNARIA_NO_CACHE=1 (a full multi-GB inst/ rebuild) as the fix, for
        # damage a few seconds of re-unzipping the small xapk repairs. Re-
        # extract pkg/ whenever it is missing, independent of $cache_hit,
        # which still gates the one expensive step (inst/) below unchanged.
        if [ "$cache_hit" -eq 0 ] || [ ! -f "$xapk_dir/manifest.json" ]; then
            mkdir -p "$xapk_dir"
            unzip -qo "$inputfile" -d "$xapk_dir" || err "extract xapk failed"
        fi
        _xapk_info="$(python3 - "$xapk_dir" <<'PYEOF'
import json, os, sys
root = sys.argv[1]
with open(os.path.join(root, 'manifest.json'), encoding='utf-8') as f:
    manifest = json.load(f)
apks = manifest.get('split_apks') or []
base = next((a.get('file') for a in apks if a.get('id') == 'base'), None)
if not base:
    base = next((a.get('file') for a in apks if a.get('file', '').endswith('.apk')), None)
if not base or not os.path.isfile(os.path.join(root, base)):
    raise SystemExit('XAPK manifest has no usable base APK')
print(base)
for apk in apks:
    name = apk.get('file')
    if name and name != base and os.path.isfile(os.path.join(root, name)):
        print(name)
PYEOF
)" || err "invalid xapk manifest"
        _xapk_base="$(printf '%s\n' "$_xapk_info" | head -n 1)"
        xapk_splits="$(printf '%s\n' "$_xapk_info" | tail -n +2)"
        pkgfile="$xapk_dir/$_xapk_base"
        msg "xapk base: $_xapk_base"
        [ -n "$xapk_splits" ] && msg "xapk splits: $(printf '%s' "$xapk_splits" | tr '\n' ' ')"
        ;;
    *.apks|*.APKS)
        # bundletool / SAI format: ZIP of split APKs; base APK is always base.apk.
        if [ -n "$cache_dir" ]; then
            xapk_dir="$cache_dir/pkg"
        else
            xapk_dir="$(mktemp -d)"
        fi
        # See the matching .xapk case above: pkg/ can be reaped independently
        # of inst/ under disk pressure, and is cheap to re-extract on its own.
        if [ "$cache_hit" -eq 0 ] || [ ! -f "$xapk_dir/base.apk" ]; then
            mkdir -p "$xapk_dir"
            unzip -qo "$inputfile" -d "$xapk_dir" || err "extract apks failed"
        fi
        _xapk_base="base.apk"
        [ -f "$xapk_dir/$_xapk_base" ] || err "apks has no base.apk"
        xapk_splits="$(find "$xapk_dir" -maxdepth 1 -name '*.apk' ! -name 'base.apk' \
                        -exec basename {} \; | sort)"
        pkgfile="$xapk_dir/$_xapk_base"
        msg "apks base: $_xapk_base"
        [ -n "$xapk_splits" ] && msg "apks splits: $(printf '%s' "$xapk_splits" | tr '\n' ' ')"
        ;;
esac

# Everything derived from the package below (ABI, package name, launcher
# activity, split names) is a property of the package, not of this run: it costs
# a full zip scan of every APK plus four AXML parses, and it cannot change while
# the input file does not.  Read it back from the cache when there is one.
derived="${cache_dir:+$cache_dir/derived.env}"
_need_write_derived=0
if [ "$cache_hit" -eq 1 ] && [ -s "$derived" ]; then
    # Old format had unquoted values: launch_info=a|b|c caused the shell to
    # run 'b' and 'c' as pipeline commands (subshell assignment → unset).
    # New format wraps every value in single quotes (written by _q below).
    # Detect the old format and discard it so we re-derive cleanly.
    if grep -q "^derived_schema='3'" "$derived" 2>/dev/null; then
        # shellcheck disable=SC1090  # generated by this script, one VAR=value per line
        . "$derived"
        msg "package metadata from cache"
    else
        msg "derived.env: stale format — re-deriving (package tree still valid)"
        rm -f "$derived"
        _need_write_derived=1
    fi
fi

# Prefer arm64-v8a (A64 JIT); fall back to armeabi-v7a (A32 JIT).
# Override with LUNARIA_ARCH=armeabi-v7a|arm64-v8a for comparisons.
if [ -n "$LUNARIA_ARCH" ]; then
    arch="$LUNARIA_ARCH"
elif [ -n "$arch" ]; then
    :   # from derived.env
elif unzip -l "$pkgfile" 2>/dev/null | grep -q 'lib/arm64-v8a/' ||
     { [ -n "$xapk_splits" ] && printf '%s\n' "$xapk_splits" | while IFS= read -r _s; do
           unzip -l "$xapk_dir/$_s" 2>/dev/null
       done | grep -q 'lib/arm64-v8a/'; }; then
    arch="arm64-v8a"
else
    arch="armeabi-v7a"
fi

[ -n "$pkgname" ] || pkgname="$(python3 - "$pkgfile" <<'PYEOF'
import sys, zipfile, struct, re

def parse_axml_package(data):
    """Parse binary AXML (standard APK AndroidManifest.xml)."""
    if struct.unpack_from('<I', data, 0)[0] != 0x00080003:
        return None
    i = 8
    strings = []
    while i < len(data) - 8:
        chunk_type, header_size, chunk_size = struct.unpack_from('<HHI', data, i)
        if chunk_size == 0:
            break
        if chunk_type == 0x0001:  # STRING_POOL
            str_count, style_count, flags, strings_start = struct.unpack_from('<IIII', data, i + 8)
            is_utf8 = bool(flags & (1 << 8))
            offsets_base = i + header_size
            strings_base = i + strings_start
            for k in range(str_count):
                off = struct.unpack_from('<I', data, offsets_base + k * 4)[0]
                p = strings_base + off
                if is_utf8:
                    char_len = data[p + 1]
                    s = data[p + 2: p + 2 + char_len].decode('utf-8', errors='replace')
                else:
                    slen = struct.unpack_from('<H', data, p)[0]
                    s = data[p + 2: p + 2 + slen * 2].decode('utf-16-le', errors='replace')
                strings.append(s)
        elif chunk_type == 0x0102:  # START_ELEMENT
            ns_ref, name_idx = struct.unpack_from('<ii', data, i + 16)
            attr_start, attr_size, attr_count = struct.unpack_from('<HHH', data, i + 24)
            elem_name = strings[name_idx] if 0 <= name_idx < len(strings) else ''
            if elem_name == 'manifest':
                attrs_base = i + 16 + attr_start
                for a in range(attr_count):
                    ao = attrs_base + a * attr_size
                    ns2, name2, raw_val, val_size, val_res, val_type, val_data = struct.unpack_from('<iiIHBBI', data, ao)
                    aname = strings[name2] if 0 <= name2 < len(strings) else ''
                    if aname == 'package' and val_type == 0x03 and 0 <= val_data < len(strings):
                        return strings[val_data]
        i += chunk_size
    return None

def parse_proto_package(data):
    """Extract package name from protobuf-encoded manifest (App Bundle split format).
    Scans raw bytes for Java package-name patterns, excluding known framework packages."""
    text = data.decode('utf-8', errors='replace')
    candidates = re.findall(r'\b([a-zA-Z][a-zA-Z0-9_]*(?:\.[a-zA-Z][a-zA-Z0-9_]*){2,})\b', text)
    exclude = ('com.google.', 'com.android.', 'com.unity3d.', 'android.', 'java.', 'javax.')
    seen = set()
    for p in candidates:
        if p not in seen and not any(p.startswith(e) for e in exclude):
            seen.add(p)
            return p
    return None

def get_package(path):
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        # Standard APK: AndroidManifest.xml (binary AXML)
        if 'AndroidManifest.xml' in names:
            return parse_axml_package(z.read('AndroidManifest.xml'))
        # App Bundle split: base/manifest/AndroidManifest.xml (protobuf)
        candidates = [n for n in names if n.endswith('AndroidManifest.xml')]
        base = [n for n in candidates if n.startswith('base/')]
        manifest = base[0] if base else (candidates[0] if candidates else None)
        if manifest:
            return parse_proto_package(z.read(manifest))
    return None

pkg = get_package(sys.argv[1])
if pkg:
    print(pkg)
PYEOF
)"
[ -z "$pkgname" ] && err "not a valid apk (missing package name)"

if [ -n "$cache_dir" ]; then
    tmpdir="$cache_dir/inst"
    # The cached tree is the installed package; only a run that built its own
    # throwaway copy may delete anything on the way out.
    trap '' EXIT
else
    tmpdir="$(mktemp -d)"
    trap 'rm -rf "$tmpdir"; [ -z "$xapk_dir" ] || rm -rf "$xapk_dir"' EXIT
fi
if [ "$cache_hit" -eq 0 ]; then
    mkdir -p "$tmpdir"
    # An unpack that fails has to stop the run.  Without the check a truncated
    # package still reached the end of setup and got stamped .ready, so every
    # later run reused a tree whose OBB was half a file — the engine then found
    # no .uproject and put up "Failed to open descriptor file".  The tree looks
    # complete from the outside, so nothing recovers from it by itself.
    unzip -q "$pkgfile" -d "$tmpdir" \
        || err "failed to unpack $pkgfile (incomplete or corrupt archive)"
    if [ -n "$xapk_splits" ]; then
        printf '%s\n' "$xapk_splits" > "$tmpdir/.splits"
        while IFS= read -r _split; do
            [ -n "$_split" ] || continue
            unzip -q -n "$xapk_dir/$_split" -d "$tmpdir" \
                || err "failed to unpack split $_split"
        done < "$tmpdir/.splits"
        rm -f "$tmpdir/.splits"
    fi
fi

# Mono looks for assemblies at <PACKAGE_CODE_PATH>/assets/bin/Data/Managed/mono/2.0/
# Also needs mono/config in the mono/ directory.
# Create symlinks so Mono finds everything via the extracted dir.
managed_dir="$tmpdir/assets/bin/Data/Managed"

# The installed package view keeps split assets exactly as shipped.  Lunaria's
# AssetManager/open bridge concatenates name.split0..N lazily when the guest
# asks for the logical asset, matching Android without rewriting or repacking
# any APK.  Keep an ASCII path for native engines that open the base APK.
_apk_guest="$tmpdir/base.apk"
[ -f "$_apk_guest" ] || ln "$pkgfile" "$_apk_guest" 2>/dev/null \
    || cp -f "$pkgfile" "$_apk_guest" \
    || err "stage apk for guest open failed"
export ANDROID_APK_FILE="$_apk_guest"

# PackageManager exposes the installed APK's signer certificate through
# PackageInfo.signatures / signingInfo.  Keep the APK's PKCS#7 block beside
# the staged package so the in-process Android runtime can return the real
# certificate bytes (never a synthetic fingerprint).  A v1 block is also
# present in modern v2/v3-signed bundles for compatibility; Android's
# Signature object contains the signer certificate, not the PKCS#7 wrapper.
_signer_entry=$(unzip -Z1 "$pkgfile" 2>/dev/null |
    awk 'toupper($0) ~ /^META-INF\/.*\.(RSA|DSA|EC)$/ { print; exit }')
if [ -n "$_signer_entry" ]; then
    _signer_file="$tmpdir/.lunaria-apk-signer.p7b"
    if unzip -p "$pkgfile" "$_signer_entry" > "$_signer_file"; then
        export ANDROID_APK_SIGNER_PKCS7="$_signer_file"
    else
        rm -f "$_signer_file"
        unset ANDROID_APK_SIGNER_PKCS7
    fi
else
    unset ANDROID_APK_SIGNER_PKCS7
fi

# ART/Dalvik reads dex entries from the installed APK through a code path.  The
# in-process DVM uses the equivalent unpacked view, so keep classes*.dex beside
# the staged base APK.  Cached install trees can be partially reaped by macOS
# while base.apk and the cache stamp survive; repair that case on every run
# instead of declaring bytecode unavailable and silently falling back to a
# native-only Activity startup.
if ! find "$tmpdir" -maxdepth 1 -type f -name 'classes*.dex' -print -quit |
     grep -q .; then
    if unzip -Z1 "$pkgfile" 'classes*.dex' 2>/dev/null | grep -q .; then
        unzip -qo "$pkgfile" 'classes*.dex' -d "$tmpdir" \
            || err "failed to stage APK dex files"
        msg "repaired installed dex view from base APK"
    fi
fi

# A cached install is an overlay of the base APK and every split APK.  Restore
# missing executable payloads: losing one produces an install Android could
# never have (for example libUE4.so present but libc++_shared.so absent).  Do
# not stat every resource entry on every launch; the cache-format key rebuilds
# trees whose overlay rules change, while these large payloads are the entries
# that can be evicted independently in practice.
if [ -n "$xapk_splits" ]; then
    printf '%s\n' "$xapk_splits" | while IFS= read -r _split; do
        [ -n "$_split" ] || continue
        _split_apk="$xapk_dir/$_split"
        [ -f "$_split_apk" ] || err "missing cached split $_split"
        unzip -Z1 "$_split_apk" 'lib/*/*.so' 'assets/main.obb.png' \
              'assets/patch.obb.png' 2>/dev/null | while IFS= read -r _entry; do
            case "$_entry" in
                ''|*/) continue ;;
            esac
            if [ ! -f "$tmpdir/$_entry" ]; then
                mkdir -p "$tmpdir/$(dirname "$_entry")"
                unzip -qo "$_split_apk" "$_entry" -d "$tmpdir" \
                    || err "failed to repair $_entry from split $_split"
                msg "repaired split entry: $_entry"
            fi
        done
    done
fi

if [ -d "$managed_dir" ]; then
    # Mono's mono_assembly_load_corlib() searches for corlib at
    #   <assembly_rootdir>/mono/<framework_version>/mscorlib.dll
    # where framework_version is taken from the selected runtime in
    # supported_runtimes[].  Unity's bundled mono (4.x) falls back to
    # DEFAULT_RUNTIME_VERSION = "v1.1.4322" (framework_version "1.0") when the
    # exe/runtime version can't be resolved, so the DLLs must be reachable under
    # mono/1.0 as well as mono/2.0 (and 4.0).  Mirroring them under every
    # version dir means corlib loads regardless of which runtime mono picks;
    # otherwise load_in_path() finds nothing and mono_init trips
    # g_assert_not_reached() at domain.c:1254 → exit(1) every frame (black screen).
    for _ver in 1.0 2.0 4.0 net_4_x-linux; do
        mkdir -p "$managed_dir/mono/$_ver"
        for _dll in "$managed_dir"/*.dll; do
            [ -f "$_dll" ] && ln -sf "../../$(basename "$_dll")" \
                "$managed_dir/mono/$_ver/$(basename "$_dll")"
        done
    done
    # mono/config: create a minimal one if not already present
    if [ ! -f "$managed_dir/mono/config" ]; then
        printf '<configuration>\n</configuration>\n' > "$managed_dir/mono/config"
    fi
fi

export ANDROID_PACKAGE_CODE_PATH="$tmpdir"
export ANDROID_PACKAGE_NAME="$pkgname"

# An install-time asset pack IS a split APK.  Play Core finds one through
# ApplicationInfo.splitNames / splitSourceDirs, so the emulator has to report
# the same set the installer would have written — otherwise
# SplitInstallInfoProvider says "No splits are found" and AssetPackStorage
# cannot resolve the pack by name.  The name is the <manifest split="…">
# attribute of the split APK, not its file name.
if [ -n "$xapk_splits" ]; then
    # shellcheck disable=SC2086  # the split list is one file name per line
    ANDROID_SPLIT_APKS="$(python3 - "$xapk_dir" $xapk_splits <<'PYEOF'
import os, struct, sys, zipfile

def axml_manifest_attr(data, want):
    if struct.unpack_from('<I', data, 0)[0] != 0x00080003:
        return None
    i, strings = 8, []
    while i < len(data) - 8:
        chunk_type, header_size, chunk_size = struct.unpack_from('<HHI', data, i)
        if chunk_size == 0:
            break
        if chunk_type == 0x0001:  # STRING_POOL
            str_count, _, flags, strings_start = struct.unpack_from('<IIII', data, i + 8)
            is_utf8 = bool(flags & (1 << 8))
            offsets_base, strings_base = i + header_size, i + strings_start
            for k in range(str_count):
                off = struct.unpack_from('<I', data, offsets_base + k * 4)[0]
                p = strings_base + off
                if is_utf8:
                    s = data[p + 2: p + 2 + data[p + 1]].decode('utf-8', 'replace')
                else:
                    slen = struct.unpack_from('<H', data, p)[0]
                    s = data[p + 2: p + 2 + slen * 2].decode('utf-16-le', 'replace')
                strings.append(s)
        elif chunk_type == 0x0102:  # START_ELEMENT
            _, name_idx = struct.unpack_from('<ii', data, i + 16)
            attr_start, attr_size, attr_count = struct.unpack_from('<HHH', data, i + 24)
            if 0 <= name_idx < len(strings) and strings[name_idx] == 'manifest':
                base = i + 16 + attr_start
                for a in range(attr_count):
                    _, nm, _, _, _, vt, vd = struct.unpack_from('<iiIHBBI', data, base + a * attr_size)
                    aname = strings[nm] if 0 <= nm < len(strings) else ''
                    if aname == want and vt == 0x03 and 0 <= vd < len(strings):
                        return strings[vd]
                return None
        i += chunk_size
    return None

root = sys.argv[1]
out = []
for name in sys.argv[2:]:
    if not name:
        continue
    path = os.path.join(root, name)
    split = None
    try:
        with zipfile.ZipFile(path) as z:
            split = axml_manifest_attr(z.read('AndroidManifest.xml'), 'split')
    except Exception:
        pass
    if not split:
        # bundletool names the file after the split it carries.
        split = os.path.splitext(name)[0]
    out.append(f'{split}|{path}')
print(';'.join(out))
PYEOF
)"
    export ANDROID_SPLIT_APKS
    msg "splits: $(printf '%s' "$ANDROID_SPLIT_APKS" | tr ';' '\n' | cut -d'|' -f1 | tr '\n' ' ')"
fi

# The launcher Activity — the package's real entry point, and what the loader
# needs to start an APK from its dex rather than from an engine's exported
# native symbol.  It is the <activity> whose <intent-filter> carries both
# android.intent.action.MAIN and android.intent.category.LAUNCHER; a name
# starting with '.' is relative to the package.
[ -n "$launch_info" ] || launch_info="$(python3 - "$pkgfile" "$pkgname" <<'PYEOF'
import sys, zipfile, struct

def parse(data, pkg):
    if struct.unpack_from('<I', data, 0)[0] != 0x00080003:
        return None
    i, strings = 8, []
    # <activity> currently being walked, and what its intent-filter has said.
    cur_name = None
    cur_target = None
    cur_orientation = None
    cur_theme = None
    activity_orientations = {}
    application_name = None
    application_theme = None
    providers = []
    permissions = []
    query_actions = []
    query_packages = []
    depth_queries = -1
    min_sdk = None
    target_sdk = None
    cur_provider = None
    depth_provider = -1
    launch = None
    depth_activity = -1
    saw_main = saw_launcher = False
    depth = 0
    while i < len(data) - 8:
        chunk_type, header_size, chunk_size = struct.unpack_from('<HHI', data, i)
        if chunk_size == 0:
            break
        if chunk_type == 0x0001:  # STRING_POOL
            str_count, _, flags, strings_start = struct.unpack_from('<IIII', data, i + 8)
            is_utf8 = bool(flags & (1 << 8))
            offsets_base, strings_base = i + header_size, i + strings_start
            for k in range(str_count):
                off = struct.unpack_from('<I', data, offsets_base + k * 4)[0]
                p = strings_base + off
                if is_utf8:
                    s = data[p + 2: p + 2 + data[p + 1]].decode('utf-8', 'replace')
                else:
                    slen = struct.unpack_from('<H', data, p)[0]
                    s = data[p + 2: p + 2 + slen * 2].decode('utf-16-le', 'replace')
                strings.append(s)
        elif chunk_type == 0x0102:  # START_ELEMENT
            depth += 1
            _, name_idx = struct.unpack_from('<ii', data, i + 16)
            attr_start, attr_size, attr_count = struct.unpack_from('<HHH', data, i + 24)
            elem = strings[name_idx] if 0 <= name_idx < len(strings) else ''
            attrs = {}
            base = i + 16 + attr_start
            for a in range(attr_count):
                ao = base + a * attr_size
                _, nm, _, _, _, vt, vd = struct.unpack_from('<iiIHBBI', data, ao)
                aname = strings[nm] if 0 <= nm < len(strings) else ''
                if vt == 0x03 and 0 <= vd < len(strings):
                    attrs[aname] = strings[vd]
                elif vt in (0x01, 0x10, 0x11, 0x12):
                    attrs[aname] = vd
            if elem in ('activity', 'activity-alias'):
                cur_name = attrs.get('name')
                cur_target = attrs.get('targetActivity')
                cur_orientation = attrs.get('screenOrientation')
                cur_theme = attrs.get('theme')
                depth_activity = depth
                saw_main = saw_launcher = False
            elif elem == 'application':
                application_name = attrs.get('name')
                application_theme = attrs.get('theme')
            elif elem == 'queries':
                depth_queries = depth
            elif depth_queries >= 0 and elem == 'package' and attrs.get('name'):
                query_packages.append(str(attrs['name']))
            elif elem == 'provider' and attrs.get('name'):
                provider = attrs['name']
                if provider.startswith('.'):
                    provider = pkg + provider
                cur_provider = [provider, str(attrs.get('authorities', '')), []]
                depth_provider = depth
            elif elem == 'uses-permission' and attrs.get('name'):
                permissions.append(str(attrs['name']))
            elif elem == 'uses-sdk':
                min_sdk = attrs.get('minSdkVersion')
                target_sdk = attrs.get('targetSdkVersion')
            elif elem == 'meta-data' and cur_provider is not None:
                key = attrs.get('name')
                value = attrs.get('resource', attrs.get('value'))
                if key is not None and value is not None:
                    cur_provider[2].append((str(key), str(value)))
            elif elem == 'action' and attrs.get('name'):
                action = str(attrs['name'])
                if depth_queries >= 0:
                    query_actions.append(action)
                if action == 'android.intent.action.MAIN':
                    saw_main = True
            elif elem == 'category' and attrs.get('name') == 'android.intent.category.LAUNCHER':
                saw_launcher = True
        elif chunk_type == 0x0103:  # END_ELEMENT
            if depth == depth_queries:
                depth_queries = -1
            if depth == depth_provider and cur_provider is not None:
                providers.append(cur_provider)
                cur_provider, depth_provider = None, -1
            if depth == depth_activity:
                if saw_main and saw_launcher and cur_name:
                    name = cur_target or cur_name
                    orientation = cur_orientation
                    if orientation is None and cur_target:
                        orientation = activity_orientations.get(cur_target)
                    name = pkg + name if name.startswith('.') else name
                    app = application_name
                    if app and app.startswith('.'):
                        app = pkg + app
                    launch = (name, orientation, app, cur_theme or application_theme)
                if cur_name and cur_orientation is not None:
                    name = pkg + cur_name if cur_name.startswith('.') else cur_name
                    activity_orientations[name] = cur_orientation
                cur_name, cur_target, cur_orientation, cur_theme, depth_activity = None, None, None, None, -1
            depth -= 1
        i += chunk_size
    return launch + (providers, permissions, min_sdk, target_sdk,
                     query_actions, query_packages) if launch else None

try:
    with zipfile.ZipFile(sys.argv[1]) as z:
        result = parse(z.read('AndroidManifest.xml'), sys.argv[2])
    if result:
        name, orientation, application, theme, providers, permissions, min_sdk, target_sdk, query_actions, query_packages = result
        encoded_providers = ';'.join(
            f"{name}|{authority}|" + ','.join(f'{key}~{value}' for key, value in metadata)
            for name, authority, metadata in providers)
        encoded_permissions = ','.join(permissions)
        print(f"{name}|{'' if orientation is None else orientation}|{application or ''}|{theme or ''}|{encoded_providers}#{encoded_permissions}#{min_sdk or ''}#{target_sdk or ''}#{','.join(query_actions)}#{','.join(query_packages)}")
except Exception:
    pass
PYEOF
)"
launch_activity="${launch_info%%|*}"
if [ -n "$launch_activity" ]; then
    export ANDROID_LAUNCH_ACTIVITY="$launch_activity"
    msg "launcher activity: $launch_activity"
fi
launch_rest="${launch_info#*|}"
launch_orientation="${launch_rest%%|*}"
launch_app_rest="${launch_rest#*|}"
launch_application="${launch_app_rest%%|*}"
launch_theme_rest="${launch_app_rest#*|}"
launch_theme="${launch_theme_rest%%|*}"
launch_install_info="${launch_theme_rest#*|}"
launch_providers="${launch_install_info%%#*}"
launch_permissions_rest="${launch_install_info#*#}"
launch_permissions="${launch_permissions_rest%%#*}"
launch_sdk_rest="${launch_permissions_rest#*#}"
launch_min_sdk="${launch_sdk_rest%%#*}"
launch_target_rest="${launch_sdk_rest#*#}"
launch_target_sdk="${launch_target_rest%%#*}"
launch_query_rest="${launch_target_rest#*#}"
launch_query_actions="${launch_query_rest%%#*}"
launch_query_packages="${launch_query_rest#*#}"
case "$launch_orientation" in
    ?*)
        ANDROID_SCREEN_ORIENTATION="$launch_orientation"
        export ANDROID_SCREEN_ORIENTATION
        msg "screen orientation: $ANDROID_SCREEN_ORIENTATION"
        ;;
esac
if [ -n "$launch_application" ] && [ "$launch_application" != "$launch_app_rest" ]; then
    export ANDROID_APPLICATION_CLASS="$launch_application"
    msg "application class: $ANDROID_APPLICATION_CLASS"
fi
case "$launch_theme" in
    ?*)
        export ANDROID_THEME_RESOURCE="$launch_theme"
        msg "theme resource: $ANDROID_THEME_RESOURCE"
        ;;
esac
if [ -n "$launch_providers" ] && [ "$launch_providers" != "$launch_app_rest" ]; then
    export ANDROID_CONTENT_PROVIDERS="$launch_providers"
    msg "content providers: $(printf '%s' "$launch_providers" | tr ';' '\n' | wc -l)"
fi
if [ -n "$launch_permissions" ] && [ "$launch_permissions" != "$launch_install_info" ]; then
    export ANDROID_REQUESTED_PERMISSIONS="$launch_permissions"
    msg "requested permissions: $(printf '%s' "$launch_permissions" | tr ',' '\n' | wc -l)"
fi
case "$launch_min_sdk" in *[!0-9]*|'') ;; *) export ANDROID_MIN_SDK="$launch_min_sdk" ;; esac
case "$launch_target_sdk" in *[!0-9]*|'') ;; *) export ANDROID_TARGET_SDK="$launch_target_sdk" ;; esac
if [ -n "$ANDROID_TARGET_SDK" ]; then
    msg "manifest SDK: min=${ANDROID_MIN_SDK:-unknown} target=$ANDROID_TARGET_SDK"
fi
if [ -n "$launch_query_actions" ] && [ "$launch_query_actions" != "$launch_query_rest" ]; then
    export ANDROID_QUERY_ACTIONS="$launch_query_actions"
fi
if [ -n "$launch_query_packages" ] && [ "$launch_query_packages" != "$launch_query_rest" ]; then
    export ANDROID_QUERY_PACKAGES="$launch_query_packages"
fi

# The window is the device's screen, and the device is the one lunaria.conf
# selects: the emulator turns that panel the way the manifest's
# android:screenOrientation (exported above) asks for, and LUNARIA_SCALE sizes
# it for the host desktop.  There is no per-title table here any more — a list
# of package names deciding the display meant that adding a title was the only
# way to get the right shape, and that every title not on the list got a
# landscape window whether or not it was a landscape game.

# Unity nativeFile receives the byte-for-byte base APK staged above.  Split
# assets remain in the installed-package directory for the AssetManager/open
# bridge; no joined or repacked APK is created.
if [ -z "$ANDROID_APK_FILE" ] || [ ! -f "$ANDROID_APK_FILE" ]; then
    export ANDROID_APK_FILE="$pkgfile"
fi

# persistentDataPath / getExternalFilesDir.
#
# Deliberately outside the package cache.  This is where a title puts the
# content it downloads at runtime, and Cross Worlds downloads 13.7 GB into it.
# While it lived under $cache_dir/inst, anything that rebuilt the cache — a
# newer launcher format, a corrupt tree, an interrupted unpack — silently threw
# all of that away, and the next run started the patcher again from nothing.
# The cache holds what was derived from the APK and can be derived again; this
# holds what only exists because it was downloaded once.
# LUNARIA_DATA_ROOT relocates everything the guest writes.  The default keeps
# it beside the launcher, which is convenient but inherits whatever the source
# tree's path happens to be — and a guest that mishandles a non-ASCII path
# component then fails for a reason that has nothing to do with the guest.
#
# What lives under it is the one thing the launcher cannot recreate: this
# title downloads 13.7 GB the first time it runs, and a run that looks in the
# wrong place downloads the lot again.  That makes it exactly the kind of
# setting lunaria.conf is for, so it is written there — once — rather than
# remembered in a dotfile of the launcher's own.  A memo file that no command
# line mentions is a second, invisible source of truth for the same setting,
# and the two disagree the moment either is edited.
: "${LUNARIA_DATA_ROOT:=$PWD}"
msg "data root: $LUNARIA_DATA_ROOT (LUNARIA_DATA_ROOT in lunaria.conf)"

# The app's directories live at "$LUNARIA_DATA_ROOT/data/<pkg>/…".  They used
# to carry an extra "local/" component, which said nothing — the root itself
# already is the device's storage — and it showed up doubled in every path the
# guest printed.  Rename the old tree once rather than leaving an install
# (tens of gigabytes for some titles) stranded under the old name.  A rename
# inside one filesystem is atomic and cannot lose the tree; if the new name
# already exists the old one is left alone for a human to look at.
if [ -d "$LUNARIA_DATA_ROOT/local/data" ] && [ ! -e "$LUNARIA_DATA_ROOT/data" ]; then
    if mv "$LUNARIA_DATA_ROOT/local/data" "$LUNARIA_DATA_ROOT/data" 2>/dev/null; then
        msg "moved app data to $LUNARIA_DATA_ROOT/data (was .../local/data)"
        rmdir "$LUNARIA_DATA_ROOT/local" 2>/dev/null || :
    fi
fi

# The emulator reads this too: a guest that hands an absolute path back as the
# *child* of a File(parent, child) would otherwise repeat the whole root
# inside the name (see file_init in src/dvm/dvm_runtime.c).
export LUNARIA_DATA_ROOT
export ANDROID_EXTERNAL_FILES_DIR="$LUNARIA_DATA_ROOT/data/$pkgname/external/files"
mkdir -p "$ANDROID_EXTERNAL_FILES_DIR"
# One migration for trees staged by earlier launchers, so an existing install
# is not re-downloaded just because this moved.
if [ -n "$cache_dir" ] && [ -d "$tmpdir/local/files" ] &&
   ! find "$ANDROID_EXTERNAL_FILES_DIR" -mindepth 1 -maxdepth 1 2>/dev/null | grep -q .; then
    msg "moving staged external files out of the package cache"
    (cd "$tmpdir/local/files" && tar cf - .) | (cd "$ANDROID_EXTERNAL_FILES_DIR" && tar xf -) \
        && rm -rf "$tmpdir/local/files"
fi
export ANDROID_EXTERNAL_OBB_DIR="$LUNARIA_DATA_ROOT/data/$pkgname/obb"
mkdir -p "$ANDROID_EXTERNAL_OBB_DIR"

# Android's credential-protected application data.  Keep this outside the
# transient APK extraction tree: databases/preferences/files survive process
# restarts on a device and framework code (notably Room/WorkManager) relies on
# the directories being distinct from the APK code path.
export ANDROID_FILES_DIR="$LUNARIA_DATA_ROOT/data/$pkgname/files"
export ANDROID_CACHE_DIR="$LUNARIA_DATA_ROOT/data/$pkgname/cache"
export ANDROID_CODE_CACHE_DIR="$LUNARIA_DATA_ROOT/data/$pkgname/code_cache"
export ANDROID_DATABASES_DIR="$LUNARIA_DATA_ROOT/data/$pkgname/databases"
export ANDROID_NO_BACKUP_DIR="$LUNARIA_DATA_ROOT/data/$pkgname/no_backup"
# SharedPreferences, in the same place and the same XML a device keeps them in.
# They persist across launches: an SDK that writes the account it just created
# here has to find it again next time, or every launch is a new user.
export ANDROID_PREFS_DIR="$LUNARIA_DATA_ROOT/data/$pkgname/shared_prefs"
mkdir -p "$ANDROID_FILES_DIR" "$ANDROID_CACHE_DIR" "$ANDROID_CODE_CACHE_DIR" \
    "$ANDROID_DATABASES_DIR" \
    "$ANDROID_NO_BACKUP_DIR" "$ANDROID_PREFS_DIR"

# Expansion files (.obb).  UE4 ships all game content (Content/Paks/*.pak) in
# main.<ver>.<pkg>.obb, which Google Play installs next to the APK — it is NOT
# inside the APK.  Without it the engine finds no project and PreInit fails
# ("Project file not found" → LaunchAndroid.cpp assert).  Pick up an .obb
# sitting beside the APK and expose it both under the conventional on-device
# path and via ANDROID_OBB_MAIN/PATCH (handed to nativeSetObbFilePaths).
# Only accept package-scoped OBB names (main.<ver>.<pkg>.obb).  A bare
# main.*.obb fallback would steal a neighbour title's expansion when several
# APKs share a directory — Blade & Soul sits next to FPSMobile's OBB in
# test/, and the wrong file then blocks assets/main.obb.png below.
apkdir="$(dirname "$pkgfile")"
for _obb in "$apkdir"/main.*."$pkgname".obb; do
    [ -f "$_obb" ] || continue
    export ANDROID_OBB_MAIN="$_obb"
    break
done
for _obb in "$apkdir"/patch.*."$pkgname".obb; do
    [ -f "$_obb" ] || continue
    export ANDROID_OBB_PATCH="$_obb"
    break
done

# Play Asset Delivery/XAPK packages commonly put the expansion zip in a split
# APK as assets/main.obb.png.  It is still an ordinary, byte-for-byte OBB zip;
# the .png suffix merely keeps bundle tooling from treating it specially.  The
# split has already been overlaid into the temporary installed-package view, so
# expose that exact file to UE's nativeSetObbFilePaths instead of claiming that
# the base APK itself contains the OBB.  This neither repacks nor patches any
# package content.
if [ -z "$ANDROID_OBB_MAIN" ] && [ -f "$tmpdir/assets/main.obb.png" ]; then
    export ANDROID_OBB_MAIN="$tmpdir/assets/main.obb.png"
fi
if [ -z "$ANDROID_OBB_PATCH" ] && [ -f "$tmpdir/assets/patch.obb.png" ]; then
    export ANDROID_OBB_PATCH="$tmpdir/assets/patch.obb.png"
fi
if [ -n "$ANDROID_OBB_MAIN" ]; then
    # /sdcard/Android/obb/<pkg>/ layout, for the engine's default search path.
    # UE looks for main.<version>.<package>.obb (version comes from
    # nativeSetObbInfo; lunaria passes 1).  Also keep the raw basename link.
    _obbdir="$ANDROID_EXTERNAL_FILES_DIR/Android/obb/$pkgname"
    mkdir -p "$_obbdir"
    ln -sf "$ANDROID_OBB_MAIN" "$_obbdir/$(basename "$ANDROID_OBB_MAIN")"
    ln -sf "$ANDROID_OBB_MAIN" "$_obbdir/main.1.$pkgname.obb"
    ln -sf "$ANDROID_OBB_MAIN" "$ANDROID_EXTERNAL_OBB_DIR/$(basename "$ANDROID_OBB_MAIN")"
    ln -sf "$ANDROID_OBB_MAIN" "$ANDROID_EXTERNAL_OBB_DIR/main.1.$pkgname.obb"
    if [ -n "$ANDROID_OBB_PATCH" ]; then
        ln -sf "$ANDROID_OBB_PATCH" "$_obbdir/$(basename "$ANDROID_OBB_PATCH")"
        ln -sf "$ANDROID_OBB_PATCH" "$_obbdir/patch.1.$pkgname.obb"
    fi
    # Nothing is extracted.  A device never unpacks the expansion: UE's
    # FAndroidPlatformFile opens main.<ver>.<pkg>.obb (or the APK asset
    # main.obb.png) as a zip and mounts Content/Paks/*.pak from inside it, and
    # decrypts the pak indexes itself with the key compiled into the engine.
    # The external-files tree <EngineDir>/<Project>/ belongs to the game
    # (Saved/, downloaded patches).  Loose copies staged there once used to
    # shadow the package forever: after an app update the engine kept mounting
    # the previous base pak and its Config/DefaultGame.ini AppVersion, and the
    # server answered "requires an update".
    msg "obb: $ANDROID_OBB_MAIN"
fi

# Mono assembly search path: without this, mono_assembly_load_corlib's
# load_in_path() iterates over an empty search list and returns NULL with
# status OK, tripping g_assert_not_reached() at domain.c:1254 → exit(1) every
# frame (black screen).  Point MONO_PATH at the Managed dir (and mono/2.0).
if [ -d "$managed_dir" ]; then
    export MONO_PATH="$managed_dir:$managed_dir/mono/2.0"
    mono_cfg="$tmpdir/mono-etc"
    mkdir -p "$mono_cfg/mono"
    printf '%s\n' '<configuration></configuration>' > "$mono_cfg/mono/config"
    export MONO_CFG_DIR="$mono_cfg"
    export MONO_CONFIG="$mono_cfg/mono/config"
fi

export LD_LIBRARY_PATH="$script_dir:$script_dir/runtime${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ "$(uname -s)" = Darwin ]; then
    export DYLD_LIBRARY_PATH="$script_dir:$script_dir/runtime${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
fi

# Run the package's real launcher Activity from dex by default.  Set
# LUNARIA_DEX_START=0 only when comparing against the legacy, engine-specific
# native startup path.
# Unreal saves the scalability group it resolved, and one of them decides how
# big the 3D scene is drawn.  sg.ResolutionQuality is a percentage; 0 is not a
# percentage, it is "nothing ever set me", and UE does not treat it as 100 --
# FLegacyScreenPercentageDriver clamps the fraction to its floor of 0.1, so the
# world is rendered at a tenth of the surface on each axis and stretched back
# up.  Measured on Cross Worlds: a 1024x576 surface drew its scene into
# 103x58.
#
# It reads 0 because the device profile UE selected does not set it.  The game
# ships Nk.ResolutionQuality in Android_Low/Mid/High/Ultra (70/80/90/100) and
# in the per-GPU profiles that inherit them -- Android_Mali_G78 inherits
# Android_Ultra -- but *not* in the bare [Android DeviceProfile] it falls back
# to when no rule matches.  So this line means the emulator did not give UE
# enough to recognise the device, which is an emulator bug and not a setting
# the operator should have to fix.
#
# Say so at launch rather than leaving it to be discovered as "the graphics
# look soft": the file is read before the run, so the warning costs nothing.
_gus="$LUNARIA_DATA_ROOT/data/$pkgname/external/files/UE4Game/ProjectN/ProjectN/Saved/Config/Android/GameUserSettings.ini"
if [ -f "$_gus" ]; then
    _rq=$(sed -n 's/^sg\.ResolutionQuality=\([0-9.]*\).*/\1/p' "$_gus" | head -n 1)
    case "$_rq" in
        ''|0|0.*|[0-9].0*)
            msg "warning: sg.ResolutionQuality=${_rq:-unset} in $_gus"
            msg "warning: UE will clamp the scene to its 10% floor and upscale it."
            msg "warning: the device profile did not match — UE fell back to the bare"
            msg "warning: [Android DeviceProfile], which sets no Nk.ResolutionQuality."
            ;;
    esac
fi

: "${LUNARIA_DEX_START:=1}"
export LUNARIA_DEX_START

# Standard APK: lib/$arch/  or  App Bundle split APK: base/lib/$arch/
libdir="$tmpdir/lib/$arch"
[ -d "$libdir" ] || libdir="$tmpdir/base/lib/$arch"
[ -d "$libdir" ] || err "no lib/$arch found in APK (tried lib/ and base/lib/)"

# ApplicationInfo.nativeLibraryDir is set on every installed package, whatever
# started the process.  Exporting it only on the bytecode-startup path below
# left it empty for engine-native titles, so a guest that builds a library
# path out of it -- NMSS opens nativeLibraryDir + "/libnmsssa.so" -- asked for
# "/libnmsssa.so" and got nothing.
export ANDROID_NATIVE_LIB_DIR="$libdir"

# Main native library: Unity → libunity.so, UE4 → libUE4.so/libUnreal.so, else first lib*.so
main_so=""
for cand in libunity.so libUE4.so libUnreal.so libmain.so; do
    if [ -f "$libdir/$cand" ]; then
        main_so="$libdir/$cand"
        break
    fi
done
if [ -z "$main_so" ]; then
    # No engine-native entry point: Android starts the manifest Activity in
    # bytecode and lets System.loadLibrary() load JNI code in application
    # order.  Choosing the first lib*.so is both unordered and incorrect.
    case "$arch" in
        arm64-v8a) main_so="--apk-process-arm64" ;;
        armeabi-v7a) main_so="--apk-process-arm32" ;;
    esac
fi
[ -n "$main_so" ] || err "no startup path for $arch"
case "$main_so" in
    --apk-process-*) : ;;
    *) [ -f "$main_so" ] || err "no main native library in $libdir" ;;
esac
msg "main lib: $main_so"

# The package is fully unpacked and staged: record what was derived from it and
# mark the tree usable.  The marker goes last, so a run killed part-way through
# leaves a tree that the next run rebuilds instead of trusting.
if [ -n "$cache_dir" ] && { [ "$cache_hit" -eq 0 ] || [ "$_need_write_derived" -eq 1 ]; }; then
    # These values carry spaces, '|' and ';' (the provider list is one long
    # field), so each has to come back out of the file as a single word.
    python3 - "$arch" "$pkgname" "$launch_info" > "$derived" <<'PYEOF'
import shlex, sys
print("derived_schema='3'")
for key, value in zip(('arch', 'pkgname', 'launch_info'), sys.argv[1:]):
    print(f'{key}={shlex.quote(value)}')
PYEOF
    # xapk_splits is deliberately not cached: it is one file name per line
    # and the manifest read that produces it is already cheap.
    if [ "$cache_hit" -eq 0 ]; then
        # Only stamp a tree the whole of setup actually produced.  derived.env
        # is written by the python above and is the last thing staging makes,
        # so an empty one means a step before it failed — and a stamped broken
        # tree is worse than no cache at all, because it never rebuilds.
        [ -s "$derived" ] \
            || err "staging did not complete; not caching $cache_dir"
        : > "$cache_dir/.ready"
        msg "package cached: $cache_dir"
    else
        msg "package metadata re-cached: $cache_dir"
    fi
fi

lunaria_bin="${LUNARIA_BIN:-$script_dir/lunaria}"

# Cold-start JIT can spend a long stretch translating with a blank window.
# The luna-ui progress card is on by default; LUNARIA_JIT_UI=0 turns it off.
: "${LUNARIA_JIT_UI:=1}"
export LUNARIA_JIT_UI

# Real AArch64 platform libraries for the guest, if `make syslib` fetched them.
# Every symbol answered from one of these runs as guest code instead of leaving
# the JIT for an SVC; libm alone was 40% of the guest's exits.  Unset when the
# directory is not there, which puts those symbols back on the SVC bridge.
if [ -z "${LUNARIA_SYSLIB_DIR:-}" ] && [ -d "$script_dir/syslib-arm64" ]; then
    LUNARIA_SYSLIB_DIR="$script_dir/syslib-arm64"
    export LUNARIA_SYSLIB_DIR
fi
# Without a guest libm, pow/sincosf and the rest leave the JIT for an SVC on
# every call — 40% of Cross Worlds' exits during the post-title load, which is
# the single biggest reason that stretch is slow.  It is a one-time `make
# syslib`, so say plainly that it is missing rather than running slow silently.
if [ -z "${LUNARIA_SYSLIB_DIR:-}" ] && [ "$arch" = arm64-v8a ]; then
    msg "no syslib-arm64/ — guest libm (pow, sincosf, ...) will run as SVC thunks."
    msg "run 'make syslib' once for a real AArch64 libm; the post-title load is ~40% JIT exits without it."
fi

# A signal aimed at this script (Ctrl-C, `timeout`) has to reach the emulator.
# Run it in the background and forward, rather than leaving an orphan behind:
# a killed launcher used to leave the emulator running, and the next launch
# then competed with it for the CPU — which quietly ruins any timing anyone
# measures afterwards.  (It only appeared to stop before because the exit trap
# deleted the tree out from under it.)
"$lunaria_bin" "$main_so" &
lunaria_pid=$!
if [ -n "$cache_dir" ]; then
    trap 'kill "$lunaria_pid" 2>/dev/null' EXIT INT TERM
else
    trap 'kill "$lunaria_pid" 2>/dev/null; rm -rf "$tmpdir"; [ -z "$xapk_dir" ] || rm -rf "$xapk_dir"' EXIT INT TERM
fi
wait "$lunaria_pid"
