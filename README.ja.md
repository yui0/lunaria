# Lunaria

### Android を起動せずに、Linux 上で Android ゲームエンジンを動かす。

[![License: MPL 2.0](https://img.shields.io/badge/license-MPL--2.0-5b5bd6.svg)](LICENSE)
[![Guests](https://img.shields.io/badge/guest-ARM32%20%7C%20ARM64-20232a.svg)](#仕組み)
[![Engines](https://img.shields.io/badge/engines-Unity%20%7C%20Unreal-20232a.svg)](#互換性)
[![Sponsor](https://img.shields.io/badge/sponsor-%E2%99%A5-ea4aaa.svg)](https://github.com/sponsors/yui0)

Lunaria は、Android 向けアプリを Linux 上で動かすための実験的な変換レイヤーです。
APK、XAPK、APKS からネイティブライブラリを読み込み、ARM32/ARM64 コードを
[dynarmic](https://github.com/merryhime/dynarmic) で実行し、Android API、JNI、EGL、
OpenGL ES、MediaCodec を Linux ホスト側へ橋渡しします。APK 内の Java コードは、
内蔵の Dalvik バイトコードエミュレーターで実行されるため、Android のシステムイメージは不要です。

`android.webkit.WebView` は Chrome DevTools のパイプ経由でホスト側ブラウザーに描画されます。
利用できるのは、インストール済みの Chrome / Chromium、または内蔵の
[luna-browser](luna-browser/)（luna-ui + QuickJS）です。

> [!IMPORTANT]
> Lunaria は現在も開発中の互換性プロジェクトであり、完全な Android エミュレーターではありません。
> 対応状況はタイトルやゲームエンジンのバージョンによって異なります。

<p align="center">
  <img src="screenshot_genshin_shore.png" width="49%" alt="原神：モンド城外の水辺に立つ旅人。HUD 全体を表示">
  &nbsp;
  <img src="screenshot_crossworlds_cutscene.png" width="49%" alt="二ノ国：Cross Worlds のゲーム内カットシーン。空を背景に白髪の少年">
</p>

<p align="center">
  <img src="screenshot_genshin_dialog.png" width="49%" alt="原神プロローグ：蛍、パイモン、浮遊する仲間が赤く光る石を囲む場面">
  &nbsp;
  <img src="screenshot_genshin_cutscene.png" width="49%" alt="原神プロローグのカットシーン：指を差すパイモンと見守る蛍">
</p>

<p align="center"><sub>原神 7.1.0 · Unity IL2CPP · arm64-v8a &nbsp;·&nbsp; 二ノ国：Cross Worlds · Unreal Engine 4 · arm64-v8a<br>Linux 上のゲストフレームバッファー。APK の改変なし。</sub></p>

## Lunaria の特徴

| | 内容 |
|---|---|
| **2種類のゲストアーキテクチャ** | dynarmic JIT により ARMv7 と AArch64 を実行 |
| **Android システムイメージ不要** | Linux から `.apk`、`.xapk`、`.apks` を直接起動 |
| **実際のグラフィックス経路** | EGL と OpenGL ES 3 の呼び出しをホスト GPU に渡します。Vulkan は任意（`LUNARIA_VULKAN=1`）で、現時点では GLES が主経路です |
| **ゲームエンジンを意識したブリッジ** | JNI、AssetManager、OBB、pthread、OpenSL ES、Android API スタブ |
| **ホスト上の Dalvik** | ホスト側スタブが存在しない場合、APK の `classes*.dex` を `src/dvm/` で実行 |
| **MediaCodec 経路** | H.264 は openh264、AAC は libavcodec で処理。イントロ動画も最後まで再生可能 |
| **ホスト上の WebView** | Chrome / Chromium または luna-browser を同じ DevTools パイプで使用し、ビュー内に合成 |
| **実在端末として認識** | `lunaria.conf` で実在する市販端末プロファイルを返します（既定は Pixel 6） |
| **AArch64 の並列実行** | `LUNARIA_A64_ENGINES>1` にすると、ホストスレッド上で複数のゲストワーカーを実行 |
| **タッチ HUD をキーボード操作** | 原神と Cross Worlds 用のキーマップを同梱。WASD、スキル、マウス視点操作を同時使用可能 |
| **ヘッドレス対応** | X11 ウィンドウを利用できない場合は、サーフェスレス EGL pbuffer にフォールバック |
| **診断機能を重視** | フレームキャプチャ、JIT プロファイリング、SVC トレース、ゲストメモリ監視に対応 |

## ギャラリー

以下の画像は、Lunaria のゲストフレームバッファー（GLFW ウィンドウまたはヘッドレス EGL）
から直接取得したものです。スマートフォンの画面キャプチャや、Android エミュレーターの
ウィンドウを撮影したものではありません。

### 原神 7.1.0

arm64-v8a 上の Unity IL2CPP、フレームバッファーは **1024×576** です。
現在の実行では、HoYoverse のスプラッシュ画面、ログイン、サーバー選択を通過し、
シェーダーコンパイルとリソースダウンロードを完了した後、**TAP TO BEGIN** を越えて
実際のゲーム世界へ入り、モンド城外の海岸やパイモンとのプロローグカットシーンまで到達します。
これらのフレームでは、キャラクターや背景もカラーで描画されています。

以前の海岸シーンのキャプチャ（`screenshot_genshin_field.png`、
`screenshot_genshin_swim.png`、`screenshot_genshin_chat.png`）では、
旅人がまだ青いシルエットとして表示されています。ログイン画面の柱のマテリアルも引き続き確認中です。
歩行入力と長時間実行についても、現在検証を続けています。

<p align="center">
  <img src="screenshot_genshin_shaders.png" width="48%" alt="原神のシェーダーコンパイル中ムービー。青空の下の柱と橋">
  &nbsp;
  <img src="screenshot_genshin_login.png" width="48%" alt="原神のログイン画面。雲上の橋と夜空">
</p>

<p align="center">
  <img src="screenshot_genshin_server.png" width="48%" alt="原神のサーバー選択画面。Asia を選択">
  &nbsp;
  <img src="screenshot_genshin_mail.png" width="48%" alt="原神のギフトメール画面。メールを開いた状態">
</p>

<p align="center">
  <img src="screenshot_genshin_language.png" width="72%" alt="原神の言語設定。日本語を表示">
</p>

<p align="center"><sub>シェーダーコンパイル · ログイン · サーバー · メール · 言語設定</sub></p>

同じ起動過程では、その途中のフレームも取得されています。
シェーダー進行画面（`screenshot_genshin_compile.png`、`screenshot_genshin_compile_early.png`）、
データ読み込み（`screenshot_genshin_loading.png`）、**TAP TO BEGIN**
（`screenshot_genshin_begin.png`）、元素のスプラッシュ画面
（`screenshot_genshin_elements.png`）、HoYoverse カード
（`screenshot_genshin_splash.png`）です。

### 二ノ国：Cross Worlds

arm64-v8a 上で動作する商用 UE4 タイトルです。ゲスト側のログイン、サーバー選択、
キャラクター選択、開始地点の村、ストーリー会話、音声付きカットシーンを経て、
ゲーム内プレイまで到達しています。APK やゲスト側には一切パッチを当てておらず、
修正はすべてエミュレーター側で行っています。

プログラムからのタップ入力には `LUNARIA_TOUCH_TEST` を使用します。
単純な `x,y` はフレームバッファーに対する百分率として扱われます。
つまり `50,84` と `50%,84%` は同じ位置です。`640px,606px` のように指定すれば、
ゲスト側のピクセル座標になります。`xdotool` の合成クリックは GLFW に無視されます。
フレームバッファーは **1024×576** です。

<p align="center">
  <img src="screenshot_crossworlds_title.png" width="48%" alt="Cross Worlds のタイトル画面">
  &nbsp;
  <img src="screenshot_crossworlds_servers.png" width="48%" alt="Cross Worlds のサーバー選択画面（Luxelion）">
</p>

<p align="center">
  <img src="screenshot_crossworlds_account.png" width="48%" alt="Cross Worlds のアカウント連携ダイアログ">
  &nbsp;
  <img src="screenshot_crossworlds_character_select.png" width="48%" alt="Cross Worlds のキャラクター選択画面">
</p>

<p align="center">
  <img src="screenshot_crossworlds_village.png" width="48%" alt="Cross Worlds の開始地点の村">
  &nbsp;
  <img src="screenshot_crossworlds_ingame.png" width="48%" alt="Cross Worlds のゲーム内プレイ。クエストと HUD を表示">
</p>

<p align="center">
  <img src="screenshot_crossworlds_dialog.png" width="48%" alt="Cross Worlds のストーリー会話。クロエ">
  &nbsp;
  <img src="screenshot_crossworlds_evermore.png" width="48%" alt="Cross Worlds のエバーモア到着場面">
</p>

<p align="center">
  <img src="screenshot_crossworlds_intro.png" width="48%" alt="Cross Worlds の 3D イントロ">
  &nbsp;
  <img src="screenshot_crossworlds_power_save.png" width="48%" alt="Cross Worlds の省電力画面。休憩中">
</p>

<p align="center"><sub>タイトル · サーバー · アカウント · キャラクター選択 · 村 · ゲーム内 · クロエ · エバーモア · イントロ · クライアント内蔵の休憩画面</sub></p>

### オープンソースおよび小規模タイトル

<p align="center">
  <img src="screenshot_btw_menu.png" width="48%" alt="Between Two Worlds のメインメニュー">
  &nbsp;
  <img src="screenshot_fpsmobile_map.png" width="48%" alt="Unreal Engine 4 の FirstPersonExampleMap">
</p>

<p align="center"><sub>Between Two Worlds · Unity 2023 IL2CPP · 1280×720 &nbsp;·&nbsp; FPSMobile · UE4 · armeabi-v7a · Mesa llvmpipe</sub></p>

<p align="center">
  <img src="screenshot_unitysample_gameplay.png" width="48%" alt="UnitySampleGame の 3D ゲームプレイ">
  &nbsp;
  <img src="screenshot_timelocker_gameplay.png" width="28%" alt="TIME LOCKER の縦画面チュートリアル">
</p>

<p align="center"><sub>UnitySampleGame：以前の 1280×720 実行でプレイ可能シーンまで到達 &nbsp;·&nbsp; TIME LOCKER：720×1280 のチュートリアル</sub></p>

<p align="center">
  <img src="screenshot_blackclover_splash.png" width="360" alt="Black Clover の Unity スプラッシュ画面">
</p>

<p align="center"><sub>Black Clover: Asta Fight · Unity スプラッシュ画面、ヘッドレス</sub></p>

### Lunaria 本体

大規模タイトルでは、最初のゲストフレームが表示されるまでに、
リンク、変換、dex コンパイルに長い時間がかかる場合があります。
その待ち時間に表示されるのが Lunaria 独自の起動画面です。

ゲスト側の描画が始まった後は、右クリックでホストメニューをフレーム上に表示できます。
メニューから、スクリーンショット（F12 でも可能）、入力時の貼り付け、Back、音量、ミュート、
ALSA 出力、キーボードマップ、WebView のズームとエンジン、再読み込み、
フルスクリーン、終了を操作できます。

<p align="center">
  <img src="screenshot_lunaria_bootcard.png" width="48%" alt="Lunaria の起動画面。青空を背景に進捗リングが 95% を示す">
  &nbsp;
  <img src="screenshot_host_menu.png" width="42%" alt="Lunaria のホストメニュー。WebView エンジンのサブメニューを表示">
</p>

<p align="center"><sub>起動画面 · ホストメニュー（WebView エンジンは luna-browser、ズーム 200%、ミュート有効）</sub></p>

## クイックスタート

### 1. ビルド依存パッケージをインストール

Ubuntu または Debian の場合：

```bash
sudo apt install \
  build-essential cmake pkg-config \
  libboost-dev libbsd-dev libunwind-dev \
  libglfw3-dev libegl1-mesa-dev libgles2-mesa-dev \
  libssl-dev libicu-dev zlib1g-dev \
  libasound2-dev libvulkan-dev
```

`libvulkan-dev` は Vulkan ブリッジ用のヘッダーを提供します。
ブリッジは `LUNARIA_VULKAN=1` を設定するまで無効です。有効にすると、
ホスト側の `libvulkan.so.1` を `dlopen` します。

### 2. ビルド

```bash
make dynarmic-build
make x86_64 -j"$(nproc)"   # ホストが x86_64 の場合。単純な `make` は 32bit x86 ABI をビルド
```

`make` / `make x86_64` は、MediaCodec の H.264 に必要な Cisco の openh264 共有ライブラリを
`runtime/` に自動取得します。更新したい場合は、明示的に `make fetch-openh264` を再実行してください。

`make syslib` は `syslib-arm64/` に AArch64 用の libm、bionic libc、libz を配置します。
このディレクトリが存在する場合、ランチャーは `LUNARIA_SYSLIB_DIR` をそこへ向けます。
これにより、純粋な算術処理（`pow`、`sincosf`）や inflate をゲスト内で処理できます。

`make guestlib` は `src/lib/guest.c` から `liblunaria_guest.so` をビルドします。
ゼロ時間の `nanosleep` は JIT の外へ出ることなく処理されます。

### 3. パッケージを起動

```bash
./lunaria-apk.sh path/to/game.apk
./lunaria-apk.sh path/to/game.xapk
./lunaria-apk.sh path/to/game.apks
LUNARIA_ALSA_DEVICE="hw:7,0" ./lunaria-apk.sh Cross+Worlds_5.03.04_APKPure.xapk
```

ランチャーは `arm64-v8a` または `armeabi-v7a` を判定し、メインのネイティブライブラリを探し、
インストール済みパッケージのように見える一時構成を準備してランタイムを起動します。

XAPK / APKS では、ベース APK 自体は変更せず、split APK の内容をその一時構成へ重ね合わせます。
展開済みツリーは、パッケージ識別情報をキーとして
`${LUNARIA_CACHE_DIR:-/tmp/lunaria-cache}` 以下にキャッシュされます。
`LUNARIA_NO_CACHE=1` を指定すると、毎回新しく展開します。

1回の起動ではなくインストール単位の設定は、`lunaria.conf`
（`lunaria.conf.sample` を参照）に記述します。ここでは、ゲストに報告する端末、
ゲスト `/data` の保存先（`LUNARIA_DATA_ROOT`）、使用する WebView エンジンなどを設定します。
同じ変数が環境変数として指定されている場合は、その起動に限り環境変数が優先されます。

複数の ABI を含むパッケージでゲストアーキテクチャを固定したい場合：

```bash
LUNARIA_ARCH=armeabi-v7a ./lunaria-apk.sh game.apk
LUNARIA_ARCH=arm64-v8a  ./lunaria-apk.sh game.apk
```

## 互換性

以下は実際に確認できた到達点であり、一般的な互換性を保証するものではありません。
詳細な状況や起動手順は `PROGRESS.md` に記載しています。

| タイトル | エンジン / ABI | 現在の状況 |
|---|---|---|
| **Ni no Kuni: Cross Worlds** | Unreal Engine 4 · AArch64 XAPK | Linux 上で、ゲストログイン、村、ストーリー会話、ゲーム内カットシーン、実際のプレイまで描画を確認。SharedPreferences も保持されます。現行ビルドの確認では 3D イントロまで描画できていますが、そのビルドでの完全なプレイ確認はまだ未完了です。macOS ではフレームバッファーには描画されるものの、実際のウィンドウは黒いままです |
| **Blade & Soul Masia** | Unreal Engine 5 · AArch64 APKS | オープニング動画が EOS まで到達。タイトル画面と追加パッチのダイアログを表示でき、Agree を押すとダウンロードが進みます |
| **Genshin Impact 7.1.0** | Unity IL2CPP · AArch64 XAPK | HoYoverse スプラッシュ、ログイン、サーバー選択を通過し、シェーダーコンパイル、リソースダウンロード、**TAP TO BEGIN**、モンド城外の海岸、プロローグカットシーン（パイモン、日本語会話）まで到達。最近のフレームではキャラクターもカラー描画されています。ホスト WebView でアカウントページを開くことも可能です。歩行入力と長時間実行は引き続き検証中です |
| **Between Two Worlds** | Unity 2023 IL2CPP · ARMv7 | プレイ可能。メインストーリーのシーンまで到達 |
| **Between Two Worlds** | Unity 2023 IL2CPP · AArch64 | 記録済みの実行ではメインメニューまで到達。最近の確認では言語選択画面まで約 70 fps |
| **FPSMobile** | Unreal Engine 4 · ARMv7 | FirstPersonExampleMap を描画。16,000 回以上の swap を確認 |
| **UnitySampleGame** | Unity · ARMv7 | 以前の実行では 3D シーンまで到達。現在のビルドでは、SVC 内部から `System.loadLibrary` が ARM32 JIT に再入して abort します |
| **TIME LOCKER** | Unity · ARMv7 | 縦画面のチュートリアルゲームプレイまで到達 |
| **Black Clover: Asta Fight** | Unity IL2CPP · AArch64 XAPK | Base + split の読み込みに成功し、Unity スプラッシュ画面をヘッドレスで描画 |
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

LUNARIA_ARCH=armeabi-v7a ./lunaria-apk.sh test/FPSMobile-armv7.apk
```

### Between Two Worlds / Unity 2023 IL2CPP

ソース: [ShutovKS/Between-two-worlds](https://github.com/ShutovKS/Between-two-worlds/releases/tag/1.0.5)

```bash
make fetch-btw

LUNARIA_ARCH=armeabi-v7a ./lunaria-apk.sh test/btw-android.apk
LUNARIA_ARCH=arm64-v8a  ./lunaria-apk.sh test/btw-android.apk
```

### Daggerfall Unity / Unity Mono

ソース: [Vwing/daggerfall-unity-android](https://github.com/Vwing/daggerfall-unity-android/releases/tag/v1.1.1.8)

```bash
make fetch-libunity
./lunaria-apk.sh test/dfu-mono-32bit.apk
```

</details>

## キーボード操作

タッチ操作を前提としたゲームでも、キーと画面上のタッチ位置を対応付けることで、
キーボードから操作できます。

右クリックメニューから **Keyboard Controls** を開き、キーマップファイルを選ぶと、
ゲーム実行中でもレイアウトを切り替えられます。別のフォルダーを参照して、
独自のキーマップを読み込むこともできます。

キーボードマッピングを無効にするには **Off**、編集後のキーマップを読み直すには
**Reload Current File** を選択します。キーマップの読み込みに失敗した場合、
現在の設定は変更されません。起動時にキーマップを指定することもできます。

```sh
LUNARIA_KEYMAP=genshin ./lunaria-apk.sh /path/to/Genshin.xapk
LUNARIA_KEYMAP=crossworlds ./lunaria-apk.sh /path/to/CrossWorlds.apks
```

同梱レイアウトでは、**WASD** を移動、**F** を通常攻撃、
**E/Q/1/2/3** をスキルまたはキャラクター選択、**Space** をジャンプ、
**Shift** をダッシュに割り当てています。Cross Worlds では、
**Space** と **Shift** は回避操作に割り当てられています。

左マウスボタンでドラッグするとカメラを操作できます。
移動、割り当て済みボタン、マウス入力は同時に使用できます。

テキスト入力欄にフォーカスがある場合は、ゲーム操作よりテキスト入力が優先されます。
ウィンドウがフォーカスを失った場合、押しっぱなしになっているキーは自動的に解除されます。

別のゲームや異なる HUD レイアウトに合わせる場合は、`keymaps/*.conf` のいずれかをコピーし、
座標を調整して `LUNARIA_KEYMAP=/path/to/my.conf` で読み込んでください。
この設定は `lunaria.conf` に保存することもできます。
キーボードマッピングを無効にするには `LUNARIA_KEYMAP=off` を指定します。

```text
stick 0.15625 0.764 0.09
button SPACE 0.922 0.665
button F 0.826 0.769
```

`stick` は、WASD で操作する仮想スティックの中心 X/Y 座標と半径を定義します。
`button` は、キーを X/Y のタッチ位置へ割り当てます。
X と Y は画面の幅・高さを基準とした 0〜1 の正規化座標です。
スティック半径は画面の短辺を基準にしています。

最大 8 個のボタンを割り当てられます。
対応キーは大文字英字、数字、`SPACE`、`SHIFT` です。
同梱レイアウトは横画面 HUD 向けに調整されているため、
HUD の大きさや配置を変更した場合は座標も調整してください。

## フレームをキャプチャする

指定した swap 番号のフレームを保存する場合：

```bash
mkdir -p /tmp/lunaria-shots
LUNARIA_DUMP_DIR=/tmp/lunaria-shots \
LUNARIA_DUMP_FRAME=0,60,120 \
./lunaria-apk.sh game.apk
```

または、*N* 回の swap ごとに保存する場合：

```bash
LUNARIA_DUMP_DIR=/tmp/lunaria-shots \
LUNARIA_SCREENSHOT_EVERY=60 \
./lunaria-apk.sh game.xapk
```

フレームは PNG として保存されます（例：`lunaria_0000.png`）。
`$LUNARIA_SHOT_TRIGGER`（既定は `/tmp/lunaria-shot`）を作成すると、追加で 1 フレーム保存します。

F12 キー、またはホストメニューの **Take Screenshot** を使用した場合は、
`~/Pictures/Lunaria YYYY-MM-DD HH.MM.SS.png` に保存されます。

## 仕組み

```text
 APK / XAPK / APKS
     │  base、split、native library、assets、OBB を展開
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

- **ARM32:** fastmem を使うフラットな 4 GiB のゲストメモリ空間。
- **ARM64:** ゲスト仮想アドレスをホストアドレスへ 1 対 1 でマッピングします。
  高位アドレス側のイメージ領域に、読み込んだ ELF、トランポリン、JNI テーブル、
  スタックを配置し、dynarmic fastmem を利用できるようにしています。
  `LUNARIA_A64_ENGINES`（1〜8、既定値はホスト CPU コア数から決定）を使うと、
  複数の JIT エンジンをホストスレッド上で並行実行できます。
  `lunaria-apk.sh` は `LUNARIA_A64_SELF_SCHED=1` を設定するため、
  各エンジンは実行可能なゲストを継続的に取得します。
  `LUNARIA_A64_SELF_SCHED=0` にすると、各 frame-pump パスで同期バリアを使う方式へ戻ります。
  自由実行型のエンジンは実機に近い動作となり、スループットが大幅に向上します。
  一方、Cross Worlds のセキュリティモジュールでは、このモードで約 25 秒後に abort した例があります。
- **ネイティブ呼び出し:** ゲストの libc、EGL、GLES、JNI 呼び出しは、
  自動生成された `SVC #n` トランポリンを通ってホスト側実装へ渡されます。
- **ゲストライブラリ:** `make syslib` は、AArch64 用の libm、libc、libz を
  `syslib-arm64/` に配置し、ゲストコードとして実行できるようにします。
  `make guestlib` は、毎回 trap させたくない処理用に `liblunaria_guest.so` を追加します。
  現在その対象になっているのは、ゼロ時間の `nanosleep` です。
  ディレクトリが存在する場合、ランチャーは `LUNARIA_SYSLIB_DIR` をそこへ向けます。
- **スレッド:** ゲスト pthread は協調的なラウンドロビンスケジューリングを使用し、
  ワーカーごとに独立した JIT コンテキストを持ちます。
  mutex の unlock 時には待機中スレッドへ直接制御を渡せます。
  ゲストの sleep は実時間だけ待機します（`LUNARIA_GUEST_SLEEP` は既定で有効、
  `0` を指定した場合のみ無効）。ブロッキング read は通常スケジューラー上に残りますが、
  `LUNARIA_FD_PARK` を設定すると park します。
- **Java:** JNI 呼び出しに対応するホストスタブがない場合、
  `src/dvm/` が APK 内の `classes*.dex` からそのメソッドを実行します
  （`LUNARIA_DVM=1` が既定）。
- **Assets:** `AssetManager` は APK および OBB 拡張ファイル内の
  DEFLATE / STORE エントリを読み込みます。
- **Media:** `android.media.MediaCodec` は、H.264 を openh264、
  AAC を利用可能な場合は libavcodec でデコードします。
  `LUNARIA_OPENH264` / `LUNARIA_LIBAVCODEC` で共有ライブラリのパスを上書きできます。
- **WebView:** `android.webkit.WebView` は DevTools パイプ経由でホストブラウザーと通信します。
  `LUNARIA_WEB_ENGINE` には `auto`（Chrome / Chromium があればそれを使用、
  なければ luna-browser）、`chrome`、`luna`、`off` を指定できます。
  `LUNARIA_WEB_BROWSER` ではブラウザーのバイナリを直接指定できます。
  ページは画像としてビュー内に表示され、タッチやテキスト入力は同じパイプ経由で戻されます。
  フォールバック用ブラウザーは `make luna-browser` でビルドできます。
- **Vulkan:** `LUNARIA_VULKAN=1` を設定しない限り無効です。
  ゲストの `vk*` 呼び出しはホストローダーへ渡され、Android surface と swapchain は
  エミュレートされます。present された画像はコンポジターへ渡されます。
  Vulkan を検出しないタイトルは GLES のままで動作します。
  上記タイトルで実際に使われているのも現在は GLES 経路です。

## 実行時設定

よく使う設定を以下にまとめます。
ソースには互換性検証用の、より限定的な診断スイッチも含まれています。
すべての診断設定については `PROGRESS.md` を参照してください。

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
| `LUNARIA_GUEST_SLEEP` / `LUNARIA_FD_PARK` | on / off | ゲスト sleep を実時間で park / ブロッキング read を park（`LUNARIA_GUEST_SLEEP=0` は診断用） |
| `LUNARIA_SYSLIB_DIR` | `syslib-arm64/`（存在する場合） | ゲストコードとして実行する AArch64 プラットフォームライブラリ。`make syslib` で libm、libc、libz を取得し、`make guestlib` で `liblunaria_guest.so` を作成 |
| `LUNARIA_VULKAN` | off | `1` で Vulkan を公開し、`vk*` 呼び出しをホストへ橋渡し。既定は GLES |
| `LUNARIA_TOUCH_TEST` | off | `x,y[;x,y…]` 形式でタップを注入（最大 8 個）。単純な数値はフレームバッファーに対する百分率、`640px` はゲストピクセル |
| `LUNARIA_TOUCH_FIFO` | off | 実行中のタップ入力。`echo 'x,y[,hold]' > $LUNARIA_TOUCH_FIFO` |
| `LUNARIA_KEYMAP` | off | `genshin`、`crossworlds`、`off`、またはキーマップファイルのパス。ホストメニューから実行中に変更可能 |
| `LUNARIA_DEVICE` | `pixel6` | 端末プロファイル：`pixel6`、`pixel7`、`galaxys21`、`lunaria`。通常は `lunaria.conf` を推奨 |
| `LUNARIA_DATA_ROOT` | launcher directory | ゲストの `/data` と `/storage`。大規模タイトルのダウンロード先もここ |
| `LUNARIA_CONF` | `./lunaria.conf` → ランチャーディレクトリ → `~/.config/lunaria/lunaria.conf` | 読み込む設定ファイル。環境変数が優先 |
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
| `LUNARIA_DUMP_LAST_SVC` | off | 終了時に直近の SVC リングを表示 |
| `LUNARIA_JIT_UI` | `1` | 起動画面（月、変換進捗、dex 進捗）を表示。`0` ではゲスト描画開始まで空の surface |
| `LUNARIA_DVM` | `1` | Dalvik バイトコードエミュレーター。`0` 無効、`1` ホストスタブがない場合のみ APK の dex を実行、`2` ホストスタブより dex を優先 |
| `LUNARIA_DVM_TRACE` | off | エミュレーターが処理できなかったメソッドを `[dvm] miss …` として記録 |
| `LUNARIA_DEX_START` | on | APK の Activity ライフサイクルを dex から実行。旧来の手書き起動シーケンスと比較する場合のみ `0` |
| `LUNARIA_NET` | on | `0` でゲストの全 socket 通信を拒否 |
| `LUNARIA_CACHE_DIR` | `/tmp/lunaria-cache` | ランチャーの永続展開キャッシュ |
| `LUNARIA_NO_CACHE` | off | APK / XAPK / APKS を毎回新しく展開 |

エンジンのエントリポイント（`ANativeActivity_onCreate`、`UnityPlayer.initJni`）を持たない APK でも、
現在は拒否せず、launcher Activity から起動します。

`lunaria-apk.sh` が manifest から対象 Activity を読み取り、
`ANDROID_LAUNCH_ACTIVITY` として渡します。
ローダーはバイトコード VM 経由で `onCreate` / `onStart` / `onResume` を実行します。

tick 値には `K`、`M`、`G` の接尾辞を使用できます。
例：`LUNARIA_ONLOAD_TICKS=5G`

## リポジトリ構成

| パス | 役割 |
|---|---|
| `src/arm_exec.cpp` | dynarmic エンジン、SVC dispatch、JNI、EGL/GLES、スレッド処理 |
| `src/arm.c` / `src/arm.h` | 共通 ARM 実行ロックとマルチエンジン補助 |
| `src/dvm/` | Dalvik バイトコードエミュレーター。APK の `classes*.dex` に含まれる Java を実行 |
| `src/loader.c` | ランタイムのエントリポイントと Unity レンダーループの制御 |
| `src/linker/` | ホスト向けに調整した Android ELF linker |
| `src/jvm/` | 軽量 JVM / JNI オブジェクトモデルとスタブ |
| `src/lib/` | Android ネイティブコードへ公開するホスト側実装。`guest.c` はゲスト内 libc |
| `src/luna_vulkan.c` | 任意の Vulkan ブリッジ（`LUNARIA_VULKAN=1`） |
| `src/luna_overlay.c` | エミュレーター独自 UI：起動画面、ホストメニュー、ゲストフレーム上への合成 |
| `src/luna_boot.c` | 起動画面：空、雪、進捗リング、ロゴ、変換進捗、dex 進捗 |
| `src/luna_ime.c` | ホスト IME からゲストテキスト入力へのブリッジ |
| `src/webview_cdp.c` | Chrome と luna-browser が共有する DevTools パイプクライアント |
| `luna-browser/` | 内蔵 WebView エンジン（luna-ui + QuickJS）。`make luna-browser` でビルド |
| `keymaps/` | 同梱タッチマップ（`genshin.conf`、`crossworlds.conf`） |
| `lunaria.conf.sample` | 端末プロファイル、data root、画面スケール、パッケージ台帳、WebView エンジン |
| `runtime/` | 生成された Android 互換ホスト共有ライブラリ |
| `lunaria-apk.sh` | APK / XAPK / APKS の解析、展開、起動パイプライン |
| `PROGRESS.md` | 詳細な互換性メモと現在の開発状況 |

## トラブルシューティング

<details>
<summary><strong>起動時のムービーが再生されない</strong></summary>

Lunaria は H.264 のデコードに openh264 を使用しており、
Constrained Baseline を読み取れます。

一方、多くのタイトルのスプラッシュ動画は CABAC や B-frame を含む Main / High profile です。
Blade & Soul Revolution の動画も該当し、これらは現状ここではデコードできません。

エミュレーターはこの状態を検出すると
（16 個の access unit を処理しても画像が得られない場合）、
ゲストへ `MEDIA_ERROR_UNSUPPORTED` を返し、ゲームエンジンが動画をスキップして
起動を続けられるようにします。

以前は、実際にはフレームが出ない動画を「再生中」として処理し続けていたため、
ゲーム側がその動画の終了を待ち続け、白画面から先へ進めなくなることがありました。

ストリームが Constrained Baseline のタイトル
（例：Blade & Soul Masia のオープニング）は、
openh264 が利用可能な場合に `BUFFER_FLAG_END_OF_STREAM` まで到達します
（`make fetch-openh264`）。

AAC 音声は、ホスト側で利用可能な場合に libavcodec を使用します。
コーデックの状態遷移を確認するには `LUNARIA_TRACE_MEDIA=1` を使用してください。

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

まず OBB / split ファイルを確認し、その後、必要なトレースを取得してください。

```bash
LUNARIA_TRACE_EXC=1 LUNARIA_DUMP_LAST_SVC=1 \
./lunaria-apk.sh game.apk 2>&1 | tee lunaria.log
```

UE5 タイトルでは、可能であればハードウェア GL ドライバーを使用してください。
`LIBGL_ALWAYS_SOFTWARE=1`（llvmpipe）では、同じ起動経路でも 10 倍以上遅くなる場合があり、
実際にはソフトウェアラスタライザーが遅いだけなのに、
エミュレーターが停止したように見えることがあります。

</details>

<details>
<summary><strong>描画が遅い</strong></summary>

Mesa llvmpipe はソフトウェアレンダラーであり、ヘッドレス検証には便利ですが、
性能評価には向きません。リアルタイム描画には GPU アクセラレーション対応の
ホスト EGL / OpenGL スタックを使用してください。

AArch64 タイトルでは、まずシングルエンジンで安定動作を確認してから
`LUNARIA_A64_ENGINES=4` を試してください。

速度を測る場合は、`LUNARIA_SLICE_DETAIL` /
`LUNARIA_SVC_HISTO` を設定しないでください。

</details>

<details>
<summary><strong>クリックしても反応しない</strong></summary>

GLFW は `xdotool` / `XSendEvent` による合成クリックを無視します。

代わりに、
`LUNARIA_TOUCH_TEST='50,84;…'`
（フレームバッファーに対する百分率。ゲストピクセルを指定する場合は `px` を付ける）、
タイトル起動後の `LUNARIA_TOUCH_FIFO`、
または実際の GLFW ウィンドウ内をクリックしてください。

右クリックはホストメニューを開くため、ゲスト側には渡されません。

</details>

## プロジェクトの状況

Lunaria は研究開発段階のソフトウェアです。

Linux では、ウィンドウ表示とフレームバッファーの内容が一致します。
macOS ビルド（`make dist-mac`）ではフレームバッファーへ描画できるものの、
画面上の実ウィンドウが黒いままになる場合があります。

Windows 向け OS レイヤーは存在しますが、Windows バイナリはまだありません。
JIT が現時点でも `mmap` を直接呼び出しているためです。

`make dist` は、ビルドディレクトリに依存しない Linux 向け配布ツリーを作成します。

Android API の未実装、タイトル固有の問題、大量の診断出力、
破壊的変更が含まれることを前提にしてください。

コントリビューションを行う場合は、タイトル名、ABI、エンジンのバージョン、
最後に成功した到達点、ログ、キャプチャしたフレームを添えていただけると特に役立ちます。

[Mozilla Public License 2.0](LICENSE) の下でライセンスされています。

Project Lunaria © 2026 Yuichiro Nakada.
