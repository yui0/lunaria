# Lunaria

### Run Android game engines on Linux — without booting Android.

[![License: MPL 2.0](https://img.shields.io/badge/license-MPL--2.0-5b5bd6.svg)](LICENSE)
[![Guests](https://img.shields.io/badge/guest-ARM32%20%7C%20ARM64-20232a.svg)](#how-it-works)
[![Engines](https://img.shields.io/badge/engines-Unity%20%7C%20Unreal-20232a.svg)](#compatibility)
[![Sponsor](https://img.shields.io/badge/sponsor-%E2%99%A5-ea4aaa.svg)](https://github.com/sponsors/yui0)

Lunaria is an experimental Android-to-Linux translation layer. It loads native
libraries from an APK, XAPK or APKS, executes ARM32/ARM64 code through
[dynarmic](https://github.com/merryhime/dynarmic), and bridges Android APIs,
JNI, EGL, OpenGL ES and MediaCodec to the Linux host. The APK's own Java runs
through a built-in Dalvik bytecode emulator — no Android system image required.
`android.webkit.WebView` is drawn by a host browser over the Chrome DevTools
pipe: an installed Chrome or Chromium, or the built-in
[luna-browser](luna-browser/) (luna-ui and QuickJS).

> [!IMPORTANT]
> Lunaria is a compatibility project under active development, not a complete
> Android emulator. Support varies by title and engine version.

![Ni no Kuni: Cross Worlds intro, rendered through Lunaria](screenshot_crossworlds_intro.png)

<p align="center"><sub>Ni no Kuni: Cross Worlds (二ノ国) · UE4 · arm64-v8a · 1024×576 · guest framebuffer</sub></p>

## Why Lunaria

| | What it means |
|---|---|
| **Two guest architectures** | ARMv7 and AArch64 execution through dynarmic JIT |
| **No Android system image** | Launch an `.apk`, `.xapk` or `.apks` directly from Linux |
| **Real graphics path** | EGL and OpenGL ES 3 calls pass through to the host |
| **Engine-aware bridges** | JNI, AssetManager, OBB, pthread, OpenSL ES and Android API stubs |
| **Dalvik on the host** | APK `classes*.dex` run in `src/dvm/` when no host stub exists |
| **MediaCodec path** | H.264 via openh264, AAC via libavcodec — intro movies can finish |
| **WebView on the host** | Chrome/Chromium or luna-browser, same DevTools pipe, composited into the view |
| **A named device** | `lunaria.conf` reports a real retail profile (Pixel 6 by default) |
| **Parallel AArch64** | `LUNARIA_A64_ENGINES>1` runs guest workers on host threads |
| **Headless-capable** | Falls back to a surfaceless EGL pbuffer when no X11 window is available |
| **Built for diagnosis** | Frame capture, JIT profiling, SVC tracing and guest-memory watchpoints |

## Quick start

### 1. Install build dependencies

On Ubuntu or Debian:

```bash
sudo apt install \
  build-essential cmake pkg-config \
  libboost-dev libbsd-dev libunwind-dev \
  libglfw3-dev libegl1-mesa-dev libgles2-mesa-dev \
  libssl-dev libicu-dev zlib1g-dev
```

### 2. Build

```bash
make dynarmic-build
make x86_64 -j"$(nproc)"   # host is x86_64; plain `make` builds the 32-bit x86 ABI
```

`make` / `make x86_64` already pulls Cisco's openh264 shared library into
`runtime/` (required for MediaCodec H.264). Re-run explicitly with
`make fetch-openh264` if you need to refresh it.

### 3. Launch a package

```bash
./lunaria-apk.sh path/to/game.apk
./lunaria-apk.sh path/to/game.xapk
./lunaria-apk.sh path/to/game.apks
LUNARIA_ALSA_DEVICE="hw:7,0" ./lunaria-apk.sh Cross+Worlds_5.03.04_APKPure.xapk
```

The launcher detects `arm64-v8a` or `armeabi-v7a`, finds the main native
library, prepares an installed-package view, and starts the runtime. For XAPK
and APKS files it keeps the base APK intact and overlays split APK contents
into that temporary view. Unpacked trees are cached under
`${LUNARIA_CACHE_DIR:-/tmp/lunaria-cache}` keyed on the package identity;
`LUNARIA_NO_CACHE=1` forces a fresh unpack.

Settings that belong to the installation, not to one launch, live in
`lunaria.conf` (see `lunaria.conf.sample`): which device to report, where the
guest's `/data` lives (`LUNARIA_DATA_ROOT`), and which WebView engine to use.
A variable already set in the environment wins for that run.

Force a guest architecture when a package contains both:

```bash
LUNARIA_ARCH=armeabi-v7a ./lunaria-apk.sh game.apk
LUNARIA_ARCH=arm64-v8a  ./lunaria-apk.sh game.apk
```

## While it loads

A large title spends a long time between the launcher's last message and its
first frame — Blade & Soul Revolution links a 200 MB library, translates tens
of thousands of ARM blocks and compiles 28 MB of dex across four files before
the engine starts. Lunaria draws its own card over that gap: a sky, falling
snow, a progress ring, and the wordmark, with the current translation and dex
lines underneath. The card is a luna-ui document, so the wait shows what is
happening rather than a black rectangle.

![Lunaria's boot card](screenshot_lunaria_bootcard.png)

<p align="center"><sub>The progress ring and the wordmark, captured from the guest framebuffer</sub></p>

The card comes down the moment the guest takes the surface, and a guest dialog
always takes priority over it. `LUNARIA_JIT_UI=0` turns it off, and
`make boot-card-test` renders it on its own, without booting a title.

Past it, the title can reach its own UI:

![Blade & Soul Revolution rendering its own UI through Lunaria](screenshot_bladesoul_ui.png)

<p align="center"><sub>The client's own System dialog after the patch server
refuses the connection · layout, type and buttons are the game's</sub></p>

## Compatibility

These are observed milestones, not a general compatibility guarantee.
Details and launch recipes live in `PROGRESS.md`.

| Title | Engine / ABI | Current result |
|---|---|---|
| **Ni no Kuni: Cross Worlds** | Unreal Engine 4 · AArch64 XAPK | Linux keeps drawing through guest login, the village, story dialogue and in-world play. SharedPreferences persist. On macOS the framebuffer is drawn, but the real window stays black |
| **Blade & Soul Masia** | Unreal Engine 5 · AArch64 APKS | Opening movie reaches EOS; title screen; additional-patch dialog is readable and Agree advances the download |
| **Genshin Impact 7.0.0** | Unity IL2CPP · AArch64 XAPK | HoYoverse splash, then the login scene. A host WebView can open the account page. Sign-in is not finished: the region query still goes out without its signed parameters |
| **Between Two Worlds** | Unity 2023 IL2CPP · ARMv7 | Playable; reaches the main story scene |
| **Between Two Worlds** | Unity 2023 IL2CPP · AArch64 | Main menu on a recorded run; recent checks reach language selection at about 70 fps |
| **FPSMobile** | Unreal Engine 4 · ARMv7 | FirstPersonExampleMap renders; 16,000+ swaps observed |
| **UnitySampleGame** | Unity · ARMv7 | An earlier run reached the 3D scene. Current builds abort: `System.loadLibrary` re-enters the ARM32 JIT from inside an SVC |
| **TIME LOCKER** | Unity · ARMv7 | Reaches the portrait tutorial gameplay scene |
| **Black Clover: Asta Fight** | Unity IL2CPP · AArch64 XAPK | Base + split load; Unity splash renders headlessly |
| **Blade & Soul Revolution** | Unreal Engine 4 · AArch64 | Boots through dex / SDK setup into the game's own dialog; without a route to the patch server the client asks to reconnect |
| **Daggerfall Unity** | Unity Mono · ARMv7 | Mono runtime boots; rendering remains blocked |

<details>
<summary><strong>Run the open-source test titles</strong></summary>

### FPSMobile / Unreal Engine 4

Source: [Abhishrut/UnrealEngineAndroidSamples](https://github.com/Abhishrut/UnrealEngineAndroidSamples)

Place both files in `test/`; the launcher discovers a same-directory OBB
automatically.

```bash
curl -L -o test/FPSMobile-armv7.apk \
  "https://raw.githubusercontent.com/Abhishrut/UnrealEngineAndroidSamples/main/FPSMobile-armv7.apk"
curl -L -o test/main.1.com.YourCompany.FPSMobile.obb \
  "https://raw.githubusercontent.com/Abhishrut/UnrealEngineAndroidSamples/main/main.1.com.YourCompany.FPSMobile.obb"

LUNARIA_ARCH=armeabi-v7a ./lunaria-apk.sh test/FPSMobile-armv7.apk
```

### Between Two Worlds / Unity 2023 IL2CPP

Source: [ShutovKS/Between-two-worlds](https://github.com/ShutovKS/Between-two-worlds/releases/tag/1.0.5)

```bash
make fetch-btw

LUNARIA_ARCH=armeabi-v7a ./lunaria-apk.sh test/btw-android.apk
LUNARIA_ARCH=arm64-v8a  ./lunaria-apk.sh test/btw-android.apk
```

### Daggerfall Unity / Unity Mono

Source: [Vwing/daggerfall-unity-android](https://github.com/Vwing/daggerfall-unity-android/releases/tag/v1.1.1.8)

```bash
make fetch-libunity
./lunaria-apk.sh test/dfu-mono-32bit.apk
```

</details>

## Gallery — captured from Lunaria

These frames are read from Lunaria's guest framebuffer (GLFW window or headless
EGL) — not phone captures or Android emulator windows.

### Ni no Kuni: Cross Worlds · title to story

A commercial UE4 title on arm64-v8a: Guest login, server select, character
select, the starting village, story dialogue, then in-world play.
No APK or guest patches — emulator-side fixes only. Programmatic taps use
`LUNARIA_TOUCH_TEST`. A bare `x,y` is a percentage of the framebuffer
(`50,84` and `50%,84%` are the same point); `640px,606px` is a guest pixel.
`xdotool` synthetic clicks are ignored by GLFW. Framebuffer is **1024×576**.

<p align="center">
  <img src="screenshot_crossworlds_title.png" width="48%" alt="Cross Worlds title screen">
  &nbsp;
  <img src="screenshot_crossworlds_servers.png" width="48%" alt="Cross Worlds server select (Luxelion)">
</p>

<p align="center">
  <img src="screenshot_crossworlds_account.png" width="48%" alt="Cross Worlds account-link dialog">
  &nbsp;
  <img src="screenshot_crossworlds_character_select.png" width="48%" alt="Cross Worlds character select">
</p>

<p align="center">
  <img src="screenshot_crossworlds_village.png" width="48%" alt="Cross Worlds starting village">
  &nbsp;
  <img src="screenshot_crossworlds_ingame.png" width="48%" alt="Cross Worlds in-world play, quests and HUD">
</p>

<p align="center">
  <img src="screenshot_crossworlds_dialog.png" width="48%" alt="Cross Worlds story dialogue (Chloe)">
  &nbsp;
  <img src="screenshot_crossworlds_evermore.png" width="48%" alt="Cross Worlds arrival at Evermore">
</p>

<p align="center">
  <img src="screenshot_crossworlds_power_save.png" width="72%" alt="Cross Worlds power-save screen, resting">
</p>

<p align="center"><sub>Title · server · account · character select · village · in-world · dialogue · Evermore · the client's own rest screen</sub></p>

### Genshin Impact · splash, then the login scene

![Genshin Impact login scene rendered through Lunaria](screenshot_genshin_login.png)

<p align="center"><sub>Unity IL2CPP · arm64-v8a · OSREL Android 7.0.0 · the login scene after the splash</sub></p>

![Genshin Impact HoYoverse splash rendered through Lunaria](screenshot_genshin_splash.png)

<p align="center"><sub>The HoYoverse splash, the first guest frame after GLES 3.2 init</sub></p>

### Between Two Worlds · main menu

![Between Two Worlds main menu running through Lunaria](screenshot_btw_menu.png)

<p align="center"><sub>Unity 2023 IL2CPP · 1280×720</sub></p>

### UnitySampleGame · 3D gameplay

An earlier run reached the title screen, injected a touch on **Start**, and
entered the playable 3D scene: character, crystal objective, health HUD and
touch controls at 1280×720. Current builds stop earlier, on a Dynarmic
re-entry while a class initializer loads a native library.

![UnitySampleGame 3D gameplay running through Lunaria](screenshot_unitysample_gameplay.png)

### TIME LOCKER · tutorial gameplay

The portrait build advances beyond the Unity logo into the first interactive
scene. The player character, score HUD, playfield, and touch tutorial are
rendered at 720×1280.

<p align="center">
  <img src="screenshot_timelocker_gameplay.png" width="420" alt="TIME LOCKER tutorial gameplay running through Lunaria">
</p>

### Black Clover: Asta Fight · splash

<p align="center">
  <img src="screenshot_blackclover_splash.png" width="480" alt="Black Clover Unity splash through Lunaria">
</p>

### FPSMobile · Unreal sample

![Unreal Engine 4 FirstPersonExampleMap running through Lunaria](screenshot_fpsmobile_map.png)

<p align="center"><sub>UE4 FirstPersonExampleMap · armeabi-v7a · 1280×720 · Mesa llvmpipe</sub></p>

### The host menu

Right-click opens Lunaria's own menu, drawn by luna-ui over the guest and over
the boot card. From here: a screenshot (also F12), paste-on-type, Back, volume
and mute, WebView zoom and engine, reload, full screen, quit.

<p align="center">
  <img src="screenshot_host_menu.png" width="480" alt="Lunaria host menu with the WebView engine submenu open">
</p>

<p align="center"><sub>WebView engine set to luna-browser · zoom 200% · mute on</sub></p>

## Capture frames

Capture selected swap indices:

```bash
mkdir -p /tmp/lunaria-shots
LUNARIA_DUMP_DIR=/tmp/lunaria-shots \
LUNARIA_DUMP_FRAME=0,60,120 \
./lunaria-apk.sh game.apk
```

Or capture every *N* swaps:

```bash
LUNARIA_DUMP_DIR=/tmp/lunaria-shots \
LUNARIA_SCREENSHOT_EVERY=60 \
./lunaria-apk.sh game.xapk
```

Frames are written as PNG (`lunaria_0000.png` in that directory). Creating
`$LUNARIA_SHOT_TRIGGER` (default `/tmp/lunaria-shot`) dumps one more frame.
F12, and **Take Screenshot** in the host menu, write
`~/Pictures/Lunaria YYYY-MM-DD HH.MM.SS.png` instead.

## How it works

```text
 APK / XAPK / APKS
     │  extract base, splits, native libraries, assets and OBB
     ▼
 Android ELF loader ─── relocations and dependency resolution
     │
     ▼
 dynarmic JIT ───────── ARMv7 or AArch64 guest instructions
     │
     ├── SVC bridge ─── libc · pthread · filesystem · Android APIs · sockets
     ├── JNI / DVM ─── classes · methods · AssetManager · MediaCodec · dex
     ├── WebView ────── Chrome DevTools pipe · Chrome/Chromium or luna-browser
     └── graphics ───── EGL · OpenGL ES 3 · host Mesa / GPU
```

- **ARM32:** a flat 4 GiB guest memory window with fastmem.
- **ARM64:** guest virtual addresses map one-to-one to host addresses; a
  high-address image window holds loaded ELFs, trampolines, JNI tables and
  stacks, enabling dynarmic fastmem. `LUNARIA_A64_ENGINES` (1–8; defaults from
  host core count) can run multiple JIT engines on host threads. By default
  the frame pump barriers engines at pass boundaries; `LUNARIA_A64_SELF_SCHED=1`
  lets engines pull runnable guests freely (breaks some anti-cheat / timing
  checks — off by default).
- **Native calls:** guest libc, EGL, GLES and JNI calls cross generated
  `SVC #n` trampolines into host implementations.
- **Threads:** guest pthreads use cooperative round-robin scheduling with a
  separate JIT context per worker; mutex unlock can hand off directly to a
  waiter. Optional parking (`LUNARIA_GUEST_SLEEP`, `LUNARIA_FD_PARK`) is
  implemented but off by default — enabling it changes wake timing that some
  titles measure.
- **Java:** when a JNI call has no host stub, `src/dvm/` executes the method
  from the APK's `classes*.dex` (`LUNARIA_DVM=1` by default).
- **Assets:** `AssetManager` reads DEFLATE/STORE entries from APKs and OBB
  expansion files.
- **Media:** `android.media.MediaCodec` decodes H.264 with openh264 and AAC
  with libavcodec when available (`LUNARIA_OPENH264` / `LUNARIA_LIBAVCODEC`
  override the shared-library paths).
- **WebView:** `android.webkit.WebView` talks to a host browser over the
  DevTools pipe. `LUNARIA_WEB_ENGINE` is `auto` (Chrome or Chromium if one is
  installed, otherwise luna-browser), `chrome`, `luna`, or `off`.
  `LUNARIA_WEB_BROWSER` names a binary directly. The page is shown as an image
  inside the view; touch and text go back over the same pipe. Build the
  fallback with `make luna-browser`.

## Runtime controls

Common controls are listed here; the source contains additional narrow
diagnostic switches used during compatibility work. See `PROGRESS.md` for the
full diagnostic set.

| Variable | Default | Purpose |
|---|---:|---|
| `LUNARIA_ARCH` | auto | `armeabi-v7a` or `arm64-v8a` |
| `LUNARIA_WIDTH` / `LUNARIA_HEIGHT` | `1024` / `768` (landscape app: `768`/`1024` swapped) | Window or EGL surface size — see `LUNARIA_SCALE` for scaling the device panel instead |
| `LUNARIA_PBUFFER` | auto fallback | Set `1` to skip GLFW and force headless EGL |
| `LUNARIA_MAX_FRAMES` | unlimited | Stop the render loop after *N* frames |
| `LUNARIA_MEM_TOTAL_MB` | `6144` | RAM reported to the guest |
| `LUNARIA_HEAP_MB` | A32 `256` / A64 window (~`2560`) | Guest malloc arena; A64 defaults to the full `[HEAP_BASE, MMAP2)` window |
| `LUNARIA_THREAD_TICKS` | `200M` | ARM32 worker scheduling slice |
| `LUNARIA_A64_THREAD_TICKS` | `20K` | AArch64 worker scheduling slice |
| `LUNARIA_A64_ENGINES` | auto | Host threads for AArch64 JIT engines (1–8; defaults to 4 / 2 / 1 from host core count) |
| `LUNARIA_A64_SELF_SCHED` | off | Engines pull runnable guests freely instead of barrier-pooled passes |
| `LUNARIA_A64_FASTMEM` | `1` | Set `0` to route memory through callbacks |
| `LUNARIA_A64_CODE_CACHE_MB` | `128` | Per-JIT translated-code cache |
| `LUNARIA_GUEST_SLEEP` / `LUNARIA_FD_PARK` | on / off | Park guest sleeps for real wall time / park blocking reads (`LUNARIA_GUEST_SLEEP=0` is diagnostic only) |
| `LUNARIA_TOUCH_TEST` | off | Inject taps at `x,y[;x,y…]` (max 8). Bare numbers are framebuffer percentages; `640px` is a guest pixel |
| `LUNARIA_TOUCH_FIFO` | off | Same taps while running: `echo 'x,y[,hold]' > $LUNARIA_TOUCH_FIFO` |
| `LUNARIA_DEVICE` | `pixel6` | Device profile: `pixel6`, `pixel7`, `galaxys21`, `lunaria`. Prefer `lunaria.conf` |
| `LUNARIA_DATA_ROOT` | launcher directory | Guest `/data` and `/storage`. A large title's downloads live here |
| `LUNARIA_CONF` | `./lunaria.conf`, then the launcher directory, then `~/.config/lunaria/lunaria.conf` | Which settings file to read. Environment variables win |
| `LUNARIA_WEB_ENGINE` | `auto` | `auto`, `chrome`, `luna`, or `off`. The host menu overrides it for the session |
| `LUNARIA_SCALE` | off | Scale the selected device's own panel (`0.5` is half). Unset, the surface is `LUNARIA_WIDTH` × `LUNARIA_HEIGHT` |
| `LUNARIA_TOUCH_FRAME` / `_HOLD` / `_GAP` | `60` / `10` / `60` | First DOWN frame / hold / gap between taps |
| `LUNARIA_PERF_S` | `10` | `[perf]` interval seconds; `0` disables |
| `LUNARIA_DUMP_FRAME` | off | Comma-separated swap indices to capture |
| `LUNARIA_SCREENSHOT_EVERY` | off | Capture every *N* swaps |
| `LUNARIA_DUMP_DIR` | `/tmp` | Frame output directory |
| `LUNARIA_TRACE_SVC` | off | Trace guest-to-host SVC calls |
| `LUNARIA_TRACE_HEAP` | off | Diagnose corrupt guest malloc free-list entries |
| `LUNARIA_HEAP_POISON` | off | Fill non-calloc bump allocations with `0xFF` to expose uninitialized reads |
| `LUNARIA_HEAP_SELFTEST` | off | Verify alignment, calloc reuse, trim, coalescing, realloc growth and memalign before loading guest code |
| `LUNARIA_TRACE_MEDIA` | off | MediaCodec state transitions |
| `LUNARIA_A64_PCPROF` | off | Sample and report hot AArch64 guest PCs, per thread |
| `LUNARIA_A64_PCPROF_EVERY` | `20000` | Samples between profiler reports |
| `LUNARIA_SCHED_DUMP` | off | Dump every guest thread's state every *N* scheduler passes |
| `LUNARIA_SCHED_DUMP_STACK` | off | Add a return-address scan of each thread's stack to that dump |
| `LUNARIA_DUMP_LAST_SVC` | off | Print the recent SVC ring at shutdown |
| `LUNARIA_JIT_UI` | `1` | Boot card (moon, translation and dex progress); `0` leaves the surface blank until the guest draws |
| `LUNARIA_DVM` | `1` | Dalvik bytecode emulator: `0` off, `1` run the APK's dex only where no host stub exists, `2` prefer the dex over host stubs |
| `LUNARIA_DVM_TRACE` | off | Log the methods the emulator declined (`[dvm] miss …`) |
| `LUNARIA_DEX_START` | on | Run the APK's Activity lifecycle from dex; set to `0` only to compare with the legacy hand-written startup sequence |
| `LUNARIA_NET` | on | Set `0` to deny all guest sockets |
| `LUNARIA_CACHE_DIR` | `/tmp/lunaria-cache` | Persistent unpack cache for the launcher |
| `LUNARIA_NO_CACHE` | off | Force a fresh APK/XAPK/APKS unpack |

An APK with no engine entry point (no `ANativeActivity_onCreate`, no
`UnityPlayer.initJni`) is now started from its launcher Activity instead of
being rejected: `lunaria-apk.sh` reads it out of the manifest and passes it as
`ANDROID_LAUNCH_ACTIVITY`, and the loader runs its `onCreate`/`onStart`/
`onResume` through the bytecode VM.

Tick values accept `K`, `M`, and `G` suffixes, for example
`LUNARIA_ONLOAD_TICKS=5G`.

## Repository map

| Path | Role |
|---|---|
| `src/arm_exec.cpp` | dynarmic engines, SVC dispatch, JNI, EGL/GLES and threading |
| `src/arm.c` / `src/arm.h` | shared ARM execution lock and multi-engine helpers |
| `src/dvm/` | Dalvik bytecode emulator: runs the APK's own Java from classes*.dex |
| `src/loader.c` | runtime entry point and Unity render-loop orchestration |
| `src/linker/` | Android ELF linker adapted for the host |
| `src/jvm/` | lightweight JVM/JNI object model and stubs |
| `src/lib/` | host implementations exposed to Android native code |
| `src/luna_overlay.c` | the emulator's own UI surface: boot card, host menu, and compositing over the guest frame |
| `src/luna_boot.c` | the boot card: sky, snow, progress ring, wordmark, translation and dex lines |
| `src/luna_ime.c` | host IME bridge into guest text input |
| `src/webview_cdp.c` | DevTools pipe client shared by Chrome and luna-browser |
| `luna-browser/` | the built-in WebView engine (luna-ui + QuickJS); `make luna-browser` |
| `lunaria.conf.sample` | device profile, data root, screen scale, package ledger, WebView engine |
| `runtime/` | generated Android-compatible host shared libraries |
| `lunaria-apk.sh` | APK/XAPK/APKS inspection, extraction and launch pipeline |
| `PROGRESS.md` | detailed compatibility notes and current engineering work |

## Troubleshooting

<details>
<summary><strong>The startup movie does not play</strong></summary>

Lunaria decodes H.264 with openh264, which reads Constrained Baseline. A
title's splash movies are routinely Main or High profile with CABAC and
B-frames — Blade & Soul Revolution's are — and those cannot be decoded here.

The emulator detects that (sixteen access units with no picture) and reports
`MEDIA_ERROR_UNSUPPORTED` to the guest, so the engine skips the movie and
carries on booting. It used to go on "playing" a movie that would never
produce a frame, and a title waiting on that movie waited for ever behind a
white screen.

Titles whose streams *are* Constrained Baseline (for example Blade & Soul
Masia's opening) reach `BUFFER_FLAG_END_OF_STREAM` when openh264 is present
(`make fetch-openh264`). AAC audio uses libavcodec when that library is
available on the host. Use `LUNARIA_TRACE_MEDIA=1` to watch codec state.

</details>

<details>
<summary><strong>The game reports “project file not found”</strong></summary>

The title probably expects an OBB expansion file. Put it beside the APK using
its original name.

</details>

<details>
<summary><strong>A runtime stub library is missing</strong></summary>

Build the requested targets explicitly:

```bash
make runtime/libmediandk.so runtime/libGLESv3.so
```

</details>

<details>
<summary><strong>The screen is black or startup stalls</strong></summary>

Check the OBB/split files first, then collect a focused trace:

```bash
LUNARIA_TRACE_EXC=1 LUNARIA_DUMP_LAST_SVC=1 \
./lunaria-apk.sh game.apk 2>&1 | tee lunaria.log
```

For UE5 titles, prefer a hardware GL driver. `LIBGL_ALWAYS_SOFTWARE=1`
(llvmpipe) can make the same boot path an order of magnitude slower and look
like an emulator hang when it is only the software rasterizer.

</details>

<details>
<summary><strong>Rendering is slow</strong></summary>

Mesa llvmpipe is a software renderer and is useful for headless validation,
not performance testing. Use a GPU-accelerated host EGL/OpenGL stack for
real-time rendering. For AArch64 titles, try `LUNARIA_A64_ENGINES=4` once
the single-engine path is stable; leave `LUNARIA_SLICE_DETAIL` /
`LUNARIA_SVC_HISTO` unset when measuring speed.

</details>

<details>
<summary><strong>Clicks do nothing</strong></summary>

GLFW ignores `xdotool` / `XSendEvent` synthetic clicks. Use
`LUNARIA_TOUCH_TEST='50,84;…'` (percent of the framebuffer; add `px` for
guest pixels), `LUNARIA_TOUCH_FIFO` once the title is up, or click inside the
real GLFW window. Right-click opens the host menu and does not go to the guest.

</details>

## Project status

Lunaria is research software. Linux is the host where a window and the
framebuffer agree. macOS builds (`make dist-mac`) can fill the framebuffer
while the on-screen window stays black. The Windows OS layer exists; a Windows
binary does not, because the JIT still calls `mmap` directly.
`make dist` packs a Linux tree that does not depend on the build directory.
Expect incomplete Android APIs, title-specific issues, heavy diagnostic
output, and breaking changes. Contributions are most useful when they include
the title, ABI, engine version, last successful milestone, log, and a captured
frame.

Licensed under the [Mozilla Public License 2.0](LICENSE).

Project Lunaria © 2026 Yuichiro Nakada.
