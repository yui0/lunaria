# Copyright © 2026 Yuichiro Nakada / Project Lunaria
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

PREFIX ?= /usr/local
BINDIR ?= /bin
LIBDIR ?= /lib
RUNTIMEDIR ?= /lunaria

.SUFFIXES:

RM = rm -f

WARNINGS = -Wall -Wextra -Wpedantic -Wformat=2 -Wstrict-aliasing=3 -Wstrict-overflow=3 -Wstack-usage=4096000 \
	-Wfloat-equal -Wcast-align -Wpointer-arith -Wchar-subscripts -Warray-bounds=2 -Wno-unused-parameter

CFLAGS ?= -g -O2 $(WARNINGS)
CFLAGS += -std=c11
CPPFLAGS ?= -D_FORTIFY_SOURCE=2
CPPFLAGS += -Isrc -DANDROID_X86_LINKER # -DVERBOSE_FUNCTIONS
# Default: x86 (32-bit). Use targets below for other ABIs.

bins = lunaria
libs = runtime/libpthread.so runtime/libdl.so runtime/libc.so runtime/libandroid.so \
       runtime/liblog.so runtime/libEGL.so runtime/libOpenSLES.so runtime/libjvm.so \
       runtime/libm.so runtime/libz.so runtime/libmediandk.so runtime/libGLESv3.so
libs += runtime/libvulkan.so

all: $(bins)

# https://developer.android.com/ndk/guides/abis
# https://android.googlesource.com/platform/ndk/+/ics-mr0/docs/STANDALONE-TOOLCHAIN.html
# https://android.googlesource.com/platform/ndk/+/ics-mr0/docs/CPU-ARCH-ABIS.html
# you can also try compiling with your custom ABI with the all target,
# but compatibility with android binaries is not guaranteed

x86:
	$(MAKE) all \
	    CFLAGS="$(CFLAGS) -march=i686 -mtune=intel -mssse3 -mstackrealign -mfpmath=sse -m32" \
	    LDFLAGS="$(LDFLAGS) -march=i686 -m32"

x86_64:
	$(MAKE) all \
	    CFLAGS="$(CFLAGS) -march=x86-64 -msse4.2 -mpopcnt -m64 -mtune=intel -fPIC" \
	    LDFLAGS="$(LDFLAGS) -march=x86-64 -m64" \
	    CPPFLAGS="$(CPPFLAGS) -UANDROID_X86_LINKER -DANDROID_X86_64_LINKER"

armeabi:
	$(MAKE) all \
	    CFLAGS="$(CFLAGS) -march=armv5te -mthumb" \
	    LDFLAGS="$(LDFLAGS) -march=armv5te"

armeabi-v7a:
	$(MAKE) all \
	    CFLAGS="$(CFLAGS) -march=armv7-a -mfloat-abi=softfp -mthumb" \
	    LDFLAGS="$(LDFLAGS) -march=armv7-a -Wl,--fix-cortex-a8"

armeabi-v7a-neon:
	$(MAKE) all \
	    CFLAGS="$(CFLAGS) -march=armv7-a -mfloat-abi=softfp -mthumb -mfpu=neon" \
	    LDFLAGS="$(LDFLAGS) -march=armv7-a -Wl,--fix-cortex-a8"

arm64-v8a:
	$(MAKE) all \
	    CPPFLAGS="$(CPPFLAGS) -UANDROID_X86_LINKER -DANDROID_AARCH64_LINKER"

trace.o: src/trace.c src/trace.h
	$(CC) $(CFLAGS) -fvisibility=hidden -fPIC $(CPPFLAGS) -D_GNU_SOURCE -c src/trace.c -o $@

runtime/libpthread.so: src/lib/pthread.c
	mkdir -p runtime
	$(CC) $(CFLAGS) -fPIC $(CPPFLAGS) -D_GNU_SOURCE $(LDFLAGS) -shared src/lib/pthread.c -lpthread -lrt -o $@

runtime/libdl.so: trace.o src/linker/dlfcn.c src/linker/linker.c src/linker/linker_environ.c src/linker/rt.c src/linker/strlcpy.c
	mkdir -p runtime
	$(CC) $(CFLAGS) -fPIC $(CPPFLAGS) -D_GNU_SOURCE -DLINKER_DEBUG=1 -DRUNTIMEPATH='"$(PREFIX)$(LIBDIR)$(RUNTIMEDIR)"' \
	    -Wno-pedantic -Wno-variadic-macros -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-incompatible-pointer-types \
	    $(LDFLAGS) -shared trace.o \
	    src/linker/dlfcn.c src/linker/linker.c src/linker/linker_environ.c src/linker/rt.c src/linker/strlcpy.c \
	    -ldl -lpthread -o $@

runtime/libc.so: trace.o src/lib/libc.c src/lib/libc-ctype.h src/lib/libc-sysconf.h src/lib/libc-verbose.h
	mkdir -p runtime
	$(CC) $(CFLAGS) -fPIC -Wno-deprecated-declarations $(CPPFLAGS) -D_GNU_SOURCE \
	    -Wl,-wrap,_IO_file_xsputn $(LDFLAGS) -shared \
	    trace.o src/lib/libc.c \
	    `pkg-config --libs libbsd libunwind` -o $@

# Small Android API stubs: one driver (src/lib/stub.c), one -DLUNARIA_STUB_* per .so
STUB_SO = $(CC) $(CFLAGS) -fPIC $(CPPFLAGS) $(LDFLAGS) -Isrc/lib -shared src/lib/stub.c

runtime/libandroid.so:
	mkdir -p runtime
	$(STUB_SO) -DLUNARIA_STUB_ANDROID -o $@ `pkg-config --libs glfw3`

runtime/liblog.so:
	mkdir -p runtime
	$(STUB_SO) -DLUNARIA_STUB_LOG -o $@

runtime/libEGL.so:
	mkdir -p runtime
	$(STUB_SO) -DLUNARIA_STUB_EGL -D_GNU_SOURCE -o $@ -lEGL `pkg-config --libs glfw3`

runtime/libOpenSLES.so: trace.o
	mkdir -p runtime
	$(CC) $(CFLAGS) -Wno-pedantic -fPIC $(CPPFLAGS) $(LDFLAGS) -Isrc/lib -shared trace.o \
	    src/lib/stub.c -DLUNARIA_STUB_OPENSLES -o $@

DVM_SRC = src/dvm/dex.c src/dvm/dvm.c src/dvm/dvm_runtime.c src/dvm/dvm_jni.c \
          src/dvm/dvm_net.c src/dvm/dvm_media.c
DVM_HDR = src/dvm/dex.h src/dvm/dvm.h src/dvm/dvm_internal.h src/dvm/dvm_jni.h \
          src/dvm/dvm_net.h src/dvm/dvm_media.h

# luna-ui: the HTML/CSS engine src/luna_overlay.c draws the emulator's own UI
# with.  It is header-only and lives in its own repository, so it is fetched
# rather than vendored -- the same treatment openh264 and the sample APKs get.
# The headers are a build-time dependency: $(LUNA_UI_HDR) is a prerequisite of
# the object that includes it, so a plain `make` fetches them once.
LUNA_UI_DIR ?= luna-ui
LUNA_UI_RAW  = https://raw.githubusercontent.com/Berry-OS/luna-ui/master
LUNA_UI_HDRS = luna-ui.h cssparser.h stb_truetype.h stb_image.h stb_image_write.h
LUNA_UI_HDR  = $(LUNA_UI_DIR)/luna-ui.h

fetch-luna-ui: $(LUNA_UI_HDR)
	@printf 'luna-ui headers ready: $(LUNA_UI_DIR)\n'

$(LUNA_UI_HDR):
	mkdir -p $(LUNA_UI_DIR)
	for h in $(LUNA_UI_HDRS); do \
	    curl -L --fail --retry 3 "$(LUNA_UI_RAW)/$$h" -o "$(LUNA_UI_DIR)/$$h" || exit 1; \
	done

# luna_overlay.o lives in libjvm.so rather than in the executable: the widget
# layer that publishes documents (src/dvm/dvm_runtime.c) is compiled into this
# library, and the presenter that draws them (src/arm_exec.cpp) links against
# it, so one copy here gives both sides the same overlay state.
LUNA_UI_CFLAGS = -I$(LUNA_UI_DIR) \
	-Wno-pedantic -Wno-unused-function -Wno-sign-compare \
	-Wno-missing-field-initializers -Wno-cast-align -Wno-float-equal \
	-Wno-stack-usage -Wno-array-bounds -Wno-strict-overflow

luna_overlay.o: src/luna_overlay.c src/luna_overlay.h $(LUNA_UI_HDR)
	$(CC) $(CFLAGS) -fPIC $(CPPFLAGS) -D_GNU_SOURCE $(LUNA_UI_CFLAGS) \
	    -c src/luna_overlay.c -o $@

# The boot screen.  It calls luna-ui but does not define its implementation —
# luna_overlay.o is the one translation unit that does, and both land in
# libjvm.so, so the engine is linked once.
luna_splash.o: src/luna_splash.c src/luna_splash.h src/luna_overlay.h $(LUNA_UI_HDR)
	$(CC) $(CFLAGS) -fPIC $(CPPFLAGS) -D_GNU_SOURCE $(LUNA_UI_CFLAGS) \
	    -c src/luna_splash.c -o $@

# The Dalvik bytecode emulator lives in libjvm.so: it is reached from jvm.c
# (a JNI call with no host stub) and it calls back out through the same JNI
# table, so the two have to be in one object.
runtime/libjvm.so: trace.o luna_overlay.o luna_splash.o src/jvm/jvm.c src/jvm/jni_stubs.c src/jvm/jvm.h src/jvm/jni.h $(DVM_SRC) $(DVM_HDR)
	mkdir -p runtime
	$(CC) $(CFLAGS) -fPIC $(CPPFLAGS) -D_GNU_SOURCE -Wno-pedantic $(LDFLAGS) -shared \
	    trace.o luna_overlay.o luna_splash.o src/jvm/jvm.c src/jvm/jni_stubs.c \
	    $(DVM_SRC) \
	    -lm -lssl -lcrypto -licuuc \
	    -lEGL -lGLESv2 -lz -o $@

runtime/libm.so:
	mkdir -p runtime
	$(STUB_SO) -DLUNARIA_STUB_MATH -D_GNU_SOURCE -o $@ -ldl -lm

runtime/libz.so:
	mkdir -p runtime
	$(STUB_SO) -DLUNARIA_STUB_ZLIB -o $@ -ldl -lz

runtime/libmediandk.so:
	mkdir -p runtime
	$(STUB_SO) -DLUNARIA_STUB_MEDIANDK -o $@

runtime/libGLESv3.so:
	mkdir -p runtime
	$(STUB_SO) -DLUNARIA_STUB_GLESV3 -D_GNU_SOURCE -o $@ -lGLESv2

runtime/libvulkan.so:
	mkdir -p runtime
	$(STUB_SO) -DLUNARIA_STUB_VULKAN -o $@

# trick linker to link against unversioned libs
libdl.so: runtime/libdl.so
	ln -s runtime/libdl.so $@
libpthread.so: runtime/libpthread.so
	ln -s runtime/libpthread.so $@

# arm_exec.o: compiled with C++20 and dynarmic headers; linked into lunaria
arm_exec.o: src/arm_exec.cpp src/arm_exec.h src/jvm/jvm.h $(DYNARMIC_LIB)
	$(CXX) -std=c++20 -O2 -g -fPIC \
	    $(DYNARMIC_INCS) \
	    -Isrc -D_GNU_SOURCE \
	    -c src/arm_exec.cpp -o $@

# loader.o: compiled as C11 (arm_exec.h is C-compatible)
loader.o: src/loader.c src/arm_exec.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -D_GNU_SOURCE -c src/loader.c -o $@

# lunaria: link with g++ so arm_exec.o (C++) and dynarmic (C++) are handled correctly
lunaria: loader.o arm_exec.o trace.o libdl.so libpthread.so \
       runtime/libpthread.so runtime/libc.so \
       runtime/libandroid.so runtime/liblog.so \
       runtime/libEGL.so runtime/libOpenSLES.so \
       runtime/libjvm.so runtime/libm.so runtime/libz.so \
       runtime/libmediandk.so runtime/libGLESv3.so
lunaria: runtime/libvulkan.so
	$(CXX) -std=c++20 -O2 -g \
	    -L. -Wl,-Y,runtime,-rpath,$(PREFIX)$(LIBDIR)$(RUNTIMEDIR) $(LDFLAGS) \
	    loader.o arm_exec.o trace.o \
	    $(DYNARMIC_LIBS) \
	    -ldl -lpthread -ljvm \
	    `pkg-config --libs glfw3` -lEGL -lGLESv2 -lz -lcrypto -o $@

install-bin: $(bins)
	install -Dm755 $(bins) -t "$(DESTDIR)$(PREFIX)$(BINDIR)"

install-lib: $(libs)
	install -Dm755 $(libs) -t "$(DESTDIR)$(PREFIX)$(LIBDIR)$(RUNTIMEDIR)"

install: install-bin install-lib

clean:
	$(RM) $(bins) trace.o arm_exec.o loader.o luna_overlay.o luna_splash.o \
	    libdl.so libpthread.so
	$(RM) -r runtime
	$(RM) test/test_dynarmic_arm test/test_unity test/test_dvm test/dvm_test.dex
	$(RM) test/test_splash
	$(RM) test/libabitest64.so test/libabitest32.so test/abi_test_values.h
	$(RM) test/abi_pkg/classes.dex

test: lunaria test/libunity.so test/test_dynarmic_arm
	sh test/run_tests.sh

# --- Dalvik bytecode emulator ---------------------------------------------
# test/dvm_test.dex is assembled by test/make_dex.py: there is no d8 in this
# tree, and a real APK only exercises the opcodes that app happens to use.
test/dvm_test.dex: test/make_dex.py
	python3 test/make_dex.py $@

# Built with the sanitizers on: the interesting failure mode for an
# interpreter over third-party bytecode is reading outside the mapping, which
# a plain wrong-answer check would not catch.
# Sanitizers are on by default but need libasan at link time; pass
# DVM_TEST_SAN= to build without them where that runtime is not installed.
DVM_TEST_SAN ?= -fsanitize=address,undefined
test/test_dvm: test/dvm_test.c $(DVM_SRC) $(DVM_HDR)
	$(CC) -std=c11 -g -O1 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -Isrc \
	    $(DVM_TEST_SAN) \
	    test/dvm_test.c $(DVM_SRC:src/dvm/dvm_jni.c=) -lm -lssl -lcrypto \
	    -lGLESv2 -o $@

# Pass a real classes.dex as DVM_DEX to also run every method in it.
DVM_DEX ?=
# The boot screen, rendered headlessly so it can be looked at without waiting
# through a real title's boot.
test/test_splash: test/splash_test.c luna_overlay.o luna_splash.o \
                  src/luna_overlay.h src/luna_splash.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -D_GNU_SOURCE -Wno-pedantic \
	    test/splash_test.c luna_overlay.o luna_splash.o \
	    -lEGL -lGLESv2 -lm -o $@

splash-test: test/test_splash
	mkdir -p /tmp/lunaria-splash
	./test/test_splash 12 /tmp/lunaria-splash

dvm-test: test/test_dvm test/dvm_test.dex
	./test/test_dvm test/dvm_test.dex $(DVM_DEX)

# Guest-side socket exercise: an AArch64 .so with no libc, talking to a local
# HTTP server through the emulator's socket SVCs (see test/net_test.c).
# Needs clang with the aarch64 target and lld; both come with the clang
# package listed in the prerequisites.
test/libnettest.so: test/net_test.c
	clang -target aarch64-linux-gnu -fPIC -shared -nostdlib -O1 -fuse-ld=lld \
	    -Wl,--unresolved-symbols=ignore-all -Wl,-soname,libnettest.so \
	    -o $@ $<

# The loader exits non-zero after JNI_OnLoad because a bare .so has no game
# entry point, so the verdict comes from the test's own RESULT line.
net-test: lunaria test/libnettest.so
	@python3 test/net_test_server.py & echo $$! > .nettest.pid; \
	sleep 1; \
	LD_LIBRARY_PATH="$(PWD):$(PWD)/runtime" ./lunaria test/libnettest.so 2>&1 \
	    | tee .nettest.out | grep -E 'nettest|^\[net\]'; \
	kill `cat .nettest.pid` 2>/dev/null; rm -f .nettest.pid; \
	grep -q 'RESULT PASS' .nettest.out; rc=$$?; rm -f .nettest.out; exit $$rc

# Guest-side POSIX exercise: regex, scandir (whose filter and comparator are
# guest functions) and process_vm_readv, all through the guest's own struct
# layouts — see test/posix_test.c.  Needs clang with the aarch64 target and
# lld, as net-test does.
test/libposixtest.so: test/posix_test.c
	clang -target aarch64-linux-gnu -fPIC -shared -nostdlib -O1 -fuse-ld=lld \
	    -Wl,--unresolved-symbols=ignore-all -Wl,-soname,libposixtest.so \
	    -o $@ $<

posix-test: lunaria test/libposixtest.so
	@LD_LIBRARY_PATH="$(PWD):$(PWD)/runtime" ./lunaria test/libposixtest.so 2>&1 \
	    | tee .posixtest.out | grep -E 'posixtest|unresolved'; \
	grep -q 'RESULT PASS' .posixtest.out; rc=$$?; rm -f .posixtest.out; exit $$rc

# Guest-side JNI calling-convention exercise.  The dex declares `native`
# methods taking jlong / jfloat / jdouble — and more of them than either ABI
# has argument registers for — and the guest .so checks what actually arrived
# and what came back.  Built for both ABIs on purpose: armeabi-v7a passes
# floating point in even-aligned core register pairs (softfp), arm64-v8a in a
# separate v-register sequence, so one build proves nothing about the other.
test/abi_test_values.h test/abi_pkg/classes.dex: test/make_abi_dex.py test/make_dex.py
	python3 test/make_abi_dex.py test/abi_pkg/classes.dex test/abi_test_values.h

# -ffreestanding -nostdlibinc: there is no target sysroot here, so jni.h's
# <stdint.h> / <stdarg.h> have to come from clang's own resource directory.
ABI_TEST_CFLAGS = -fPIC -shared -nostdlib -nostdlibinc -ffreestanding -O1 \
	-fuse-ld=lld -Isrc -Itest -Wall -Wextra -Wno-unused-parameter \
	-Wl,--unresolved-symbols=ignore-all

test/libabitest64.so: test/abi_test.c test/abi_test_values.h
	clang -target aarch64-linux-gnu $(ABI_TEST_CFLAGS) \
	    -Wl,-soname,libabitest64.so -o $@ $<

test/libabitest32.so: test/abi_test.c test/abi_test_values.h
	clang -target armv7a-linux-gnueabi -mfloat-abi=softfp -mfpu=vfpv3 \
	    $(ABI_TEST_CFLAGS) -Wl,-soname,libabitest32.so -o $@ $<

test/libschedtest64.so: test/sched_test.c
	clang -target aarch64-linux-gnu $(ABI_TEST_CFLAGS) \
	    -Wl,-soname,libschedtest64.so -o $@ $<

# Scheduler hand-off: a thread parked on a mutex must be woken by the release,
# not by whether a scheduler pass happens to sample the lock while it is free.
# armeabi-v7a is not covered — see the comment in test/sched_test.c.
sched-test: lunaria test/libschedtest64.so
	@LD_LIBRARY_PATH="$(PWD):$(PWD)/runtime" ./lunaria test/libschedtest64.so 2>&1 \
	    | tee .schedtest.out | grep -E 'schedtest' || true; \
	rc=0; grep -q 'RESULT PASS' .schedtest.out || rc=1; \
	rm -f .schedtest.out; exit $$rc

# As with net-test, the loader exits non-zero because a bare .so has no game
# entry point; the verdict is the test's own RESULT line.
abi-test: lunaria test/libabitest64.so test/libabitest32.so test/abi_pkg/classes.dex
	@rc=0; for so in test/libabitest64.so test/libabitest32.so; do \
	    printf '=== %s\n' "$$so"; \
	    ANDROID_PACKAGE_CODE_PATH="$(PWD)/test/abi_pkg" \
	    LD_LIBRARY_PATH="$(PWD):$(PWD)/runtime" ./lunaria $$so 2>&1 \
	        | tee .abitest.out | grep -E 'abitest|^\[dvm\]' || true; \
	    grep -q 'RESULT PASS' .abitest.out || rc=1; \
	    grep -q 'VARARGS 11 22 33 44 55 66 ok 1122334455667788' .abitest.out \
	        || { printf 'FAIL: host mis-formatted the guest log line\n'; rc=1; }; \
	    rm -f .abitest.out; \
	done; exit $$rc

# Download a real Unity APK and extract libunity.so for static-analysis tests.
# Source: Daggerfall Unity Android port (open source, MIT-licensed Unity wrapper)
# https://github.com/Vwing/daggerfall-unity-android
LIBUNITY_APK_URL = https://github.com/Vwing/daggerfall-unity-android/releases/download/v1.1.1.8/dfu-mono-32bit-v1.1.1.8_mods-supported.apk
LIBUNITY_APK     = test/dfu-mono-32bit.apk

fetch-libunity: $(LIBUNITY_APK)
	unzip -jo $(LIBUNITY_APK) "lib/armeabi-v7a/*.so" -d test/
	@printf 'Extracted ARM libraries to test/\n'

$(LIBUNITY_APK):
	curl -L "$(LIBUNITY_APK_URL)" -o $@

# Auto-extract libunity.so if APK is already present, or download+extract
test/libunity.so: $(LIBUNITY_APK)
	unzip -jo $(LIBUNITY_APK) "lib/armeabi-v7a/libunity.so" -d test/

# Build dynarmic A32 JIT library (Release, A32 frontend only)
DYNARMIC_DIR     = dynarmic
DYNARMIC_BUILD   = $(DYNARMIC_DIR)/build
DYNARMIC_LIB     = $(DYNARMIC_BUILD)/src/dynarmic/libdynarmic.a
DYNARMIC_FMT_LIB = $(DYNARMIC_BUILD)/externals/fmt/libfmt.a
DYNARMIC_MCL_LIB = $(DYNARMIC_BUILD)/externals/mcl/src/libmcl.a
DYNARMIC_ZYD_LIB = $(DYNARMIC_BUILD)/externals/zydis/libZydis.a
DYNARMIC_ZYC_LIB = $(DYNARMIC_BUILD)/externals/zydis/zycore/libZycore.a

DYNARMIC_INCS = \
	-I$(DYNARMIC_DIR)/src \
	-I$(DYNARMIC_DIR)/externals/mcl/include \
	-I$(DYNARMIC_DIR)/externals/fmt/include \
	-I$(DYNARMIC_DIR)/externals/zydis/include \
	-I$(DYNARMIC_DIR)/externals/zydis/zycore/include \
	-I$(DYNARMIC_BUILD)/externals/zydis \
	-I$(DYNARMIC_BUILD)/externals/zydis/zycore

DYNARMIC_LIBS = \
	$(DYNARMIC_LIB) $(DYNARMIC_FMT_LIB) $(DYNARMIC_MCL_LIB) \
	$(DYNARMIC_ZYD_LIB) $(DYNARMIC_ZYC_LIB)

$(DYNARMIC_LIB):
	cmake -B $(DYNARMIC_BUILD) -S $(DYNARMIC_DIR) \
	    -DDYNARMIC_WARNINGS_AS_ERRORS=OFF \
	    -DDYNARMIC_TESTS=OFF \
	    -DCMAKE_BUILD_TYPE=Release \
	    -DDYNARMIC_FRONTENDS="A32;A64" \
	    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	cmake --build $(DYNARMIC_BUILD) -j$(shell nproc)

test/test_dynarmic_arm: test/test_dynarmic_arm.cpp $(DYNARMIC_LIB)
	$(CXX) -std=c++20 -O2 -g \
	    $(DYNARMIC_INCS) \
	    test/test_dynarmic_arm.cpp \
	    $(DYNARMIC_LIBS) \
	    -lpthread -o $@

dynarmic-build: $(DYNARMIC_LIB)

# ---------------------------------------------------------------------------
# Between-two-worlds (ShutovKS) — Unity IL2CPP ARM32 APK (MIT-licensed game)
# https://github.com/ShutovKS/Between-two-worlds
# ---------------------------------------------------------------------------
BTW_APK_URL = https://github.com/ShutovKS/Between-two-worlds/releases/download/1.0.5/Android_1.0.5.apk
BTW_APK     = test/btw-android.apk

fetch-btw: $(BTW_APK)
	unzip -p $(BTW_APK) "base/lib/armeabi-v7a/libunity.so" > test/btw_libunity.so
	@printf 'Extracted btw libunity.so to test/btw_libunity.so\n'

$(BTW_APK):
	curl -L "$(BTW_APK_URL)" -o $@

test/btw_libunity.so: $(BTW_APK)
	unzip -p $< "base/lib/armeabi-v7a/libunity.so" > $@

# ---------------------------------------------------------------------------
# Blade & Soul Revolution (UnrealEngineAndroidSamples) — UE4 arm64 APK
# https://github.com/Abhishrut/UnrealEngineAndroidSamples
# ---------------------------------------------------------------------------
BLADE_SOUL_APK_URL = https://raw.githubusercontent.com/Abhishrut/UnrealEngineAndroidSamples/main/Blade%20Soul%20Revolution_v2.00.146.1_apkpure.com.apk
BLADE_SOUL_APK     = test/blade-soul.apk

fetch-blade-soul: $(BLADE_SOUL_APK)
	@printf 'Blade & Soul Revolution APK ready: $(BLADE_SOUL_APK)\n'

$(BLADE_SOUL_APK):
	curl -L --fail --retry 3 "$(BLADE_SOUL_APK_URL)" -o $@

# ---------------------------------------------------------------------------
# openh264 — the H.264 decoder behind android.media.MediaCodec
#
# Cisco publishes these binaries itself and pays the H.264 patent royalties for
# them, which is what makes them usable here; a decoder we compiled ourselves
# would not carry that.  So the binary is downloaded, never vendored, and
# loaded with dlopen() at run time: no build- or link-time dependency, and an
# emulator without it reports "no decoder" instead of pretending to have one.
# https://github.com/cisco/openh264 — BSD-2-Clause source, binaries per
# http://www.openh264.org/BINARY_LICENSE.txt
# ---------------------------------------------------------------------------
OPENH264_VERSION = 2.5.1
OPENH264_ABI     = 7
OPENH264_ARCH   ?= linux64
OPENH264_URL     = http://ciscobinary.openh264.org/libopenh264-$(OPENH264_VERSION)-$(OPENH264_ARCH).$(OPENH264_ABI).so.bz2
OPENH264_SO      = runtime/libopenh264.so

fetch-openh264: $(OPENH264_SO)
	@printf 'openh264 ready: $(OPENH264_SO)\n'

$(OPENH264_SO):
	mkdir -p runtime
	curl -L --fail --retry 3 "$(OPENH264_URL)" -o $@.bz2
	bunzip2 -c $@.bz2 > $@
	$(RM) $@.bz2

# Aggregate: download all sample APKs used for development / regression.
fetch: fetch-libunity fetch-btw fetch-blade-soul fetch-openh264

.PHONY: all x86 x86_64 armeabi armeabi-v7a armeabi-v7a-neon arm64-v8a \
        clean install install-bin install-lib test net-test dvm-test abi-test \
        posix-test splash-test \
        fetch fetch-libunity fetch-btw fetch-blade-soul fetch-openh264 \
        fetch-luna-ui \
        dynarmic-build
