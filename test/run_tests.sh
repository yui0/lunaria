#!/bin/sh
#
# Copyright © 2026 Yuichiro Nakada / Project Lunaria
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Test suite for Lunaria
# Usage: sh test/run_tests.sh [test_name]
#
# All tests must be run from the project root directory.

LUNARIA=./lunaria
RUNTIME_PATH=./runtime:.
TESTDIR=./test
PASS=0
FAIL=0

log_pass() { printf '\033[32mPASS\033[0m %s\n' "$1"; PASS=$((PASS+1)); }
log_fail() { printf '\033[31mFAIL\033[0m %s — %s\n' "$1" "$2"; FAIL=$((FAIL+1)); }
log_skip() { printf '\033[33mSKIP\033[0m %s — %s\n' "$1" "$2"; }

run() {
    LD_LIBRARY_PATH="$RUNTIME_PATH" "$@" 2>&1
}

# ---------------------------------------------------------------------------
# t_build: Rebuild test artefacts from source
# ---------------------------------------------------------------------------
t_build() {
    # hello_jni_i386.so — i386 JNI library (NASM, no libc)
    nasm -f elf32 "$TESTDIR/hello_jni_i386.asm" -o "$TESTDIR/hello_jni_i386.o" 2>&1 || return 1
    ld -m elf_i386 -shared --hash-style=sysv \
        -o "$TESTDIR/hello_jni_i386.so" "$TESTDIR/hello_jni_i386.o" 2>&1 || return 1

    # hello_android — i386 static ELF executable (NASM, no libc)
    nasm -f elf32 "$TESTDIR/hello_android.asm" -o "$TESTDIR/hello_android.o" 2>&1 || return 1
    ld -m elf_i386 "$TESTDIR/hello_android.o" -o "$TESTDIR/hello_android" 2>&1 || return 1
}

# ---------------------------------------------------------------------------
# t_lunaria_exists: lunaria binary must exist and be executable
# ---------------------------------------------------------------------------
t_lunaria_exists() {
    if [ ! -x "$LUNARIA" ]; then
        log_fail "lunaria_exists" "lunaria not found or not executable at $LUNARIA"
        return
    fi
    log_pass "lunaria_exists"
}

# ---------------------------------------------------------------------------
# t_lunaria_usage: running without args must print usage
# ---------------------------------------------------------------------------
t_lunaria_usage() {
    out=$(run "$LUNARIA" 2>&1)
    if echo "$out" | grep -q "usage:"; then
        log_pass "lunaria_usage"
    else
        log_fail "lunaria_usage" "expected 'usage:' in output, got: $out"
    fi
}

# ---------------------------------------------------------------------------
# t_runtime_libs: all runtime .so files must exist
# ---------------------------------------------------------------------------
t_runtime_libs() {
    missing=""
    for lib in \
        runtime/libdl.so runtime/libc.so runtime/libandroid.so \
        runtime/liblog.so runtime/libEGL.so runtime/libOpenSLES.so \
        runtime/libjvm.so runtime/libvulkan.so
    do
        [ -f "$lib" ] || missing="$missing $lib"
    done
    if [ -z "$missing" ]; then
        log_pass "runtime_libs"
    else
        log_fail "runtime_libs" "missing:$missing"
    fi
}

# ---------------------------------------------------------------------------
# t_elf32_check: test binary must be ELF 32-bit i386
# ---------------------------------------------------------------------------
t_elf32_check() {
    so="$TESTDIR/hello_jni_i386.so"
    if [ ! -f "$so" ]; then
        log_skip "elf32_check" "$so not built — run t_build first"
        return
    fi
    out=$(file "$so")
    if echo "$out" | grep -qE "ELF 32-bit.*(Intel i386|Intel 80386|EM_386)"; then
        log_pass "elf32_check"
    else
        log_fail "elf32_check" "unexpected file type: $out"
    fi
}

# ---------------------------------------------------------------------------
# t_sysv_hash: .so must have DT_HASH (SysV), not only GNU_HASH
# ---------------------------------------------------------------------------
t_sysv_hash() {
    so="$TESTDIR/hello_jni_i386.so"
    if [ ! -f "$so" ]; then
        log_skip "sysv_hash" "$so not built"
        return
    fi
    if readelf -d "$so" | grep -q "(HASH)"; then
        log_pass "sysv_hash"
    else
        log_fail "sysv_hash" "DT_HASH not found — bionic linker cannot resolve symbols"
    fi
}

# ---------------------------------------------------------------------------
# t_jni_onload: lunaria must call JNI_OnLoad and produce expected output
# ---------------------------------------------------------------------------
t_jni_onload() {
    so="$TESTDIR/hello_jni_i386.so"
    if [ ! -f "$so" ]; then
        log_skip "jni_onload" "$so not built"
        return
    fi
    out=$(run "$LUNARIA" "$so" 2>&1)
    if echo "$out" | grep -q "JNI_OnLoad called"; then
        log_pass "jni_onload"
    else
        log_fail "jni_onload" "JNI_OnLoad output not found. Got: $out"
    fi
}

# ---------------------------------------------------------------------------
# t_elf_load: bionic linker must successfully map the .so
# ---------------------------------------------------------------------------
t_elf_load() {
    so="$TESTDIR/hello_jni_i386.so"
    if [ ! -f "$so" ]; then
        log_skip "elf_load" "$so not built"
        return
    fi
    out=$(run "$LUNARIA" "$so" 2>&1)
    # "not a valid ELF object" or "dlopen failed" indicates a hard load error
    if echo "$out" | grep -qE "not a valid ELF|dlopen failed"; then
        log_fail "elf_load" "$out"
    else
        log_pass "elf_load"
    fi
}

# ---------------------------------------------------------------------------
# t_bad_elf: passing a non-ELF file must not crash lunaria (exit cleanly)
# ---------------------------------------------------------------------------
t_bad_elf() {
    tmp=$(mktemp /tmp/not_an_elf_XXXX)
    printf 'this is not an elf file\n' > "$tmp"
    run "$LUNARIA" "$tmp" > /dev/null 2>&1
    status=$?
    rm -f "$tmp"
    # We expect a non-zero exit (error), but not a signal (crash = 128+N or 139)
    if [ "$status" -ge 128 ]; then
        log_fail "bad_elf" "lunaria crashed with signal (exit $status) on non-ELF input"
    else
        log_pass "bad_elf"
    fi
}

# ---------------------------------------------------------------------------
# t_missing_file: passing a nonexistent path must not crash
# ---------------------------------------------------------------------------
t_missing_file() {
    run "$LUNARIA" /nonexistent/path/lib.so > /dev/null 2>&1
    status=$?
    if [ "$status" -ge 128 ]; then
        log_fail "missing_file" "lunaria crashed with signal (exit $status)"
    else
        log_pass "missing_file"
    fi
}

# ---------------------------------------------------------------------------
# t_x86_64_rejected: an x86_64 .so must be rejected (bionic linker is i386)
# ---------------------------------------------------------------------------
t_x86_64_rejected() {
    so="$TESTDIR/hello_jni.so"
    if [ ! -f "$so" ]; then
        log_skip "x86_64_rejected" "$so not present"
        return
    fi
    out=$(run "$LUNARIA" "$so" 2>&1)
    if echo "$out" | grep -q "not a valid ELF object"; then
        log_pass "x86_64_rejected"
    else
        log_fail "x86_64_rejected" "expected rejection of x86_64 ELF, got: $out"
    fi
}

# ---------------------------------------------------------------------------
# Real libunity.so tests (static analysis — no ARM execution needed)
# Obtain the file with: make fetch-libunity
# ---------------------------------------------------------------------------

# t_libunity_elf: must be a valid ARM 32-bit ELF shared object
t_libunity_elf() {
    so="$TESTDIR/libunity.so"
    if [ ! -f "$so" ]; then
        log_skip "libunity_elf" "test/libunity.so absent — run: make fetch-libunity"
        return
    fi
    out=$(file "$so")
    if echo "$out" | grep -qE "ELF 32-bit.*shared object.*ARM|ELF 32-bit.*ARM.*shared object"; then
        log_pass "libunity_elf"
    else
        log_fail "libunity_elf" "unexpected file type: $out"
    fi
}

# t_libunity_jni_onload: must export JNI_OnLoad (entry point lunaria calls)
t_libunity_jni_onload() {
    so="$TESTDIR/libunity.so"
    if [ ! -f "$so" ]; then
        log_skip "libunity_jni_onload" "test/libunity.so absent"
        return
    fi
    if nm -D "$so" 2>/dev/null | grep -q " T JNI_OnLoad"; then
        log_pass "libunity_jni_onload"
    else
        log_fail "libunity_jni_onload" "JNI_OnLoad not found in dynamic symbol table"
    fi
}

# t_libunity_reflectionhelper: must contain the ReflectionHelper class name string
# Unity calls RegisterNatives for this class — libjvm.so (jni_stubs.c) provides the stubs.
t_libunity_reflectionhelper() {
    so="$TESTDIR/libunity.so"
    if [ ! -f "$so" ]; then
        log_skip "libunity_reflectionhelper" "test/libunity.so absent"
        return
    fi
    if strings "$so" | grep -q "com/unity3d/player/ReflectionHelper"; then
        log_pass "libunity_reflectionhelper"
    else
        log_fail "libunity_reflectionhelper" "ReflectionHelper class name not found in binary"
    fi
}

# t_libunity_deps_covered: DT_NEEDED libs that lunaria must provide must exist in runtime/
# System libs (libz, libm, libmediandk) are expected from the host OS, not lunaria runtime.
t_libunity_deps_covered() {
    so="$TESTDIR/libunity.so"
    if [ ! -f "$so" ]; then
        log_skip "libunity_deps_covered" "test/libunity.so absent"
        return
    fi
    if [ ! -d "runtime" ]; then
        log_skip "libunity_deps_covered" "runtime/ not built yet — run: make"
        return
    fi
    missing=""
    needed=$(readelf -d "$so" 2>/dev/null | awk '/\(NEEDED\)/{gsub(/.*\[|\]/, ""); print}')
    for lib in $needed; do
        case "$lib" in
            libz.so|libm.so|libmediandk.so) continue ;;
        esac
        [ -f "runtime/$lib" ] || missing="$missing $lib"
    done
    if [ -z "$missing" ]; then
        log_pass "libunity_deps_covered"
    else
        log_fail "libunity_deps_covered" "missing runtime stubs:$missing"
    fi
}

# ---------------------------------------------------------------------------
# Between-two-worlds (ShutovKS) Unity IL2CPP ARM32 tests
# Source: https://github.com/ShutovKS/Between-two-worlds (MIT-licensed game)
# Obtain with: make fetch-btw
# ---------------------------------------------------------------------------
BTW_APK="$TESTDIR/btw-android.apk"
BTW_LIBUNITY="$TESTDIR/btw_libunity.so"

# t_btw_fetch: auto-download BTW APK and extract libunity.so if absent
t_btw_fetch() {
    if [ -f "$BTW_LIBUNITY" ]; then
        log_pass "btw_fetch"
        return
    fi
    printf '  INFO test/btw_libunity.so absent — fetching via make fetch-btw\n'
    if make fetch-btw > /dev/null 2>&1; then
        log_pass "btw_fetch"
    else
        log_fail "btw_fetch" "make fetch-btw failed (network unavailable?)"
    fi
}

# t_btw_elf: must be a valid ARM 32-bit ELF shared object
t_btw_elf() {
    if [ ! -f "$BTW_LIBUNITY" ]; then
        log_skip "btw_elf" "test/btw_libunity.so absent — run: make fetch-btw"
        return
    fi
    out=$(file "$BTW_LIBUNITY")
    if echo "$out" | grep -qE "ELF 32-bit.*shared object.*ARM|ELF 32-bit.*ARM.*shared object"; then
        log_pass "btw_elf"
    else
        log_fail "btw_elf" "unexpected file type: $out"
    fi
}

# t_btw_jni_onload: Unity libunity.so must export JNI_OnLoad
t_btw_jni_onload() {
    if [ ! -f "$BTW_LIBUNITY" ]; then
        log_skip "btw_jni_onload" "test/btw_libunity.so absent"
        return
    fi
    if nm -D "$BTW_LIBUNITY" 2>/dev/null | grep -q " T JNI_OnLoad"; then
        log_pass "btw_jni_onload"
    else
        log_fail "btw_jni_onload" "JNI_OnLoad not found in dynamic symbol table"
    fi
}

# t_btw_il2cpp_in_apk: APK must contain armeabi-v7a/libil2cpp.so (IL2CPP build)
t_btw_il2cpp_in_apk() {
    if [ ! -f "$BTW_APK" ]; then
        log_skip "btw_il2cpp_in_apk" "test/btw-android.apk absent — run: make fetch-btw"
        return
    fi
    if unzip -l "$BTW_APK" 2>/dev/null | grep -q "lib/armeabi-v7a/libil2cpp.so"; then
        log_pass "btw_il2cpp_in_apk"
    else
        log_fail "btw_il2cpp_in_apk" "base/lib/armeabi-v7a/libil2cpp.so not found in APK"
    fi
}

# t_btw_reflectionhelper: Unity libunity.so must reference ReflectionHelper
t_btw_reflectionhelper() {
    if [ ! -f "$BTW_LIBUNITY" ]; then
        log_skip "btw_reflectionhelper" "test/btw_libunity.so absent"
        return
    fi
    if strings "$BTW_LIBUNITY" | grep -q "com/unity3d/player/ReflectionHelper"; then
        log_pass "btw_reflectionhelper"
    else
        log_fail "btw_reflectionhelper" "ReflectionHelper class name not found in binary"
    fi
}

# t_btw_deps_covered: DT_NEEDED libs from btw libunity.so must exist in runtime/
t_btw_deps_covered() {
    if [ ! -f "$BTW_LIBUNITY" ]; then
        log_skip "btw_deps_covered" "test/btw_libunity.so absent"
        return
    fi
    if [ ! -d "runtime" ]; then
        log_skip "btw_deps_covered" "runtime/ not built yet — run: make"
        return
    fi
    missing=""
    needed=$(readelf -d "$BTW_LIBUNITY" 2>/dev/null | awk '/\(NEEDED\)/{gsub(/.*\[|\]/, ""); print}')
    for lib in $needed; do
        case "$lib" in
            libz.so|libm.so|libmediandk.so) continue ;;
        esac
        [ -f "runtime/$lib" ] || missing="$missing $lib"
    done
    if [ -z "$missing" ]; then
        log_pass "btw_deps_covered"
    else
        log_fail "btw_deps_covered" "missing runtime stubs:$missing"
    fi
}

# t_dynarmic_btw: run dynarmic ARM emulation against btw libunity.so
t_dynarmic_btw() {
    bin="$TESTDIR/test_dynarmic_arm"
    if [ ! -x "$bin" ]; then
        log_skip "dynarmic_btw" "$bin not built"
        return
    fi
    if [ ! -f "$BTW_LIBUNITY" ]; then
        log_skip "dynarmic_btw" "test/btw_libunity.so absent — run: make fetch-btw"
        return
    fi
    out=$("$bin" "$BTW_LIBUNITY" 2>&1)
    if echo "$out" | grep -q "FAIL"; then
        log_fail "dynarmic_btw" "$out"
    else
        count=$(echo "$out" | grep -oE 'executed [0-9]+ instructions' | head -1)
        log_pass "dynarmic_btw${count:+ ($count)}"
    fi
}

# ---------------------------------------------------------------------------
# t_build_unity: compile ReflectionHelper stub unit-test binary (host-native)
# ---------------------------------------------------------------------------
t_build_unity() {
    ${CC:-cc} -std=c11 -g -Isrc -D_GNU_SOURCE \
        "$TESTDIR/test_unity.c" src/jvm/jni_stubs.c \
        -o "$TESTDIR/test_unity" 2>&1 || return 1
}

# ---------------------------------------------------------------------------
# t_fetch_libunity: auto-download libunity.so if absent
# ---------------------------------------------------------------------------
t_fetch_libunity() {
    if [ -f "$TESTDIR/libunity.so" ]; then
        log_pass "fetch_libunity"
        return
    fi
    printf '  INFO test/libunity.so absent — fetching via make fetch-libunity\n'
    if make fetch-libunity > /dev/null 2>&1; then
        log_pass "fetch_libunity"
    else
        log_fail "fetch_libunity" "make fetch-libunity failed (network unavailable?)"
    fi
}

# ---------------------------------------------------------------------------
# t_dynarmic_synth: dynarmic synthetic ARM32 + Thumb tests
# ---------------------------------------------------------------------------
t_dynarmic_synth() {
    bin="$TESTDIR/test_dynarmic_arm"
    if [ ! -x "$bin" ]; then
        log_skip "dynarmic_synth" "$bin not built — run: make test/test_dynarmic_arm"
        return
    fi
    out=$("$bin" /dev/null 2>&1)
    if echo "$out" | grep -q "FAIL"; then
        log_fail "dynarmic_synth" "$out"
    else
        log_pass "dynarmic_synth"
    fi
}

# ---------------------------------------------------------------------------
# t_dynarmic_libunity: run dynarmic ARM emulation against real libunity.so
# ---------------------------------------------------------------------------
t_dynarmic_libunity() {
    bin="$TESTDIR/test_dynarmic_arm"
    so="$TESTDIR/libunity.so"
    if [ ! -x "$bin" ]; then
        log_skip "dynarmic_libunity" "$bin not built"
        return
    fi
    if [ ! -f "$so" ]; then
        log_skip "dynarmic_libunity" "test/libunity.so absent"
        return
    fi
    out=$("$bin" "$so" 2>&1)
    if echo "$out" | grep -q "FAIL"; then
        log_fail "dynarmic_libunity" "$out"
    else
        # extract instruction count for info
        count=$(echo "$out" | grep -oE 'executed [0-9]+ instructions' | head -1)
        log_pass "dynarmic_libunity${count:+ ($count)}"
    fi
}

# ---------------------------------------------------------------------------
# t_unity_symbols: libjvm.so must export both ReflectionHelper symbols
# ---------------------------------------------------------------------------
t_unity_symbols() {
    so="runtime/libjvm.so"
    if [ ! -f "$so" ]; then
        log_skip "unity_symbols" "$so not built"
        return
    fi
    missing=""
    for sym in \
        com_unity3d_player_ReflectionHelper_getMethodID \
        com_unity3d_player_ReflectionHelper_getFieldID
    do
        nm -D "$so" | grep -q " T $sym" || missing="$missing $sym"
    done
    if [ -z "$missing" ]; then
        log_pass "unity_symbols"
    else
        log_fail "unity_symbols" "missing exports:$missing"
    fi
}

# ---------------------------------------------------------------------------
# t_unity_logic: run the ReflectionHelper unit-test binary
# ---------------------------------------------------------------------------
t_unity_logic() {
    if [ ! -x "$TESTDIR/test_unity" ]; then
        log_skip "unity_logic" "test/test_unity not built"
        return
    fi
    out=$("$TESTDIR/test_unity" 2>&1)
    if "$TESTDIR/test_unity" > /dev/null 2>&1; then
        log_pass "unity_logic"
    else
        log_fail "unity_logic" "$out"
    fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
cd "$(dirname "$0")/.." || exit 1

# Run a single test if named on command line (no lunaria required for static tests)
if [ -n "$1" ]; then
    if [ "${VERBOSE:-0}" = "1" ]; then
        t_build_unity || true
    else
        t_build_unity > /dev/null 2>&1 || true
    fi
    "t_$1" 2>/dev/null || printf 'Unknown test: %s\n' "$1"
    printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
    exit "$([ "$FAIL" -eq 0 ] && echo 0 || echo 1)"
fi

# Full suite: require lunaria
if [ ! -x "$LUNARIA" ]; then
    printf 'lunaria binary not found. Build with: make x86_64\n' >&2
    exit 1
fi

# Rebuild test artefacts quietly unless VERBOSE=1
if [ "${VERBOSE:-0}" = "1" ]; then
    t_build || { printf 'Build failed\n' >&2; exit 1; }
    t_build_unity || { printf 'Build of unity test binary failed\n' >&2; exit 1; }
else
    t_build > /dev/null 2>&1 || { printf 'Build of test artefacts failed\n' >&2; exit 1; }
    t_build_unity > /dev/null 2>&1 || { printf 'Build of unity test binary failed\n' >&2; exit 1; }
fi

t_lunaria_exists
t_lunaria_usage
t_runtime_libs
t_elf32_check
t_sysv_hash
t_elf_load
t_jni_onload
t_bad_elf
t_missing_file
t_x86_64_rejected
t_unity_symbols
t_unity_logic
t_fetch_libunity
t_libunity_elf
t_libunity_jni_onload
t_libunity_reflectionhelper
t_libunity_deps_covered
t_dynarmic_synth
t_dynarmic_libunity

t_btw_fetch
t_btw_elf
t_btw_jni_onload
t_btw_il2cpp_in_apk
t_btw_reflectionhelper
t_btw_deps_covered
t_dynarmic_btw

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
