#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Linux -> Windows x86_64 UCRT distribution. All paths may be overridden.
set -eu
fail() { echo "dist-cross-win: $*" >&2; exit 1; }
cross=${WIN_CROSS_PREFIX:-x86_64-w64-mingw32-}
for tool in "${cross}gcc" "${cross}g++" "${cross}objdump" cmake ninja python3 zip curl bunzip2; do
    command -v "$tool" >/dev/null || fail "missing $tool"
done
deps=${WIN_DEPS:-/tmp/lunaria-windows-deps}
if [ -z "${WIN_DEPS:-}" ] && { [ ! -f "$deps/.ready" ] || [ ! -f "$deps/include/elf.h" ]; }; then
    command -v curl >/dev/null || fail 'missing curl'
    command -v tar >/dev/null || fail 'missing tar (with zstd support)'
    python3 scripts/fetch-windows-deps.py "$deps"
fi
deps=$(cd "$deps" && pwd)
angle=${WIN_ANGLE:-$deps}
[ ! -d "$deps/ucrt64" ] || angle=${WIN_ANGLE:-$deps/ucrt64}
[ ! -d "$deps/angle/ucrt64" ] || angle=${WIN_ANGLE:-$deps/angle/ucrt64}
glfw=${WIN_GLFW_LIB:-$deps/lib/libglfw3.a}
[ ! -f "$deps/ucrt64/lib/libglfw3.dll.a" ] || glfw=${WIN_GLFW_LIB:-$deps/ucrt64/lib/libglfw3.dll.a}
[ ! -f "$deps/glfw-build/src/libglfw3.a" ] || glfw=${WIN_GLFW_LIB:-$deps/glfw-build/src/libglfw3.a}
boost=${WIN_BOOST_INCLUDE:-$deps/include}
[ ! -d "$deps/boost" ] || boost=${WIN_BOOST_INCLUDE:-$deps}
[ ! -d "$deps/boost-headers/boost" ] || boost=${WIN_BOOST_INCLUDE:-$deps/boost-headers}
[ -f "$glfw" ] || fail "missing $glfw; set WIN_GLFW_LIB"
[ -f "$angle/lib/libEGL.dll.a" ] || fail "missing ANGLE import libraries in $angle; set WIN_ANGLE"
[ -d "$boost/boost" ] || fail "missing Boost headers in $boost; set WIN_BOOST_INCLUDE"
printf 'int main(void) { return 0; }\n' | "${cross}gcc" -mcrtdll=ucrt -x c -fsyntax-only - \
    || fail 'the cross compiler must support -mcrtdll=ucrt'
export OBJDUMP="${cross}objdump"
export WINDOWS_DLL_DIRS="$deps/bin:$angle/bin:$deps${WINDOWS_DLL_DIRS:+:$WINDOWS_DLL_DIRS}"
echo "dist-cross-win: dependencies=$deps, ANGLE=$angle, GLFW=$glfw"
work=${WIN_BUILD_DIR:-/tmp/lunaria-cross-win}
mkdir -p "$work"
exec "${1:-make}" dist \
    LUNA_OS_NAME=MINGW64 DIST_OS=windows DIST_ARCH=x86_64 \
    CC="${cross}gcc" CXX="${cross}g++" \
    BUILD_DIR="$work" LUNARIA_BIN="$work/lunaria.exe" \
    OPENH264_SO="$work/runtime/openh264.dll" SYSLIB_DIR="$work/syslib-arm64" \
    DIST_BUILD_DIR="${WIN_DIST_BUILD_DIR:-/tmp/lunaria-dist-build/cross-windows-x86_64}" \
    HOST_CPPFLAGS="-I$angle/include -I$deps/include" \
    HOST_WINDOW_LIBS="$glfw" \
    HOST_GL_LIBS="-L$angle/lib -lEGL -lGLESv2" \
    HOST_CRYPTO_LIBS="-L$angle/lib -L$deps/lib -lssl -lcrypto" \
    HOST_Z_LIBS="-L$angle/lib -L$deps/lib -lz" \
    DYNARMIC_CMAKE_FLAGS="-G Ninja -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=AMD64 -DCMAKE_C_COMPILER=${cross}gcc -DCMAKE_CXX_COMPILER=${cross}g++ -DCMAKE_C_FLAGS=-mcrtdll=ucrt -DCMAKE_CXX_FLAGS=-mcrtdll=ucrt -DBoost_INCLUDE_DIR=$boost -DBoost_NO_BOOST_CMAKE=ON -DDYNARMIC_USE_LLVM=OFF"
