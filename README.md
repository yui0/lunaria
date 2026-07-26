**Lunaria — Translation layer for iOS/Android** 🎮🐧

**Run Android ARM/x86_64 JNI libraries (especially Unity's `libunity.so`) on Linux** using **dynarmic JIT** emulation! 🚀

[![Sponsor](https://img.shields.io/badge/Sponsor%20this%20project-%E2%9D%A4%EF%B8%8F-white?logo=githubsponsors&logoColor=EA4AAA&labelColor=EA4AAA)](https://github.com/sponsors/yui0)

---

### ✨ Features
- **Full ARM32 emulation** with dynarmic JIT
- **Unity & Mono** game support (tested with Unity 4.x/5.x/2023)
- **Unreal Engine 4** support (UE4 armeabi-v7a — FirstPersonExampleMap renders, 16 000+ frames stable)
- **Cooperative threading** (guest threads run smoothly)
- **AssetManager bridge** — reads assets directly from APK + OBB expansion files
- **OpenGL ES 3 + EGL** passthrough to host (23 GLES3 entry points, full per-thread context isolation)
- **Headless support** via Xvfb / llvmpipe
- **Memory-efficient** flat 4 GB guest address space with free-list `munmap`

---

### 🛠️ Build Instructions

**Required packages (Ubuntu/Debian):**
```bash
sudo apt install libglfw3-dev libegl1-mesa-dev libgles2-mesa-dev \
                 libbsd-dev libunwind-dev libboost-dev zlib1g-dev
```

```bash
make dynarmic-build     # Build dynarmic A32 JIT (requires cmake, libboost-dev)
make                    # Build lunaria binary + core runtime stubs
```

---

### 🎮 Test Games

#### FPSMobile (Unreal Engine 4 / armeabi-v7a) ✅

Source: [Abhishrut/UnrealEngineAndroidSamples](https://github.com/Abhishrut/UnrealEngineAndroidSamples)

Both the APK and its OBB expansion file must be placed in `test/`.
`lunaria-apk.sh` auto-detects the OBB when it is in the same directory as the APK.

```bash
# Download APK + OBB (≈139 MB total)
curl -L -o test/FPSMobile-armv7.apk \
  "https://raw.githubusercontent.com/Abhishrut/UnrealEngineAndroidSamples/main/FPSMobile-armv7.apk"
curl -L -o test/main.1.com.YourCompany.FPSMobile.obb \
  "https://raw.githubusercontent.com/Abhishrut/UnrealEngineAndroidSamples/main/main.1.com.YourCompany.FPSMobile.obb"

# Run
DISPLAY=:0 ./lunaria-apk.sh test/FPSMobile-armv7.apk
```

Expected output after ~60 s of startup:
```
[egl] swap#10 draws=+24 clears=+4 viewport=0,0,1280x720
```

![FPSMobile — FirstPersonExampleMap running on Lunaria](screenshot_fpsmobile.png)

*UE4 FirstPersonExampleMap rendered on Linux via Lunaria (swap #10, llvmpipe).*

---

#### Between Two Worlds (Unity 2023 IL2CPP / armeabi-v7a) ✅

Source: [ShutovKS/Between-two-worlds](https://github.com/ShutovKS/Between-two-worlds/releases/tag/1.0.5)

```bash
make fetch-btw          # Downloads test/btw-android.apk automatically

# Or manually:
curl -L -o test/btw-android.apk \
  "https://github.com/ShutovKS/Between-two-worlds/releases/download/1.0.5/Android_1.0.5.apk"

DISPLAY=:0 ./lunaria-apk.sh test/btw-android.apk
```

Status: playable — reaches the main story conversation scene.

---

#### Daggerfall Unity (Unity Mono / armeabi-v7a) 🔶

Source: [Vwing/daggerfall-unity-android](https://github.com/Vwing/daggerfall-unity-android/releases/tag/v1.1.1.8)

```bash
make fetch-libunity     # Downloads test/dfu-mono-32bit.apk automatically

# Or manually:
curl -L -o test/dfu-mono-32bit.apk \
  "https://github.com/Vwing/daggerfall-unity-android/releases/download/v1.1.1.8/dfu-mono-32bit-v1.1.1.8_mods-supported.apk"

DISPLAY=:0 ./lunaria-apk.sh test/dfu-mono-32bit.apk
```

Status: boots and loads the Mono runtime; rendering is currently blocked.

---

#### Generic Unity ARM32 APK

Any Unity armeabi-v7a APK can be run directly:

```bash
DISPLAY=:0 ./lunaria-apk.sh MyGame.apk
```

![Unity game running on Lunaria](screenshot_01.jpg)

*Unity ARM32 game running on Linux via Lunaria.*

---

### 🚀 Headless / Xvfb Mode

If there is no physical display, run with Xvfb (software renderer):

```bash
Xvfb :99 -screen 0 1280x720x24 &
DISPLAY=:99 ./lunaria-apk.sh test/FPSMobile-armv7.apk
```

Capture a screenshot at a specific swap index:
```bash
LUNARIA_DUMP_DIR=/tmp/shots LUNARIA_DUMP_FRAME=10 \
  DISPLAY=:99 ./lunaria-apk.sh test/FPSMobile-armv7.apk
# Output: /tmp/shots/frame10.ppm  — convert with ffmpeg -i frame10.ppm out.png
```

---

### ⚙️ Environment Variables

| Variable                   | Default  | Description |
|----------------------------|----------|-------------|
| `LUNARIA_CALL_TICKS`       | 2G       | Tick limit per JNI call |
| `LUNARIA_ONLOAD_TICKS`     | 5G       | Tick limit for `JNI_OnLoad` |
| `LUNARIA_THREAD_TICKS`     | 200M     | Tick slice per guest thread |
| `LUNARIA_TRACE_SVC`        | off      | Log SVC calls (first 1000) |
| `LUNARIA_TRACE_BLOCKS`     | 0        | Log JIT block execution |
| `LUNARIA_DUMP_FRAME`       | off      | Dump PPM at listed swap indices (`0,5,10`) |
| `LUNARIA_DUMP_DIR`         | `/tmp`   | Screenshot output directory |
| `LUNARIA_SCREENSHOT_EVERY` | off      | Dump every N swaps |
| `LUNARIA_WIDTH`/`HEIGHT`   | 1280×720 | Window / EGL surface size |
| `LUNARIA_TRACE_FUTEX`      | off      | Log futex wait/wake |
| `LUNARIA_TRACE_JITINIT`    | off      | Trace `mono_jit_init_version` |
| `GC_DONT_GC`               | 1        | Disable Boehm GC (set by loader) |

**Tip:** Tick counts accept `K`/`M`/`G` suffixes: `LUNARIA_ONLOAD_TICKS=5G`

---

### 🏗️ Architecture Overview

- **Flat 4 GB Memory** — Entire guest 32-bit address space is one `mmap` region (with fastmem)
- **SVC Trampolines** — All native calls (`libc`, `EGL`, `GLES3`, `JNI`, etc.) are routed through `SVC #n`
- **Cooperative Threads** — Guest `pthread_create` runs on an auxiliary JIT with round-robin scheduling
- **Relocations** — Full support for `R_ARM_RELATIVE`, cross-library symbols, non-zero ELF base
- **Asset Bridge** — `getAssets().open()` reads directly from APK (DEFLATE + STORE) and OBB files
- **Safety Guards** — Prevents wild jumps, infinite JIT growth, and memory corruption

---

### 📁 Project Structure (Key Files)

- `arm_exec.cpp` — Core ARM emulation engine (SVC dispatch, EGL/GLES3, JNI, threading)
- `lunaria-apk.sh` — APK/OBB launcher script
- `runtime/` — Host-side stub shared libraries loaded by the guest linker
- `test/` — Sample APKs and libraries (gitignored large files)

---

### 🎯 Supported Games

| APK | Engine | Status |
|-----|--------|--------|
| `FPSMobile-armv7.apk` | UE4 armeabi-v7a | ✅ FirstPersonExampleMap — 24 draw/4 clear/frame, 16 000+ swaps stable |
| `btw-android.apk` | Unity 2023 IL2CPP | ✅ Playable — main story scene reached |
| `dfu-mono-32bit.apk` | Unity Mono armeabi-v7a | 🔶 Boots, rendering blocked |
| Generic Unity ARM32 | Unity any | ✅ Generally supported |

---

### 🔧 Troubleshooting

- **UE4 "Project file not found"** — The OBB expansion file is missing. Place
  `main.1.com.YourCompany.FPSMobile.obb` in the same directory as the APK.
- **`runtime/libmediandk.so: No such file or directory`** — Run
  `make runtime/libmediandk.so runtime/libGLESv3.so` (not included in default `make`).
- **Black screen** — Try `LUNARIA_TRACE_EXC=1` or check that the OBB is present.
- **Slow rendering** — Expected on llvmpipe (software GL). Use a GPU-accelerated host for real-time speed.
- **Crashes** — Enable `LUNARIA_DUMP_LAST_SVC=1` and share the log.

---

**Made with ❤️ for the retro Android gaming & emulation community**

Enjoy running your favorite Android games natively on Linux! 🎉

---

*Project Lunaria © 2026 Yuichiro Nakada*
