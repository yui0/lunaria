# Lunaria

### Androidゲームを、いつものPCで。

[![License: MPL 2.0](https://img.shields.io/badge/license-MPL--2.0-5b5bd6.svg)](LICENSE)
[![Guests](https://img.shields.io/badge/guest-ARM32%20%7C%20ARM64-20232a.svg)](#仕組み)
[![Engines](https://img.shields.io/badge/engines-Unity%20%7C%20Unreal%20%7C%20native-20232a.svg)](#互換性)
[![Sponsor](https://img.shields.io/badge/sponsor-%E2%99%A5-ea4aaa.svg)](https://github.com/sponsors/yui0)

[English](README.md)

**Lunariaは、Androidのシステムイメージを使わずに、AndroidアプリやゲームをPC上で動かすための互換レイヤーです。**
Android OS全体を仮想マシンで起動する代わりに、アプリ本体を直接読み込み、ARM32／ARM64のコードを
[dynarmic](https://github.com/merryhime/dynarmic)で実行します。JNI、EGL、OpenGL ES、MediaCodecなどの
Android向け機能はホスト側へ橋渡しし、APKに含まれるJavaバイトコードは内蔵のDalvikインタプリタで処理します。

`.apk`、`.xapk`、`.apks`、`.aab` に対応。パッケージの展開やマニフェスト解析、split APKの統合、キャッシュ管理、
アカウントごとのデータ分離まで、Lunariaがまとめて行います。WebViewはChrome／Chromium、または内蔵の
[luna-browser](luna-browser/)を使って表示します。

**動作を確認したタイトル：** 原神、二ノ国：Cross Worlds、アルケミアストーリー、
気球！エアロノート（2013年のARM32向けAPK）、Black Clover: Asta Fightなど。
ただし、プレイできる範囲はタイトルによって異なります。詳しくは[互換性](#互換性)をご覧ください。

起動処理も、Androidアプリ本来の流れを尊重しています。マニフェストに従って`Application`と
ContentProviderを初期化し、ランチャー`Activity`を起動します。Unreal Engineなどの`NativeActivity`を使う
アプリも、Androidのライフサイクルに沿って処理するため、ゲームごとの専用ローダーは必要ありません。

> [!IMPORTANT]
> **Lunariaは開発中の実験的なソフトウェアです。** 完全なAndroidエミュレーターではなく、
> ゲームによって動作状況が異なります。実際にプレイできるものもあれば、タイトル画面までのものもあります。
> 以下の画像は、別のエミュレーターではなく、Lunariaが描画したフレームを直接取得したものです。

<p align="center">
  <img src="shots/genshin-mondstadt-night.png" width="100%" alt="原神：夜のモンド城を滑空する蛍。風車と街灯、タッチ HUD">
</p>

<p align="center">
  <img src="shots/genshin-glide-coast.png" width="32%" alt="原神：夕暮れの海岸へ向かって滑空する蛍">
  &nbsp;
  <img src="shots/genshin-lisa.png" width="32%" alt="原神：四風を語るリサ。日本語の会話 UI">
  &nbsp;
  <img src="shots/cross-worlds-crystal.png" width="32%" alt="二ノ国：Cross Worlds、装飾された広間に浮かぶ青い結晶">
</p>

<p align="center"><sub>夜のモンド城を飛ぶ · 夕暮れの海岸 · リサとの会話 · Cross Worldsの結晶の間<br>
原神 7.1.0（Unity IL2CPP、arm64-v8a）· 二ノ国：Cross Worlds（Unreal Engine 4、arm64-v8a）<br>
Linux上で取得したゲストフレーム。ゲーム本体は改変していません。</sub></p>

<p align="center">
  <img src="shots/alchemia-story-title.png" width="48%" alt="Alchemia Story のタイトル画面">
  &nbsp;
  <img src="shots/cross-worlds-lighthouse.png" width="48%" alt="Cross Worlds：夕暮れの灯台そばでフィールドプレイ">
</p>

<p align="center"><sub>Alchemia Story 1.0.154 · Cross Worlds のフィールド</sub></p>

<p align="center">
  <img src="shots/launcher.png" width="72%" alt="Lunaria ランチャー：APK / XAPK / APKS / AAB のドロップと設定">
</p>

<p align="center"><sub>パッケージをドラッグ＆ドロップするだけで起動できます。データ保存先や端末設定は<code>lunaria.conf</code>に保存できます。</sub></p>

## Lunariaの特徴

| 特徴 | できること |
|---|---|
| **Androidのシステムイメージが不要** | Android OSを仮想マシンで起動せず、`.apk`、`.xapk`、`.apks`、`.aab`を直接読み込みます。 |
| **ARM32／ARM64に対応** | dynarmicのJITコンパイラでARMv7とAArch64のコードを実行します。 |
| **ホストGPUを活用** | EGL／OpenGL ES 3をホスト側へ橋渡しします。Vulkanも利用可能な環境では公開します。 |
| **ゲームに必要なAndroid機能を実装** | JNI、AssetManager、OBB、pthread、OpenSL ES、各種Android APIの互換処理を提供します。 |
| **APK内のJavaコードを実行** | ホスト側に対応するスタブがない場合、内蔵Dalvikインタプリタで`classes*.dex`のメソッドを処理します。 |
| **動画・音声にも対応** | 対応するH.264ストリームはOpenH264で、AACはlibavcodecが使える環境でデコードします。 |
| **WebViewを表示** | Chrome／Chromiumや内蔵のluna-browserを使って、Webコンテンツをゲーム画面へ合成します。 |
| **端末プロファイルを切り替え** | `lunaria.conf`で端末情報を設定できます。標準設定はPixel 6です。 |
| **AArch64の並列実行** | `LUNARIA_A64_ENGINES`で複数のホストスレッドに処理を分散できます。 |
| **キーボード・マウスで操作** | タッチ操作をキーへ割り当て、移動・スキル・視点操作を組み合わせられます。マクロや入力の録画・再生にも対応します。 |
| **設定とアカウントを保持** | ゲームの設定を次回起動へ引き継ぎ、`--profile`でアカウント別のデータを分離できます。 |
| **画面なしの実行・診断** | X11を使わないEGL描画、フレームの保存、JITの性能計測、SVCトレースなどを利用できます。 |

## ギャラリー

掲載画像は、Lunariaが描画したフレームをGLFWウィンドウまたはヘッドレスEGLから直接保存したものです。スマートフォンや、ほかのAndroidエミュレーターの画面を撮影したものではありません。

### 原神 7.1.0

Unity IL2CPP版（`arm64-v8a`）で、**1024×576**の描画を確認しています。Google Playのアプリ内課金には、現時点で対応していません。

<p align="center">
  <img src="shots/genshin-wings.png" width="48%" alt="原神：青空を背景に風の翼を開いた蛍">
  &nbsp;
  <img src="shots/genshin-lisa-obtained.png" width="48%" alt="原神：新キャラクター獲得画面のリサ">
</p>

<p align="center">
  <img src="shots/genshin-prologue.png" width="48%" alt="原神プロローグ：蛍、パイモン、浮遊する仲間が赤く光る石を囲む場面">
  &nbsp;
  <img src="shots/genshin-paimon.png" width="48%" alt="原神プロローグのカットシーン：指を差すパイモンと見守る蛍">
</p>

<p align="center">
  <img src="shots/genshin-knights.png" width="48%" alt="原神：西風騎士団の門前に立つ旅人。近くにガイアとアンバー">
  &nbsp;
  <img src="shots/genshin-jean.png" width="48%" alt="原神：騎士団の図書室に立つジン">
</p>

<p align="center">
  <img src="shots/genshin-lakeshore.png" width="48%" alt="原神：湖畔に立つ旅人">
  &nbsp;
  <img src="shots/genshin-shore.png" width="48%" alt="原神：夕暮れのモンド城外、水辺に立つ旅人">
</p>

<p align="center">
  <img src="shots/genshin-swim.png" width="100%" alt="原神：夕暮れのモンド城外を泳ぐ旅人">
</p>

<p align="center"><sub>飛行 · リサ獲得 · プロローグ · 騎士団 · 湖畔 · 夕暮れの水辺</sub></p>

<details>
<summary>ログイン、ダウンロード、設定</summary>

<p align="center">
  <img src="shots/genshin-shaders.png" width="48%" alt="原神のシェーダーコンパイル中の映像。青空の下の柱廊">
  &nbsp;
  <img src="shots/genshin-tap.png" width="48%" alt="原神の夕焼けの門。TAP TO BEGIN">
</p>

<p align="center">
  <img src="shots/genshin-download.png" width="48%" alt="原神：夜の柱廊の上でアップデートをダウンロード中">
  &nbsp;
  <img src="shots/genshin-login.png" width="48%" alt="原神のログイン画面。雲上の橋と夜空">
</p>

<p align="center">
  <img src="shots/genshin-server.png" width="48%" alt="原神のサーバー選択画面。Asia を選択">
  &nbsp;
  <img src="shots/genshin-library.png" width="48%" alt="原神：図書室の時計の横、横顔のジン">
</p>

<p align="center">
  <img src="shots/genshin-language.png" width="72%" alt="原神の言語設定。日本語を表示">
</p>

<p align="center"><sub>シェーダーコンパイル · TAP TO BEGIN · ダウンロード · ログイン · サーバー · 図書室 · 言語</sub></p>

</details>

このほか、シェーダーのコンパイル画面（`shots/genshin-compile.png`、
`shots/genshin-compile-early.png`）、データ読み込み（`shots/genshin-loading.png`）、
別の **TAP TO BEGIN** 画面（`shots/genshin-begin.png`）、元素の演出
（`shots/genshin-elements.png`）、HoYoverseのロゴ画面（`shots/genshin-splash.png`）、
ゲーム内メール（`shots/genshin-mail.png`）の画像もあります。

### 二ノ国：Cross Worlds

Unreal Engine 4を採用した商用タイトルです。ログイン、サーバー選択、キャラクター選択から、最初の村、ストーリー会話、音声付きムービーを経て、フィールドでのゲームプレイまで確認できています。描画サイズは**1024×576**です。

<p align="center">
  <img src="shots/cross-worlds-crystal.png" width="48%" alt="Cross Worlds：広間に浮かぶ青い結晶">
  &nbsp;
  <img src="shots/cross-worlds-peek.png" width="48%" alt="Cross Worlds：茂みから覗く少女と使い魔">
</p>

<p align="center">
  <img src="shots/cross-worlds-village.png" width="48%" alt="Cross Worlds の開始地点の村">
  &nbsp;
  <img src="shots/cross-worlds-play.png" width="48%" alt="Cross Worlds のゲーム内プレイ。クエストと HUD を表示">
</p>

<p align="center">
  <img src="shots/cross-worlds-chloe.png" width="48%" alt="Cross Worlds のストーリー会話。クロエ">
  &nbsp;
  <img src="shots/cross-worlds-characters.png" width="48%" alt="Cross Worlds のキャラクター選択画面">
</p>

<p align="center">
  <img src="shots/cross-worlds-cutscene.png" width="72%" alt="Cross Worlds のカットシーン">
</p>

<p align="center"><sub>結晶の間 · 茂みから顔を出す少女と使い魔 · 最初の村 · フィールド探索 · クロエとの会話 · キャラクター選択 · ムービーシーン</sub></p>

タッチ操作を自動化する場合は`LUNARIA_TOUCH_TEST`を使用します。`50,84`のような数値は画面サイズに対する割合で、`50%,84%`と同じ位置を示します。`640px,606px`のように`px`を付けると、ゲーム側のピクセル座標を直接指定できます。なお、GLFWは`xdotool`による疑似クリックを受け付けません。

<details>
<summary>タイトル、サーバー、イントロ</summary>

<p align="center">
  <img src="shots/cross-worlds-title.png" width="48%" alt="Cross Worlds のタイトル画面">
  &nbsp;
  <img src="shots/cross-worlds-servers.png" width="48%" alt="Cross Worlds のサーバー選択画面（Luxelion）">
</p>

<p align="center">
  <img src="shots/cross-worlds-intro.png" width="48%" alt="Cross Worlds の 3D イントロ">
  &nbsp;
  <img src="shots/cross-worlds-evermore.png" width="48%" alt="Cross Worlds のエバーモア到着場面">
</p>

<p align="center">
  <img src="shots/cross-worlds-account.png" width="48%" alt="Cross Worlds のアカウント連携ダイアログ">
  &nbsp;
  <img src="shots/cross-worlds-rest.png" width="48%" alt="Cross Worlds の省電力画面。休憩中">
</p>

</details>

### アルケミアストーリー（Alchemia Story）

アソビモのRPG『アルケミアストーリー』も、Unity IL2CPP（`arm64-v8a`）で動作を確認しています。タイトル画面、ストーリー、キャラクター作成画面を描画でき、横画面**1024×576**と縦画面**576×1024**の両方に対応します。

<p align="center">
  <img src="shots/alchemia-story-cave.png" width="48%" alt="Alchemia Story：洞窟で驚く黒髪のヒロイン。日本語の会話枠">
  &nbsp;
  <img src="shots/alchemia-story-neko.png" width="26%" alt="Alchemia Story：白髪の猫耳少女。縦画面">
</p>

<p align="center"><sub>横画面のストーリーシーン · 縦画面のキャラクター表示</sub></p>

### [気球！エアロノート](https://github.com/yui0/Kikyu)

2013年に制作された`armeabi`版APKも、ARM32実行環境で起動できます。オリジナルの`libCatNative.so`を読み込み、月夜の街並みやドット絵の主人公が登場するタイトル画面を描画しています。

<p align="center">
  <img src="shots/kikyu-title.png" width="48%" alt="気球！エアロノートのタイトル画面">
</p>

<p align="center"><sub>2013年のAPKを改変せずに実行</sub></p>

### Cross Worldsのほかのシーン

<p align="center">
  <img src="shots/cross-worlds-portal.png" width="32%" alt="Cross Worlds：青い結晶の前の二人">
  &nbsp;
  <img src="shots/cross-worlds-gatling.png" width="32%" alt="Cross Worlds：ガトリングを構える赤髪のヒロイン">
  &nbsp;
  <img src="shots/cross-worlds-lighthouse.png" width="32%" alt="Cross Worlds：夕暮れのフィールドと HUD">
</p>

### オープンソース作品・小規模なゲーム

<p align="center">
  <img src="shots/between-two-worlds.png" width="48%" alt="Between Two Worlds のメインメニュー">
  &nbsp;
  <img src="shots/fpsmobile.png" width="48%" alt="FPSMobile：First Person Template と仮想スティック">
</p>

<p align="center"><sub>Between Two Worlds · Unity 2023 IL2CPP · 1280×720 &nbsp;·&nbsp; FPSMobile · UE4 · armeabi-v7a</sub></p>

<p align="center">
  <img src="shots/unity-sample.png" width="48%" alt="UnitySampleGame の 3D ゲームプレイ">
  &nbsp;
  <img src="shots/black-clover.png" width="48%" alt="Black Clover: Asta Fight のステージプレイ">
</p>

<p align="center"><sub>UnitySampleGame：Start から 3D シーンへ · 1280×720 &nbsp;·&nbsp; Black Clover：ゲームプレイ · 1024×576</sub></p>

<p align="center">
  <img src="shots/demo-project-light.png" width="48%" alt="Demo Project Light のタイトル。TAP THE SCREEN">
  &nbsp;
  <img src="shots/unreal-first-person.png" width="48%" alt="Unreal Engine 4 の FirstPersonExampleMap">
</p>

<p align="center">
  <img src="shots/time-locker.png" width="28%" alt="TIME LOCKER の縦画面チュートリアル">
</p>

<p align="center"><sub>Demo Project Light · Unreal サンプル · Mesa llvmpipe &nbsp;·&nbsp; TIME LOCKER · 720×1280</sub></p>

### Lunariaの操作画面

Lunariaを引数なしで起動すると、パッケージを選ぶランチャーが開きます。`.apk`、`.xapk`、`.apks`、`.aab`を
ドラッグ＆ドロップするだけで読み込めます。保存先や端末プロファイルなどの設定は、`lunaria.conf`に記録できます。

大規模なゲームでは、ライブラリの読み込みやARMコードの変換、dexのコンパイルに時間がかかることがあります。
その間は専用の起動画面に進行状況が表示されます。

ゲーム画面で**右クリックするとLunariaのメニュー**を開けます。スクリーンショット（**F12**）、
文字入力、戻る操作、音量調整、キーボード割り当て、WebViewの設定、入力の録画・再生、
プロファイル切り替え、再起動、フルスクリーン表示、終了などをここから操作できます。

<p align="center">
  <img src="shots/launcher.png" width="56%" alt="Lunaria ランチャー。青空の上の Open an application">
  &nbsp;
  <img src="shots/boot.png" width="40%" alt="Lunaria 起動カード：月のワードマークと ARM／dex 進捗">
</p>

<p align="center">
  <img src="shots/host-menu-kikyu.png" width="56%" alt="気球！エアロノート上のホストメニュー。Keyboard Controls とキーマップ一覧">
  &nbsp;
  <img src="shots/host-menu.png" width="40%" alt="WebView エンジン選択。luna-browser を選択中">
</p>

<p align="center"><sub>ランチャー · 起動カード · 2013 年 APK 上のホストメニュー · WebView エンジン</sub></p>

## クイックスタート

### リリース版のインストール（Linux／macOS）

```bash
curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | bash
```

[GitHub Releases](https://github.com/yui0/lunaria/releases)から環境に合うアセットを取得し、`~/.local`（root実行時は`/usr/local`）へ入れます。macOSではあわせて`Lunaria.app`を`~/Applications`（root時は`/Applications`）へコピーします。

```bash
# タグを指定
curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | bash -s -- --version 2026.10.15

# システム全体へ導入
curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | sudo bash -s -- --prefix /usr/local

# アンインストール（設定ファイルは残す）
curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | bash -s -- --uninstall
```

導入後は`lunaria /path/to/app.apk`で起動できます（引数なしでグラフィカルランチャー）。手動ダウンロードは[リリースページ](https://github.com/yui0/lunaria/releases)を参照してください。

### 1. 必要なパッケージをインストール（ソースからビルドする場合）

**Ubuntu／Debian**

```bash
sudo apt install \
  build-essential cmake pkg-config \
  libboost-dev libbsd-dev libunwind-dev \
  libglfw3-dev libegl1-mesa-dev libgles2-mesa-dev \
  libssl-dev zlib1g-dev \
  libasound2-dev libvulkan-dev
```

`libvulkan-dev`はVulkanブリッジに必要なヘッダーです。ホスト側にVulkanライブラリがあれば、ブリッジは標準で有効になります。無効にする場合は`LUNARIA_VULKAN=0`を指定してください。利用時には`libvulkan.so.1`を動的に読み込みます。

**macOS（Apple Silicon）**

このディレクトリで`make`を実行すると、バージョンを固定したANGLE、MoltenVK、OpenH264などを取得してビルドします。XAPKの起動には`./lunaria-apk.sh /path/to/game.xapk`を使用してください。画面表示には現在制限があるため、[開発状況](#開発状況)も参照してください。

**Windows（x86_64）**

[MSYS2 UCRT64シェル](https://www.msys2.org/docs/environments/)を開き、次のパッケージをインストールします。

```bash
pacman -S --needed git make zip unzip bzip2 \
  mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,python,boost,glfw,angleproject,openssl,zlib,libgnurx,vulkan-headers}
```

`ANDROID_HOME`にAndroid SDK（Build ToolsのARM64用RenderScriptライブラリを含むもの）、`ANDROID_NDK_HOME`にNDKのディレクトリを指定してからビルドします。

```bash
make -j4 dist CC=gcc CXX=g++ DYNARMIC_JOBS=4
```

Windows版では、JVM、Dalvikインタプリタ、UI、Android向け互換APIを`lunaria.exe`にまとめます。完成したZIPを展開し、PowerShellから`.\lunaria.exe game.apk`を実行するか、APKを実行ファイルへドラッグ＆ドロップしてください。実行時にMSYS2、Python、別のシェル、ICUをインストールする必要はありません。

LinuxからWindows版ZIPをクロスビルドする場合は、`make -j4 dist-cross-win`を実行します。初回のみ、MSYS2 UCRT64の依存パッケージや実行用DLL、Boostのヘッダーを`/tmp/lunaria-windows-deps`へダウンロードし、次回以降は再利用します。

クロスビルドにはUCRT対応の`x86_64-w64-mingw32-gcc/g++`、CMake、Ninja、Python 3.11以降、curl、zstd対応のtar、zip、および前述のAndroid SDK／NDKが必要です。中間ファイルは`/tmp/lunaria-cross-win`と`/tmp/lunaria-dist-build/cross-windows-x86_64`へ保存され、完成したZIPは作業ディレクトリに出力されます。

既存の依存ライブラリを使用するには`WIN_DEPS=/path/to/deps make dist-cross-win`と指定します。`WIN_CROSS_PREFIX`、`WIN_ANGLE`、`WIN_GLFW_LIB`、`WIN_BOOST_INCLUDE`、`WIN_BUILD_DIR`、`WIN_DIST_BUILD_DIR`も変更可能です。

配布ZIPにはホスト側のDLLとOpenH264、ゲストJIT用のAArch64ライブラリ（`syslib-arm64/`）が含まれます。ICUやホスト用の`runtime/*.so`スタブは含まれません。WindowsのUTF-16コマンドライン引数はUTF-8に変換して処理します。

### 2. ビルド

WebP画像のデコードには、同梱の[simplewebp](https://github.com/MikuAuahDark/simplewebp/blob/main/simplewebp.h)（C言語のシングルヘッダーライブラリ、20260718版）を使用します。別途WebP用コーデックをインストールする必要はありません。BSDライセンスと特許許諾の文面は`LICENSE.simplewebp`に収録しています。

```bash
make dynarmic-build
make x86_64 -j"$(nproc)"   # ホストが x86_64 の場合。単純な `make` は 32bit x86 ABI をビルド
```

`make`や`make x86_64`を実行すると、MediaCodecのH.264処理で使うCiscoのOpenH264共有ライブラリが`runtime/`に自動的に取得されます。更新したい場合は`make fetch-openh264`を実行してください。

`make syslib`を実行すると、AArch64用の`libm`、bionic `libc`、`libz`が`syslib-arm64/`に配置されます。このディレクトリがある場合、ランチャーは`LUNARIA_SYSLIB_DIR`を自動設定します。`pow`や`sincosf`などの算術処理や一部のメモリ処理は、ホスト呼び出しを介さずゲストJIT内で実行できます。zlibについては、計測で高速だったホスト実装をSVC経由で利用します。

`make guestlib`は`src/lib/guest.c`から`liblunaria_guest.so`を生成します。現在は、待ち時間が0の`nanosleep`などをJIT内で完結させるために利用しています。

Unicodeの文字分類・正規化・IDNには組み込みテーブルを使用します。その他の文字コードはWindowsのコードページ、またはLinux／macOSの`iconv`を使用します。Windowsの日時処理にはOSのタイムゾーン情報と小規模なCLDR対応表を利用するため、**ビルド時も実行時もICUは不要です。**

### 3. パッケージを起動

```bash
./lunaria                         # ランチャー UI
./lunaria path/to/game.apk
./lunaria path/to/game.xapk
./lunaria path/to/game.apks
./lunaria path/to/game.aab
LUNARIA_ALSA_DEVICE="hw:7,0" ./lunaria Cross+Worlds_5.04.12_APKPure.xapk
```

ビルド環境に依存しない配布用パッケージも作成できます。

```bash
make dist        # lunaria-<version>-linux-x86_64.tar.gz
make dist-mac    # Lunaria.app + .dmg（Darwin のみ）
```

Lunariaはパッケージを直接解析し、ZIP／ZIP64、バイナリマニフェスト、署名ブロック、split APK、OBB、展開キャッシュを処理します。`arm64-v8a`または`armeabi-v7a`を判定した後、ネイティブライブラリを読み込んでランタイムを起動します。

XAPK／APKSでは、元のベースAPKを書き換えずに、必要なsplit APKを実行環境に重ね合わせます。AABはprotobuf形式の`BundleConfig`からベースモジュールとアセットパックを読み込みます。

展開したデータは、指定した保存先の`cache/packages/`にキャッシュされます。保存先を変更するには`LUNARIA_CACHE_DIR`、毎回展開し直すには`LUNARIA_NO_CACHE=1`を指定してください。

展開処理では進捗率、処理済み容量（MiB）、ファイル数、処理中のファイル名が表示されます。大きなファイルのコピー中も進行状況を確認できます。OBBは可能な限りハードリンクやシンボリックリンクで共有し、起動のたびに数GBをコピーするのを防ぎます。

端末プロファイルや保存先、WebViewの種類など、起動後も使う設定は`lunaria.conf`（記入例：`lunaria.conf.sample`）に保存します。環境変数にも同じ項目がある場合、その起動では環境変数が優先されます。

設定ファイルとデータ保存先の標準位置は、Lunariaを起動した作業ディレクトリです。設定ファイルは`--config FILE`または`LUNARIA_CONF`、データ保存先は`--data-root DIR`または`LUNARIA_DATA_ROOT`で変更できます。相対パスは起動ディレクトリを基準とし、空白を含むパスも使用できます。

```bash
./lunaria --config ./lunaria.conf --data-root "/mnt/games/Lunaria data" game.xapk
# macOS: --data-root /Volumes/Games/Lunaria
# Windows のシェル: --data-root D:/Games/Lunaria
```

**アカウントを分けても、ゲーム本体は共有できます。** プロファイルごとに設定、データベース、ログイン情報などを分離しつつ、インストール済みのAPKや指定したゲームリソースは共通で利用します。`default`プロファイルは従来のデータ保存先をそのまま使用します。

```bash
./lunaria --choose-profile game.xapk
./lunaria --profile account2 game.xapk
./lunaria --list-profiles game.xapk
```

`LUNARIA_PROFILE_PROMPT=1`を指定すると、起動するたびにプロファイルを選べます。共有するリソースは`LUNARIA_SHARED_ASSETS`へコロン区切りで指定し、新しいプロファイルへ一度だけコピーするファイルの一覧は`LUNARIA_PROFILE_SEED_FILES`へ指定します。原神向けの設定例も付属しています。

ARM32とARM64の両方を含むパッケージでは、実行するアーキテクチャを明示的に選択できます。

```bash
LUNARIA_ARCH=armeabi-v7a ./lunaria game.apk
LUNARIA_ARCH=arm64-v8a  ./lunaria game.apk
```

## 互換性

以下は、各タイトルで**実際に確認できた動作範囲**です。「起動できた」ことと「最後までプレイできる」ことは区別しています。ゲームのバージョンやOS、GPUドライバーによって結果が変わる場合があります。詳しい検証記録と起動方法は`PROGRESS.md`をご覧ください。

| タイトル | エンジン / ABI | 現在の状況 |
|---|---|---|
| **Ni no Kuni: Cross Worlds** | Unreal Engine 4 · AArch64 XAPK | Linux上ではログイン、サーバー・キャラクター選択、村でのストーリーや音声付きムービー、フィールドでの操作を確認しました。SharedPreferencesも保持されます。Apple Silicon搭載Macではバージョン5.04.12のオープニング動画、3Dイントロ、利用規約画面まで確認しています。規約同意後の動作は未検証です。 |
| **Blade & Soul Masia** | Unreal Engine 5 · AArch64 APKS | オープニング動画を最後まで再生し、タイトル画面と追加データの確認画面を表示。Agreeを選ぶとダウンロードが始まります。 |
| **Genshin Impact 7.1.0** | Unity IL2CPP · AArch64 XAPK | Linux上でオープンワールドまで到達。プロローグ、図書室、モンド城上空での滑空を描画できました。ログイン状態、Asiaサーバーの選択、音量設定は再起動後も保持されます。キーボード移動も動作し、ある検証環境ではフィールドで約18～22fps、設定画面で約30fpsを記録しました。アカウント用WebViewも表示できますが、Google Playの課金には未対応です。 |
| **Alchemia Story 1.0.154** | Unity IL2CPP · AArch64 APK | タイトル画面、日本語のストーリー会話（横画面・縦画面）、キャラクター作成画面を描画しました（2026-10-09）。 |
| **Kikyu! Aeronaut** | native `armeabi` · ARM32 APK（2013） | オリジナルのタイトル画面を描画し、Returnキーの入力を待つ状態まで確認しました。 |
| **Between Two Worlds** | Unity 2023 IL2CPP · ARMv7 | プレイ可能。メインストーリーのシーンまで到達 |
| **Between Two Worlds** | Unity 2023 IL2CPP · AArch64 | 記録済みの実行ではメインメニューまで到達。最近の確認では言語選択画面まで約 70 fps |
| **FPSMobile** | Unreal Engine 4 · ARMv7 | FirstPersonExampleMap を描画。16,000 回以上の swap を確認 |
| **UnitySampleGame** | Unity · ARMv7 | Start から 3D ゲーム画面に遷移し、描画が継続します（2026-10-02 確認） |
| **TIME LOCKER** | Unity · ARMv7 | 縦画面のチュートリアルゲームプレイまで到達 |
| **Black Clover: Asta Fight** | Unity IL2CPP · AArch64 XAPK | Base + split の読み込みに成功し、タイトル・ステージ選択・ゲーム画面をヘッドレスで描画 |
| **Blade & Soul Revolution** | Unreal Engine 4 · AArch64 | dex / SDK 初期化を通過してゲーム独自ダイアログまで起動。パッチサーバーへの経路がない場合、クライアントは再接続を要求します |
| **Daggerfall Unity** | Unity Mono · ARMv7 | Mono ランタイムは起動。描画はまだブロックされています |

<details>
<summary><strong>オープンソースのテストタイトルを実行する</strong></summary>

### FPSMobile / Unreal Engine 4

ソース: [Abhishrut/UnrealEngineAndroidSamples](https://github.com/Abhishrut/UnrealEngineAndroidSamples)

2つのファイルを `test/` に配置してください。ランチャーは同じディレクトリにある OBB を自動検出します。

```bash
curl -L -o test/FPSMobile-armv7.apk \
  "https://raw.githubusercontent.com/Abhishrut/UnrealEngineAndroidSamples/main/FPSMobile-armv7.apk"
curl -L -o test/main.1.com.YourCompany.FPSMobile.obb \
  "https://raw.githubusercontent.com/Abhishrut/UnrealEngineAndroidSamples/main/main.1.com.YourCompany.FPSMobile.obb"

LUNARIA_ARCH=armeabi-v7a ./lunaria test/FPSMobile-armv7.apk
```

### Between Two Worlds / Unity 2023 IL2CPP

ソース: [ShutovKS/Between-two-worlds](https://github.com/ShutovKS/Between-two-worlds/releases/tag/1.0.5)

```bash
make fetch-btw

LUNARIA_ARCH=armeabi-v7a ./lunaria test/btw-android.apk
LUNARIA_ARCH=arm64-v8a  ./lunaria test/btw-android.apk
```

この配布ファイルは拡張子が `.apk` ですが、中身は Android App Bundle です。
ランチャーは protobuf マニフェスト、base モジュール、Unity アセットパックを
直接読み込みます。`.aab` 拡張子でも起動できます。


### Daggerfall Unity / Unity Mono

ソース: [Vwing/daggerfall-unity-android](https://github.com/Vwing/daggerfall-unity-android/releases/tag/v1.1.1.8)

```bash
make fetch-libunity
./lunaria test/dfu-mono-32bit.apk
```

</details>

## キーボード・マウスで操作する

Lunariaでは、画面上の仮想ボタンやスティックにキーを割り当てることで、タッチ操作のゲームをキーボードとマウスから操作できます。

ゲーム画面で右クリックし、**Keyboard Controls**からキーマップを選んでください。ゲームの実行中でも切り替えられます。独自の設定ファイルを読み込むことも可能です。**Off**で無効化、**Reload Current File**で編集内容を再読み込みできます。読み込みに失敗しても、それまでの設定は維持されます。

```sh
LUNARIA_KEYMAP=genshin ./lunaria /path/to/Genshin.xapk
LUNARIA_KEYMAP=crossworlds ./lunaria /path/to/CrossWorlds.apks
```

付属の原神・Cross Worlds用レイアウトでは、**WASD**で移動、**F**で通常攻撃、**E／Q／1／2／3**でスキルやキャラクターを選択します。**Space**と**Shift**はゲームに応じてジャンプ・ダッシュ・回避などに割り当てられます。マウスの左ドラッグで視点を動かせ、キーによる移動やスキル操作と組み合わせることもできます。

**Ctrlを押しながらマウスホイールを回すと、2本指のピンチ操作**になります。上に回すと拡大方向、下に回すと縮小方向に動き、カーソル位置が操作の中心になります。ほかのキーやマウスによるタッチ操作は、先に解除してください。

テキスト入力欄を選んでいる間は文字入力が優先されます。また、ウィンドウが非アクティブになると押下中のキーを自動解除するため、操作が入りっぱなしになるのを防げます。

### キーマップをカスタマイズする

`keymaps/*.conf`をコピーして座標を変更し、`LUNARIA_KEYMAP=/path/to/my.conf`で読み込んでください。`lunaria.conf`へ設定を保存することもできます。マッピングを無効にする場合は`LUNARIA_KEYMAP=off`です。

```text
stick 0.15625 0.764 0.18
walk CTRL 0.04
button SPACE 0.922 0.665
button F 0.826 0.769
```

`stick`はWASDで動かす仮想スティックの中心座標（X、Y）と半径、`button`はキーを押したときにタッチする位置を指定します。XとYは画面サイズに対する**0～1の相対座標**、スティックの半径は画面の短辺を基準にした値です。

`walk`は指定したキーを押したときにスティックの可動半径を小さくします。原神では**WASDで走り、Ctrl＋WASDで歩く**設定です。

設定できるのはボタン最大8個、マクロ最大8個です。キーには大文字の英字、数字、`SPACE`、`SHIFT`、`CTRL`を使用できます。付属の設定は横画面用なので、HUDの配置や大きさを変えた場合はタッチ座標も調整してください。

### 同時タップ・連続入力・マクロ

```text
button F 0.10 0.20 0.30 0.40 0.50 0.60     # 1キーで3点を同時にタップ
button G 0.70 0.70 repeat=100 hold=20      # 押下中は100msごとに20msタップ
macro Z 0.5,0.5|0.6,0.6/40 @100 0.9,0.9    # 2点を40ms同時タップ→100ms待機→1点タップ
macro X 0.1,0.1 loop                       # キーを押している間、繰り返す
```

`button`には最大4組のX／Y座標を指定でき、同時にタップできます。`repeat=MS`はキーを押している間の繰り返し間隔（10～60000ms）、`hold=MS`は各タップの押下時間です（標準30ms。繰り返し間隔の半分が30msより短ければそちらを使用）。

`macro`では、タップ位置（`x,y`）、複数点同時タップ（`|x,y`）、押下時間（`/MS`）、待機（`@MS`）を組み合わせられます。`loop`を付けると押下中に繰り返します。実行タイミングはフレームクロックに従い、60fpsなら約16ms刻みです。

### 操作の録画・再生

**F9**または右クリックメニューの**Record Input**で、マウス・キーマップ・マクロによるタッチ操作を録画できます。データは`~/Documents/Lunaria <date>.rec`に保存され、使用できない場合は`~/`へ保存されます。

**F10**または**Play Recording**で直前の操作を再生できます。`LUNARIA_PLAY`で録画ファイルを指定し、`LUNARIA_PLAY_LOOP=1`で繰り返し再生することも可能です。座標は画面サイズに対する相対値で記録されるため、ウィンドウの大きさが変わっても再生できます。

## スクリーンショットとフレーム保存

動作中の画面を一時停止したいときは、右クリックメニューから**Pause**を選びます。**Resume**で再開します。一時停止中もホストメニューは操作できますが、ゲーム本体にはキーやマウスの入力が送られません。一時停止状態は次回起動へ引き継がれません。

特定のフレーム（swap番号）を保存するには、次のように指定します。

```bash
mkdir -p /tmp/lunaria-shots
LUNARIA_DUMP_DIR=/tmp/lunaria-shots \
LUNARIA_DUMP_FRAME=0,60,120 \
./lunaria game.apk
```

一定間隔で保存したい場合は、次のように指定します。

```bash
LUNARIA_DUMP_DIR=/tmp/lunaria-shots \
LUNARIA_SCREENSHOT_EVERY=60 \
./lunaria game.xapk
```

保存形式はPNGで、`lunaria_0000.png`のような名前が付きます。`$LUNARIA_SHOT_TRIGGER`で指定したファイル（既定は`/tmp/lunaria-shot`）を作成すると、追加の1フレームを保存できます。

通常のスクリーンショットは**F12**、またはホストメニューの**Take Screenshot**で撮影できます。保存先は`~/Pictures/Lunaria YYYY-MM-DD HH.MM.SS.png`です。

## 仕組み

```text
 APK / XAPK / APKS / AAB
     │  ベース・分割APK、ネイティブライブラリ、アセット、OBBを展開
     ▼
 Android ELF loader ─── relocation と依存関係解決
     │
     ▼
 dynarmic JIT ───────── ARMv7 または AArch64 のゲスト命令
     │
     ├── SVC bridge ─── libc · pthread · filesystem · Android APIs · sockets
     ├── JNI / DVM ─── classes · methods · AssetManager · MediaCodec · dex
     ├── WebView ────── Chrome DevTools pipe · Chrome/Chromium または luna-browser
     └── graphics ───── EGL · OpenGL ES 3 · optional Vulkan · host GPU
```

- **ARM32：** fastmemを利用した、4GiBの連続したゲストメモリ空間を使用します。
- **ARM64：** ゲストの仮想アドレスをホストアドレスに1対1で対応させ、ELF、トランポリン、JNIテーブル、スタックを高位アドレス領域へ配置します。これによりdynarmicのfastmemを利用できます。`LUNARIA_A64_ENGINES`（1～8、標準ではCPUコア数に応じて自動選択）で複数のJITエンジンを並列実行できます。標準設定の`LUNARIA_A64_SELF_SCHED=1`では各エンジンが独立して実行可能なスレッドを取得し、`0`ではフレームごとに同期する方式へ戻ります。並列実行は高速化に役立ちますが、Cross Worldsのセキュリティモジュールが約25秒後に異常終了した事例もあります。
- **ネイティブ呼び出し:** ゲストの libc、EGL、GLES、JNI 呼び出しは、
  自動生成された `SVC #n` トランポリンを通ってホスト側実装へ渡されます。
- **ゲストライブラリ:** `make syslib` は、AArch64 用の libm、libc、libz を
  `syslib-arm64/` に配置し、ゲストコードとして実行できるようにします。
  `make guestlib` は、毎回 trap させたくない処理用に `liblunaria_guest.so` を追加します。
  現在その対象になっているのは、ゼロ時間の `nanosleep` です。
  ディレクトリが存在する場合、ランチャーは `LUNARIA_SYSLIB_DIR` をそこへ向けます。
- **スレッド：** ゲストのpthreadは協調的なラウンドロビン方式で管理し、各ワーカーが独立したJITコンテキストを持ちます。mutexのロック解除時には待機スレッドへ直接処理を引き継げます。sleepは実時間に従い、ブロッキングreadは`LUNARIA_FD_PARK`で待機方式を変更できます。
- **Java：** JNI呼び出しに対応するホスト側スタブがない場合、内蔵の`src/dvm/`がAPK内の`classes*.dex`から該当メソッドを実行します（標準設定は`LUNARIA_DVM=1`）。
- **アプリの起動：** マニフェストに従って`Application`とContentProviderを初期化し、ランチャー`Activity`の`onCreate`／`onStart`／`onResume`を実行します。`NativeActivity`を利用するエンジンは`ANativeActivity_onCreate`から入り、ウィンドウや入力キューは`APP_CMD_*`として渡されます。
- **ライブラリの読み込み：** `System.loadLibrary`で指定されたライブラリとその依存関係を、Androidのリンカーに近い順序で解決します。
- **音声：** OpenSL ES、AAudio、Javaの`AudioTrack`は共通のミキサーを通してALSAへ出力されます。ブロッキング`AudioTrack.write()`が描画ループを止めないようにしています。
- **アセット：** `AssetManager`がAPKやOBBに含まれるDEFLATE／STORE形式のデータを読み込みます。
- **メディア：** `android.media.MediaCodec`はH.264をOpenH264で、AACを利用可能な場合はlibavcodecでデコードします。`LUNARIA_OPENH264`と`LUNARIA_LIBAVCODEC`で共有ライブラリの場所を指定できます。
- **WebView:** `android.webkit.WebView` は DevTools パイプ経由でホストブラウザーと通信します。
  `LUNARIA_WEB_ENGINE` には `auto`（Chrome / Chromium があればそれを使用、
  なければ luna-browser）、`chrome`、`luna`、`off` を指定できます。
  `LUNARIA_WEB_BROWSER` ではブラウザーのバイナリを直接指定できます。
  ページは画像としてビュー内に表示され、タッチやテキスト入力は同じパイプ経由で戻されます。
  フォールバック用ブラウザーは `make luna-browser` でビルドできます。
- **Vulkan:** ホストのブリッジが利用可能なら既定で有効です。`LUNARIA_VULKAN=0` で無効にできます。
  ゲストの `vk*` 呼び出しはホストローダーへ渡され、Android surface と swapchain は
  エミュレートされます。present された画像はコンポジターへ渡されます。
  Vulkan を検出しないタイトルは GLES のままで動作します。
  上記タイトルで実際に使われているのも現在は GLES 経路です。

## 実行時設定

主な起動オプションと診断用の環境変数をまとめました。ここにない開発・検証用の設定については、ソースコードと`PROGRESS.md`を参照してください。

| 変数 | 既定値 | 用途 |
|---|---:|---|
| `LUNARIA_ARCH` | auto | `armeabi-v7a` または `arm64-v8a` |
| `LUNARIA_WIDTH` / `LUNARIA_HEIGHT` | `1024` / `768`（横画面アプリでは `768` / `1024` を入れ替え） | ウィンドウまたは EGL surface のサイズ。端末パネル自体を拡大縮小する場合は `LUNARIA_SCALE` を使用 |
| `LUNARIA_PBUFFER` | auto fallback | `1` で GLFW を使わず、ヘッドレス EGL を強制 |
| `LUNARIA_MAX_FRAMES` | unlimited | *N* フレーム後にレンダーループを停止 |
| `LUNARIA_MEM_TOTAL_MB` | `6144` | ゲストへ報告する RAM 容量 |
| `LUNARIA_HEAP_MB` | A32 `256` / A64 window（約 `2560`） | ゲスト malloc arena。A64 は既定で `[HEAP_BASE, MMAP2)` 全体を使用 |
| `LUNARIA_THREAD_TICKS` | `200M` | ARM32 ワーカーのスケジューリングスライス |
| `LUNARIA_A64_THREAD_TICKS` | `20K` | AArch64 ワーカーのスケジューリングスライス |
| `LUNARIA_A64_ENGINES` | auto | AArch64 JIT エンジン用ホストスレッド数（1〜8。ホスト CPU コア数から 4 / 2 / 1 を選択） |
| `LUNARIA_A64_SELF_SCHED` | `1` | ランチャーの既定値。エンジンが実行可能ゲストを自由に取得。`0` で barrier-pooled pass に戻す。バイナリ単体ではこの変数が設定されるまで無効 |
| `LUNARIA_A64_FASTMEM` | `1` | `0` でメモリアクセスをコールバック経由に変更 |
| `LUNARIA_A64_CODE_CACHE_MB` | `128` | JIT エンジンごとの変換済みコードキャッシュ |
| `LUNARIA_FD_PARK` | on / off | ブロッキングreadを待機状態に移すか指定 |
| `LUNARIA_SYSLIB_DIR` | `syslib-arm64/`（存在する場合） | ゲストコードとして実行する AArch64 プラットフォームライブラリ。`make syslib` で libm、libc、libz を取得し、`make guestlib` で `liblunaria_guest.so` を作成 |
| `LUNARIA_VULKAN` | 利用可能なら on | `0` で Vulkan ブリッジと端末機能の公開を無効にする。タイトルは GLES も選択可能 |
| `LUNARIA_TOUCH_TEST` | off | `x,y[;x,y…]` 形式でタップを注入（最大 8 個）。単純な数値はフレームバッファーに対する百分率、`640px` はゲストピクセル |
| `LUNARIA_TOUCH_FIFO` | off | 実行中のタップやドラッグを入力。タップは`x,y[,hold]`、ドラッグは`down x,y`、`move x,y`、`up x,y`。座標は割合または`px`指定。Windowsでは`\\.\pipe\lunaria-touch`（名前付きパイプ）。 |
| `LUNARIA_KEYMAP` | off | `genshin`、`crossworlds`、`off`、またはキーマップファイルのパス。ホストメニューから実行中に変更可能 |
| `LUNARIA_DEVICE` | `pixel6` | 端末プロファイル：`pixel6`、`pixel7`、`galaxys21`、`lunaria`。通常は `lunaria.conf` を推奨 |
| `LUNARIA_DATA_ROOT` | 起動ディレクトリ | ゲストの `/data` と `/storage`。`--data-root DIR` でも指定可能 |
| `LUNARIA_CONF` | 起動ディレクトリ直下の `lunaria.conf` | 読み書きする設定ファイル。`--config FILE` でも指定可能。別の場所を自動検索しない |
| `LUNARIA_WEB_ENGINE` | `auto` | `auto`、`chrome`、`luna`、`off`。ホストメニューからセッション中に上書き可能 |
| `LUNARIA_SCALE` | off | 選択した端末パネル自体を拡大縮小（`0.5` なら半分）。未設定時は `LUNARIA_WIDTH` × `LUNARIA_HEIGHT` |
| `LUNARIA_TOUCH_FRAME` / `_HOLD` / `_GAP` | `60` / `10` / `60` | 最初の DOWN フレーム / 押下時間 / タップ間隔 |
| `LUNARIA_PERF_S` | `10` | `[perf]` の出力間隔（秒）。`0` で無効 |
| `LUNARIA_DUMP_FRAME` | off | キャプチャする swap 番号をカンマ区切りで指定 |
| `LUNARIA_SCREENSHOT_EVERY` | off | *N* 回の swap ごとにキャプチャ |
| `LUNARIA_DUMP_DIR` | `/tmp` | フレーム出力先 |
| `LUNARIA_TRACE_SVC` | off | ゲスト→ホストの SVC 呼び出しをトレース |
| `LUNARIA_TRACE_HEAP` | off | ゲスト malloc の破損した free-list エントリを診断 |
| `LUNARIA_HEAP_POISON` | off | calloc 以外の bump allocation を `0xFF` で埋め、未初期化読み込みを発見しやすくする |
| `LUNARIA_HEAP_SELFTEST` | off | ゲストコード読み込み前に alignment、calloc reuse、trim、coalescing、realloc growth、memalign を検証 |
| `LUNARIA_TRACE_MEDIA` | off | MediaCodec の状態遷移をトレース |
| `LUNARIA_A64_PCPROF` | off | AArch64 ゲストのホット PC をスレッド単位でサンプリングして表示 |
| `LUNARIA_A64_PCPROF_EVERY` | `20000` | profiler レポート間のサンプル数 |
| `LUNARIA_SCHED_DUMP` | off | *N* 回の scheduler pass ごとに全ゲストスレッドの状態を出力 |
| `LUNARIA_SCHED_DUMP_STACK` | off | 上記出力に各スレッドのスタック内リターンアドレススキャンを追加 |
| `LUNARIA_DUMP_LAST_SVC` | off | 終了時に直近のSVC呼び出し履歴を表示 |
| `LUNARIA_JIT_UI` | `1` | ARMコード変換やdex処理の進捗画面を表示。`0`ではゲームの描画が始まるまで何も表示しない |
| `LUNARIA_DVM` | `1` | Dalvik バイトコードエミュレーター。`0` 無効、`1` ホストスタブがない場合のみ APK の dex を実行、`2` ホストスタブより dex を優先 |
| `LUNARIA_DVM_TRACE` | off | Dalvikインタプリタで処理できなかったメソッドを`[dvm] miss …`として記録 |
| `LUNARIA_DEX_START` | on | APK の Activity ライフサイクルを dex から実行。旧来の手書き起動シーケンスと比較する場合のみ `0` |
| `LUNARIA_NET` | on | `0` でゲストの全 socket 通信を拒否 |
| `LUNARIA_CACHE_DIR` | 起動時のデータ保存先の `cache/packages/` | ランチャーの永続展開キャッシュ。個別に移動可能 |
| `LUNARIA_NO_CACHE` | off | APK / XAPK / APKS を毎回新しく展開 |

特定のエンジン用エントリポイントがなくても、Lunariaはマニフェストで指定されたランチャー`Activity`から起動を試みます。`ANDROID_LAUNCH_ACTIVITY`と`ANDROID_APPLICATION_CLASS`にクラス情報を渡し、バイトコードVMでアプリのライフサイクルを実行します。ネイティブライブラリはアプリ自身の`System.loadLibrary`から読み込まれます。

tick値には`K`、`M`、`G`の接尾辞が使えます（例：`LUNARIA_ONLOAD_TICKS=5G`）。

## リポジトリ構成

| パス | 役割 |
|---|---|
| `src/arm_exec.cpp` | dynarmic エンジン、SVC dispatch、JNI、EGL/GLES、スレッド処理 |
| `src/arm.c` / `src/arm.h` | 共通 ARM 実行ロックとマルチエンジン補助 |
| `src/dvm/` | Dalvik バイトコードエミュレーター。APK の `classes*.dex` に含まれる Java を実行 |
| `src/loader.c` | マニフェストに基づく`Application`・ContentProvider・`Activity`の起動とフレームループ |
| `src/dvm/audio_track.c` | Javaの`AudioTrack`から共通ミキサーへのキュー処理 |
| `src/dvm/apk_signers.c` | `PackageInfo`で参照するAPK署名ブロックの解析 |
| `src/linker/` | ホスト向けに調整した Android ELF linker |
| `src/jvm/` | 軽量 JVM / JNI オブジェクトモデルとスタブ |
| `src/lib/` | Android ネイティブコードへ公開するホスト側実装。`guest.c` はゲスト内 libc |
| `src/luna_vulkan.c` | ホストドライバーが利用可能なら有効になる Vulkan ブリッジ |
| `src/luna_overlay.c` | エミュレーター独自 UI：起動画面、ホストメニュー、ゲストフレーム上への合成 |
| `src/luna_boot.c` | 起動画面：空、雪、進捗リング、ロゴ、変換進捗、dex 進捗 |
| `src/luna_ime.c` | ホスト IME からゲストテキスト入力へのブリッジ |
| `src/webview_cdp.c` | Chrome と luna-browser が共有する DevTools パイプクライアント |
| `luna-browser/` | 内蔵 WebView エンジン（luna-ui + QuickJS）。`make luna-browser` でビルド |
| `keymaps/` | 同梱タッチマップ（`genshin.conf`、`crossworlds.conf`） |
| `lunaria.conf.sample` | 端末プロファイル、data root、画面スケール、パッケージ台帳、WebView エンジン |
| `runtime/` | 生成された Android 互換ホスト共有ライブラリ |
| `src/apk.c` | APK／XAPK／APKS／AABの読み込み、展開キャッシュ、マニフェスト解析、起動処理 |
| `src/luna_unicode.c` | 組み込み Unicode 分類・正規化・IDNA2003 |
| `PROGRESS.md` | 詳細な互換性メモと現在の開発状況 |

## トラブルシューティング

<details>
<summary><strong>起動時のムービーが再生されない</strong></summary>

LunariaはH.264動画のデコードにOpenH264を使用します。ただし、現在対応しているConstrained Baselineプロファイルでは、すべてのゲーム内動画を再生できるわけではありません。例えば、Blade & Soul Revolutionのオープニングなどに使われているMain／Highプロファイル（CABACやBフレームを含む）は未対応です。

デコードを16アクセスユニット試しても映像が得られない場合、Lunariaは`MEDIA_ERROR_UNSUPPORTED`をゲーム側へ通知します。これにより、対応したゲームエンジンは再生できない動画をスキップし、起動処理を続けられます。以前のように映像の終了を待ち続け、白画面から進めなくなる状況を避けるための処理です。

Blade & Soul Masiaのオープニングなど、対応するプロファイルの動画はOpenH264が利用可能であれば最後まで再生できます（`make fetch-openh264`）。AAC音声には、ホスト側で利用可能な場合にlibavcodecを使用します。デコードの状況は`LUNARIA_TRACE_MEDIA=1`で確認できます。

</details>

<details>
<summary><strong>ゲームに「project file not found」と表示される</strong></summary>

そのタイトルが OBB 拡張ファイルを必要としている可能性があります。
元のファイル名のまま APK と同じ場所へ配置してください。

</details>

<details>
<summary><strong>ランタイム用スタブライブラリが見つからない</strong></summary>

必要なターゲットを明示的にビルドしてください。

```bash
make runtime/libmediandk.so runtime/libGLESv3.so
```

</details>

<details>
<summary><strong>画面が真っ黒、または起動途中で止まる</strong></summary>

必要なOBBファイルやsplit APKが揃っているか確認してください。それでも止まる場合は、次のコマンドで診断ログを取得します。

```bash
LUNARIA_TRACE_EXC=1 LUNARIA_DUMP_LAST_SVC=1 \
./lunaria game.apk 2>&1 | tee lunaria.log
```

Unreal Engine 5のゲームでは、可能な限りGPUによるOpenGL描画を使用してください。`LIBGL_ALWAYS_SOFTWARE=1`を指定するとllvmpipeによるソフトウェア描画になり、起動が極端に遅くなる場合があります。フリーズに見えても、実際には描画処理が進んでいる可能性があります。

</details>

<details>
<summary><strong>描画が遅い</strong></summary>

MesaのllvmpipeはGPUを使わないソフトウェアレンダラーです。画面なしでの動作確認には便利ですが、性能を比較する用途には向きません。実際のゲームプレイではGPUアクセラレーションを有効にしてください。

AArch64では、まず単一のJITエンジンで安定動作を確認し、その後`LUNARIA_A64_ENGINES=4`を試すとよいでしょう。性能測定時は`LUNARIA_SLICE_DETAIL`と`LUNARIA_SVC_HISTO`を無効のままにしてください。

</details>

<details>
<summary><strong>クリックしても反応しない</strong></summary>

GLFWは`xdotool`や`XSendEvent`からの疑似クリックを受け付けません。ゲーム画面を直接クリックするか、`LUNARIA_TOUCH_TEST='50,84;…'`でタップ位置を指定してください。位置は画面に対する割合で、`px`を付ければピクセル単位でも指定できます。ゲーム起動後は`LUNARIA_TOUCH_FIFO`からも入力できます。

なお、右クリックはLunariaのホストメニュー専用で、ゲーム側には送られません。

</details>

## 開発状況

Lunariaは**現在も開発を進めている研究段階のソフトウェア**です。以下は検証環境で確認した状況であり、すべてのPCやタイトルで同じ結果になるとは限りません。

**Linux：** 検証環境では、描画されたフレームバッファーとウィンドウの表示が一致しています。

**Apple Silicon搭載Mac：** `make dist-mac`で配布物を作成できます。ただし、内部では描画できていても、ウィンドウが黒いままになる場合があります。

**Windows：** `make dist`で実行ファイルと依存DLLをまとめたZIPを作成できます。Wine上ではARM32／ARM64のABIテストとEGL／GLESの初期化を確認しました。Windows実機での画面表示と音声出力は未検証です。

配布用ビルドは`-O3`、`-DNDEBUG`、LTOで最適化し、dynarmicもRelease＋IPOで別途ビルドします。`-march=native`は使用しないため、ビルドしたPC固有の命令セットへの依存を避けています。

未実装のAndroid APIやゲーム固有の不具合、詳細な診断ログ、互換性を壊す変更が残っています。安定版の代替としてではなく、開発・検証中のプロジェクトとしてご利用ください。

**不具合報告や開発への参加を歓迎します。** ゲーム名、エンジンのバージョン、ABI、正常に進めた最後の画面、ログ、描画画像を添えていただけると、原因を調べるうえで大変助かります。

ライセンスは[Mozilla Public License 2.0](LICENSE)です。

Project Lunaria © 2026 Yuichiro Nakada.
