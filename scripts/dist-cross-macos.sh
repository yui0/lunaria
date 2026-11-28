#!/usr/bin/env bash
# Cross-build the same arm64 .app using a prepared macOS dependency prefix.
set -euo pipefail
cd "$(dirname "$0")/.."
fail() { echo "macOS cross: $*" >&2; exit 1; }
export MACOS_CROSS=1
source scripts/setup-macos-cross.sh
SDKROOT=$(realpath -e "$SDKROOT")
MAC_OPENSSL=$(realpath -e "$MAC_OPENSSL")
# An explicit prefix supplies prebuilt dependencies. Otherwise build a separate
# dependency tree with the target compilers and the Linux host's CMake.
prepared=${MAC_DEPS:-}
MAC_DEPS=$(realpath -m "${MAC_DEPS:-.deps-cross-macos-arm64}")
export SDKROOT MAC_OPENSSL MAC_DEPS OSXCROSS_SDKROOT="$SDKROOT"
for path in "$SDKROOT/usr/lib/libz.tbd" "$MAC_OPENSSL/include/openssl/ssl.h"; do
    [[ -s "$path" ]] || fail "missing $path"
done
cc=$(command -v "${MAC_CC:-oa64-clang}") || fail 'missing osxcross oa64-clang'
cxx=$(command -v "${MAC_CXX:-oa64-clang++}") || fail 'missing osxcross oa64-clang++'
triple=$("$cc" -dumpmachine)
case "$triple" in arm64-apple-darwin*|aarch64-apple-darwin*) ;; *) fail "unexpected compiler target $triple";; esac
export MAC_OTOOL=${MAC_OTOOL:-$triple-otool}
export MAC_INSTALL_NAME_TOOL=${MAC_INSTALL_NAME_TOOL:-$triple-install_name_tool}
for tool in "$MAC_OTOOL" "$MAC_INSTALL_NAME_TOOL" rcodesign; do
    command -v "$tool" >/dev/null || fail "missing $tool"
done
export MAC_AR=${MAC_AR:-llvm-ar}
command -v "$MAC_AR" >/dev/null || fail "missing $MAC_AR"
if [[ -n "$prepared" ]]; then
    for path in "$MAC_DEPS"/lib/{libglfw3.a,libEGL.dylib,libGLESv2.dylib,libMoltenVK.dylib,libharfbuzz.0.dylib,moltenvk_icd.json}; do
        [[ -s "$path" ]] || fail "missing $path"
    done
fi
[[ ${1:-} != --check ]] || exit 0
# Make's recursive release build retains the command-line target settings.
export CMAKE_TOOLCHAIN_FILE="$(pwd)/scripts/osxcross-arm64.cmake"
export CC="$cc" CXX="$cxx"
export MAC_TARGET_ARCH=arm64 MACOSX_DEPLOYMENT_TARGET=13.0
if [[ -z "$prepared" ]]; then
    sh scripts/fetch-macos-deps.sh
    sh scripts/build-macos-glfw.sh
fi
make -j"$2" dist "DIST_NAME=$1" DIST_ARCH=arm64 LUNA_OS_NAME=Darwin \
    LB_ICU=0 CMAKE=cmake MACOS_CROSS=1 "CC=$cc" "CXX=$cxx" "MAC_SDK=$SDKROOT" \
    "MAC_DEPS=$MAC_DEPS" "MAC_OPENSSL=$MAC_OPENSSL" OPENH264_SO=runtime/libopenh264.dylib
