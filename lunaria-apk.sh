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

[ -z "$1" ] && err 'usage: <apk>'
pkgfile="$(realpath "$1")"

# Prefer arm64-v8a (A64 JIT); fall back to armeabi-v7a (A32 JIT).
# Override with LUNARIA_ARCH=armeabi-v7a|arm64-v8a for comparisons.
if [ -n "$LUNARIA_ARCH" ]; then
    arch="$LUNARIA_ARCH"
elif unzip -l "$1" 2>/dev/null | grep -q 'lib/arm64-v8a/'; then
    arch="arm64-v8a"
else
    arch="armeabi-v7a"
fi

pkgname="$(python3 - "$1" <<'PYEOF'
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

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
unzip "$1" -d "$tmpdir"

# Mono looks for assemblies at <PACKAGE_CODE_PATH>/assets/bin/Data/Managed/mono/2.0/
# Also needs mono/config in the mono/ directory.
# Create symlinks so Mono finds everything via the extracted dir.
managed_dir="$tmpdir/assets/bin/Data/Managed"

# Keep the input APK byte-for-byte intact by default: repacking Play Asset
# Delivery entries changes bundle bytes/order and makes Addressables reject
# otherwise valid content.  Legacy Unity players, however, require the old
# `name.split0..N` convention to be materialised as one regular asset.
has_legacy_splits=0
if find "$tmpdir/assets/bin/Data" -type f -name '*.split[0-9]*' -print -quit 2>/dev/null | grep -q .; then
    python3 - "$tmpdir/assets/bin/Data" <<'PYEOF'
import os, sys
root = sys.argv[1]
joined = 0
for dirpath, _, files in os.walk(root):
    bases = {}
    for name in files:
        base, marker, suffix = name.rpartition('.split')
        if marker and suffix.isdigit():
            bases.setdefault(base, []).append((int(suffix), name))
    for base, parts in bases.items():
        parts.sort()
        if [n for n, _ in parts] != list(range(len(parts))):
            continue
        with open(os.path.join(dirpath, base), 'wb') as out:
            for _, name in parts:
                with open(os.path.join(dirpath, name), 'rb') as part:
                    while chunk := part.read(1 << 20):
                        out.write(chunk)
                os.unlink(os.path.join(dirpath, name))
        joined += 1
print(f"[lunaria-apk] joined {joined} legacy split asset(s)", file=sys.stderr)
PYEOF
    has_legacy_splits=1
fi

# Detecting split files before joining is sufficient: this temporary archive
# is solely for the legacy split-file convention.
if [ "$has_legacy_splits" -eq 1 ]; then
    repacked="$tmpdir/lunaria-legacy-splits.apk"
    ( cd "$tmpdir" && zip -0 -q -r "$repacked" . -x 'lunaria-legacy-splits.apk' ) || err "repack apk failed"
    export ANDROID_APK_FILE="$repacked"
else
    export ANDROID_APK_FILE="$pkgfile"
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

# Unity の nativeFile へ渡す実 APK ファイル。上で split 結合済みの
# lunaria-joined.apk を優先（元 APK は ANDROID 用参照として残さない）。
# 展開ディレクトリは AssetManager ブリッジ/Mono 用に維持。
if [ -z "$ANDROID_APK_FILE" ] || [ ! -f "$ANDROID_APK_FILE" ]; then
    export ANDROID_APK_FILE="$pkgfile"
fi

# persistentDataPath / getExternalFilesDir
export ANDROID_EXTERNAL_FILES_DIR="$tmpdir/local/files"
mkdir -p "$ANDROID_EXTERNAL_FILES_DIR"
export ANDROID_EXTERNAL_OBB_DIR="$PWD/local/data/$pkgname/obb"
mkdir -p "$ANDROID_EXTERNAL_OBB_DIR"

# Expansion files (.obb).  UE4 ships all game content (Content/Paks/*.pak) in
# main.<ver>.<pkg>.obb, which Google Play installs next to the APK — it is NOT
# inside the APK.  Without it the engine finds no project and PreInit fails
# ("Project file not found" → LaunchAndroid.cpp assert).  Pick up an .obb
# sitting beside the APK and expose it both under the conventional on-device
# path and via ANDROID_OBB_MAIN/PATCH (handed to nativeSetObbFilePaths).
apkdir="$(dirname "$pkgfile")"
for _obb in "$apkdir"/main.*."$pkgname".obb "$apkdir"/main.*.obb; do
    [ -f "$_obb" ] || continue
    export ANDROID_OBB_MAIN="$_obb"
    break
done
for _obb in "$apkdir"/patch.*."$pkgname".obb "$apkdir"/patch.*.obb; do
    [ -f "$_obb" ] || continue
    export ANDROID_OBB_PATCH="$_obb"
    break
done
if [ -n "$ANDROID_OBB_MAIN" ]; then
    # /sdcard/Android/obb/<pkg>/ layout, for the engine's default search path
    _obbdir="$ANDROID_EXTERNAL_FILES_DIR/Android/obb/$pkgname"
    mkdir -p "$_obbdir"
    ln -sf "$ANDROID_OBB_MAIN" "$_obbdir/$(basename "$ANDROID_OBB_MAIN")"
    ln -sf "$ANDROID_OBB_MAIN" "$ANDROID_EXTERNAL_OBB_DIR/$(basename "$ANDROID_OBB_MAIN")"
    [ -n "$ANDROID_OBB_PATCH" ] && \
        ln -sf "$ANDROID_OBB_PATCH" "$_obbdir/$(basename "$ANDROID_OBB_PATCH")"
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

export LD_LIBRARY_PATH="$PWD:$PWD/runtime${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

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
    main_so="$(find "$libdir" -maxdepth 1 -name 'lib*.so' ! -name 'libc++_shared.so' | head -1)"
fi
[ -n "$main_so" ] && [ -f "$main_so" ] || err "no main native library in $libdir"
msg "main lib: $main_so"
./lunaria "$main_so"
