# Lunaria

### Android apps and games, on your desktop.

No Android system image required. ARM32 and ARM64, from the original package.

[![License: MPL 2.0](https://img.shields.io/badge/license-MPL--2.0-5b5bd6.svg)](LICENSE)
[![Guests](https://img.shields.io/badge/guest-ARM32%20%7C%20ARM64-20232a.svg)](#how-it-works)
[![Engines](https://img.shields.io/badge/engines-Unity%20%7C%20Unreal%20%7C%20native-20232a.svg)](#compatibility)
[![Sponsor](https://img.shields.io/badge/sponsor-%E2%99%A5-ea4aaa.svg)](https://github.com/sponsors/yui0)

[Install](#install) · [Usage](#usage) · [Gallery](#gallery) · [Compatibility](#compatibility) · [日本語](README.ja.md)

**Lunaria runs Android apps and games on the desktop without booting Android.**
It loads the app's own package, executes its ARM32 or ARM64 code with
[dynarmic](https://github.com/merryhime/dynarmic), and bridges JNI, EGL, OpenGL ES, MediaCodec and other
Android services to the host. A built-in Dalvik interpreter runs the app's `classes*.dex`.

Open an `.apk`, `.xapk`, `.apks`, or `.aab` directly; extraction, split APKs, caching and per-account
profiles are handled for you. WebView content uses Chrome/Chromium or the bundled [luna-browser](luna-browser/).

**Demonstrated:** Genshin Impact, Ni no Kuni: Cross Worlds, Where Winds Meet (NetEase Messiah engine),
Gakuen Idolmaster, Alchemia Story, Kikyu! Aeronaut (a 2013 ARM32 APK), Layton Brothers: Mystery Room,
Black Clover: Asta Fight, Kingdoms & Lords, other Unity and Unreal titles, and Java apps such as
TVer (full-episode playback), LINE and APKPure.
See [Compatibility](#compatibility) for exact stages and limits.

> [!IMPORTANT]
> **Experimental software.** Lunaria is a compatibility layer, not a complete Android emulator.
> Some titles reach gameplay; others stop at menus or fail to render. All screenshots are real Lunaria output.

<p align="center">
  <img src="shots/genshin-mondstadt-night.png" width="100%" alt="Genshin Impact: Lumine gliding through Mondstadt at night, windmills lit below, full touch HUD">
</p>

<p align="center">
  <img src="shots/genshin-glide-coast.png" width="32%" alt="Genshin Impact: Lumine gliding over turquoise water toward a green coast at sunset">
  &nbsp;
  <img src="shots/cross-worlds-portal.png" width="32%" alt="Ni no Kuni: Cross Worlds, a glowing blue portal in a golden hall with characters and a familiar">
  &nbsp;
  <img src="shots/genshin-lisa.png" width="32%" alt="Genshin Impact: Lisa recounting the Four Winds in Japanese dialogue">
</p>

<p align="center"><sub>Genshin Impact 7.2.0 (Unity IL2CPP, arm64-v8a) · Ni no Kuni: Cross Worlds (Unreal Engine 4, arm64-v8a)</sub></p>

## Install

### Prebuilt (recommended)

**Linux / macOS (Apple Silicon)**

```bash
curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | bash
```

Installs the latest [release](https://github.com/yui0/lunaria/releases) under `~/.local` (or `/usr/local` with `sudo`) and adds a `lunaria` command; on macOS it also places `Lunaria.app` in `~/Applications`.
Add `~/.local/bin` to your `PATH` if needed.

```bash
# Options: pin a release / system-wide / uninstall (keeps lunaria.conf and app data)
curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | bash -s -- --version 2026.10.15
curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | sudo bash -s -- --prefix /usr/local
curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | bash -s -- --uninstall
```

Manual install: unpack `lunaria-*-linux-x86_64.tar.gz` / `…-aarch64.tar.gz` and run `./lunaria`, or open the macOS `.dmg` and drag `Lunaria.app` out.

**Windows (x86_64)**

Download `lunaria-*-windows-x86_64.zip`, unzip, and run `lunaria.exe app.apk` (or drag an APK onto it). No MSYS2 or extra runtime is needed.

### Build from source

**1. Dependencies**

<details open>
<summary>Ubuntu / Debian</summary>

```bash
sudo apt install \
  build-essential cmake pkg-config \
  libboost-dev libbsd-dev libunwind-dev \
  libglfw3-dev libegl1-mesa-dev libgles2-mesa-dev \
  libssl-dev zlib1g-dev \
  libasound2-dev libvulkan-dev
```

`libvulkan-dev` is for the optional Vulkan bridge (on by default when a host Vulkan library exists; `LUNARIA_VULKAN=0` disables it).

</details>

<details>
<summary>macOS (Apple Silicon)</summary>

`make` downloads the pinned ANGLE, MoltenVK and OpenH264 and builds. See [Project status](#project-status) for display limits.

</details>

<details>
<summary>Windows (MSYS2 UCRT64)</summary>

```bash
pacman -S --needed git make zip unzip bzip2 \
  mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,python,boost,glfw,angleproject,openssl,zlib,libgnurx,vulkan-headers}
```

Set `ANDROID_HOME` (SDK, including Build Tools' ARM64 RenderScript libraries) and `ANDROID_NDK_HOME`, then:

```bash
make -j4 dist CC=gcc CXX=g++ DYNARMIC_JOBS=4
```

The result is a self-contained `lunaria.exe` ZIP (about 35 MiB) with no MSYS2, Python or ICU needed at runtime.

To cross-build the ZIP from Linux, run `make -j4 dist-cross-win` (needs UCRT `x86_64-w64-mingw32-gcc/g++`, CMake, Ninja, Python 3.11+, curl, zstd-capable tar, zip, Android SDK/NDK). Dependencies are cached in `/tmp/lunaria-windows-deps`; override with `WIN_DEPS`, `WIN_CROSS_PREFIX`, `WIN_ANGLE`, `WIN_GLFW_LIB`, `WIN_BOOST_INCLUDE`, `WIN_BUILD_DIR`, `WIN_DIST_BUILD_DIR`.

</details>

**2. Build**

```bash
make dynarmic-build
make x86_64 -j"$(nproc)"   # x86_64 host; plain `make` builds the 32-bit x86 ABI
make syslib                # optional: AArch64 libm/libc/libz run as guest code in syslib-arm64/
make guestlib              # optional: liblunaria_guest.so (zero-time nanosleep without a host trap)
```

OpenH264 is fetched into `runtime/` automatically (`make fetch-openh264` refreshes it). WebP uses the bundled [simplewebp](https://github.com/MikuAuahDark/simplewebp/blob/main/simplewebp.h) (`LICENSE.simplewebp`), and Unicode tables are embedded, so **ICU is not required**.

**3. Android framework resources**

Themes and resource inheritance use SDK Platform 31 resources:

```bash
curl -fL https://dl.google.com/android/repository/platform-31_r01.zip -o /tmp/platform-31_r01.zip
python3 scripts/install-framework.py /tmp/platform-31_r01.zip
```

The installer verifies Google's SHA-1 and copies the files unchanged into `runtime/framework-res/` (override with `LUNARIA_FRAMEWORK_RES`). App APKs are never modified.

**4. Package a distributable** (optional)

```bash
make dist        # lunaria-<version>-linux-x86_64.tar.gz
make dist-mac    # Lunaria.app + .dmg (Darwin only)
```

## Usage

```bash
lunaria                        # graphical launcher (drop a package on the window)
lunaria path/to/app.apk        # or .xapk / .apks / .aab
LUNARIA_ARCH=arm64-v8a lunaria app.apk   # pick an ABI when a package has several
```

From a source build, use `./lunaria` in place of `lunaria`.

- **Host menu:** right-click in a running game for screenshots (also **F12**), paste, Back, audio, keyboard maps, WebView, input recording, profiles, restart, fullscreen, and quit.
- **Display density:** right-click **Display → SCREEN DENSITY (DPI)** to choose a density. It is saved per app and applies after restart; **Automatic** restores the device default.
- **Find apps:** in the launcher, type a name under *Find an app on APKPure*, pick a result (check the package ID), and Lunaria downloads and opens the XAPK. Downloads go to `downloads` under the package cache, else `cache/packages/downloads` in the data folder, else `~/Downloads/Lunaria`.
- **Packages:** splits are layered over the base APK without modifying it; AABs load the base module and asset packs; OBBs are hard-linked or symlinked where possible. Extractions are cached under `cache/packages/` (`LUNARIA_CACHE_DIR` to move, `LUNARIA_NO_CACHE=1` to disable).
- **Settings:** persistent options live in `lunaria.conf` (see `lunaria.conf.sample`); environment variables override it per run. The file and guest data default to the startup directory.

  ```bash
  ./lunaria --config ./lunaria.conf --data-root "/mnt/games/Lunaria data" game.xapk
  ```

- **Profiles:** accounts share the installed package and external/shared storage (including OBB), but keep internal files, preferences and databases apart. `default` uses `data/<package>/`; others use `profiles/<package>/`.

  ```bash
  ./lunaria --choose-profile game.xapk     # pick at launch (or LUNARIA_PROFILE_PROMPT=1)
  ./lunaria --profile account2 game.xapk
  ./lunaria --list-profiles game.xapk
  ```

- **Google services (optional):** the right-click **Google services** menu installs a local Google Play services, Play Store or Services Framework package and reports each separately. **Sign in to a new environment…** creates a GMS environment (`system/gms/gms-N/`) that other apps can reuse; restart the app afterward. `LUNARIA_GMS_PROFILE=gms-2` selects one directly, independent of `--profile`.

  ```bash
  ./lunaria --google-services --data-root /path/to/data
  ./lunaria --data-root /path/to/data --install-system GooglePlayServices.xapk   # headless install
  ```

<p align="center">
  <img src="shots/launcher.png" width="48%" alt="The Lunaria graphical launcher with an app list and a drop target">
  &nbsp;
  <img src="shots/host-menu.png" width="48%" alt="The in-game host menu opened over Kikyu! Aeronaut: Back, Input, Display, Sound, WebView, Profile, Restart, and a Keyboard Controls submenu listing genshin.conf and crossworlds.conf">
</p>

<p align="center"><sub>The launcher · the in-game host menu is on right-click</sub></p>

## Features

| Feature | What it does |
|---|---|
| **No Android image** | Launches `.apk`, `.xapk`, `.apks`, `.aab` without a VM. |
| **ARM32 + ARM64** | ARMv7 and AArch64 through dynarmic's JIT; optional parallel engines (`LUNARIA_A64_ENGINES`). |
| **Host graphics** | EGL and OpenGL ES 3 on the host GPU; Vulkan when the bridge is available. |
| **Android services** | JNI, AssetManager, OBB, pthread, OpenSL ES and compatible Android APIs. |
| **Java and UI** | Dalvik interpreter for `classes*.dex`; Android views, text and layouts rendered by the host. |
| **Media and WebView** | H.264 via OpenH264, AAC via libavcodec; WebView through Chrome/Chromium or luna-browser. |
| **Input** | Keyboard/mouse mapped to touch, with macros and record/replay. |
| **Profiles and identity** | Per-account data via `--profile`; device profile in `lunaria.conf` (default Pixel 6). |
| **Google services (GMS)** | Optional Google Play services, Play Store and Services Framework run from your own genuine packages; reusable sign-in environments. |
| **Headless and diagnostics** | EGL without X11, frame capture, JIT profiles, SVC traces. |

### Google services (GMS)

Lunaria presents a Pixel-like device to the app, including `com.google.android.gms`, `com.android.vending` and `com.google.android.gsf` in the package ledger. **No Google-signed APK is bundled**: until you provide genuine ones, these packages are empty placeholders, so apps that need real Play services will not get them. Apps that do not require them run without any setup.

- **Install your own:** use the right-click **Google services** menu, or install headless with `--install-system GooglePlayServices.xapk` (Play services, Play Store and Services Framework are installed and reported separately). A genuine APK can also be placed at `<device-root>/product/priv-app/PrebuiltGmsCore/PrebuiltGmsCore.apk`; apps verify its Google signature.
- **Sign-in environments:** **Sign in to a new environment…** creates `system/gms/gms-N/`, which any app can reuse. Select one from the menu or with `LUNARIA_GMS_PROFILE=gms-2`; restart the app afterward. This is independent of `--profile`.
- **Service hosting:** a Service of another installed package is bound in that package's own process (`LUNARIA_SERVICE_HOSTS=com.android.vending,com.google.android.gms`).
- **Without GMS:** drop them from the ledger with `LUNARIA_PACKAGES=default;-com.google.android.gms;-com.google.android.gsf` for a device that has none.
- **Limits:** Google Play billing is not supported (Genshin Impact, for example). Results depend on the title; see [Compatibility](#compatibility).

See `lunaria.conf.sample` for the full set of package and service-host options.

## Gallery

### Java apps: TVer plays video

Lunaria runs app dex bytecode and implements Android views, layouts, text, resources and lifecycle on the host. **TVer 6.40.0** goes from the welcome screen through the home feed (with video tiles) into episode details, the ad break, and full-episode playback from its unmodified XAPK. Startup is heavy and a few screens keep layout gaps.

<p align="center">
  <img src="shots/tver-playback.png" width="26%" alt="TVer playing an episode of The Apothecary Diaries: the video frame above the episode title, broadcast dates, and Watch Later / Favorite buttons in Japanese">
  &nbsp;
  <img src="shots/tver-episode-portrait.png" width="26%" alt="TVer episode details in portrait: a pink-haired heroine in the video frame above episode 5, broadcast dates, and a favorite button">
  &nbsp;
  <img src="shots/tver-playback-portrait.png" width="26%" alt="TVer playing episode 54 of The Apothecary Diaries in portrait: Maomao peeking through a sliding door in the video frame above the episode details">
  &nbsp;
  <img src="shots/tver-welcome.png" width="12%" alt="TVer portrait welcome screen with program artwork and the consent button">
</p>

<p align="center"><sub>TVer 6.40.0 · landscape playback · episode details · portrait playback · welcome screen</sub></p>

APKPure renders its store, app details and download manager (download progress observed; completed install not yet verified).
**LINE 15.15.1** goes from the welcome screen into phone-number registration and the country-selection list
(CollapsingToolbar + RecyclerView); scrolling, Back and edge-swipe gestures all work.

### Genshin Impact 7.2.0

Unity IL2CPP, `arm64-v8a`, **1024 × 576**. Google Play billing is not supported.

<p align="center">
  <img src="shots/genshin-wings.png" width="48%" alt="Genshin Impact: Lumine with glider wings open against a blue sky">
  &nbsp;
  <img src="shots/genshin-lisa-obtained.png" width="48%" alt="Genshin Impact: new character screen for Lisa, four stars, Japanese copy">
</p>

<p align="center">
  <img src="shots/genshin-prologue.png" width="48%" alt="Genshin Impact prologue: Lumine, Paimon, and a floating companion around a glowing red stone">
  &nbsp;
  <img src="shots/genshin-knights.png" width="48%" alt="Genshin Impact: the Traveler at the Knights of Favonius gate, Kaeya and Amber nearby">
</p>

<p align="center">
  <img src="shots/genshin-lakeshore.png" width="48%" alt="Genshin Impact open world: the Traveler at a lakeshore, autumn trees, full HUD">
  &nbsp;
  <img src="shots/genshin-swim.png" width="48%" alt="Genshin Impact: swimming at sunset outside Mondstadt, the quest marker still pointing toward the city">
</p>

<p align="center">
  <img src="shots/genshin-mondstadt.png" width="48%" alt="Genshin Impact: gliding high above Mondstadt, the whole city and the storm around the cathedral below">
  &nbsp;
  <img src="shots/genshin-paimon.png" width="48%" alt="Genshin Impact: Paimon pointing something out to Lumine, Japanese subtitle">
</p>

<p align="center"><sub>Wings · Lisa joins · prologue · the Knights · lakeshore · swimming at dusk · gliding above Mondstadt · Paimon points the way</sub></p>

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
  <img src="shots/genshin-jean.png" width="48%" alt="Genshin Impact: Jean and Lisa in the Knights' library">
  &nbsp;
  <img src="shots/genshin-language.png" width="48%" alt="Genshin Impact language settings in Japanese">
</p>

</details>

### Ni no Kuni: Cross Worlds

Unreal Engine 4: login and server selection through character creation, the starting village, voiced cutscenes, and field gameplay, at **1024 × 576** on `arm64-v8a`.

<p align="center">
  <img src="shots/cross-worlds-peek.png" width="32%" alt="Cross Worlds: orange-haired girl and familiar peeking from behind a bush at night">
  &nbsp;
  <img src="shots/cross-worlds-village.png" width="32%" alt="Cross Worlds starting village, a teal-roofed house under a broad tree">
  &nbsp;
  <img src="shots/cross-worlds-play.png" width="32%" alt="Cross Worlds in the field, quests and the touch HUD up">
</p>

<p align="center">
  <img src="shots/cross-worlds-chloe.png" width="32%" alt="Cross Worlds story dialogue with Chloe, autumn trees behind her">
  &nbsp;
  <img src="shots/cross-worlds-gatling.png" width="32%" alt="Cross Worlds: a red-haired heroine levels a huge gatling gun against swirling orange flames">
  &nbsp;
  <img src="shots/cross-worlds-lighthouse.png" width="32%" alt="Cross Worlds field play at sunset with the HUD, quest list, and skill ring">
</p>

<details>
<summary>More: title, servers, characters, intro</summary>

<p align="center">
  <img src="shots/cross-worlds-title.png" width="32%" alt="Cross Worlds title, Luxelion selected, snowy peaks behind the logo">
  &nbsp;
  <img src="shots/cross-worlds-servers.png" width="32%" alt="Cross Worlds server select, Luxelion">
  &nbsp;
  <img src="shots/cross-worlds-characters.png" width="32%" alt="Cross Worlds character select">
</p>

<p align="center">
  <img src="shots/cross-worlds-intro.png" width="32%" alt="Cross Worlds 3D intro">
  &nbsp;
  <img src="shots/cross-worlds-evermore.png" width="32%" alt="Cross Worlds, arrival at Evermore">
  &nbsp;
  <img src="shots/cross-worlds-cutscene.png" width="32%" alt="Cross Worlds, a voiced cutscene against a bright sky">
</p>

</details>

### Where Winds Meet (燕雲十六声)

NetEase's open-world title runs on its own **Messiah engine** (`MessiahNativeActivity`, `libGame.so`,
`arm64-v8a`). Protected DEX loading, the game's resource check, Japanese news, the
NetEase / Google / Guest login choice, consent and guide screens, and character creation all
render; gameplay is not yet reached. Along the way this title drove fixes for persistent GL buffer
mappings, Java serialization (HashMap / ArrayList / arrays), WebView `loadDataWithBaseURL`,
NDK MediaFormat and the AAC encoder.

<p align="center">
  <img src="shots/where-winds-meet.png" width="48%" alt="Where Winds Meet character creation: a young heroine in white-and-indigo hanfu, gender selector and 次へ button">
</p>

<p align="center"><sub>Character creation · the engine's own renderer draws on the host GPU</sub></p>

### More titles

<p align="center">
  <img src="shots/black-clover-gameplay.png" width="48%" alt="Black Clover: Asta Fight gameplay, Asta fighting a monster with the full touch HUD">
  &nbsp;
  <img src="shots/alchemia-story-title.png" width="48%" alt="Alchemia Story title screen, the cast gathered around the logo">
</p>

<p align="center">
  <img src="shots/alchemia-story-cave.png" width="48%" alt="Alchemia Story: a black-haired heroine covers her mouth in surprise inside a cave, Japanese dialogue box below">
  &nbsp;
  <img src="shots/alchemia-story-neko.png" width="26%" alt="Alchemia Story portrait scene: a white-haired cat-eared girl cheers the player up, Skip and Auto buttons on the right">
</p>

<p align="center"><sub>Black Clover: Asta Fight · Alchemia Story (Unity IL2CPP; landscape 1024×576 and portrait 576×1024)</sub></p>

<p align="center">
  <img src="shots/kikyu-title.png" width="48%" alt="Kikyu! Aeronaut title screen: a crescent moon over a city skyline, the pixel-art heroine on a pile of rubble, Press Return to Embark">
  &nbsp;
  <img src="shots/kingdoms-lords-village.png" width="48%" alt="Kingdoms & Lords: a green island village with huts and fences under construction, the quest arrow pointing to a lamp post, and a speech bubble where Lisa introduces herself as the player's lady-at-arms">
</p>

<p align="center"><sub><a href="https://github.com/yui0/Kikyu">Kikyu! Aeronaut</a>: a 2013 <code>armeabi</code> APK, unmodified · Kingdoms &amp; Lords: village scene drawn through the Android view layer</sub></p>

<p align="center">
  <img src="shots/disney-magic-kingdoms-loading.png" width="48%" alt="Disney Magic Kingdoms: Minnie and Mickey on an ice stage in a Disney On Ice event, the Japanese content-verify message with a green progress bar below">
  &nbsp;
  <img src="shots/trickcal-server-loading.jpg" width="48%" alt="Trickcal Chibi+Go 1.6.1 displaying its illustrated town and server configuration loading progress">
</p>

<p align="center"><sub>Disney Magic Kingdoms: content check, waiting on its servers · Trickcal Chibi+Go: server configuration loading (gameplay not yet reached)</sub></p>

<p align="center">
  <img src="shots/layton-brothers-title.png" width="48%" alt="Layton Brothers: Mystery Room title screen, the LEVEL-5 logo on a film strip with scenes from the game">
  &nbsp;
  <img src="shots/layton-brothers-dialogue.png" width="48%" alt="Layton Brothers: Mystery Room dialogue scene, two detectives talking in a station hallway, Japanese speech bubble">
</p>

<p align="center"><sub><a href="https://www.level5.co.jp/">Layton Brothers: Mystery Room</a>: a 2013 LEVEL-5 title whose Java UI is drawn entirely through the Android view layer</sub></p>

<p align="center">
  <img src="shots/bedrock-addons.png" width="30%" alt="A Flutter-based Minecraft Bedrock add-on manager showing Villager Soldier, Dragon Bedrock, Zombie Bedrock and Simple Magnets Bedrock add-ons">
  &nbsp;
  <img src="shots/gakumas-setup.png" width="34%" alt="Gakuen Idolmaster title screen during first setup, downloading resources at 26.61 percent">
  &nbsp;
  <img src="shots/gakumas-loading.png" width="26%" alt="Gakuen Idolmaster NOW LOADING screen with a chibi idol">
</p>

<p align="center"><sub>A Flutter-based Minecraft Bedrock add-on manager (startup, list and detail WebView) ·
Gakuen Idolmaster 3.5.3: setup and consent screens, first asset download from the game CDN (Unity 6000.0 IL2CPP, Vulkan, CRI)</sub></p>

<details>
<summary>Open-source and sample titles</summary>

<p align="center">
  <img src="shots/between-two-worlds.png" width="48%" alt="Between Two Worlds main menu, Continue, New Game, Load, Exit">
  &nbsp;
  <img src="shots/fpsmobile.png" width="48%" alt="FPSMobile Unreal first-person template, rifle aimed at a white cube, virtual sticks">
</p>

<p align="center">
  <img src="shots/unity-sample.png" width="48%" alt="Unity sample game, 3D gameplay">
  &nbsp;
  <img src="shots/black-clover-village.png" width="48%" alt="Black Clover: Asta Fight village stage, Asta holding an oversized sword over glowing peach pickups, full touch HUD">
</p>

<p align="center">
  <img src="shots/demo-project-light.png" width="48%" alt="Demo Project Light title, TAP THE SCREEN over a wizard study">
  &nbsp;
  <img src="shots/unreal-first-person.png" width="48%" alt="Unreal Engine 4 First Person map, a pistol and grey blocks">
</p>

<p align="center">
  <img src="shots/time-locker.png" width="28%" alt="TIME LOCKER portrait tutorial">
</p>

<p align="center"><sub>Between Two Worlds · FPSMobile · UnitySampleGame · Black Clover village stage · Demo Project Light · Unreal first-person sample · TIME LOCKER</sub></p>

</details>

Programmatic taps use `LUNARIA_TOUCH_TEST`: a bare `x,y` is a percentage of the framebuffer (`50,84` = `50%,84%`), `640px,606px` is a guest pixel. GLFW ignores `xdotool` synthetic clicks.

## Compatibility

Results describe **what was actually observed** on particular builds and machines, not a guarantee. Details and launch notes are in [PROGRESS.md](PROGRESS.md).

| Title | Engine / ABI | Current result |
|---|---|---|
| **Ni no Kuni: Cross Worlds** | Unreal Engine 4 · AArch64 XAPK | Linux: login, server and character selection, village, dialogue, voiced cutscenes, field gameplay; SharedPreferences persist. Apple Silicon: 5.04.12 reached the opening movie, 3D intro and terms dialog; later stages unverified. |
| **Where Winds Meet** | NetEase Messiah · AArch64 XAPK | Title with resource download, Japanese news, login choice (NetEase / Google / Guest), consent and guide screens, character creation. Gameplay not yet reached; startup videos need Main/High H.264. |
| **Gakuen Idolmaster 3.5.3** | Unity 6000.0 IL2CPP (Vulkan, CRI) · AArch64 XAPK | Setup and consent screens render; the first asset download from the game CDN starts (~806 MB observed). Gameplay not yet reached. |
| **Blade & Soul Masia** | Unreal Engine 5 · AArch64 APKS | Opening video completes, title screen appears, accepting the additional-patch prompt starts the download. |
| **Genshin Impact 7.2.0** | Unity IL2CPP · AArch64 XAPK | Open world on Linux (prologue, Mondstadt, gliding). Login, server and audio settings survive restart; keyboard movement works; about 18–22 fps in the field and 30 fps in menus on one test host. Account WebView opens; Play billing unsupported. |
| **Alchemia Story 1.0.154** | Unity IL2CPP · AArch64 APK | Title, Japanese story scenes (landscape and portrait), character-creation room (2026-10-09). |
| **Kikyu! Aeronaut** | native `armeabi` · ARM32 APK (2013) | Title screen draws and waits for Return. |
| **Layton Brothers: Mystery Room** | Java / dex Android UI (2013) | Title screen and dialogue scenes render through the Android view layer. |
| **Between Two Worlds** | Unity 2023 IL2CPP · ARMv7 / AArch64 | ARMv7: playable, reaches the main story scene. AArch64: main menu, recent checks reach language selection at about 70 fps. |
| **FPSMobile** | Unreal Engine 4 · ARMv7 | FirstPersonExampleMap renders; 16,000+ swaps. |
| **UnitySampleGame** | Unity · ARMv7 | Start opens the 3D gameplay scene. |
| **TIME LOCKER** | Unity · ARMv7 | Reaches the portrait tutorial gameplay scene. |
| **Black Clover: Asta Fight** | Unity IL2CPP · AArch64 XAPK | Base + split load; title, stage selection and gameplay render headlessly. |
| **Blade & Soul Revolution** | Unreal Engine 4 · AArch64 | Boots to the game's own dialog; without a patch-server route it asks to reconnect. |
| **Trickcal Chibi+Go 1.6.1** | Unity 6000.3 IL2CPP · ARMv7 XAPK | Terms Activity and server-configuration loading; gameplay unverified. |
| **Disney Magic Kingdoms 12.1.1c** | Unity · ARMv7 XAPK | Content-verification screen draws; pauses asking its servers for an update. |
| **Kingdoms & Lords** | Gameloft village builder | Island village, resource counters and dialogue render (2026-11-09); progression unverified. |
| **TVer 6.40.0** | Java / dex Android UI | Welcome, home feed with playing tiles, episode details and main-feature playback. Heavy startup, some layout gaps. |
| **LINE 15.15.1** | Java / dex Android UI | Welcome, phone-number registration and country selection (CollapsingToolbar + RecyclerView); scroll, Back and edge-swipe work. Authentication beyond registration not reached. |
| **APKPure 3.20.8002** | Java / dex Android UI | Store, recommendations, details and download manager render; download progress observed, completed install unverified. |
| **Minecraft Bedrock add-on manager** | Flutter (dex + Skia) · ARM32/ARM64 APK | Startup, START, add-on list and detail WebView; Back returns to the list. The detail CDN answers 404, so add-on content does not load. |
| **Another Eden** | Native · ARMv7 XAPK | Existing-account load screen; loading the game aborts. |
| **Daggerfall Unity** | Unity Mono · ARMv7 | Mono starts; rendering not yet working. |

<details>
<summary><strong>Run the open-source test titles</strong></summary>

**FPSMobile** (Unreal Engine 4) — [Abhishrut/UnrealEngineAndroidSamples](https://github.com/Abhishrut/UnrealEngineAndroidSamples). Put both files in `test/`; a same-directory OBB is found automatically.

```bash
curl -L -o test/FPSMobile-armv7.apk \
  "https://raw.githubusercontent.com/Abhishrut/UnrealEngineAndroidSamples/main/FPSMobile-armv7.apk"
curl -L -o test/main.1.com.YourCompany.FPSMobile.obb \
  "https://raw.githubusercontent.com/Abhishrut/UnrealEngineAndroidSamples/main/main.1.com.YourCompany.FPSMobile.obb"
LUNARIA_ARCH=armeabi-v7a ./lunaria test/FPSMobile-armv7.apk
```

**Between Two Worlds** (Unity 2023 IL2CPP) — [ShutovKS/Between-two-worlds](https://github.com/ShutovKS/Between-two-worlds/releases/tag/1.0.5). The release `.apk` is really an App Bundle; Lunaria reads it directly.

```bash
make fetch-btw
LUNARIA_ARCH=armeabi-v7a ./lunaria test/btw-android.apk
LUNARIA_ARCH=arm64-v8a  ./lunaria test/btw-android.apk
```

**Daggerfall Unity** (Unity Mono) — [Vwing/daggerfall-unity-android](https://github.com/Vwing/daggerfall-unity-android/releases/tag/v1.1.1.8)

```bash
make fetch-libunity
./lunaria test/dfu-mono-32bit.apk
```

</details>

## Keyboard controls

Keyboard and mouse input become touch gestures. Open **Keyboard Controls** in the right-click menu to switch layouts live, choose **Off**, or **Reload Current File** (a failed load keeps the current mapping).

```sh
LUNARIA_KEYMAP=genshin ./lunaria /path/to/Genshin.xapk
LUNARIA_KEYMAP=crossworlds ./lunaria /path/to/CrossWorlds.apks
```

The bundled Genshin and Cross Worlds layouts use **WASD** to move, **F** to attack, **E/Q/1/2/3** for abilities or characters, **Space**/**Shift** to jump, dash or dodge, and left-drag for the camera. **Ctrl + scroll** pinches (scroll up spreads, down closes; the cursor is the center). Text fields take priority, and held keys are released when the window loses focus.

**Custom layouts:** copy a file from `keymaps/*.conf`, edit it, and set `LUNARIA_KEYMAP=/path/to/my.conf` (or save it in `lunaria.conf`).

```text
stick 0.15625 0.764 0.18        # joystick center (x y) and radius, driven by WASD
walk CTRL 0.04                  # smaller radius while the key is held
button SPACE 0.922 0.665        # key -> touch position
button F 0.10 0.20 0.30 0.40    # one key, several simultaneous contacts (up to 4)
button G 0.70 0.70 repeat=100 hold=20   # tap every 100 ms while held, 20 ms each
macro Z 0.5,0.5|0.6,0.6/40 @100 0.9,0.9 # taps, |simultaneous, /hold ms, @wait ms
macro X 0.1,0.1 loop            # repeat while held
```

Coordinates are 0–1 of screen width/height (radius against the shorter side). Up to eight buttons and eight macros; keys are `A–Z`, `0–9`, `SPACE`, `SHIFT`, `CTRL`. `repeat` is 10–60,000 ms. Timing follows the frame clock (about 16 ms at 60 fps). The bundled maps assume a landscape HUD.

**Record and replay:** **F9** (Record Input) saves touch input to `~/Documents/Lunaria <date>.rec`; **F10** (Play Recording) replays the latest, or set `LUNARIA_PLAY` (and `LUNARIA_PLAY_LOOP=1`). Positions are relative, so recordings work at any window size.

## Capture frames

Right-click **Pause** / **Resume** freezes the guest while keeping the host menu. **F12** or **Take Screenshot** saves `~/Pictures/Lunaria YYYY-MM-DD HH.MM.SS.png`. For scripted capture:

```bash
LUNARIA_DUMP_DIR=/tmp/lunaria-shots LUNARIA_DUMP_FRAME=0,60,120 ./lunaria game.apk   # chosen swaps
LUNARIA_DUMP_DIR=/tmp/lunaria-shots LUNARIA_SCREENSHOT_EVERY=60 ./lunaria game.xapk  # every N swaps
```

Frames are saved as `lunaria_0000.png`. Creating `$LUNARIA_SHOT_TRIGGER` (default `/tmp/lunaria-shot`) requests one more capture.

## How it works

```text
 APK / XAPK / APKS / AAB
     │  extract base modules, splits, native libraries, assets and OBB
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

- **Memory:** ARM32 uses a flat 4 GiB fastmem window. ARM64 maps guest addresses directly to host addresses, with ELF images, trampolines, JNI tables and stacks in a high region.
- **Threads:** guest pthreads are scheduled cooperatively, each with its own JIT context. `LUNARIA_A64_ENGINES` (1–8, auto by core count) runs several AArch64 engines on host threads; the launcher sets `LUNARIA_A64_SELF_SCHED=1` so engines pick up runnable guests independently (`0` synchronizes per frame; a Cross Worlds security module has aborted after about 25 s with self-scheduling on).
- **Native calls:** guest libc, EGL, GLES and JNI calls cross generated `SVC #n` trampolines into host code. `make syslib` / `make guestlib` move hot routines (`pow`, `sincosf`, memory ops, zero-time `nanosleep`) into guest code; zlib stays on the host, which measured faster.
- **Java:** when a JNI call has no host stub, `src/dvm/` runs the method from the APK's `classes*.dex`.
- **Lifecycle:** the manifest `Application` and content providers start before the launcher `Activity`'s `onCreate`/`onStart`/`onResume`. Native-activity engines enter through `ANativeActivity_onCreate` with `APP_CMD_*` events. A `System.loadLibrary` root heads the lookup scope of its dependencies, as Android's linker does.
- **Assets and media:** `AssetManager` reads DEFLATE/STORE entries from APKs and OBBs. `MediaCodec` decodes H.264 with openh264 and AAC with libavcodec (`LUNARIA_OPENH264` / `LUNARIA_LIBAVCODEC` set paths).
- **Audio:** OpenSL ES, AAudio and `AudioTrack` share one mixer that outputs through ALSA; blocking writes do not stall the frame pump.
- **WebView:** talks to a host browser over the DevTools pipe and shows the page as an image in the view. `LUNARIA_WEB_ENGINE` is `auto` (Chrome/Chromium, else luna-browser), `chrome`, `luna` or `off`; `LUNARIA_WEB_BROWSER` names a binary. Build the fallback with `make luna-browser`.
- **Vulkan:** guest `vk*` calls reach the host loader, with Android surfaces and swapchains emulated. Games that don't use Vulkan keep using GLES, the path behind the demos above.

## Runtime controls

Everyday and diagnostic options; development-only switches are in the source and `PROGRESS.md`. Tick values accept `K`, `M`, `G` suffixes.

| Variable | Default | Purpose |
|---|---:|---|
| `LUNARIA_ARCH` | auto | `armeabi-v7a` or `arm64-v8a` |
| `LUNARIA_WIDTH` / `LUNARIA_HEIGHT` | `1024` / `768` (swapped for landscape apps) | Window / EGL surface size; see `LUNARIA_SCALE` |
| `LUNARIA_SCALE` | off | Scale the device's own panel (`0.5` is half) |
| `LUNARIA_PBUFFER` | auto | `1` forces headless EGL |
| `LUNARIA_MAX_FRAMES` | unlimited | Stop after *N* frames |
| `LUNARIA_MEM_TOTAL_MB` | `6144` | RAM reported to the guest |
| `LUNARIA_HEAP_MB` | A32 `256` / A64 full window (~`2560`) | Guest malloc arena |
| `LUNARIA_THREAD_TICKS` / `LUNARIA_A64_THREAD_TICKS` | `200M` / `20K` | ARM32 / AArch64 worker scheduling slice |
| `LUNARIA_A64_ENGINES` | auto | AArch64 JIT host threads (1–8) |
| `LUNARIA_A64_SELF_SCHED` | `1` (launcher) | `0` for barrier-pooled passes; the bare binary stays off until set |
| `LUNARIA_A64_FASTMEM` | `1` | `0` routes memory through callbacks |
| `LUNARIA_A64_CODE_CACHE_MB` | `128` | Per-JIT code cache |
| `LUNARIA_FD_PARK` | on / off | Park blocking reads |
| `LUNARIA_SYSLIB_DIR` | `syslib-arm64/` if present | AArch64 platform libraries run as guest code |
| `LUNARIA_VULKAN` | on when available | `0` disables the Vulkan bridge |
| `LUNARIA_DEVICE` | `pixel6` | `pixel6`, `pixel7`, `galaxys21`, `lunaria` (prefer `lunaria.conf`) |
| `LUNARIA_DATA_ROOT` | startup directory | Guest `/data` and `/storage` (`--data-root DIR`) |
| `LUNARIA_CONF` | `lunaria.conf` in startup directory | Settings file (`--config FILE`); no fallback search |
| `LUNARIA_CACHE_DIR` / `LUNARIA_NO_CACHE` | `cache/packages/` / off | Package cache location / force fresh unpack |
| `LUNARIA_WEB_ENGINE` | `auto` | `auto`, `chrome`, `luna`, `off` (host menu overrides per session) |
| `LUNARIA_KEYMAP` | off | `genshin`, `crossworlds`, `off`, or a keymap path |
| `LUNARIA_NET` | on | `0` denies all guest sockets |
| `LUNARIA_JIT_UI` | `1` | Boot card; `0` stays blank until the guest draws |
| `LUNARIA_DVM` | `1` | Dalvik emulator: `0` off, `1` only where no host stub exists, `2` prefer dex over stubs |
| `LUNARIA_DEX_START` | on | Start the Activity lifecycle from dex; `0` only to compare the legacy launch |
| `LUNARIA_TOUCH_TEST` | off | Inject taps `x,y[;x,y…]` (max 8); `_FRAME` / `_HOLD` / `_GAP` default `60` / `10` / `60` |
| `LUNARIA_TOUCH_FIFO` | off | Taps `x,y[,hold]`, `key N` (4 = Back), drags `down`/`move`/`up x,y`; Windows: `\\.\pipe\lunaria-touch` |
| `LUNARIA_DUMP_FRAME` / `_SCREENSHOT_EVERY` / `_DUMP_DIR` | off / off / `/tmp` | Frame capture |
| `LUNARIA_TRACE_SVC` / `_TRACE_MEDIA` / `_TRACE_HEAP` | off | Trace SVC calls / MediaCodec states / corrupt free-list entries |
| `LUNARIA_DVM_TRACE` | off | Log methods the Dalvik emulator declined |
| `LUNARIA_HEAP_POISON` / `_HEAP_SELFTEST` | off | Fill bump allocations with `0xFF` / self-test the allocator at startup |
| `LUNARIA_A64_PCPROF` (`_EVERY` `20000`) | off | Sample hot guest PCs per thread |
| `LUNARIA_SCHED_DUMP` (`_STACK`) | off | Dump guest thread state every *N* scheduler passes (with stack scan) |
| `LUNARIA_DUMP_LAST_SVC` | off | Print the recent SVC ring at shutdown |

The launcher activity and application class are passed as `ANDROID_LAUNCH_ACTIVITY` and `ANDROID_APPLICATION_CLASS`; native engines load through the app's own `System.loadLibrary`.

## Repository map

| Path | Role |
|---|---|
| `src/arm_exec.cpp`, `src/arm.c` / `.h` | dynarmic engines, SVC dispatch, JNI, EGL/GLES, threading, shared ARM lock |
| `src/dvm/` | Dalvik interpreter for the APK's `classes*.dex` (media queues in `dvm_media.c`, signing blocks in `apk_signers.c`) |
| `src/loader.c` | Application, providers, launcher Activity and the frame pump |
| `src/apk.c` | Package extraction, manifest parsing, caching and launch |
| `src/linker/`, `src/jvm/`, `src/lib/` | Android ELF linker · JVM/JNI object model · host implementations (`guest.c` is the in-guest libc) |
| `src/luna_vulkan.c` | Vulkan bridge |
| `src/luna_overlay.c`, `src/luna_boot.c`, `src/luna_ime.c` | Host menu and compositing · boot card · IME bridge |
| `src/webview_cdp.c`, `luna-browser/` | DevTools client · built-in WebView engine (luna-ui + QuickJS) |
| `src/luna_unicode.c` | Unicode classification, normalization, IDNA2003 |
| `keymaps/`, `lunaria.conf.sample`, `runtime/` | Touch maps · settings sample · generated host shared libraries |
| `PROGRESS.md` | Compatibility notes and current work |

## Troubleshooting

<details>
<summary><strong>The startup movie does not play</strong></summary>

OpenH264 supports Constrained Baseline only; many startup movies (for example Blade & Soul Revolution) use Main/High features such as CABAC and B-frames. After 16 access units without a frame, Lunaria reports `MEDIA_ERROR_UNSUPPORTED` so a compatible engine can skip the video. Supported streams, such as Blade & Soul Masia's opening, reach end-of-stream with OpenH264 installed (`make fetch-openh264`). Use `LUNARIA_TRACE_MEDIA=1` to inspect.

</details>

<details>
<summary><strong>"project file not found"</strong></summary>

The app needs an OBB. Place it beside the APK under its original name.

</details>

<details>
<summary><strong>A runtime stub library is missing</strong></summary>

```bash
make runtime/libmediandk.so runtime/libGLESv3.so
```

</details>

<details>
<summary><strong>Black screen or stalled startup</strong></summary>

Check that all split APKs and OBBs are present, then capture a trace:

```bash
LUNARIA_TRACE_EXC=1 LUNARIA_DUMP_LAST_SVC=1 ./lunaria game.apk 2>&1 | tee lunaria.log
```

For Unreal Engine 5, prefer hardware OpenGL: `LIBGL_ALWAYS_SOFTWARE=1` (llvmpipe) can make startup look like a hang.

</details>

<details>
<summary><strong>Rendering is slow</strong></summary>

llvmpipe is for headless tests, not performance. Enable hardware EGL/OpenGL; on AArch64 try `LUNARIA_A64_ENGINES=4` once a single engine is stable. Leave `LUNARIA_SLICE_DETAIL` and `LUNARIA_SVC_HISTO` off when benchmarking.

</details>

<details>
<summary><strong>Clicks do nothing</strong></summary>

GLFW ignores synthetic clicks from `xdotool` and `XSendEvent`. Click the real window, or inject taps with `LUNARIA_TOUCH_TEST` / `LUNARIA_TOUCH_FIFO`. Right-click is reserved for the host menu.

</details>

## Project status

Lunaria is **research software under active development**, with known API gaps, title-specific failures and breaking changes. Results are snapshots, not guarantees.

- **Linux:** captured guest frames match the GLFW window in the tested setup.
- **Apple Silicon macOS:** `make dist-mac` builds a package, but the window may stay black even while frames render. Cross Worlds 5.04.12 reached the opening movie, 3D intro and terms dialog.
- **Windows:** `make dist` and `make dist-cross-win` (rebuilt 2026-11-09) produce the ZIP. Extracted builds pass ARM32/ARM64 ABI and EGL/GLES initialization under Wine; display and audio on native Windows are unverified.

Release builds use `-O3`, `-DNDEBUG` and LTO; dynarmic is built separately in Release with IPO and without `-march=native`.

**Contributions and reproducible reports are welcome.** Please include the title, engine version, guest ABI, last working screen, logs and, if possible, a captured frame.

Licensed under the [Mozilla Public License 2.0](LICENSE). Project Lunaria © 2026 Yuichiro Nakada.
