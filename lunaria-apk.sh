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

[ -z "$1" ] && err 'usage: <apk-or-xapk>'
inputfile="$(realpath "$1")"
pkgfile="$inputfile"
xapk_dir=""
xapk_splits=""

# XAPK is an install container. Keep every contained APK byte-for-byte intact:
# use its base APK as nativeFile and overlay split contents only in the
# temporary installed-package view created below.
case "$inputfile" in
    *.xapk|*.XAPK)
        xapk_dir="$(mktemp -d)"
        unzip -q "$inputfile" -d "$xapk_dir" || err "extract xapk failed"
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
esac

# Prefer arm64-v8a (A64 JIT); fall back to armeabi-v7a (A32 JIT).
# Override with LUNARIA_ARCH=armeabi-v7a|arm64-v8a for comparisons.
if [ -n "$LUNARIA_ARCH" ]; then
    arch="$LUNARIA_ARCH"
elif unzip -l "$pkgfile" 2>/dev/null | grep -q 'lib/arm64-v8a/' ||
     { [ -n "$xapk_splits" ] && printf '%s\n' "$xapk_splits" | while IFS= read -r _s; do
           unzip -l "$xapk_dir/$_s" 2>/dev/null
       done | grep -q 'lib/arm64-v8a/'; }; then
    arch="arm64-v8a"
else
    arch="armeabi-v7a"
fi

pkgname="$(python3 - "$pkgfile" <<'PYEOF'
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
trap 'rm -rf "$tmpdir"; [ -z "$xapk_dir" ] || rm -rf "$xapk_dir"' EXIT
unzip -q "$pkgfile" -d "$tmpdir"
if [ -n "$xapk_splits" ]; then
    printf '%s\n' "$xapk_splits" | while IFS= read -r _split; do
        [ -n "$_split" ] || continue
        unzip -q -n "$xapk_dir/$_split" -d "$tmpdir"
    done
fi

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
    # UE opens ANDROID_APK_FILE with open() after TCHAR↔narrow conversion.
    # Under the guest C locale, non-ASCII path bytes (e.g. 特許 in this
    # workspace) become '?' and the open fails — so the in-APK OBB is never
    # mounted and PreInit dies on the missing .uproject.  Hand the guest an
    # ASCII path by hard-linking (or copying) into the already-ASCII tmpdir.
    _apk_guest="$tmpdir/base.apk"
    ln "$pkgfile" "$_apk_guest" 2>/dev/null || cp -f "$pkgfile" "$_apk_guest" \
        || err "stage apk for guest open failed"
    export ANDROID_APK_FILE="$_apk_guest"
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
    # Stage OBB under the loose UE4Game tree.  With obbInAPK or a mounted
    # expansion, UE still probes
    #   <files>/UE4Game/<Project>/<Project>/Content/Paks/*.pak
    # OBB zip layouts vary — discover the project name from Content/Paks,
    # never hard-code a title (a fabricated .uproject / fixed Project name
    # breaks other APKs).
    #   A) UE4Game/<P>/<P>/Content/Paks  — already device-shaped
    #   B) <P>/<P>/Content/Paks          — missing UE4Game/
    #   C) <P>/Content/Paks              — missing outer <P>/ (common on Android)
    _ue_game="$ANDROID_EXTERNAL_FILES_DIR/UE4Game"
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
                    # Move project tree into UE4Game/<P>/<P>/ (Content + siblings).
                    mv "$_inner" "$_dest" 2>/dev/null \
                        || { mkdir -p "$_dest"; cp -a "$_inner/." "$_dest/"; }
                fi
                # UE4CommandLine.txt often sits beside the project folder in the OBB.
                for _cmd in "$_obb_x/UE4CommandLine.txt" \
                            "$_obb_x/$_proj/UE4CommandLine.txt" \
                            "$(dirname "$_inner")/UE4CommandLine.txt"; do
                    if [ -f "$_cmd" ] && [ ! -f "$_ue_game/$_proj/UE4CommandLine.txt" ]; then
                        cp -f "$_cmd" "$_ue_game/$_proj/UE4CommandLine.txt"
                    fi
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
    _ue_so=""
    for _cand in "$tmpdir/lib/$arch/libUE4.so" "$tmpdir/lib/arm64-v8a/libUE4.so" \
                 "$tmpdir/lib/armeabi-v7a/libUE4.so"; do
        [ -f "$_cand" ] && _ue_so="$_cand" && break
    done
    _stage_py="$(dirname "$argv0")/scripts/ue4_stage_encrypted_paks.py"
    if [ -f "$_stage_py" ] && [ -n "$_ue_so" ]; then
        find "$_ue_game" -type d \( -path '*/Content/Paks' -o -path '*/Content/CBPaks' \) \
            2>/dev/null | while read -r _paks; do
            # .../UE4Game/<P>/<P>/Content/Paks → out = .../UE4Game/<P>
            _out=$(dirname "$(dirname "$(dirname "$_paks")")")
            python3 "$_stage_py" --so "$_ue_so" --paks "$_paks" --out "$_out" \
                || msg "encrypted pak stage failed for $_paks (continuing)"
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
lunaria_bin="${LUNARIA_BIN:-./lunaria}"
"$lunaria_bin" "$main_so"
