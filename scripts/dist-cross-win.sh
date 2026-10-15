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
# Prefer GCC's -mcrtdll=ucrt (Fedora/newer MinGW). Ubuntu's apt MinGW (GCC 13)
# lacks it; llvm-mingw UCRT toolchains already default to UCRT and reject the flag.
# Probe with a real link (not -fsyntax-only) so a wrong-arch compiler on PATH
# (e.g. x86_64 llvm-mingw on aarch64) fails here with a clear message.
crt_flags=
crt_probe=$(mktemp -d)
printf 'int main(void) { return 0; }\n' >"$crt_probe/t.c"
crt_err=$crt_probe/err.txt
if "${cross}gcc" -mcrtdll=ucrt "$crt_probe/t.c" -o "$crt_probe/t.exe" >"$crt_err" 2>&1; then
    crt_flags=-mcrtdll=ucrt
elif "${cross}gcc" "$crt_probe/t.c" -o "$crt_probe/t.exe" >"$crt_err" 2>&1; then
    if ! "${cross}objdump" -p "$crt_probe/t.exe" | grep -Eiq 'ucrtbase|api-ms-win-crt-'; then
        rm -rf "$crt_probe"
        fail 'the cross compiler must target UCRT (GCC with -mcrtdll=ucrt, or llvm-mingw UCRT)'
    fi
else
    _msg=$(cat "$crt_err" 2>/dev/null | tr '\n' ' ' | sed 's/[[:space:]]\+/ /g')
    rm -rf "$crt_probe"
    fail "cross compiler failed to link a trivial program (${cross}gcc): ${_msg:-no output; check host arch vs toolchain}"
fi
rm -rf "$crt_probe"
export OBJDUMP="${cross}objdump"
# Make variables are not always in the recipe environment; export so
# bundle-windows.py can locate the same cross toolchain that linked the exe.
export CC="${cross}gcc"
export CXX="${cross}g++"
# llvm-mingw ships libc++.dll / libunwind.dll under $prefix/x86_64-w64-mingw32/bin
# (and sometimes copies them into $prefix/bin). GCC MinGW puts libstdc++-6.dll
# next to the cross compiler or in the sys-root. Without these on
# WINDOWS_DLL_DIRS, bundle-windows.py fails with "missing libc++.dll".
toolchain_dll_dirs=
cross_cxx=$(command -v "${cross}g++" || true)
if [ -n "$cross_cxx" ]; then
    # Use the PATH entry first (llvm-mingw wrappers symlink within $prefix/bin).
    # Also try the fully resolved binary, in case /opt/llvm-mingw is a symlink
    # to a versioned tree.
    _resolved=$(readlink -f "$cross_cxx" 2>/dev/null || echo "$cross_cxx")
    for _cxx_bin in "$(dirname "$cross_cxx")" "$(dirname "$_resolved")"; do
        _cxx_prefix=$(dirname "$_cxx_bin")
        for _d in \
            "$_cxx_bin" \
            "$_cxx_prefix/x86_64-w64-mingw32/bin" \
            "$_cxx_prefix/x86_64-w64-mingw32/lib" \
            "/usr/x86_64-w64-mingw32/sys-root/mingw/bin" \
            "/opt/llvm-mingw/x86_64-w64-mingw32/bin"; do
            [ -d "$_d" ] || continue
            case ":$toolchain_dll_dirs:" in
                *":$_d:"*) ;;
                *) toolchain_dll_dirs="${toolchain_dll_dirs:+$toolchain_dll_dirs:}$_d" ;;
            esac
        done
    done
fi
export WINDOWS_DLL_DIRS="$deps/bin:$angle/bin:$deps${toolchain_dll_dirs:+:$toolchain_dll_dirs}${WINDOWS_DLL_DIRS:+:$WINDOWS_DLL_DIRS}"
echo "dist-cross-win: dependencies=$deps, ANGLE=$angle, GLFW=$glfw, CRT_FLAGS=${crt_flags:-(toolchain default UCRT)}"
echo "dist-cross-win: WINDOWS_DLL_DIRS=$WINDOWS_DLL_DIRS"
work=${WIN_BUILD_DIR:-/tmp/lunaria-cross-win}
mkdir -p "$work"
# Guest AArch64 libs are host-arch independent. Reuse a tree-local syslib when
# present (release script builds it once and shares it); otherwise stage under $work.
if [ -z "${SYSLIB_DIR:-}" ]; then
    if [ -f syslib-arm64/libm.so ] && [ -f syslib-arm64/libc-pure.so ]; then
        SYSLIB_DIR=$(pwd)/syslib-arm64
        echo "dist-cross-win: reusing $SYSLIB_DIR"
    else
        SYSLIB_DIR=$work/syslib-arm64
    fi
fi
# FMT_CONSTEVAL=: bundled fmt 10.x fails Clang 20+ consteval checks (llvm-mingw).
cxx_flags="${crt_flags:+$crt_flags }-DFMT_CONSTEVAL="
exec "${1:-make}" dist \
    LUNA_OS_NAME=MINGW64 DIST_OS=windows DIST_ARCH=x86_64 \
    CC="${cross}gcc" CXX="${cross}g++" \
    WINDOWS_CRT_FLAGS="$crt_flags" \
    BUILD_DIR="$work" LUNARIA_BIN="$work/lunaria.exe" \
    OPENH264_SO="$work/runtime/openh264.dll" SYSLIB_DIR="$SYSLIB_DIR" \
    DIST_BUILD_DIR="${WIN_DIST_BUILD_DIR:-/tmp/lunaria-dist-build/cross-windows-x86_64}" \
    HOST_CPPFLAGS="-I$angle/include -I$deps/include" \
    HOST_WINDOW_LIBS="$glfw" \
    HOST_GL_LIBS="-L$angle/lib -lEGL -lGLESv2" \
    HOST_CRYPTO_LIBS="-L$angle/lib -L$deps/lib -lssl -lcrypto" \
    HOST_Z_LIBS="-L$angle/lib -L$deps/lib -lz" \
    DYNARMIC_CMAKE_FLAGS="-G Ninja -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=AMD64 -DCMAKE_C_COMPILER=${cross}gcc -DCMAKE_CXX_COMPILER=${cross}g++ -DCMAKE_C_FLAGS=\"$crt_flags\" -DCMAKE_CXX_FLAGS=\"$cxx_flags\" -DBoost_INCLUDE_DIR=$boost -DBoost_NO_BOOST_CMAKE=ON -DDYNARMIC_USE_LLVM=OFF"
