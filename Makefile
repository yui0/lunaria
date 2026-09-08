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

# The host operating system, one file per platform.  A build compiles exactly
# one of them, so each is free to include whatever its own platform needs and
# none of them carries an #ifdef for the other two.  See src/lunaria_os.h.
LUNA_OS_NAME := $(shell uname -s)
ifeq ($(LUNA_OS_NAME),Darwin)
LUNA_OS_SRC = src/lunaria_mac.c
else ifneq (,$(findstring MINGW,$(LUNA_OS_NAME)))
LUNA_OS_SRC = src/lunaria_windows.c
else ifneq (,$(findstring MSYS,$(LUNA_OS_NAME)))
LUNA_OS_SRC = src/lunaria_windows.c
else ifneq (,$(findstring CYGWIN,$(LUNA_OS_NAME)))
LUNA_OS_SRC = src/lunaria_windows.c
else
LUNA_OS_SRC = src/lunaria_linux.c
endif
LUNA_OS_OBJ = lunaria_os.o

# The dynarmic sub-build's driver.  Only the macOS branch below used to set
# this, so `make dynarmic-build` on a Linux host ran an empty command and
# reported the archive as up to date without ever producing it.
CMAKE ?= cmake

# Host graphics and system libraries.  The macOS build uses GLFW's Cocoa
# backend and ANGLE's Metal backend from .deps/; no X11 or Linux libEGL is
# involved.  `make macos-deps` produces these files without Homebrew.
ifeq ($(LUNA_OS_NAME),Darwin)
MAC_DEPS          = .deps
MAC_OPENSSL      ?= /opt/homebrew/opt/openssl@3
MAC_SDK           = $(shell xcrun --show-sdk-path)
CMAKE             = $(MAC_DEPS)/bin/cmake
DYNARMIC_CMAKE_FLAGS = -DBOOST_ROOT=$(abspath $(MAC_DEPS)/boost) \
	-DBoost_NO_SYSTEM_PATHS=ON -DCMAKE_CXX_FLAGS=-DFMT_CONSTEVAL=
HOST_CPPFLAGS     = -DEGL_NO_PLATFORM_SPECIFIC_TYPES \
	-I$(MAC_DEPS)/linux-abi -I$(MAC_DEPS)/khronos/include \
	-I$(MAC_DEPS)/glfw/include -I$(MAC_DEPS)/icu/include \
	-I$(MAC_OPENSSL)/include
HOST_GL_LIBS      = $(MAC_DEPS)/lib/libEGL.dylib $(MAC_DEPS)/lib/libGLESv2.dylib
HOST_Z_LIBS       = $(MAC_SDK)/usr/lib/libz.tbd
HOST_WINDOW_LIBS  = $(MAC_DEPS)/lib/libglfw3.a -framework Cocoa \
	-framework IOKit -framework CoreFoundation -framework QuartzCore \
	-framework AudioToolbox
HOST_CRYPTO_LIBS  = -L$(MAC_OPENSSL)/lib -lssl -lcrypto
HOST_ICU_LIBS     = -licucore
HOST_DL_LIBS      = runtime/libdl.so
HOST_SYSTEM_DL_LIBS =
HOST_RT_LIBS      =
HOST_LIBC_LIBS    =
HOST_LIBC_LDFLAGS =
HOST_SO_LDFLAGS   = -Wl,-undefined,dynamic_lookup \
	-Wl,-install_name,@rpath/$(@F)
HOST_EXPORT       = -Wl,-export_dynamic
HOST_RPATH        = -Wl,-rpath,@loader_path/runtime \
	-Wl,-rpath,@loader_path/.deps/lib -Wl,-rpath,$(MAC_OPENSSL)/lib
HOST_TEST_RPATH   = -Wl,-rpath,@loader_path/../.deps/lib
LUNARIA_LIBDIRS   = -L. -Lruntime

OPENH264_SO       = runtime/libopenh264.dylib
OPENH264_ARCH    ?= mac-arm64
OPENH264_URL      = http://ciscobinary.openh264.org/libopenh264-$(OPENH264_VERSION)-$(OPENH264_ARCH).dylib.bz2
PTHREAD_SRC       = src/lib/pthread_mac.c
HOST_AUDIO_LIBS   =
HOST_BIONIC_LIBC  =
else
HOST_CPPFLAGS     = $(shell pkg-config --cflags glfw3)
HOST_GL_LIBS      = -lEGL -lGLESv2
HOST_Z_LIBS       = -lz
HOST_WINDOW_LIBS  = $(shell pkg-config --libs glfw3)
HOST_CRYPTO_LIBS  = -lssl -lcrypto
HOST_ICU_LIBS     = -licuuc
HOST_DL_LIBS      = -ldl
HOST_SYSTEM_DL_LIBS = -ldl
HOST_RT_LIBS      = -lrt
# The sound card, through src/alsa.h.  ALSA is the kernel's own audio API on
# GNU/Linux — no daemon, no client library beyond this one.
HOST_AUDIO_LIBS   = -lasound
HOST_LIBC_LIBS    = $(shell pkg-config --libs libbsd libunwind)
HOST_LIBC_LDFLAGS = -Wl,-wrap,_IO_file_xsputn
HOST_SO_LDFLAGS   =
HOST_EXPORT       = -rdynamic
HOST_RPATH        = -Wl,-Y,runtime,-rpath,$(PREFIX)$(LIBDIR)$(RUNTIMEDIR)
HOST_TEST_RPATH   =
# Guest Android stubs live in runtime/, but -Lruntime must not appear on the
# host executable link line: ld would resolve -lc to runtime/libc.so (the guest
# bionic shim) instead of the system libc.  -Wl,-Y,runtime below is enough for
# -ljvm and the libdl/libpthread symlinks in .
LUNARIA_LIBDIRS   = -L.
OPENH264_SO       = runtime/libopenh264.so
OPENH264_ARCH    ?= linux64
OPENH264_ABI      = 7
OPENH264_URL      = http://ciscobinary.openh264.org/libopenh264-$(OPENH264_VERSION)-$(OPENH264_ARCH).$(OPENH264_ABI).so.bz2
PTHREAD_SRC       = src/lib/pthread.c
HOST_BIONIC_LIBC  = runtime/libc.so
endif

CPPFLAGS += $(HOST_CPPFLAGS)

bins = lunaria
libs = runtime/libpthread.so runtime/libdl.so runtime/libc.so runtime/libandroid.so \
       runtime/liblog.so runtime/libEGL.so runtime/libOpenSLES.so runtime/libjvm.so \
       runtime/libm.so runtime/libz.so runtime/libmediandk.so runtime/libGLESv3.so
libs += runtime/libvulkan.so

# The H.264 decoder is part of a working build, not an optional extra: a title
# whose intro is an .mp4 waits on that movie, so a build without it stops at the
# splash for a reason that looks nothing like a missing decoder.  It is a
# download rather than a compile — see fetch-openh264 below for why it cannot be
# vendored — so it is cheap to have and expensive to forget.
ifeq ($(LUNA_OS_NAME),Darwin)
all: macos-deps
	$(MAKE) host-all
else
all: host-all
endif

host-all: $(bins) $(OPENH264_SO)

macos-deps:
	scripts/fetch-macos-deps.sh
	scripts/build-macos-glfw.sh

# https://developer.android.com/ndk/guides/abis
# https://android.googlesource.com/platform/ndk/+/ics-mr0/docs/STANDALONE-TOOLCHAIN.html
# https://android.googlesource.com/platform/ndk/+/ics-mr0/docs/CPU-ARCH-ABIS.html
# you can also try compiling with your custom ABI with the all target,
# but compatibility with android binaries is not guaranteed


# ---------------------------------------------------------------------------
# Real AArch64 Android platform libraries for the guest.
#
# Everything the emulator answers with an SVC thunk costs the guest a JIT exit,
# and libm alone -- pow and sincosf -- measured 40% of every exit Cross Worlds
# made.  Those are pure arithmetic that has no business leaving the JIT: give
# the guest the same code a device runs and it stays inside it.
#
# The file is bionic's arm64 libm, which Google ships inside the SDK
# build-tools archive (the renderscript intermediates are real AArch64 objects,
# unlike the NDK sysroot stubs, which carry no code).  A local SDK or NDK is
# used when there is one, so this only reaches the network when it has to.
SYSLIB_DIR    ?= syslib-arm64
SYSLIB_LIBM   := $(SYSLIB_DIR)/libm.so
BUILD_TOOLS_ZIP ?= build-tools_r34-linux.zip
BUILD_TOOLS_URL ?= https://dl.google.com/android/repository/$(BUILD_TOOLS_ZIP)

syslib: $(SYSLIB_LIBM)

$(SYSLIB_LIBM):
	@mkdir -p $(SYSLIB_DIR)
	@set -e; \
	found=""; \
	for root in "$$LUNARIA_ANDROID_SDK" "$$ANDROID_HOME" "$$ANDROID_SDK_ROOT" /root/image/android; do \
	    [ -n "$$root" ] || continue; \
	    cand=`find "$$root" -path '*/renderscript/lib/intermediates/arm64-v8a/libm.so' 2>/dev/null | head -1`; \
	    if [ -n "$$cand" ]; then found="$$cand"; break; fi; \
	done; \
	if [ -n "$$found" ]; then \
	    echo "syslib: using $$found"; \
	    cp "$$found" $@; \
	else \
	    echo "syslib: fetching $(BUILD_TOOLS_URL)"; \
	    tmp=`mktemp -d`; \
	    curl -fsSL -o "$$tmp/bt.zip" "$(BUILD_TOOLS_URL)"; \
	    entry=`unzip -Z1 "$$tmp/bt.zip" '*/renderscript/lib/intermediates/arm64-v8a/libm.so' | head -1`; \
	    [ -n "$$entry" ] || { echo "syslib: no arm64 libm in the archive" >&2; rm -rf "$$tmp"; exit 1; }; \
	    unzip -p "$$tmp/bt.zip" "$$entry" > $@; \
	    rm -rf "$$tmp"; \
	fi; \
	head -c 20 $@ | od -An -tu1 -j18 -N1 | grep -q 183 || \
	    { echo "syslib: $@ is not AArch64" >&2; rm -f $@; exit 1; }
	@echo "syslib: $@ ready"

syslib-clean:
	rm -rf $(SYSLIB_DIR)

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

runtime/libpthread.so: $(PTHREAD_SRC)
	mkdir -p runtime
	$(CC) $(CFLAGS) -fPIC $(CPPFLAGS) -D_GNU_SOURCE $(LDFLAGS) \
	    $(HOST_SO_LDFLAGS) -shared $(PTHREAD_SRC) -lpthread $(HOST_RT_LIBS) -o $@

runtime/libdl.so: trace.o src/linker/dlfcn.c src/linker/linker.c src/linker/linker_environ.c src/linker/rt.c src/linker/strlcpy.c
	mkdir -p runtime
	$(CC) $(CFLAGS) -fPIC $(CPPFLAGS) -D_GNU_SOURCE -DLINKER_DEBUG=1 -DRUNTIMEPATH='"$(PREFIX)$(LIBDIR)$(RUNTIMEDIR)"' \
	    -Wno-pedantic -Wno-variadic-macros -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-incompatible-pointer-types \
	    $(LDFLAGS) $(HOST_SO_LDFLAGS) -shared trace.o \
	    src/linker/dlfcn.c src/linker/linker.c src/linker/linker_environ.c src/linker/rt.c src/linker/strlcpy.c \
	    $(HOST_SYSTEM_DL_LIBS) -lpthread -o $@

runtime/libc.so: trace.o src/lib/libc.c src/lib/libc-ctype.h src/lib/libc-sysconf.h src/lib/libc-verbose.h
	mkdir -p runtime
	$(CC) $(CFLAGS) -fPIC -Wno-deprecated-declarations $(CPPFLAGS) -D_GNU_SOURCE \
	    $(HOST_LIBC_LDFLAGS) $(LDFLAGS) $(HOST_SO_LDFLAGS) -shared \
	    trace.o src/lib/libc.c \
	    $(HOST_LIBC_LIBS) -o $@

# Small Android API stubs: one driver (src/lib/stub.c), one -DLUNARIA_STUB_* per .so
STUB_SO = $(CC) $(CFLAGS) -fPIC $(CPPFLAGS) $(LDFLAGS) $(HOST_SO_LDFLAGS) -Isrc/lib -shared src/lib/stub.c

runtime/libandroid.so:
	mkdir -p runtime
	$(STUB_SO) -DLUNARIA_STUB_ANDROID -o $@ $(HOST_WINDOW_LIBS)

runtime/liblog.so:
	mkdir -p runtime
	$(STUB_SO) -DLUNARIA_STUB_LOG -o $@

runtime/libEGL.so:
	mkdir -p runtime
	$(STUB_SO) -DLUNARIA_STUB_EGL -D_GNU_SOURCE -o $@ $(HOST_GL_LIBS) $(HOST_WINDOW_LIBS)

runtime/libOpenSLES.so: trace.o
	mkdir -p runtime
	$(CC) $(CFLAGS) -Wno-pedantic -fPIC $(CPPFLAGS) $(LDFLAGS) $(HOST_SO_LDFLAGS) -Isrc/lib -shared trace.o \
	    src/lib/stub.c -DLUNARIA_STUB_OPENSLES -o $@

DVM_SRC = src/dvm/dex.c src/dvm/dvm.c src/dvm/dvm_runtime.c src/dvm/dvm_jni.c \
          src/dvm/dvm_net.c src/dvm/dvm_media.c src/dvm/regex.c
DVM_HDR = src/dvm/dex.h src/dvm/dvm.h src/dvm/dvm_internal.h src/dvm/dvm_jni.h \
          src/dvm/dvm_net.h src/dvm/dvm_media.h src/dvm/regex.h

# The Dalvik bytecode emulator lives in libjvm.so: it is reached from jvm.c
# (a JNI call with no host stub) and it calls back out through the same JNI
# table, so the two have to be in one object.
# luna_overlay.o is linked in here, not only into the executable: the widget
# layer inside the VM publishes the document, and the swap path in arm_exec
# presents it.  Both sides then resolve to the same single instance of the
# engine — two copies would each hold half of the state.
runtime/libjvm.so: trace.o src/jvm/jvm.c src/jvm/jni_stubs.c src/jvm/jvm.h src/jvm/jni.h $(DVM_SRC) $(DVM_HDR) luna_overlay.o luna_boot.o luna_ime.o
	mkdir -p runtime
	$(CC) $(CFLAGS) -fPIC $(CPPFLAGS) -D_GNU_SOURCE -Wno-pedantic $(LDFLAGS) $(HOST_SO_LDFLAGS) -shared \
	    trace.o src/jvm/jvm.c src/jvm/jni_stubs.c $(DVM_SRC) luna_overlay.o luna_boot.o luna_ime.o \
	    -lm $(HOST_CRYPTO_LIBS) $(HOST_ICU_LIBS) \
	    $(HOST_GL_LIBS) $(HOST_Z_LIBS) -o $@

runtime/libm.so:
	mkdir -p runtime
	$(STUB_SO) -DLUNARIA_STUB_MATH -D_GNU_SOURCE -o $@ $(HOST_DL_LIBS) -lm

runtime/libz.so:
	mkdir -p runtime
	$(STUB_SO) -DLUNARIA_STUB_ZLIB -o $@ $(HOST_DL_LIBS) -lz

runtime/libmediandk.so:
	mkdir -p runtime
	$(STUB_SO) -DLUNARIA_STUB_MEDIANDK -o $@

runtime/libGLESv3.so:
	mkdir -p runtime
	$(STUB_SO) -DLUNARIA_STUB_GLESV3 -D_GNU_SOURCE -o $@ $(HOST_GL_LIBS)

runtime/libvulkan.so:
	mkdir -p runtime
	$(STUB_SO) -DLUNARIA_STUB_VULKAN -o $@

# trick linker to link against unversioned libs
libdl.so: runtime/libdl.so
	ln -sfn runtime/libdl.so $@
libpthread.so: runtime/libpthread.so
	ln -sfn runtime/libpthread.so $@

# arm_exec.o: compiled with C++20 and dynarmic headers; linked into lunaria
arm_exec.o: src/arm_exec.cpp src/arm_exec.h src/arm.h src/jvm/jvm.h src/binary128.h $(DYNARMIC_LIB)
	$(CXX) -std=c++20 -O2 -g -fPIC \
	    $(DYNARMIC_INCS) \
	    $(CPPFLAGS) -D_GNU_SOURCE \
	    -c src/arm_exec.cpp -o $@

# Plain C11, and its own translation unit: the binary128 parser touches
# nothing else in the emulator.  Not macOS-only despite the reason it exists —
# AArch64 long double is binary128 on every host (see the file's header).
binary128.o: src/binary128.c src/binary128.h
	$(CC) -std=c11 -O2 -g -fPIC $(CPPFLAGS) -c src/binary128.c -o $@

# arm.o: code common to the ARM32 and ARM64 execution paths, C11.  Currently
# the ARM execution lock; anything else neither path owns alone belongs here
# rather than in a file of its own.
arm.o: src/arm.c src/arm.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -D_GNU_SOURCE -c src/arm.c -o $@

# stb_vorbis.c: Sean Barrett's single-file Ogg Vorbis decoder (public domain /
# MIT, vendored verbatim — see the file's own header for the license text).
# Host decode for FVorbisAudioInfo::ReadCompressedData/StreamCompressedData —
# see the SVC_STB_VORBIS_* block in arm_exec.cpp for why.  Its own warnings
# are not this build's to fix (upstream, unmodified).
stb_vorbis.o: src/lib/stb_vorbis.c
	$(CC) -std=c11 -O2 -g -fPIC $(CPPFLAGS) -D_GNU_SOURCE \
	    -Wno-unused-function -Wno-unused-variable -Wno-sign-compare \
	    -c src/lib/stb_vorbis.c -o $@

# loader.o: compiled as C11 (arm_exec.h is C-compatible)
loader.o: src/loader.c src/arm_exec.h src/arm.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -D_GNU_SOURCE -c src/loader.c -o $@

# luna_overlay.o: the emulator's own UI surface, backed by luna-ui.  Built with
# the project's own warning set relaxed — luna-ui.h is a 680 KB single-header
# library from another tree, and its diagnostics are not this build's to fix.
LUNA_UI_DIR ?= ../luna-ui
luna_overlay.o: src/luna_overlay.c src/luna_overlay.h src/luna_ime.h $(LUNA_UI_DIR)/luna-ui.h
	$(CC) -std=c11 -O2 -g -fPIC $(CPPFLAGS) -I$(LUNA_UI_DIR) -D_GNU_SOURCE \
	    -Wno-unused-function -Wno-unused-variable -Wno-sign-compare \
	    -c src/luna_overlay.c -o $@

# The boot card.  It calls luna-ui but does not define its implementation —
# luna_overlay.o is the one translation unit that does, and both land in
# libjvm.so, so the engine is linked once.
luna_ime.o: src/luna_ime.c src/luna_ime.h src/luna_overlay.h $(LUNA_UI_DIR)/luna-ui.h
	$(CC) -std=c11 -O2 -g -fPIC $(CPPFLAGS) -I$(LUNA_UI_DIR) -D_GNU_SOURCE \
	    -Wno-unused-function -Wno-unused-variable -Wno-sign-compare \
	    -c src/luna_ime.c -o $@

luna_boot.o: src/luna_boot.c src/luna_boot.h src/luna_overlay.h $(LUNA_UI_DIR)/luna-ui.h
	$(CC) -std=c11 -O2 -g -fPIC $(CPPFLAGS) -I$(LUNA_UI_DIR) -D_GNU_SOURCE \
	    -Wno-unused-function -Wno-unused-variable -Wno-sign-compare \
	    -c src/luna_boot.c -o $@

# lunaria: link with g++ so arm_exec.o (C++) and dynarmic (C++) are handled correctly
lunaria_os.o: $(LUNA_OS_SRC) src/lunaria_os.h
	$(CC) $(CFLAGS) $(CPPFLAGS) \
	    -c $(LUNA_OS_SRC) -o $@

lunaria: loader.o arm_exec.o binary128.o arm.o trace.o stb_vorbis.o $(LUNA_OS_OBJ) libdl.so libpthread.so \
       runtime/libpthread.so $(HOST_BIONIC_LIBC) \
       runtime/libandroid.so runtime/liblog.so \
       runtime/libEGL.so runtime/libOpenSLES.so \
       runtime/libjvm.so runtime/libm.so runtime/libz.so \
       runtime/libmediandk.so runtime/libGLESv3.so
lunaria: runtime/libvulkan.so
	$(CXX) -std=c++20 -O2 -g $(HOST_EXPORT) \
	    $(LUNARIA_LIBDIRS) $(HOST_RPATH) $(LDFLAGS) \
	    loader.o arm_exec.o binary128.o arm.o trace.o stb_vorbis.o $(LUNA_OS_OBJ) \
	    $(DYNARMIC_LIBS) \
	    $(HOST_DL_LIBS) -lpthread -ljvm \
	    $(HOST_WINDOW_LIBS) $(HOST_GL_LIBS) $(HOST_Z_LIBS) $(HOST_CRYPTO_LIBS) \
	    $(HOST_AUDIO_LIBS) -o $@

install-bin: $(bins)
	install -Dm755 $(bins) -t "$(DESTDIR)$(PREFIX)$(BINDIR)"

install-lib: $(libs)
	install -Dm755 $(libs) -t "$(DESTDIR)$(PREFIX)$(LIBDIR)$(RUNTIMEDIR)"

install: install-bin install-lib

clean:
	$(RM) $(bins) trace.o arm_exec.o binary128.o arm.o loader.o lunaria_os.o luna_overlay.o luna_boot.o \
	    stb_vorbis.o libdl.so libpthread.so
	$(RM) -r runtime
	$(RM) test/test_dynarmic_arm test/test_unity test/test_dvm test/dvm_test.dex test/test_regex
	$(RM) test/test_boot_card
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
test/test_dvm: test/dvm_test.c $(DVM_SRC) $(DVM_HDR) luna_overlay.o luna_boot.o
	$(CC) -std=c11 -g -O1 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -Isrc \
	    $(DVM_TEST_SAN) \
	    test/dvm_test.c $(DVM_SRC:src/dvm/dvm_jni.c=) luna_overlay.o luna_boot.o \
	    -lm $(HOST_CRYPTO_LIBS) $(HOST_Z_LIBS) $(HOST_SYSTEM_DL_LIBS) $(HOST_GL_LIBS) -o $@

# Pass a real classes.dex as DVM_DEX to also run every method in it.
DVM_DEX ?=
# The boot card, rendered headlessly so it can be looked at without waiting
# through a real title's boot.
# luna_ime.o comes along because the overlay presents through it: the input
# method is part of the surface now, not a separate layer.
test/test_boot_card: test/boot_card_test.c luna_overlay.o luna_boot.o luna_ime.o \
                     src/luna_overlay.h src/luna_boot.h src/luna_ime.h
	$(CC) -std=c11 -O2 -g $(CPPFLAGS) -D_GNU_SOURCE $(HOST_TEST_RPATH) \
	    test/boot_card_test.c luna_overlay.o luna_boot.o luna_ime.o \
	    $(HOST_GL_LIBS) -lm -o $@

boot-card-test: test/test_boot_card
	mkdir -p /tmp/lunaria-boot
	./test/test_boot_card 18 /tmp/lunaria-boot

test/test_regex: test/regex_test.c src/dvm/regex.c src/dvm/regex.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc/dvm -o $@ test/regex_test.c src/dvm/regex.c -lpthread

# The java.util.regex engine on its own: it depends on nothing from the VM, so
# its behaviour can be checked without booting one.
regex-test: test/test_regex
	./test/test_regex

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

ifeq ($(LUNA_OS_NAME),Darwin)
# Dynarmic's native arm64 backend emits A64 directly and does not build the
# x86-only Zydis disassembler archives.
DYNARMIC_LIBS = $(DYNARMIC_LIB) $(DYNARMIC_FMT_LIB) $(DYNARMIC_MCL_LIB)
else
DYNARMIC_LIBS = \
	$(DYNARMIC_LIB) $(DYNARMIC_FMT_LIB) $(DYNARMIC_MCL_LIB) \
	$(DYNARMIC_ZYD_LIB) $(DYNARMIC_ZYC_LIB)
endif

$(DYNARMIC_LIB):
	$(CMAKE) -B $(DYNARMIC_BUILD) -S $(DYNARMIC_DIR) \
	    -DDYNARMIC_WARNINGS_AS_ERRORS=OFF \
	    -DDYNARMIC_TESTS=OFF \
	    -DCMAKE_BUILD_TYPE=Release \
	    $(DYNARMIC_CMAKE_FLAGS) \
	    -DDYNARMIC_FRONTENDS="A32;A64" \
	    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	$(CMAKE) --build $(DYNARMIC_BUILD) -j$(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)

# These archives are emitted by the same CMake build.  Listing the relation
# keeps a clean tree buildable: otherwise make sees them as prerequisites of
# lunaria but has no rule with which to create them.
$(DYNARMIC_FMT_LIB) $(DYNARMIC_MCL_LIB): $(DYNARMIC_LIB)
ifneq ($(LUNA_OS_NAME),Darwin)
$(DYNARMIC_ZYD_LIB) $(DYNARMIC_ZYC_LIB): $(DYNARMIC_LIB)
endif

test/test_dynarmic_arm: test/test_dynarmic_arm.cpp $(DYNARMIC_LIB)
	$(CXX) -std=c++20 -O2 -g \
	    $(DYNARMIC_INCS) \
	    test/test_dynarmic_arm.cpp \
	    $(DYNARMIC_LIBS) \
	    -lpthread -o $@

test/test_binary128: test/binary128_test.cpp src/binary128.c src/binary128.h
	$(CC) -std=c11 -O2 -g -Isrc -c src/binary128.c -o test/binary128_c.o
	$(CXX) -std=c++20 -O2 -g -Isrc test/binary128_test.cpp test/binary128_c.o -o $@

binary128-test: test/test_binary128
	./test/test_binary128

test/test_os_event: test/os_event_test.c lunaria_os.o src/lunaria_os.h
	$(CC) -std=c11 -O2 $(CPPFLAGS) -Isrc test/os_event_test.c lunaria_os.o \
	    -Wl,-undefined,dynamic_lookup -lpthread -o $@

os-event-test: test/test_os_event
	./test/test_os_event

test/test_opensl_format: test/opensl_format_test.c src/opensl_format.h
	$(CC) -std=c11 -O2 -Wall -Wextra -Isrc $< -o $@

opensl-format-test: test/test_opensl_format
	./test/test_opensl_format

# Relink when dynarmic itself is rebuilt: it is linked in statically, so a
# fresh libdynarmic.a that nothing depends on leaves the old code in the
# binary.  This has to sit below the DYNARMIC_LIBS definition — make expands
# prerequisites as it reads the rule.
lunaria: $(DYNARMIC_LIBS)

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

fetch-openh264: $(OPENH264_SO)
	@printf 'openh264 ready: $(OPENH264_SO)\n'

$(OPENH264_SO):
	mkdir -p runtime
	curl -L --fail --retry 3 "$(OPENH264_URL)" -o $@.bz2
	bunzip2 -c $@.bz2 > $@
	$(RM) $@.bz2

# Aggregate: download all sample APKs used for development / regression.
fetch: fetch-libunity fetch-btw fetch-blade-soul fetch-openh264

.PHONY: all syslib syslib-clean host-all macos-deps x86 x86_64 armeabi armeabi-v7a armeabi-v7a-neon arm64-v8a \
	        clean install install-bin install-lib test net-test dvm-test regex-test abi-test \
	        posix-test boot-card-test binary128-test \
        fetch fetch-libunity fetch-btw fetch-blade-soul fetch-openh264 \
        dynarmic-build
