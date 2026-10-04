# Lunaria

### Android games, on the computer you already have.

[![License: MPL 2.0](https://img.shields.io/badge/license-MPL--2.0-5b5bd6.svg)](LICENSE)
[![Guests](https://img.shields.io/badge/guest-ARM32%20%7C%20ARM64-20232a.svg)](#how-it-works)
[![Engines](https://img.shields.io/badge/engines-Unity%20%7C%20Unreal-20232a.svg)](#compatibility)
[![Sponsor](https://img.shields.io/badge/sponsor-%E2%99%A5-ea4aaa.svg)](https://github.com/sponsors/yui0)

[日本語](README.ja.md)

Lunaria is an Android translation layer for the desktop. Point it at an `.apk`,
`.xapk`, or `.apks`. It runs the ARM32 or ARM64 code with
[dynarmic](https://github.com/merryhime/dynarmic), and answers JNI, EGL,
OpenGL ES, and MediaCodec on the host. The package's own Java runs in a
built-in Dalvik interpreter. The same binary unpacks the archive, reads the
manifest, overlays split APKs, and keeps the install cache and account
profiles (`src/apk.c`). An Android system image is not required.

`android.webkit.WebView` is drawn by a host browser over the Chrome DevTools
pipe — Chrome or Chromium when one is installed, otherwise the built-in
[luna-browser](luna-browser/) (luna-ui and QuickJS).

> [!IMPORTANT]
> Lunaria is a compatibility experiment, and the code is still changing. What
> reaches the screen depends on the title and the engine version. The frames
> below come from runs that actually got there.

<p align="center">
  <img src="shots/genshin-mondstadt.png" width="100%" alt="Genshin Impact: Lumine gliding over Mondstadt, twin tornadoes beside the cathedral">
</p>

<p align="center">
  <img src="shots/genshin-lakeshore.png" width="32%" alt="Genshin Impact open world: the Traveler at a lakeshore, autumn trees, full HUD">
  &nbsp;
  <img src="shots/genshin-jean.png" width="32%" alt="Genshin Impact: Jean in the Knights' library, Lisa at the desk behind her">
  &nbsp;
  <img src="shots/cross-worlds-cutscene.png" width="32%" alt="Ni no Kuni: Cross Worlds, a voiced cutscene against a bright sky">
</p>

<p align="center"><sub>Over Mondstadt · the lakeshore · the Knights' library · a scene from Ni no Kuni: Cross Worlds<br>
Genshin Impact 7.1.0, Unity IL2CPP, arm64-v8a. Cross Worlds, Unreal Engine 4, arm64-v8a.<br>
Guest framebuffer on Linux.</sub></p>

## Why Lunaria

| | What it means |
|---|---|
| **Two guest architectures** | ARMv7 and AArch64, compiled as they run by dynarmic |
| **The package is the whole install** | `.apk`, `.xapk`, and `.apks` launch from the binary itself |
| **The host GPU** | EGL and OpenGL ES 3 go straight through. Vulkan is advertised by default when the host bridge is available |
| **The pieces engines expect** | JNI, AssetManager, OBB, pthread, OpenSL ES, and Android API stubs |
| **The APK's own Java** | `classes*.dex` runs in `src/dvm/` wherever a host stub is missing |
| **Movies that can finish** | H.264 through openh264, AAC through libavcodec |
| **A real browser for WebView** | Chrome, Chromium, or luna-browser, composited into the guest view |
| **A named phone** | `lunaria.conf` reports a retail profile. Pixel 6 is the default |
| **More than one JIT** | `LUNARIA_A64_ENGINES` spreads AArch64 workers across host threads |
| **A keyboard on a touch HUD** | Bundled maps for Genshin and Cross Worlds: WASD, skills, and mouse-look together |
| **Settings that come back** | Login, server, and a changed music volume are still there after a restart |
| **More than one account** | `--profile` keeps preferences and login apart. The installed package stays shared |
| **A window, or none** | With no X11 display, Lunaria falls back to a surfaceless EGL pbuffer |
| **A way to see why** | Frame capture, JIT profiles, SVC traces, and guest-memory watchpoints |

## Gallery

Every picture is Lunaria's guest framebuffer, taken from the GLFW window or
from headless EGL.

### Genshin Impact 7.1.0

Unity IL2CPP on arm64-v8a, framebuffer **1024×576**. On 1 October 2026 a
saved login and the Asia server came back, shader compile and the resource
download finished, and the open world drew. The prologue, the Knights'
library, and flight over Mondstadt are in color. Keyboard movement works in
the field. On the machine used for that measurement, a Xeon E5-2697 v2, the
field held about 18–22 fps and the settings menu about 30 fps. A music-volume
change was still there after a restart. The account page can open in the
host WebView. Google Play billing is not implemented.

<p align="center">
  <img src="shots/genshin-wings.png" width="48%" alt="Genshin Impact: Lumine with glider wings open against a blue sky">
  &nbsp;
  <img src="shots/genshin-lumine.png" width="48%" alt="Genshin Impact close-up: Lumine, asking whether you are a new ally or a new storm">
</p>

<p align="center">
  <img src="shots/genshin-prologue.png" width="48%" alt="Genshin Impact prologue: Lumine, Paimon, and a floating companion around a glowing red stone">
  &nbsp;
  <img src="shots/genshin-paimon.png" width="48%" alt="Genshin Impact prologue: Paimon pointing, Lumine watching">
</p>

<p align="center">
  <img src="shots/genshin-knights.png" width="48%" alt="Genshin Impact: the Traveler at the Knights of Favonius gate, Kaeya and Amber nearby">
  &nbsp;
  <img src="shots/genshin-shore.png" width="48%" alt="Genshin Impact: the Traveler standing in the water outside Mondstadt at dusk">
</p>

<p align="center">
  <img src="shots/genshin-swim.png" width="100%" alt="Genshin Impact: swimming at sunset outside Mondstadt, the quest marker still pointing toward the city">
</p>

<p align="center"><sub>Wings open · the question · the prologue · the gate · the shore · the swim at dusk</sub></p>

<details>
<summary>Login, download, and settings</summary>

<p align="center">
  <img src="shots/genshin-shaders.png" width="48%" alt="Genshin Impact shader-compile cinematic, a colonnade under a blue sky">
  &nbsp;
  <img src="shots/genshin-tap.png" width="48%" alt="Genshin Impact title gate at sunset, TAP TO BEGIN">
</p>

<p align="center">
  <img src="shots/genshin-download.png" width="48%" alt="Genshin Impact downloading an update over the night colonnade">
  &nbsp;
  <img src="shots/genshin-login.png" width="48%" alt="Genshin Impact login scene, night sky over the cloud bridge">
</p>

<p align="center">
  <img src="shots/genshin-server.png" width="48%" alt="Genshin Impact server select, Asia checked">
  &nbsp;
  <img src="shots/genshin-library.png" width="48%" alt="Genshin Impact: Jean in profile beside the library clock">
</p>

<p align="center">
  <img src="shots/genshin-language.png" width="72%" alt="Genshin Impact language settings in Japanese">
</p>

<p align="center"><sub>Shader compile · TAP TO BEGIN · download · login · server · library · language</sub></p>

Earlier frames from the same path are kept beside these:
[`genshin-compile.png`](shots/genshin-compile.png),
[`genshin-compile-early.png`](shots/genshin-compile-early.png),
[`genshin-loading.png`](shots/genshin-loading.png),
[`genshin-begin.png`](shots/genshin-begin.png),
[`genshin-elements.png`](shots/genshin-elements.png),
[`genshin-splash.png`](shots/genshin-splash.png),
[`genshin-mail.png`](shots/genshin-mail.png),
[`genshin-chat.png`](shots/genshin-chat.png),
[`genshin-field.png`](shots/genshin-field.png).

</details>

### Ni no Kuni: Cross Worlds

A commercial Unreal Engine 4 game on arm64-v8a. The run that produced these
frames went through guest login, server select, character select, the
starting village, story dialogue with Chloe, a voiced cutscene, and play in
the field. The title on screen is 5.03.04. A later package, 5.04.12, still
draws through to the game. The framebuffer is **1024×576**.

<p align="center">
  <img src="shots/cross-worlds-village.png" width="48%" alt="Cross Worlds starting village, a teal-roofed house under a broad tree">
  &nbsp;
  <img src="shots/cross-worlds-play.png" width="48%" alt="Cross Worlds in the field, quests and the touch HUD up">
</p>

<p align="center">
  <img src="shots/cross-worlds-chloe.png" width="48%" alt="Cross Worlds story dialogue with Chloe, autumn trees behind her">
  &nbsp;
  <img src="shots/cross-worlds-characters.png" width="48%" alt="Cross Worlds character select">
</p>

<p align="center"><sub>The first village · the field · Chloe · character select</sub></p>

<details>
<summary>Title, servers, and the road in</summary>

<p align="center">
  <img src="shots/cross-worlds-title.png" width="48%" alt="Cross Worlds title, Luxelion selected, snowy peaks behind the logo">
  &nbsp;
  <img src="shots/cross-worlds-servers.png" width="48%" alt="Cross Worlds server select, Luxelion">
</p>

<p align="center">
  <img src="shots/cross-worlds-intro.png" width="48%" alt="Cross Worlds 3D intro">
  &nbsp;
  <img src="shots/cross-worlds-evermore.png" width="48%" alt="Cross Worlds, arrival at Evermore">
</p>

<p align="center">
  <img src="shots/cross-worlds-account.png" width="48%" alt="Cross Worlds account-link dialog">
  &nbsp;
  <img src="shots/cross-worlds-rest.png" width="48%" alt="Cross Worlds power-save screen, the character resting">
</p>

</details>

Programmatic taps use `LUNARIA_TOUCH_TEST`. A bare `x,y` is a percentage of
the framebuffer (`50,84` and `50%,84%` are the same point); `640px,606px` is
a guest pixel. GLFW ignores `xdotool` synthetic clicks.

### Open-source and smaller titles

<p align="center">
  <img src="shots/between-two-worlds.png" width="48%" alt="Between Two Worlds main menu, Continue, New Game, Load, Exit">
  &nbsp;
  <img src="shots/unreal-first-person.png" width="48%" alt="Unreal Engine 4 First Person map, a pistol and grey blocks">
</p>

<p align="center"><sub>Between Two Worlds · Unity 2023 IL2CPP · 1280×720 &nbsp;·&nbsp; the Unreal first-person sample · armeabi-v7a · Mesa llvmpipe</sub></p>

<p align="center">
  <img src="shots/unity-sample.png" width="48%" alt="Unity sample game, 3D gameplay">
  &nbsp;
  <img src="shots/time-locker.png" width="28%" alt="TIME LOCKER portrait tutorial">
</p>

<p align="center"><sub>An earlier Unity sample run that reached the playable scene, 1280×720. Current builds stop before this. &nbsp;·&nbsp; TIME LOCKER tutorial, 720×1280</sub></p>

<p align="center">
  <img src="shots/black-clover.png" width="360" alt="Black Clover: Asta Fight, the Unity splash">
</p>

<p align="center"><sub>Black Clover: Asta Fight · the Unity splash, headless</sub></p>

### Lunaria itself

A large title spends a long time linking, translating, and compiling dex
before the first guest frame. The boot card is Lunaria's own surface for
that wait. Once the guest draws, right-click opens the host menu over the
frame: screenshot (also F12), paste-on-type, Back, volume and mute, ALSA
output, the keyboard map, WebView zoom and engine, reload, full screen, quit.

<p align="center">
  <img src="shots/boot.png" width="48%" alt="Lunaria boot card, a progress ring at 95 percent over a blue sky, the word LUNARIA below">
  &nbsp;
  <img src="shots/host-menu.png" width="42%" alt="Lunaria host menu, WebView engine set to luna-browser, zoom 200 percent, mute on">
</p>

<p align="center"><sub>The boot card · the host menu</sub></p>

## Quick start

### 1. Install build dependencies

On Ubuntu or Debian:

```bash
sudo apt install \
  build-essential cmake pkg-config \
  libboost-dev libbsd-dev libunwind-dev \
  libglfw3-dev libegl1-mesa-dev libgles2-mesa-dev \
  libssl-dev zlib1g-dev \
  libasound2-dev libvulkan-dev
```

`libvulkan-dev` supplies the headers for the bridge. The bridge loads the
host's Vulkan library by default; `LUNARIA_VULKAN=0` disables it.

On Apple Silicon macOS, run `make` in this directory to fetch the pinned
ANGLE, MoltenVK, and OpenH264 dependencies and build. Launch an XAPK with
`./lunaria-apk.sh /path/to/game.xapk`.

For Windows x86_64, use the [MSYS2 UCRT64 shell](https://www.msys2.org/docs/environments/):

```bash
pacman -S --needed git make zip unzip bzip2 \
  mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,python,boost,glfw,angleproject,openssl,zlib,libgnurx,vulkan-headers}
```

Set `ANDROID_HOME` to an Android SDK containing Build Tools' RenderScript
arm64 libraries, and `ANDROID_NDK_HOME` to its NDK directory. Then:

```bash
make -j4 dist CC=gcc CXX=g++ DYNARMIC_JOBS=4
```

Windows links the JVM, Dalvik, UI and Android host APIs into `lunaria.exe`.
On Linux, run `make -j4 dist-cross-win` to build the same Windows ZIP.
The first run downloads MSYS2 UCRT64 dependencies and their runtime DLL closure,
plus Boost headers, into `/tmp/lunaria-windows-deps`; subsequent runs reuse them.
Install a UCRT-capable `x86_64-w64-mingw32-gcc/g++`, CMake, Ninja, Python 3.11+,
curl, tar with zstd support, and zip, and provide the Android SDK/NDK above.
Intermediate files live in `/tmp/lunaria-cross-win` and
`/tmp/lunaria-dist-build/cross-windows-x86_64`; the ZIP is written here.
Use `WIN_DEPS=/path/to/deps make dist-cross-win` to reuse another environment.
Environment overrides also include `WIN_CROSS_PREFIX`, `WIN_ANGLE`,
`WIN_GLFW_LIB`, `WIN_BOOST_INCLUDE`, `WIN_BUILD_DIR`, and `WIN_DIST_BUILD_DIR`.
The ZIP carries the host DLLs and OpenH264, plus real AArch64 libraries in
`syslib-arm64/` for the guest JIT paths. It does not carry ICU, a shell
launcher, or host `runtime/*.so` shims. A current archive is about 35 MiB.
The process turns the UTF-16 command line into UTF-8, so an APK can be passed
with no shell in between. Run `.\lunaria.exe game.apk` from PowerShell, or
drop a package onto the executable. The runtime needs no MSYS2, Python, or
ICU installation.

### 2. Build

```bash
make dynarmic-build
make x86_64 -j"$(nproc)"   # host is x86_64; plain `make` builds the 32-bit x86 ABI
```

`make` / `make x86_64` already pulls Cisco's openh264 shared library into
`runtime/` (required for MediaCodec H.264). Re-run explicitly with
`make fetch-openh264` if you need to refresh it.

`make syslib` fills `syslib-arm64/` with an AArch64 libm, a bionic libc, and
libz. The launcher points `LUNARIA_SYSLIB_DIR` at that directory when it
exists, so audited pure arithmetic (`pow`, `sincosf`) and memory routines
stay inside the guest JIT. Platform zlib calls use the host through SVC:
existing profiles show native decompression is faster than guest JIT execution. `make guestlib` builds `liblunaria_guest.so` from `src/lib/guest.c`
(a zero-duration `nanosleep` returns without leaving the JIT).

Unicode classification, normalization and IDN use the compiled Unicode tables.
Legacy charsets use Windows code pages or Linux/macOS iconv; Windows calendars
use its timezone rules with a compact CLDR mapping. ICU is not a build or runtime dependency.

### 3. Launch a package

```bash
./lunaria path/to/game.apk
./lunaria path/to/game.xapk
./lunaria path/to/game.apks
LUNARIA_ALSA_DEVICE="hw:7,0" ./lunaria Cross+Worlds_5.03.04_APKPure.xapk
```

The binary reads the package itself: ZIP and ZIP64, the binary manifest,
signature blocks, split overlays, OBB, and the unpack cache. It detects
`arm64-v8a` or `armeabi-v7a`, finds the main native library, and starts the
runtime. An XAPK or APKS keeps the base APK intact and lays the splits over
that view. Unpacked trees are cached under `cache/packages/` in the data root
chosen at startup, keyed on the package identity. `LUNARIA_NO_CACHE=1` forces
a fresh unpack. `LUNARIA_CACHE_DIR` selects a different cache location.

Unpacking reports the percentage, MiB processed, file count and current filename,
including progress within a large file. Cache checks and large fallback copies
also report their progress. OBB aliases share the original archive by hard link
or symbolic link to avoid copying gigabytes again on each launch.

Settings that belong to the installation, not to one launch, live in
`lunaria.conf` (see `lunaria.conf.sample`): which device to report, where the
guest's `/data` lives (`LUNARIA_DATA_ROOT`), and which WebView engine to use.
A variable already set in the environment wins for that run.

Both the settings file (`lunaria.conf`) and the data root default to the working
directory at startup. Select another settings file with `--config FILE` or
`LUNARIA_CONF`, and another data volume with `--data-root DIR` or
`LUNARIA_DATA_ROOT` in the settings file. Relative paths start at the working
directory; paths with spaces are supported.

```bash
./lunaria --config ./lunaria.conf --data-root "/mnt/games/Lunaria data" game.xapk
# macOS: --data-root /Volumes/Games/Lunaria
# Windows shell: --data-root D:/Games/Lunaria
```

Account profiles share the installed APK and configured game resource paths,
while keeping preferences, databases, login files and other external files
separate. `default` keeps the existing account's directories.

```bash
./lunaria --choose-profile game.xapk
./lunaria --profile account2 game.xapk
./lunaria --list-profiles game.xapk
```

`LUNARIA_PROFILE_PROMPT=1` enables selection on each launch. Configure
`LUNARIA_SHARED_ASSETS` as colon-separated resource names and
`LUNARIA_PROFILE_SEED_FILES` as resource manifest files to copy once into a
new profile. The sample contains the Genshin resource layout. A new profile
starts with its own login state.

Force a guest architecture when a package contains both:

```bash
LUNARIA_ARCH=armeabi-v7a ./lunaria game.apk
LUNARIA_ARCH=arm64-v8a  ./lunaria game.apk
```

## Compatibility

These are observed milestones, not a general compatibility guarantee.
Details and launch recipes live in `PROGRESS.md`.

| Title | Engine / ABI | Current result |
|---|---|---|
| **Ni no Kuni: Cross Worlds** | Unreal Engine 4 · AArch64 XAPK | Drawn on Linux through guest login, the village, story dialogue, a voiced cutscene, and in-world play. SharedPreferences persist. On Apple Silicon macOS, package 5.04.12 displays the movie and 3D intro in the window and reaches the Terms of Use dialog. Progress after consent has not been checked |
| **Blade & Soul Masia** | Unreal Engine 5 · AArch64 APKS | Opening movie reaches EOS; title screen; additional-patch dialog is readable and Agree advances the download |
| **Genshin Impact 7.1.0** | Unity IL2CPP · AArch64 XAPK | Open world on Linux. Saved login and the Asia server return, then shader compile, the resource download, **TAP TO BEGIN**, and the field. Prologue, the library, and flight over Mondstadt draw in color. Keyboard movement works there; that host measured about 18–22 fps in the field and about 30 fps in settings. A changed music volume survived restart. A host WebView can open the account page. Google Play billing is not implemented |
| **Between Two Worlds** | Unity 2023 IL2CPP · ARMv7 | Playable; reaches the main story scene |
| **Between Two Worlds** | Unity 2023 IL2CPP · AArch64 | Main menu on a recorded run; recent checks reach language selection at about 70 fps |
| **FPSMobile** | Unreal Engine 4 · ARMv7 | FirstPersonExampleMap renders; 16,000+ swaps observed |
| **UnitySampleGame** | Unity · ARMv7 | Start opens the 3D gameplay scene; rendering continues (verified 2026-10-02) |
| **TIME LOCKER** | Unity · ARMv7 | Reaches the portrait tutorial gameplay scene |
| **Black Clover: Asta Fight** | Unity IL2CPP · AArch64 XAPK | Base + split load; title menu, stage selection and gameplay render headlessly |
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

LUNARIA_ARCH=armeabi-v7a ./lunaria test/FPSMobile-armv7.apk
```

### Between Two Worlds / Unity 2023 IL2CPP

Source: [ShutovKS/Between-two-worlds](https://github.com/ShutovKS/Between-two-worlds/releases/tag/1.0.5)

```bash
make fetch-btw

LUNARIA_ARCH=armeabi-v7a ./lunaria test/btw-android.apk
LUNARIA_ARCH=arm64-v8a  ./lunaria test/btw-android.apk
```

The release file is an Android App Bundle despite its `.apk` extension.
The native launcher reads its protobuf manifest and installs the base module
and Unity asset pack directly; `.aab` filenames are also accepted.


### Daggerfall Unity / Unity Mono

Source: [Vwing/daggerfall-unity-android](https://github.com/Vwing/daggerfall-unity-android/releases/tag/v1.1.1.8)

```bash
make fetch-libunity
./lunaria test/dfu-mono-32bit.apk
```

</details>

## Keyboard controls

Hold **Ctrl** and scroll the mouse wheel over the game to pinch: scroll up
spreads two fingers, scroll down brings them together. The cursor is the
gesture center. Release other mouse/keymap contacts before pinching.

Touch-driven games can be controlled from the keyboard by mapping keys to touch
positions on the screen. Open **Keyboard Controls** from the right-click menu and
select a keymap file to switch layouts while the game is running. You can also
browse to another folder and load your own keymap. Choose **Off** to disable
keyboard mapping, or **Reload Current File** to reload a keymap after editing it.
If a keymap fails to load, Lunaria keeps the current settings unchanged. A keymap
can also be selected at startup.

```sh
LUNARIA_KEYMAP=genshin ./lunaria /path/to/Genshin.xapk
LUNARIA_KEYMAP=crossworlds ./lunaria /path/to/CrossWorlds.apks
```

The bundled layouts use **WASD** for movement, **F** for the normal attack,
**E/Q/1/2/3** for skills or character selection, **Space** for jump, and
**Shift** for dash. In Cross Worlds, **Space** and **Shift** are mapped to dodge
actions instead. Drag with the left mouse button to control the camera. Movement,
mapped buttons, and mouse input can all be used at the same time.

When a text field has focus, text input takes priority over game controls. If the
window loses focus, any keys currently held down are released automatically.

To adapt the controls for another game or a different HUD layout, copy one of the
files in `keymaps/*.conf`, adjust the coordinates, and load it with
`LUNARIA_KEYMAP=/path/to/my.conf`. The setting can also be saved in
`lunaria.conf`. Set `LUNARIA_KEYMAP=off` to disable keyboard mapping.

```text
stick 0.15625 0.764 0.09
button SPACE 0.922 0.665
button F 0.826 0.769
```

`stick` defines the center X/Y position and radius of the virtual stick controlled
by WASD. `button` maps a key to an X/Y touch position. X and Y are normalized
coordinates from 0 to 1 relative to the screen width and height; the stick radius
is relative to the shorter screen dimension.

Up to eight buttons can be mapped. Supported keys are uppercase letters, digits,
`SPACE`, and `SHIFT`. The bundled layouts are tuned for landscape HUDs, so adjust
the coordinates if you change the HUD size or placement.

## Capture frames

Capture selected swap indices:

```bash
mkdir -p /tmp/lunaria-shots
LUNARIA_DUMP_DIR=/tmp/lunaria-shots \
LUNARIA_DUMP_FRAME=0,60,120 \
./lunaria game.apk
```

Or capture every *N* swaps:

```bash
LUNARIA_DUMP_DIR=/tmp/lunaria-shots \
LUNARIA_SCREENSHOT_EVERY=60 \
./lunaria game.xapk
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
     └── graphics ───── EGL · OpenGL ES 3 · optional Vulkan · host GPU
```

- **ARM32:** a flat 4 GiB guest memory window with fastmem.
- **ARM64:** guest virtual addresses map one-to-one to host addresses; a
  high-address image window holds loaded ELFs, trampolines, JNI tables and
  stacks, enabling dynarmic fastmem. `LUNARIA_A64_ENGINES` (1–8; defaults from
  host core count) can run multiple JIT engines on host threads.
  `lunaria` sets `LUNARIA_A64_SELF_SCHED=1`, so those engines pull
  runnable guests continuously. `LUNARIA_A64_SELF_SCHED=0` puts a barrier
  back at each frame-pump pass. Free-running engines match a device and
  raise throughput a lot. Cross Worlds' security module has aborted about
  twenty-five seconds in under that mode.
- **Native calls:** guest libc, EGL, GLES and JNI calls cross generated
  `SVC #n` trampolines into host implementations.
- **Guest libraries:** `make syslib` fills `syslib-arm64/` with AArch64 libm,
  libc and libz that run as guest code. `make guestlib` adds
  `liblunaria_guest.so` for routines that should not trap on every call
  (a zero-time `nanosleep` is the current one). The launcher points
  `LUNARIA_SYSLIB_DIR` at that directory when it exists.
- **Threads:** guest pthreads use cooperative round-robin scheduling with a
  separate JIT context per worker; mutex unlock can hand off directly to a
  waiter. Guest sleeps park for real wall time (`LUNARIA_GUEST_SLEEP`, on
  unless set to `0`). Blocking reads stay on the scheduler unless
  `LUNARIA_FD_PARK` is set.
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
- **Vulkan:** on when the host bridge is available. Guest `vk*` calls go to the
  host loader; Android surfaces and swapchains are emulated and the presented
  image is handed to the compositor. A title that does not see Vulkan stays
  on GLES, which is the path the titles above actually use.

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
| `LUNARIA_A64_SELF_SCHED` | `1` | Launcher default. Engines pull runnable guests freely. Set `0` for barrier-pooled passes. The binary itself stays off until the variable is set |
| `LUNARIA_A64_FASTMEM` | `1` | Set `0` to route memory through callbacks |
| `LUNARIA_A64_CODE_CACHE_MB` | `128` | Per-JIT translated-code cache |
| `LUNARIA_GUEST_SLEEP` / `LUNARIA_FD_PARK` | on / off | Park guest sleeps for real wall time / park blocking reads (`LUNARIA_GUEST_SLEEP=0` is diagnostic only) |
| `LUNARIA_SYSLIB_DIR` | `syslib-arm64/` if that directory exists | AArch64 platform libraries that run as guest code. `make syslib` fetches libm, libc and libz; `make guestlib` builds `liblunaria_guest.so` |
| `LUNARIA_VULKAN` | on when available | Set `0` to disable the Vulkan bridge and device feature. Titles may still choose GLES |
| `LUNARIA_TOUCH_TEST` | off | Inject taps at `x,y[;x,y…]` (max 8). Bare numbers are framebuffer percentages; `640px` is a guest pixel |
| `LUNARIA_TOUCH_FIFO` | off | Same taps while running: `echo 'x,y[,hold]' > $LUNARIA_TOUCH_FIFO` Windows: `\\.\pipe\lunaria-touch` (named pipe). |
| `LUNARIA_KEYMAP` | off | `genshin`, `crossworlds`, `off`, or a path to a keymap file. The host menu can change it live |
| `LUNARIA_DEVICE` | `pixel6` | Device profile: `pixel6`, `pixel7`, `galaxys21`, `lunaria`. Prefer `lunaria.conf` |
| `LUNARIA_DATA_ROOT` | Startup working directory | Guest `/data` and `/storage`. Also selectable with `--data-root DIR` |
| `LUNARIA_CONF` | `lunaria.conf` in the startup working directory | Settings file to read and write. Also selectable with `--config FILE`; no fallback search |
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
| `LUNARIA_CACHE_DIR` | `cache/packages/` under the data root selected at startup | Persistent package cache; independently relocatable |
| `LUNARIA_NO_CACHE` | off | Force a fresh APK/XAPK/APKS unpack |

An APK with no engine entry point (no `ANativeActivity_onCreate`, no
`UnityPlayer.initJni`) is now started from its launcher Activity instead of
being rejected: `lunaria` reads it out of the manifest and passes it as
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
| `src/lib/` | host implementations exposed to Android native code; `guest.c` is the in-guest libc |
| `src/luna_vulkan.c` | Vulkan bridge, enabled when a host driver is available |
| `src/luna_overlay.c` | the emulator's own UI surface: boot card, host menu, and compositing over the guest frame |
| `src/luna_boot.c` | the boot card: sky, snow, progress ring, wordmark, translation and dex lines |
| `src/luna_ime.c` | host IME bridge into guest text input |
| `src/webview_cdp.c` | DevTools pipe client shared by Chrome and luna-browser |
| `luna-browser/` | the built-in WebView engine (luna-ui + QuickJS); `make luna-browser` |
| `keymaps/` | bundled touch maps (`genshin.conf`, `crossworlds.conf`) |
| `lunaria.conf.sample` | device profile, data root, screen scale, package ledger, WebView engine |
| `runtime/` | generated Android-compatible host shared libraries |
| `src/apk.c` | in-process APK/XAPK/APKS install cache, manifest parsing and launch |
| `src/luna_unicode.c` | compiled Unicode classification, normalization and IDNA2003 |
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
./lunaria game.apk 2>&1 | tee lunaria.log
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

Lunaria is research software. Linux is the host where the window and the
framebuffer agree. `make dist` there produces
`lunaria-<version>-linux-x86_64.tar.gz`. `make dist-mac` produces a
`Lunaria.app` and a `.dmg`; on the Macs tried so far the framebuffer fills
while the window stays black. Windows `make dist` produces a ZIP of about
35 MiB. That archive passed ARM32 and ARM64 ABI tests and EGL/GLES
initialization under Wine. Display and audio on a physical Windows PC are
still untested. A `lunaria-v*` tag publishes the Linux and macOS packages
from GitHub Actions. Windows is not in that workflow yet: the OS layer is
ported, and the core still calls POSIX directly.

`make dist` packs a tree that does not depend on the build directory.
Distribution builds use `-O3`, `-DNDEBUG`, and LTO, with Dynarmic rebuilt as
Release + IPO in its own directory. They do not use `-march=native`.
Unicode classification, normalization, and IDNA live in small compiled
tables, so ICU is neither built nor shipped.

Expect missing Android APIs, title-specific gaps, noisy diagnostics, and
breaking changes. A useful report names the title, ABI, engine version, the
last milestone that worked, a log, and a captured frame.

Licensed under the [Mozilla Public License 2.0](LICENSE).

Project Lunaria © 2026 Yuichiro Nakada.
