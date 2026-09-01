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
    _cache_format=5
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
        if [ "$cache_hit" -eq 0 ]; then
            mkdir -p "$xapk_dir"
            unzip -q "$inputfile" -d "$xapk_dir" || err "extract xapk failed"
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
        if [ "$cache_hit" -eq 0 ]; then
            mkdir -p "$xapk_dir"
            unzip -q "$inputfile" -d "$xapk_dir" || err "extract apks failed"
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
    if grep -q "^launch_info='" "$derived" 2>/dev/null || \
       ! grep -q "^launch_info=" "$derived" 2>/dev/null; then
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
            elif elem == 'provider' and attrs.get('name'):
                provider = attrs['name']
                if provider.startswith('.'):
                    provider = pkg + provider
                cur_provider = [provider, str(attrs.get('authorities', '')), []]
                depth_provider = depth
            elif elem == 'uses-permission' and attrs.get('name'):
                permissions.append(str(attrs['name']))
            elif elem == 'meta-data' and cur_provider is not None:
                key = attrs.get('name')
                value = attrs.get('resource', attrs.get('value'))
                if key is not None and value is not None:
                    cur_provider[2].append((str(key), str(value)))
            elif elem == 'action' and attrs.get('name') == 'android.intent.action.MAIN':
                saw_main = True
            elif elem == 'category' and attrs.get('name') == 'android.intent.category.LAUNCHER':
                saw_launcher = True
        elif chunk_type == 0x0103:  # END_ELEMENT
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
    return launch + (providers, permissions) if launch else None

try:
    with zipfile.ZipFile(sys.argv[1]) as z:
        result = parse(z.read('AndroidManifest.xml'), sys.argv[2])
    if result:
        name, orientation, application, theme, providers, permissions = result
        encoded_providers = ';'.join(
            f"{name}|{authority}|" + ','.join(f'{key}~{value}' for key, value in metadata)
            for name, authority, metadata in providers)
        encoded_permissions = ','.join(permissions)
        print(f"{name}|{'' if orientation is None else orientation}|{application or ''}|{theme or ''}|{encoded_providers}#{encoded_permissions}")
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
launch_permissions="${launch_install_info#*#}"
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

# Portrait / landscape defaults for known titles (override with LUNARIA_WIDTH/HEIGHT)
case "$pkgname" in
    org.gekoi.timelocker)
        : "${LUNARIA_WIDTH:=720}"
        : "${LUNARIA_HEIGHT:=1280}"
        export LUNARIA_WIDTH LUNARIA_HEIGHT
        ;;
    com.YourCompany.FPSMobile)
        : "${LUNARIA_WIDTH:=1280}"
        : "${LUNARIA_HEIGHT:=720}"
        export LUNARIA_WIDTH LUNARIA_HEIGHT
        ;;
esac

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
# It is also remembered.  What lives under it is the one thing the launcher
# cannot recreate: this title downloads 13.7 GB the first time it runs, and a
# run that forgets where that went does not find it — it downloads the lot
# again.  So the first run that names a root writes it down, and every later
# run picks it up without being told.  Pass a new one to move; pass the
# default explicitly (LUNARIA_DATA_ROOT="$PWD") to go back.
_root_memo="$script_dir/.lunaria-data-root"
if [ -n "$LUNARIA_DATA_ROOT" ]; then
    printf '%s\n' "$LUNARIA_DATA_ROOT" > "$_root_memo" 2>/dev/null || :
elif [ -r "$_root_memo" ]; then
    LUNARIA_DATA_ROOT="$(cat "$_root_memo")"
    [ -d "$LUNARIA_DATA_ROOT" ] || LUNARIA_DATA_ROOT=""
    [ -n "$LUNARIA_DATA_ROOT" ] && msg "data root (remembered): $LUNARIA_DATA_ROOT"
fi
: "${LUNARIA_DATA_ROOT:=$PWD}"
export ANDROID_EXTERNAL_FILES_DIR="$LUNARIA_DATA_ROOT/local/data/$pkgname/external/files"
mkdir -p "$ANDROID_EXTERNAL_FILES_DIR"
# One migration for trees staged by earlier launchers, so an existing install
# is not re-downloaded just because this moved.
if [ -n "$cache_dir" ] && [ -d "$tmpdir/local/files" ] &&
   ! find "$ANDROID_EXTERNAL_FILES_DIR" -mindepth 1 -maxdepth 1 2>/dev/null | grep -q .; then
    msg "moving staged external files out of the package cache"
    (cd "$tmpdir/local/files" && tar cf - .) | (cd "$ANDROID_EXTERNAL_FILES_DIR" && tar xf -) \
        && rm -rf "$tmpdir/local/files"
fi
export ANDROID_EXTERNAL_OBB_DIR="$LUNARIA_DATA_ROOT/local/data/$pkgname/obb"
mkdir -p "$ANDROID_EXTERNAL_OBB_DIR"

# Android's credential-protected application data.  Keep this outside the
# transient APK extraction tree: databases/preferences/files survive process
# restarts on a device and framework code (notably Room/WorkManager) relies on
# the directories being distinct from the APK code path.
export ANDROID_FILES_DIR="$LUNARIA_DATA_ROOT/local/data/$pkgname/files"
export ANDROID_CACHE_DIR="$LUNARIA_DATA_ROOT/local/data/$pkgname/cache"
export ANDROID_CODE_CACHE_DIR="$LUNARIA_DATA_ROOT/local/data/$pkgname/code_cache"
export ANDROID_DATABASES_DIR="$LUNARIA_DATA_ROOT/local/data/$pkgname/databases"
export ANDROID_NO_BACKUP_DIR="$LUNARIA_DATA_ROOT/local/data/$pkgname/no_backup"
# SharedPreferences, in the same place and the same XML a device keeps them in.
# They persist across launches: an SDK that writes the account it just created
# here has to find it again next time, or every launch is a new user.
export ANDROID_PREFS_DIR="$LUNARIA_DATA_ROOT/local/data/$pkgname/shared_prefs"
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
    # Main engine library — needed both to pick the staging root below and to
    # pull the pak AES key later.  UE4 ships libUE4.so, UE5 libUnreal.so.
    _ue_so=""
    for _cand in "$tmpdir/lib/$arch/libUE4.so"   "$tmpdir/lib/$arch/libUnreal.so" \
                 "$tmpdir/lib/arm64-v8a/libUE4.so"   "$tmpdir/lib/arm64-v8a/libUnreal.so" \
                 "$tmpdir/lib/armeabi-v7a/libUE4.so" "$tmpdir/lib/armeabi-v7a/libUnreal.so"; do
        [ -f "$_cand" ] && _ue_so="$_cand" && break
    done

    # Stage OBB under the loose external-files tree.  With obbInAPK or a mounted
    # expansion, UE still probes
    #   <files>/<EngineDir>/<Project>/<Project>/Content/Paks/*.pak
    # <EngineDir> is "UE4Game" up to UE4 and "UnrealGame" from UE5 on
    # (FAndroidPlatformFile builds GFilePathBase + that literal).  Reading the
    # literal out of the engine binary keeps this exact instead of guessing
    # from the library name or a title list.
    # OBB zip layouts vary — discover the project name from Content/Paks,
    # never hard-code a title (a fabricated .uproject / fixed Project name
    # breaks other APKs).
    #   A) <EngineDir>/<P>/<P>/Content/Paks  — already device-shaped
    #   B) <P>/<P>/Content/Paks              — missing <EngineDir>/
    #   C) <P>/Content/Paks                  — missing outer <P>/ (common on Android)
    _ue_dirname="UE4Game"
    if [ -n "$_ue_so" ] && grep -qa -- '/UnrealGame/' "$_ue_so" 2>/dev/null; then
        _ue_dirname="UnrealGame"
    fi
    msg "UE external-files root: $_ue_dirname"
    _ue_game="$ANDROID_EXTERNAL_FILES_DIR/$_ue_dirname"
    mkdir -p "$_ue_game"
    if ! find "$_ue_game" -type d -path '*/Content/Paks' 2>/dev/null | grep -q .; then
        _obb_x="$tmpdir/obb_extract"
        mkdir -p "$_obb_x"
        if unzip -q -o "$ANDROID_OBB_MAIN" -d "$_obb_x"; then
            find "$_obb_x" -type d -path '*/Content/Paks' 2>/dev/null | while read -r _paks; do
                _content=$(dirname "$_paks")
                [ "$(basename "$_content")" = Content ] || continue
                _inner=$(dirname "$_content")
                _proj=$(basename "$_inner")
                [ -n "$_proj" ] && [ "$_proj" != Content ] || continue
                _dest="$_ue_game/$_proj/$_proj"
                if [ -d "$_dest/Content/Paks" ]; then
                    continue
                fi
                mkdir -p "$_ue_game/$_proj"
                if [ -d "$_inner" ]; then
                    # Move project tree into <EngineDir>/<P>/<P>/ (Content + siblings).
                    mv "$_inner" "$_dest" 2>/dev/null \
                        || { mkdir -p "$_dest"; cp -a "$_inner/." "$_dest/"; }
                fi
                # The staged command line often sits beside the project folder in
                # the OBB.  UE4 names it UE4CommandLine.txt, UE5 UECommandLine.txt.
                for _cmdname in UE4CommandLine.txt UECommandLine.txt; do
                    for _cmd in "$_obb_x/$_cmdname" \
                                "$_obb_x/$_proj/$_cmdname" \
                                "$(dirname "$_inner")/$_cmdname"; do
                        if [ -f "$_cmd" ] && [ ! -f "$_ue_game/$_proj/$_cmdname" ]; then
                            cp -f "$_cmd" "$_ue_game/$_proj/$_cmdname"
                        fi
                    done
                done
                # The expansion also carries engine-side staged content next to
                # the project (Engine/Config/StagedBuild_<P>.ini, ICU data,
                # Engine/Content/…).  A mounted OBB exposes all of it under the
                # same root, so move every remaining sibling across instead of
                # stopping at the project folder — otherwise the engine sees a
                # staged build with no Engine/ tree.
                _sib_root="$(dirname "$_inner")"
                for _sib in "$_sib_root"/*; do
                    [ -e "$_sib" ] || continue
                    [ -d "$_sib" ] || continue
                    _sib_name="$(basename "$_sib")"
                    [ "$_sib_name" = "$_proj" ] && continue
                    [ -e "$_ue_game/$_proj/$_sib_name" ] && continue
                    mv "$_sib" "$_ue_game/$_proj/$_sib_name" 2>/dev/null \
                        || cp -a "$_sib" "$_ue_game/$_proj/$_sib_name"
                done
                msg "obb staged under $_ue_game/$_proj"
            done
        else
            msg "obb extract failed (continuing with zip mount only)"
        fi
    fi
    # Encrypted-index paks: PreInit opens .uproject before FPakFile mounts, and
    # ShaderArchive/maps need AES-ECB index decrypt.  Stage the real cooked
    # assets (including the real .uproject from the pak) as loose files —
    # host staging only, no fabricated project descriptor.
    _stage_py="$script_dir/scripts/ue4_stage_encrypted_paks.py"
    # Skip it when the tree it writes into already has its output.  The test
    # used to be "did we hit the package cache", which stopped being the right
    # question once the external-files tree moved out of that cache: a cached
    # package with a freshly created external tree would have skipped the
    # staging that fills it.  Ask the tree itself instead.
    if [ -f "$_ue_game/.staged-encrypted-paks" ]; then
        _stage_py=""
    fi
    if [ -f "$_stage_py" ] && [ -n "$_ue_so" ]; then
        find "$_ue_game" -type d \( -path '*/Content/Paks' -o -path '*/Content/CBPaks' \) \
            2>/dev/null | while read -r _paks; do
            # .../<EngineDir>/<P>/<P>/Content/Paks → out = .../<EngineDir>/<P>
            _out=$(dirname "$(dirname "$(dirname "$_paks")")")
            python3 "$_stage_py" --so "$_ue_so" --paks "$_paks" --out "$_out" \
                || msg "encrypted pak stage failed for $_paks (continuing)"
            : > "$_ue_game/.staged-encrypted-paks"
            # UE Curl HTTPS probes several CA paths (Certificates/cacert.pem,
            # CurlCertificates/ca-bundle.pem, Engine ThirdParty).  Cooked paks
            # often omit them; install the host trust store so CDN config /
            # DownloadContent can proceed (emulator-side only).
            _proj_root="$(dirname "$(dirname "$_paks")")"
            _ue_root="$(dirname "$_proj_root")"
            _host_ca=""
            for _ca in /etc/ssl/certs/ca-certificates.crt \
                       /etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem \
                       /etc/ssl/cert.pem; do
                [ -s "$_ca" ] && _host_ca="$_ca" && break
            done
            if [ -n "$_host_ca" ]; then
                for _dest in \
                    "$_proj_root/Content/Certificates/cacert.pem" \
                    "$_proj_root/Content/CurlCertificates/ca-bundle.pem" \
                    "$_ue_root/Engine/Content/Certificates/ThirdParty/cacert.pem" \
                    "$ANDROID_EXTERNAL_FILES_DIR/ca-bundle.pem"
                do
                    mkdir -p "$(dirname "$_dest")"
                    if [ ! -s "$_dest" ]; then
                        cp -f "$_host_ca" "$_dest"
                        msg "staged host CA → $_dest"
                    fi
                done
            fi
        done
    fi
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
: "${LUNARIA_DEX_START:=1}"
export LUNARIA_DEX_START

# Standard APK: lib/$arch/  or  App Bundle split APK: base/lib/$arch/
libdir="$tmpdir/lib/$arch"
[ -d "$libdir" ] || libdir="$tmpdir/base/lib/$arch"
[ -d "$libdir" ] || err "no lib/$arch found in APK (tried lib/ and base/lib/)"

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
    export ANDROID_NATIVE_LIB_DIR="$libdir"
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
