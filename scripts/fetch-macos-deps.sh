#!/bin/sh
# Fetch the small source/header-only pieces the macOS host build needs.
# GLES 3.1 titles use ANGLE's Vulkan backend over the official MoltenVK
# private-API build.  The ANGLE bundled with Android Emulator 34.2.13 (and
# still bundled with 36.6.11) aborts in its Vulkan loader on MoltenVK before
# eglInitialize returns, and does not implement GL_EXT_texture_buffer.  Pin a
# newer standalone ANGLE build on Apple Silicon; callers can override it with
# ANGLE_LIB_DIR.  Copy dylibs locally because external applications may update.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
deps="$root/.deps"
glfw="$deps/glfw"
khr="$deps/khronos/include"
linux_abi="$deps/linux-abi"
mkdir -p "$deps" "$khr/EGL" "$khr/GLES2" "$khr/GLES3" "$khr/KHR" "$linux_abi"

# Homebrew has no bottle on older Tier-3 macOS releases.  A fixed official
# universal CMake keeps a clean Mac build reproducible without compiling the
# build system itself.
mkdir -p "$deps/bin"
if command -v cmake >/dev/null 2>&1; then
    ln -sfn "$(command -v cmake)" "$deps/bin/cmake"
elif [ ! -x "$deps/cmake/CMake.app/Contents/bin/cmake" ]; then
    archive=$(mktemp "${TMPDIR:-/tmp}/lunaria-cmake.XXXXXX.tar.gz")
    trap 'rm -f "$archive"' EXIT HUP INT TERM
    curl --fail --location --silent --show-error \
        https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-macos-universal.tar.gz \
        --output "$archive"
    mkdir -p "$deps/cmake"
    tar -xzf "$archive" -C "$deps/cmake" --strip-components=1
    rm -f "$archive"
    trap - EXIT HUP INT TERM
fi
[ -e "$deps/bin/cmake" ] || \
    ln -sfn "$deps/cmake/CMake.app/Contents/bin/cmake" "$deps/bin/cmake"

# Dynarmic uses Boost.ICL/Spirit/Variant as headers only.  Keep the dependency
# local so a Mac without a Homebrew bottle can still build from a clean tree.
if [ ! -f "$deps/boost/boost/version.hpp" ]; then
    archive=$(mktemp "${TMPDIR:-/tmp}/lunaria-boost.XXXXXX.tar.bz2")
    trap 'rm -f "$archive"' EXIT HUP INT TERM
    curl --fail --location --silent --show-error \
        https://archives.boost.io/release/1.85.0/source/boost_1_85_0.tar.bz2 \
        --output "$archive"
    mkdir -p "$deps/boost"
    tar -xjf "$archive" -C "$deps/boost" --strip-components=1
    rm -f "$archive"
    trap - EXIT HUP INT TERM
fi

if [ ! -f "$glfw/include/GLFW/glfw3.h" ]; then
    archive=$(mktemp "${TMPDIR:-/tmp}/lunaria-glfw.XXXXXX.tar.gz")
    trap 'rm -f "$archive"' EXIT HUP INT TERM
    curl --fail --location --silent --show-error \
        https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz \
        --output "$archive"
    mkdir -p "$glfw"
    tar -xzf "$archive" -C "$glfw" --strip-components=1
    rm -f "$archive"
    trap - EXIT HUP INT TERM
fi

fetch_header() {
    url=$1
    out=$2
    [ -s "$out" ] || curl --fail --location --silent --show-error "$url" --output "$out"
}

egl=https://raw.githubusercontent.com/KhronosGroup/EGL-Registry/main/api
gl=https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/api
fetch_header "$egl/EGL/egl.h"              "$khr/EGL/egl.h"
fetch_header "$egl/EGL/eglext.h"           "$khr/EGL/eglext.h"
fetch_header "https://raw.githubusercontent.com/google/angle/main/include/EGL/eglext_angle.h" \
             "$khr/EGL/eglext_angle.h"
fetch_header "$egl/EGL/eglplatform.h"      "$khr/EGL/eglplatform.h"
fetch_header "$gl/GLES2/gl2.h"             "$khr/GLES2/gl2.h"
fetch_header "$gl/GLES2/gl2ext.h"          "$khr/GLES2/gl2ext.h"
fetch_header "$gl/GLES2/gl2platform.h"     "$khr/GLES2/gl2platform.h"
fetch_header "$gl/GLES3/gl3.h"             "$khr/GLES3/gl3.h"
fetch_header "$gl/GLES3/gl3platform.h"     "$khr/GLES3/gl3platform.h"
fetch_header "$egl/KHR/khrplatform.h"      "$khr/KHR/khrplatform.h"
# macOS deliberately has no <elf.h>, but Lunaria loads Android ELF images.
# Use musl's self-contained ABI header; this is target metadata, not a Linux
# host dependency.
fetch_header "https://git.musl-libc.org/cgit/musl/plain/include/elf.h" \
             "$linux_abi/elf.h"
fetch_header "https://git.musl-libc.org/cgit/musl/plain/include/link.h" \
             "$linux_abi/link.h"
# The Vulkan bridge includes the official C headers even when a title uses
# GLES.  Keep the header version fixed with the other local build inputs.
if [ ! -f "$deps/vulkan/include/vulkan/vulkan.h" ]; then
    archive=$(mktemp "${TMPDIR:-/tmp}/lunaria-vulkan-headers.XXXXXX.tar.gz")
    trap 'rm -f "$archive"' EXIT HUP INT TERM
    curl --fail --location --silent --show-error \
        https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.330.tar.gz \
        --output "$archive"
    mkdir -p "$deps/vulkan"
    tar -xzf "$archive" -C "$deps/vulkan" --strip-components=1 \
        Vulkan-Headers-1.4.330/include
    rm -f "$archive"
    trap - EXIT HUP INT TERM
fi
angle=${ANGLE_LIB_DIR:-}
if [ -z "$angle" ] && [ "$(uname -m)" = arm64 ]; then
    angle_version=1.0.16
    angle="$deps/angle-$angle_version"
    if [ ! -f "$angle/libEGL.dylib" ] || [ ! -f "$angle/libGLESv2.dylib" ]; then
        archive=$(mktemp "${TMPDIR:-/tmp}/lunaria-angle.XXXXXX.tar.gz")
        trap 'rm -f "$archive"' EXIT HUP INT TERM
        curl --fail --location --silent --show-error \
            "https://github.com/startergo/homebrew-angle/releases/download/v${angle_version}/angle-${angle_version}.arm64_sequoia.bottle.tar.gz" \
            --output "$archive"
        expected=29fe2175b157a65f12879f9a12b5c8f94d0a76fafdf41ff009a2fdb4e9df525c
        actual=$(shasum -a 256 "$archive" | cut -d ' ' -f 1)
        [ "$actual" = "$expected" ] || { echo "ANGLE archive checksum mismatch" >&2; exit 1; }
        mkdir -p "$angle"
        tar -xzf "$archive" -C "$angle" --strip-components=3 \
            "angle/$angle_version/lib/libEGL.dylib" \
            "angle/$angle_version/lib/libGLESv2.dylib"
        rm -f "$archive"
        trap - EXIT HUP INT TERM
    fi
fi
if [ -z "$angle" ]; then
    angle=$(find "/Applications/Google Chrome.app/Contents/Frameworks" \
                  "/Applications/ChatGPT.app/Contents/Frameworks" \
                  -type f -name libEGL.dylib -print 2>/dev/null | head -n 1 || true)
    [ -z "$angle" ] || angle=$(dirname "$angle")
fi
if [ -z "$angle" ]; then
    for candidate in "${ANDROID_HOME:-}/emulator/lib64/gles_angle" \
                     "${ANDROID_SDK_ROOT:-}/emulator/lib64/gles_angle" \
                     "$HOME/Library/Android/sdk/emulator/lib64/gles_angle" \
                     "$HOME/Android/Sdk/emulator/lib64/gles_angle"; do
        if [ -f "$candidate/libEGL.dylib" ] && \
           [ -f "$candidate/libGLESv2.dylib" ]; then
            angle=$candidate
            break
        fi
    done
fi
mkdir -p "$deps/lib"
if [ -z "$angle" ] || [ ! -f "$angle/libEGL.dylib" ] || \
   [ ! -f "$angle/libGLESv2.dylib" ]; then
    if [ -f "$deps/lib/libEGL.dylib" ] && \
       [ -f "$deps/lib/libGLESv2.dylib" ]; then
        angle="$deps/lib" # reuse the already installed local copies
    else
        echo "ANGLE not found; set ANGLE_LIB_DIR to a directory containing libEGL.dylib and libGLESv2.dylib" >&2
        exit 1
    fi
fi
angle_stamp="$deps/lib/.angle-source"
old_angle=$(sed -n '1p' "$angle_stamp" 2>/dev/null || true)
if [ "$angle" != "$deps/lib" ] && \
   { [ "$old_angle" != "$angle" ] || [ -L "$deps/lib/libEGL.dylib" ] || \
     [ ! -f "$deps/lib/libEGL.dylib" ] || [ ! -f "$deps/lib/libGLESv2.dylib" ]; }; then
    rm -f "$deps/lib/libEGL.dylib" "$deps/lib/libGLESv2.dylib"
    cp "$angle/libEGL.dylib" "$deps/lib/libEGL.dylib"
    cp "$angle/libGLESv2.dylib" "$deps/lib/libGLESv2.dylib"
    chmod u+w "$deps/lib/libEGL.dylib" "$deps/lib/libGLESv2.dylib"
    install_name_tool -id @rpath/libEGL.dylib "$deps/lib/libEGL.dylib"
    install_name_tool -id @rpath/libGLESv2.dylib "$deps/lib/libGLESv2.dylib"
    codesign --force --sign - --timestamp=none "$deps/lib/libEGL.dylib"
    codesign --force --sign - --timestamp=none "$deps/lib/libGLESv2.dylib"
    printf '%s\n' "$angle" > "$angle_stamp"
fi

# ANGLE's Vulkan backend is the hardware-accelerated GLES 3.1 path on macOS.
# Pin the Khronos release so a clean build cannot silently change driver
# behaviour.  The standard MoltenVK dylib deliberately hides several Metal
# capabilities; ANGLE then correctly exposes only GLES 2.0.  Khronos ships the
# private-API variant specifically to expose those capabilities on macOS.
moltenvk_version=1.4.2
moltenvk_stamp="$deps/lib/.moltenvk-version"
old_moltenvk=$(sed -n '1p' "$moltenvk_stamp" 2>/dev/null || true)
if [ "$old_moltenvk" != "$moltenvk_version-privateapi" ] || \
   [ ! -f "$deps/lib/libMoltenVK.dylib" ] || \
   [ ! -f "$deps/moltenvk/include/vulkan/vulkan.h" ] || \
   [ ! -f "$deps/moltenvk/include/vk_video/vulkan_video_codecs_common.h" ]; then
    archive=$(mktemp "${TMPDIR:-/tmp}/lunaria-moltenvk.XXXXXX.tar")
    trap 'rm -f "$archive"' EXIT HUP INT TERM
    if [ -n "${LUNARIA_MOLTENVK_ARCHIVE:-}" ]; then
        cp "$LUNARIA_MOLTENVK_ARCHIVE" "$archive"
    else
        curl --fail --location --silent --show-error \
            "https://github.com/KhronosGroup/MoltenVK/releases/download/v${moltenvk_version}/MoltenVK-macos-privateapi.tar" \
            --output "$archive"
    fi
    mkdir -p "$deps/moltenvk"
    tar -xf "$archive" -C "$deps/moltenvk" --strip-components=5 \
        MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib
    tar -xf "$archive" -C "$deps/moltenvk" --strip-components=2 \
        MoltenVK/MoltenVK/include/vulkan \
        MoltenVK/MoltenVK/include/vk_video
    cp "$deps/moltenvk/libMoltenVK.dylib" "$deps/lib/libMoltenVK.dylib"
    install_name_tool -id @rpath/libMoltenVK.dylib "$deps/lib/libMoltenVK.dylib"
    codesign --force --sign - --timestamp=none "$deps/lib/libMoltenVK.dylib"
    printf '%s\n' "$moltenvk_version-privateapi" > "$moltenvk_stamp"
    rm -f "$archive"
    trap - EXIT HUP INT TERM
fi

# ANGLE's Vulkan loader discovers a driver through an ICD manifest.  Merely
# placing libMoltenVK beside libEGL leaves a Vulkan-enabled ANGLE falling back
# to Metal (and therefore GLES 3.0) on hosts without a system Vulkan SDK.
python3 - "$deps/lib/libMoltenVK.dylib" "$deps/lib/moltenvk_icd.json" <<'PYEOF'
import json, sys
library, manifest = sys.argv[1:]
with open(manifest, 'w', encoding='utf-8') as out:
    json.dump({'file_format_version': '1.0.0',
               'ICD': {'library_path': library, 'api_version': '1.3.0'}}, out)
    out.write('\n')
PYEOF

printf '%s\n' "macOS dependencies ready" "  GLFW:  $glfw" \
    "  headers: $khr" "  Vulkan: $deps/vulkan/include" "  ELF ABI: $linux_abi/elf.h" \
    "  CMake: $deps/bin/cmake" "  Boost: $deps/boost" "  ANGLE: $angle" \
    "  MoltenVK: $moltenvk_version-privateapi"
