# Lunaria

### Androidアプリやゲームを、いつものPCで。

Androidのシステムイメージは不要。ARM32／ARM64のコードを、元のパッケージのまま実行します。

[![License: MPL 2.0](https://img.shields.io/badge/license-MPL--2.0-5b5bd6.svg)](LICENSE)
[![Guests](https://img.shields.io/badge/guest-ARM32%20%7C%20ARM64-20232a.svg)](#仕組み)
[![Engines](https://img.shields.io/badge/engines-Unity%20%7C%20Unreal%20%7C%20native-20232a.svg)](#互換性)
[![Sponsor](https://img.shields.io/badge/sponsor-%E2%99%A5-ea4aaa.svg)](https://github.com/sponsors/yui0)

[インストール](#インストール) · [使い方](#使い方) · [ギャラリー](#ギャラリー) · [互換性](#互換性) · [English](README.md)

**Lunariaは、Androidを起動せずにAndroidアプリやゲームをPCで動かす互換レイヤーです。**
アプリ本体を直接読み込み、ARM32／ARM64のコードを[dynarmic](https://github.com/merryhime/dynarmic)で実行します。
JNI、EGL、OpenGL ES、MediaCodecなどのAndroid機能はホスト側へ橋渡しし、APK内の`classes*.dex`は内蔵のDalvikインタプリタで処理します。

`.apk`、`.xapk`、`.apks`、`.aab`をそのまま開けます。展開、split APKの統合、キャッシュ、アカウント別のデータ分離はLunariaが行います。
WebViewにはChrome／Chromium、または内蔵の[luna-browser](luna-browser/)を使います。

**動作を確認したタイトル：** 原神、二ノ国：Cross Worlds、アルケミアストーリー、気球！エアロノート（2013年のARM32向けAPK）、
Black Clover: Asta Fight、Kingdoms & Lords、そのほかのUnity／Unrealタイトル、TVer（本編再生）やAPKPureなどのJavaアプリ。
どこまで進めたかは[互換性](#互換性)をご覧ください。

> [!IMPORTANT]
> **実験的なソフトウェアです。** 完全なAndroidエミュレーターではなく、プレイできるタイトルもあれば、メニューまでで止まるもの、描画できないものもあります。
> 掲載画像はすべてLunariaが実際に描画したものです。

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

<p align="center"><sub>原神 7.2.0（Unity IL2CPP、arm64-v8a）· 二ノ国：Cross Worlds（Unreal Engine 4、arm64-v8a）</sub></p>

## インストール

### ビルド済みバイナリ（推奨）

**Linux／macOS（Apple Silicon）**

```bash
curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | bash
```

最新の[リリース](https://github.com/yui0/lunaria/releases)を`~/.local`（`sudo`なら`/usr/local`）へ入れ、`lunaria`コマンドを追加します。macOSでは`Lunaria.app`も`~/Applications`に置きます。
必要なら`~/.local/bin`を`PATH`に追加してください。

```bash
# オプション：バージョン指定／システム全体へ導入／アンインストール（lunaria.confとアプリデータは残す）
curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | bash -s -- --version 2026.10.15
curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | sudo bash -s -- --prefix /usr/local
curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | bash -s -- --uninstall
```

手動で入れる場合は、`lunaria-*-linux-x86_64.tar.gz`／`…-aarch64.tar.gz`を展開して`./lunaria`を実行するか、macOSの`.dmg`から`Lunaria.app`を取り出してください。

**Windows（x86_64）**

`lunaria-*-windows-x86_64.zip`をダウンロードして展開し、`lunaria.exe app.apk`を実行します（APKを実行ファイルへドラッグ＆ドロップでも可）。MSYS2や追加のランタイムは不要です。

### ソースからビルド

**1. 依存パッケージ**

<details open>
<summary>Ubuntu／Debian</summary>

```bash
sudo apt install \
  build-essential cmake pkg-config \
  libboost-dev libbsd-dev libunwind-dev \
  libglfw3-dev libegl1-mesa-dev libgles2-mesa-dev \
  libssl-dev zlib1g-dev \
  libasound2-dev libvulkan-dev
```

`libvulkan-dev`はVulkanブリッジ（任意）用です。ホストにVulkanライブラリがあれば標準で有効になり、`LUNARIA_VULKAN=0`で無効にできます。

</details>

<details>
<summary>macOS（Apple Silicon）</summary>

`make`を実行すると、バージョン固定のANGLE、MoltenVK、OpenH264を取得してビルドします。画面表示の制限は[開発状況](#開発状況)を参照してください。

</details>

<details>
<summary>Windows（MSYS2 UCRT64）</summary>

```bash
pacman -S --needed git make zip unzip bzip2 \
  mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,python,boost,glfw,angleproject,openssl,zlib,libgnurx,vulkan-headers}
```

`ANDROID_HOME`（Build ToolsのARM64用RenderScriptライブラリを含むSDK）と`ANDROID_NDK_HOME`を設定してから実行します。

```bash
make -j4 dist CC=gcc CXX=g++ DYNARMIC_JOBS=4
```

完成するのは`lunaria.exe`を含む単体動作のZIP（約35MiB）で、実行時にMSYS2、Python、ICUは不要です。

LinuxからクロスビルドするにはUCRT対応の`x86_64-w64-mingw32-gcc/g++`、CMake、Ninja、Python 3.11以降、curl、zstd対応のtar、zip、Android SDK／NDKを用意し、`make -j4 dist-cross-win`を実行します。依存物は`/tmp/lunaria-windows-deps`にキャッシュされ、`WIN_DEPS`、`WIN_CROSS_PREFIX`、`WIN_ANGLE`、`WIN_GLFW_LIB`、`WIN_BOOST_INCLUDE`、`WIN_BUILD_DIR`、`WIN_DIST_BUILD_DIR`で変更できます。

</details>

**2. ビルド**

```bash
make dynarmic-build
make x86_64 -j"$(nproc)"   # x86_64ホスト。単純な `make` は32bit x86 ABIをビルド
make syslib                # 任意：AArch64のlibm／libc／libzをゲストコードとして syslib-arm64/ に配置
make guestlib              # 任意：liblunaria_guest.so（待ち時間0のnanosleepをホストへ戻さず処理）
```

OpenH264は`runtime/`へ自動取得されます（更新は`make fetch-openh264`）。WebPは同梱の[simplewebp](https://github.com/MikuAuahDark/simplewebp/blob/main/simplewebp.h)（`LICENSE.simplewebp`）、Unicodeは組み込みテーブルを使うため、**ICUは不要**です。

**3. Androidフレームワークリソース**

テーマやリソースの継承にはSDK Platform 31のリソースを使います。

```bash
curl -fL https://dl.google.com/android/repository/platform-31_r01.zip -o /tmp/platform-31_r01.zip
python3 scripts/install-framework.py /tmp/platform-31_r01.zip
```

インストーラは配布元のSHA-1を検証し、ファイルを変更せずに`runtime/framework-res/`へ取り出します（別の場所は`LUNARIA_FRAMEWORK_RES`で指定）。アプリのAPKは変更しません。

**4. 配布用パッケージの作成**（任意）

```bash
make dist        # lunaria-<version>-linux-x86_64.tar.gz
make dist-mac    # Lunaria.app + .dmg（Darwinのみ）
```

## 使い方

```bash
lunaria                        # グラフィカルランチャー（ウィンドウへパッケージをドロップ）
lunaria path/to/app.apk        # .xapk / .apks / .aab も同様
LUNARIA_ARCH=arm64-v8a lunaria app.apk   # 複数ABIを含む場合は明示指定
```

ソースからビルドした場合は`lunaria`を`./lunaria`に読み替えてください。

- **ホストメニュー：** 実行中に右クリック。スクリーンショット（**F12**）、貼り付け、戻る、音量、キーマップ、WebView、入力の録画、プロファイル、再起動、フルスクリーン、終了。
- **表示密度：** 右クリックの**Display → SCREEN DENSITY (DPI)**から変更できます。アプリごとに保存され、再起動後に反映されます。**Automatic**で端末の既定値に戻せます。
- **アプリ検索：** ランチャーの*Find an app on APKPure*に名前を入力し、候補のパッケージ名を確認して選ぶと、XAPKをダウンロードして開きます。保存先はパッケージキャッシュ配下の`downloads`、なければデータフォルダの`cache/packages/downloads`、それもなければ`~/Downloads/Lunaria`です。
- **パッケージ：** split APKは元のベースAPKを書き換えずに重ね合わせ、AABはベースモジュールとアセットパックを読み込み、OBBは可能な限りハードリンク／シンボリックリンクで共有します。展開結果は`cache/packages/`にキャッシュされます（移動は`LUNARIA_CACHE_DIR`、無効化は`LUNARIA_NO_CACHE=1`）。
- **設定：** 永続的な設定は`lunaria.conf`（例：`lunaria.conf.sample`）に保存します。環境変数はその起動に限り優先されます。設定ファイルとゲストデータの標準位置は起動ディレクトリです。

  ```bash
  ./lunaria --config ./lunaria.conf --data-root "/mnt/games/Lunaria data" game.xapk
  ```

- **プロファイル：** アカウントごとに、インストール済みパッケージと外部／共有ストレージ（OBB含む）は共有しつつ、内部ファイル・設定・データベースを分離します。`default`は`data/<package>/`、それ以外は`profiles/<package>/`です。

  ```bash
  ./lunaria --choose-profile game.xapk     # 起動時に選択（LUNARIA_PROFILE_PROMPT=1でも可）
  ./lunaria --profile account2 game.xapk
  ./lunaria --list-profiles game.xapk
  ```

- **Googleサービス（任意）：** 右クリックの**Google services**から、ローカルのGoogle Play services、Play Store、Services Frameworkのパッケージを導入できます（それぞれ別に状態表示）。**Sign in to a new environment…**で独立したGMS環境（`system/gms/gms-N/`）を作成し、ほかのアプリでも再利用できます。サインイン後はアプリを再起動してください。`LUNARIA_GMS_PROFILE=gms-2`で直接選択でき、`--profile`とは独立です。

  ```bash
  ./lunaria --google-services --data-root /path/to/data
  ./lunaria --data-root /path/to/data --install-system GooglePlayServices.xapk   # 画面なしで導入
  ```

![ランチャー](shots/launcher.png)

<p align="center"><sub>ランチャー · ゲーム中のホストメニューは右クリック</sub></p>

## 特徴

| 特徴 | できること |
|---|---|
| **Androidイメージ不要** | VMなしで`.apk`、`.xapk`、`.apks`、`.aab`を起動。 |
| **ARM32＋ARM64** | dynarmicのJITでARMv7／AArch64を実行。`LUNARIA_A64_ENGINES`で並列化も可能。 |
| **ホストGPU描画** | EGL／OpenGL ES 3をホストGPUへ。利用可能ならVulkanも公開。 |
| **Androidサービス** | JNI、AssetManager、OBB、pthread、OpenSL ES、各種Android APIの互換実装。 |
| **JavaとUI** | `classes*.dex`をDalvikインタプリタで実行。ビュー、テキスト、レイアウトはホスト側で描画。 |
| **動画・音声・WebView** | H.264はOpenH264、AACはlibavcodec。WebViewはChrome／Chromiumまたはluna-browser。 |
| **入力** | キーボード・マウスをタッチへ割り当て。マクロや録画・再生にも対応。 |
| **プロファイルと端末情報** | `--profile`でアカウント別データ。端末プロファイルは`lunaria.conf`（標準はPixel 6）。 |
| **Googleサービス（GMS）** | Google Play services、Play Store、Services Frameworkを、お手元の正規パッケージから任意で実行。サインイン環境は再利用可能。 |
| **画面なし実行・診断** | X11なしのEGL描画、フレーム保存、JIT計測、SVCトレース。 |

### Googleサービス（GMS）

Lunariaはアプリに対してPixel風の端末を見せ、パッケージ台帳に`com.google.android.gms`、`com.android.vending`、`com.google.android.gsf`を含めます。**Google署名のAPKは同梱していません**。正規のものを用意するまでは空のプレースホルダなので、本物のPlay servicesが必要なアプリは動作しません。必要としないアプリは、設定なしでそのまま動きます。

- **自分で導入：** 右クリックの**Google services**メニュー、または画面なしで`--install-system GooglePlayServices.xapk`。Play services、Play Store、Services Frameworkはそれぞれ別に導入・状態表示されます。正規のAPKを`<device-root>/product/priv-app/PrebuiltGmsCore/PrebuiltGmsCore.apk`に置くこともできます（アプリはGoogleの署名を検証します）。
- **サインイン環境：** **Sign in to a new environment…**で`system/gms/gms-N/`を作成し、どのアプリでも再利用できます。メニューまたは`LUNARIA_GMS_PROFILE=gms-2`で選択し、その後アプリを再起動してください。`--profile`とは独立です。
- **サービスのホスティング：** 別のインストール済みパッケージのServiceは、そのパッケージ自身のプロセスでバインドされます（`LUNARIA_SERVICE_HOSTS=com.android.vending,com.google.android.gms`）。
- **GMSなしにする：** `LUNARIA_PACKAGES=default;-com.google.android.gms;-com.google.android.gsf`で台帳から外せます。
- **制限：** Google Playの課金は未対応です（たとえば原神）。結果はタイトルごとに異なります。[互換性](#互換性)を参照してください。

パッケージとサービスホストの全オプションは`lunaria.conf.sample`を参照してください。

## ギャラリー

### Javaアプリ：TVerで動画再生

Lunariaはアプリのdexバイトコードを実行し、Androidのビュー、レイアウト、テキスト、リソース、ライフサイクルをホスト側で実装します。**TVer 6.40.0**は、無改変のXAPKからウェルカム画面、ホームフィード（動画タイル付き）、エピソード詳細、広告、本編再生まで到達します。起動は重く、一部の画面にレイアウトの崩れが残ります。

<p align="center">
  <img src="shots/tver-playback.png" width="30%" alt="TVer：薬屋のひとりごとを再生中。動画の下にタイトル、放送日、あとで見る・お気に入りボタン">
  &nbsp;
  <img src="shots/tver-playback-portrait.png" width="30%" alt="TVer：薬屋のひとりごと第54話を縦画面で再生。引き戸から覗く猫猫">
  &nbsp;
  <img src="shots/tver-welcome.png" width="14%" alt="TVerの縦画面ウェルカム画面。番組アートワークと同意ボタン">
</p>

<p align="center"><sub>TVer 6.40.0 · 横画面と縦画面での再生 · ウェルカム画面</sub></p>

APKPureもストア、アプリ詳細、ダウンロードマネージャーを表示できます（ダウンロードの進行は確認済み、インストール完了は未検証）。

### 原神 7.2.0

Unity IL2CPP、`arm64-v8a`、**1024×576**。Google Playの課金には対応していません。

<p align="center">
  <img src="shots/genshin-wings.png" width="48%" alt="原神：青空を背景に風の翼を開いた蛍">
  &nbsp;
  <img src="shots/genshin-lisa-obtained.png" width="48%" alt="原神：新キャラクター獲得画面のリサ">
</p>

<p align="center">
  <img src="shots/genshin-prologue.png" width="48%" alt="原神プロローグ：蛍、パイモン、浮遊する仲間が赤く光る石を囲む場面">
  &nbsp;
  <img src="shots/genshin-knights.png" width="48%" alt="原神：西風騎士団の門前に立つ旅人。近くにガイアとアンバー">
</p>

<p align="center">
  <img src="shots/genshin-lakeshore.png" width="48%" alt="原神：湖畔に立つ旅人">
  &nbsp;
  <img src="shots/genshin-swim.png" width="48%" alt="原神：夕暮れのモンド城外を泳ぐ旅人">
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
  <img src="shots/genshin-library.png" width="48%" alt="原神：図書室の時計の横、横顔のジン">
  &nbsp;
  <img src="shots/genshin-language.png" width="48%" alt="原神の言語設定。日本語を表示">
</p>

</details>

### 二ノ国：Cross Worlds

Unreal Engine 4。ログイン、サーバー選択、キャラクター作成から、最初の村、音声付きムービー、フィールドでのプレイまで確認できています。`arm64-v8a`、**1024×576**。

<p align="center">
  <img src="shots/cross-worlds-peek.png" width="32%" alt="Cross Worlds：茂みから覗く少女と使い魔">
  &nbsp;
  <img src="shots/cross-worlds-village.png" width="32%" alt="Cross Worlds の開始地点の村">
  &nbsp;
  <img src="shots/cross-worlds-play.png" width="32%" alt="Cross Worlds のゲーム内プレイ。クエストと HUD を表示">
</p>

<p align="center">
  <img src="shots/cross-worlds-chloe.png" width="32%" alt="Cross Worlds のストーリー会話。クロエ">
  &nbsp;
  <img src="shots/cross-worlds-gatling.png" width="32%" alt="Cross Worlds：ガトリングを構える赤髪のヒロイン">
  &nbsp;
  <img src="shots/cross-worlds-lighthouse.png" width="32%" alt="Cross Worlds：夕暮れのフィールドと HUD">
</p>

<details>
<summary>そのほか：タイトル、サーバー、キャラクター、イントロ</summary>

<p align="center">
  <img src="shots/cross-worlds-title.png" width="32%" alt="Cross Worlds のタイトル画面">
  &nbsp;
  <img src="shots/cross-worlds-servers.png" width="32%" alt="Cross Worlds のサーバー選択画面（Luxelion）">
  &nbsp;
  <img src="shots/cross-worlds-characters.png" width="32%" alt="Cross Worlds のキャラクター選択画面">
</p>

<p align="center">
  <img src="shots/cross-worlds-intro.png" width="32%" alt="Cross Worlds の 3D イントロ">
  &nbsp;
  <img src="shots/cross-worlds-evermore.png" width="32%" alt="Cross Worlds のエバーモア到着場面">
  &nbsp;
  <img src="shots/cross-worlds-cutscene.png" width="32%" alt="Cross Worlds のカットシーン">
</p>

</details>

### そのほかのタイトル

<p align="center">
  <img src="shots/black-clover-gameplay.png" width="48%" alt="Black Clover: Asta Fight のゲームプレイ。アスタが怪物と戦う">
  &nbsp;
  <img src="shots/alchemia-story-title.png" width="48%" alt="アルケミアストーリーのタイトル画面">
</p>

<p align="center">
  <img src="shots/alchemia-story-cave.png" width="48%" alt="アルケミアストーリー：洞窟で驚く黒髪のヒロイン。日本語の会話枠">
  &nbsp;
  <img src="shots/alchemia-story-neko.png" width="26%" alt="アルケミアストーリー：白髪の猫耳少女。縦画面">
</p>

<p align="center"><sub>Black Clover: Asta Fight · アルケミアストーリー（Unity IL2CPP。横画面1024×576、縦画面576×1024）</sub></p>

<p align="center">
  <img src="shots/kikyu-title.png" width="48%" alt="気球！エアロノートのタイトル画面">
  &nbsp;
  <img src="shots/kingdoms-lords-village.png" width="48%" alt="Kingdoms & Lords：建設中の小屋と柵が並ぶ緑の島の村。リサが自己紹介する吹き出し">
</p>

<p align="center"><sub><a href="https://github.com/yui0/Kikyu">気球！エアロノート</a>：2013年の<code>armeabi</code> APKを無改変で実行 · Kingdoms &amp; Lords：Androidビュー層で描画した村のシーン</sub></p>

<p align="center">
  <img src="shots/disney-magic-kingdoms-loading.png" width="48%" alt="Disney Magic Kingdoms：氷上ステージのミニーとミッキー。下に日本語の確認メッセージと緑の進捗バー">
  &nbsp;
  <img src="shots/trickcal-server-loading.jpg" width="48%" alt="Trickcal Chibi+Go 1.6.1：イラストの街とサーバー設定の読み込み進捗">
</p>

<p align="center"><sub>Disney Magic Kingdoms：コンテンツ確認中、サーバーの応答待ち · Trickcal Chibi+Go：サーバー設定の読み込み（ゲームプレイは未到達）</sub></p>

<details>
<summary>オープンソース・サンプルタイトル</summary>

<p align="center">
  <img src="shots/between-two-worlds.png" width="48%" alt="Between Two Worlds のメインメニュー">
  &nbsp;
  <img src="shots/fpsmobile.png" width="48%" alt="FPSMobile：First Person Template と仮想スティック">
</p>

<p align="center">
  <img src="shots/unity-sample.png" width="48%" alt="UnitySampleGame の 3D ゲームプレイ">
  &nbsp;
  <img src="shots/black-clover-village.png" width="48%" alt="Black Clover: Asta Fight の村ステージ">
</p>

<p align="center">
  <img src="shots/demo-project-light.png" width="48%" alt="Demo Project Light のタイトル。TAP THE SCREEN">
  &nbsp;
  <img src="shots/unreal-first-person.png" width="48%" alt="Unreal Engine 4 の FirstPersonExampleMap">
</p>

<p align="center">
  <img src="shots/time-locker.png" width="28%" alt="TIME LOCKER の縦画面チュートリアル">
</p>

<p align="center"><sub>Between Two Worlds · FPSMobile · UnitySampleGame · Black Clover の村ステージ · Demo Project Light · Unreal のサンプル · TIME LOCKER</sub></p>

</details>

タッチ操作の自動化には`LUNARIA_TOUCH_TEST`を使います。`50,84`のような数値は画面に対する割合（`50%,84%`と同じ）、`640px,606px`はゲストのピクセル座標です。GLFWは`xdotool`の疑似クリックを受け付けません。

## 互換性

以下は特定のビルド・環境で**実際に確認できた範囲**であり、動作の保証ではありません。詳細な検証記録と起動方法は[PROGRESS.md](PROGRESS.md)にあります。

| タイトル | エンジン / ABI | 現在の状況 |
|---|---|---|
| **Ni no Kuni: Cross Worlds** | Unreal Engine 4 · AArch64 XAPK | Linux：ログイン、サーバー・キャラクター選択、村、会話、音声付きムービー、フィールド操作を確認。SharedPreferencesも保持。Apple Silicon：5.04.12でオープニング動画、3Dイントロ、利用規約画面まで。以降は未検証。 |
| **Blade & Soul Masia** | Unreal Engine 5 · AArch64 APKS | オープニング動画を最後まで再生し、タイトル画面を表示。追加データの確認で同意するとダウンロードが始まる。 |
| **Genshin Impact 7.2.0** | Unity IL2CPP · AArch64 XAPK | Linuxでオープンワールドに到達（プロローグ、モンド城、滑空）。ログイン、サーバー、音量設定は再起動後も保持。キーボード移動可。ある環境でフィールド約18～22fps、メニュー約30fps。アカウントWebViewは表示可、Google Play課金は未対応。 |
| **Alchemia Story 1.0.154** | Unity IL2CPP · AArch64 APK | タイトル、日本語のストーリー（横・縦画面）、キャラクター作成画面を描画（2026-10-09）。 |
| **Kikyu! Aeronaut** | native `armeabi` · ARM32 APK（2013） | タイトル画面を描画し、Returnキー待ち。 |
| **Between Two Worlds** | Unity 2023 IL2CPP · ARMv7 / AArch64 | ARMv7：プレイ可能でメインストーリーのシーンまで。AArch64：メインメニュー。最近の確認では言語選択画面まで約70fps。 |
| **FPSMobile** | Unreal Engine 4 · ARMv7 | FirstPersonExampleMapを描画。16,000回以上のswapを確認。 |
| **UnitySampleGame** | Unity · ARMv7 | Startから3Dゲーム画面へ遷移。 |
| **TIME LOCKER** | Unity · ARMv7 | 縦画面のチュートリアルゲームプレイまで到達。 |
| **Black Clover: Asta Fight** | Unity IL2CPP · AArch64 XAPK | Base＋splitを読み込み、タイトル・ステージ選択・ゲーム画面を画面なしで描画。 |
| **Blade & Soul Revolution** | Unreal Engine 4 · AArch64 | ゲーム独自のダイアログまで起動。パッチサーバーへの経路がないと再接続を要求。 |
| **Trickcal Chibi+Go 1.6.1** | Unity 6000.3 IL2CPP · ARMv7 XAPK | 利用規約Activityとサーバー設定の読み込み画面まで。ゲームプレイは未検証。 |
| **Disney Magic Kingdoms 12.1.1c** | Unity · ARMv7 XAPK | コンテンツ確認画面を描画。サーバーへ更新を問い合わせる段階で停止。 |
| **Kingdoms & Lords** | Gameloft製の村作りゲーム | 島の村、資源カウンター、会話枠を描画（2026-11-09）。その先は未検証。 |
| **TVer 6.40.0** | Java / dex Android UI | ウェルカム画面、動画タイル付きホーム、エピソード詳細、本編再生。起動は重く、一部レイアウトに崩れ。 |
| **APKPure 3.20.8002** | Java / dex Android UI | ストア、おすすめ、詳細、ダウンロードマネージャーを描画。ダウンロードの進行は確認、インストール完了は未検証。 |
| **Another Eden** | Native · ARMv7 XAPK | 既存アカウントの読み込み画面。ゲームの読み込み中に異常終了。 |
| **Daggerfall Unity** | Unity Mono · ARMv7 | Monoは起動。描画はまだ動作しない。 |

<details>
<summary><strong>オープンソースのテストタイトルを実行する</strong></summary>

**FPSMobile**（Unreal Engine 4）— [Abhishrut/UnrealEngineAndroidSamples](https://github.com/Abhishrut/UnrealEngineAndroidSamples)。2つのファイルを`test/`に置くと、同じ場所のOBBは自動検出されます。

```bash
curl -L -o test/FPSMobile-armv7.apk \
  "https://raw.githubusercontent.com/Abhishrut/UnrealEngineAndroidSamples/main/FPSMobile-armv7.apk"
curl -L -o test/main.1.com.YourCompany.FPSMobile.obb \
  "https://raw.githubusercontent.com/Abhishrut/UnrealEngineAndroidSamples/main/main.1.com.YourCompany.FPSMobile.obb"
LUNARIA_ARCH=armeabi-v7a ./lunaria test/FPSMobile-armv7.apk
```

**Between Two Worlds**（Unity 2023 IL2CPP）— [ShutovKS/Between-two-worlds](https://github.com/ShutovKS/Between-two-worlds/releases/tag/1.0.5)。配布ファイルは`.apk`ですが中身はApp Bundleで、Lunariaが直接読み込みます。

```bash
make fetch-btw
LUNARIA_ARCH=armeabi-v7a ./lunaria test/btw-android.apk
LUNARIA_ARCH=arm64-v8a  ./lunaria test/btw-android.apk
```

**Daggerfall Unity**（Unity Mono）— [Vwing/daggerfall-unity-android](https://github.com/Vwing/daggerfall-unity-android/releases/tag/v1.1.1.8)

```bash
make fetch-libunity
./lunaria test/dfu-mono-32bit.apk
```

</details>

## キーボード・マウス操作

キーボードとマウスの入力をタッチ操作へ変換します。右クリックメニューの**Keyboard Controls**でレイアウトを実行中に切り替えられ、**Off**で無効化、**Reload Current File**で再読み込みできます（読み込みに失敗しても現在の設定は維持）。

```sh
LUNARIA_KEYMAP=genshin ./lunaria /path/to/Genshin.xapk
LUNARIA_KEYMAP=crossworlds ./lunaria /path/to/CrossWorlds.apks
```

付属の原神・Cross Worlds用レイアウトは、**WASD**で移動、**F**で攻撃、**E／Q／1／2／3**でスキルやキャラクター選択、**Space**／**Shift**でジャンプ・ダッシュ・回避、左ドラッグで視点操作です。**Ctrl＋ホイール**で2本指のピンチ（上で拡大、下で縮小、カーソルが中心）。テキスト入力欄が優先され、ウィンドウが非アクティブになると押下中のキーは自動で解除されます。

**カスタムレイアウト：** `keymaps/*.conf`をコピーして編集し、`LUNARIA_KEYMAP=/path/to/my.conf`で読み込みます（`lunaria.conf`への保存も可）。

```text
stick 0.15625 0.764 0.18        # 仮想スティックの中心(x y)と半径。WASDで操作
walk CTRL 0.04                  # このキーを押している間は半径を小さく
button SPACE 0.922 0.665        # キー → タッチ位置
button F 0.10 0.20 0.30 0.40    # 1キーで複数点を同時にタップ（最大4点）
button G 0.70 0.70 repeat=100 hold=20   # 押下中100msごとに20msタップ
macro Z 0.5,0.5|0.6,0.6/40 @100 0.9,0.9 # タップ、|同時、/押下時間ms、@待機ms
macro X 0.1,0.1 loop            # 押している間繰り返す
```

座標は画面の幅・高さに対する0～1の相対値（半径は短辺基準）。ボタンとマクロはそれぞれ最大8個、キーは`A–Z`、`0–9`、`SPACE`、`SHIFT`、`CTRL`。`repeat`は10～60,000ms。タイミングはフレームクロックに従います（60fpsで約16ms刻み）。付属のマップは横画面のHUD向けです。

**録画と再生：** **F9**（Record Input）でタッチ入力を`~/Documents/Lunaria <date>.rec`に保存し、**F10**（Play Recording）で最新の録画を再生します。`LUNARIA_PLAY`で別ファイル、`LUNARIA_PLAY_LOOP=1`で繰り返し。座標は相対値なので、ウィンドウサイズが変わっても再生できます。

## 画面の保存

右クリックの**Pause**／**Resume**でゲームを一時停止できます（ホストメニューは操作可能）。**F12**または**Take Screenshot**は`~/Pictures/Lunaria YYYY-MM-DD HH.MM.SS.png`に保存します。スクリプトから保存する場合：

```bash
LUNARIA_DUMP_DIR=/tmp/lunaria-shots LUNARIA_DUMP_FRAME=0,60,120 ./lunaria game.apk   # 指定したswap
LUNARIA_DUMP_DIR=/tmp/lunaria-shots LUNARIA_SCREENSHOT_EVERY=60 ./lunaria game.xapk  # Nswapごと
```

ファイル名は`lunaria_0000.png`のようになります。`$LUNARIA_SHOT_TRIGGER`（既定`/tmp/lunaria-shot`）のファイルを作ると、追加で1フレーム保存します。

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

- **メモリ：** ARM32はfastmemによる4GiBの連続空間。ARM64はゲストアドレスをホストアドレスに1対1で対応させ、ELF、トランポリン、JNIテーブル、スタックを高位領域に置きます。
- **スレッド：** ゲストのpthreadは協調的に管理し、各ワーカーが独立したJITコンテキストを持ちます。`LUNARIA_A64_ENGINES`（1～8、コア数から自動選択）で複数のAArch64エンジンをホストスレッドで並列実行できます。ランチャーは`LUNARIA_A64_SELF_SCHED=1`にして各エンジンが実行可能なゲストを独立に取得します（`0`でフレームごとの同期。Cross Worldsのセキュリティモジュールが約25秒後に異常終了した例あり）。
- **ネイティブ呼び出し：** ゲストのlibc、EGL、GLES、JNI呼び出しは、自動生成された`SVC #n`トランポリンからホスト実装へ渡ります。`make syslib`／`make guestlib`で、頻出ルーチン（`pow`、`sincosf`、メモリ処理、待ち時間0の`nanosleep`）をゲストコードとして実行します。zlibは計測で高速だったホスト実装のままです。
- **Java：** JNI呼び出しに対応するホストスタブがない場合、`src/dvm/`がAPK内の`classes*.dex`から該当メソッドを実行します。
- **ライフサイクル：** マニフェストの`Application`とContentProviderを初期化してから、ランチャー`Activity`の`onCreate`／`onStart`／`onResume`を実行します。ネイティブアクティビティ系は`ANativeActivity_onCreate`から入り、`APP_CMD_*`でイベントを受け取ります。`System.loadLibrary`の起点は、Androidのリンカーと同様に依存ライブラリの検索スコープの先頭になります。
- **アセットとメディア：** `AssetManager`はAPKやOBBのDEFLATE／STOREデータを読み込みます。`MediaCodec`はH.264をOpenH264、AACをlibavcodecでデコードします（場所は`LUNARIA_OPENH264`／`LUNARIA_LIBAVCODEC`で指定）。
- **音声：** OpenSL ES、AAudio、`AudioTrack`は共通のミキサーを通してALSAへ出力します。ブロッキング書き込みで描画ループは止まりません。
- **WebView：** DevToolsパイプ経由でホストのブラウザーと通信し、ページを画像としてビュー内に表示します。`LUNARIA_WEB_ENGINE`は`auto`（Chrome／Chromium、なければluna-browser）、`chrome`、`luna`、`off`。`LUNARIA_WEB_BROWSER`でバイナリを直接指定でき、フォールバックは`make luna-browser`でビルドします。
- **Vulkan：** ゲストの`vk*`呼び出しはホストのローダーへ渡り、Android surfaceとswapchainはエミュレートされます。Vulkanを使わないタイトルは、上記のデモで使われているGLES経路のままです。

## 実行時設定

日常的な設定と診断用の変数です。開発専用のスイッチはソースと`PROGRESS.md`を参照してください。tick値には`K`、`M`、`G`の接尾辞が使えます。

| 変数 | 既定値 | 用途 |
|---|---:|---|
| `LUNARIA_ARCH` | auto | `armeabi-v7a`または`arm64-v8a` |
| `LUNARIA_WIDTH` / `LUNARIA_HEIGHT` | `1024` / `768`（横画面アプリは入れ替え） | ウィンドウ／EGL surfaceのサイズ。`LUNARIA_SCALE`も参照 |
| `LUNARIA_SCALE` | off | 端末パネル自体を拡大縮小（`0.5`で半分） |
| `LUNARIA_PBUFFER` | auto | `1`でヘッドレスEGLを強制 |
| `LUNARIA_MAX_FRAMES` | 無制限 | *N*フレームで停止 |
| `LUNARIA_MEM_TOTAL_MB` | `6144` | ゲストへ報告するRAM容量 |
| `LUNARIA_HEAP_MB` | A32 `256` / A64は全域（約`2560`） | ゲストのmallocアリーナ |
| `LUNARIA_THREAD_TICKS` / `LUNARIA_A64_THREAD_TICKS` | `200M` / `20K` | ARM32／AArch64ワーカーのスケジューリングスライス |
| `LUNARIA_A64_ENGINES` | auto | AArch64 JITのホストスレッド数（1～8） |
| `LUNARIA_A64_SELF_SCHED` | `1`（ランチャー） | `0`でbarrier-pooled passへ。バイナリ単体では設定するまで無効 |
| `LUNARIA_A64_FASTMEM` | `1` | `0`でメモリアクセスをコールバック経由に |
| `LUNARIA_A64_CODE_CACHE_MB` | `128` | JITごとの変換済みコードキャッシュ |
| `LUNARIA_FD_PARK` | on / off | ブロッキングreadを待機状態に移す |
| `LUNARIA_SYSLIB_DIR` | あれば`syslib-arm64/` | ゲストコードとして動くAArch64プラットフォームライブラリ |
| `LUNARIA_VULKAN` | 利用可能ならon | `0`でVulkanブリッジを無効化 |
| `LUNARIA_DEVICE` | `pixel6` | `pixel6`、`pixel7`、`galaxys21`、`lunaria`（通常は`lunaria.conf`を推奨） |
| `LUNARIA_DATA_ROOT` | 起動ディレクトリ | ゲストの`/data`と`/storage`（`--data-root DIR`） |
| `LUNARIA_CONF` | 起動ディレクトリの`lunaria.conf` | 設定ファイル（`--config FILE`）。別の場所は自動検索しない |
| `LUNARIA_CACHE_DIR` / `LUNARIA_NO_CACHE` | `cache/packages/` / off | パッケージキャッシュの場所 / 毎回展開し直す |
| `LUNARIA_WEB_ENGINE` | `auto` | `auto`、`chrome`、`luna`、`off`（ホストメニューでセッション中に上書き可） |
| `LUNARIA_KEYMAP` | off | `genshin`、`crossworlds`、`off`、またはキーマップのパス |
| `LUNARIA_NET` | on | `0`でゲストの全socketを拒否 |
| `LUNARIA_JIT_UI` | `1` | 起動カードを表示。`0`ならゲストが描画するまで空白 |
| `LUNARIA_DVM` | `1` | Dalvikエミュレーター。`0`無効、`1`ホストスタブがない場合のみdexを実行、`2`スタブよりdexを優先 |
| `LUNARIA_DEX_START` | on | Activityライフサイクルをdexから開始。旧来の起動と比較する時だけ`0` |
| `LUNARIA_TOUCH_TEST` | off | `x,y[;x,y…]`でタップを注入（最大8個）。`_FRAME`／`_HOLD`／`_GAP`は`60`／`10`／`60` |
| `LUNARIA_TOUCH_FIFO` | off | タップ`x,y[,hold]`、`key N`（4＝戻る）、ドラッグ`down`／`move`／`up x,y`。Windowsは`\\.\pipe\lunaria-touch` |
| `LUNARIA_DUMP_FRAME` / `_SCREENSHOT_EVERY` / `_DUMP_DIR` | off / off / `/tmp` | フレーム保存 |
| `LUNARIA_TRACE_SVC` / `_TRACE_MEDIA` / `_TRACE_HEAP` | off | SVC呼び出し／MediaCodec状態／破損したfree-listのトレース |
| `LUNARIA_DVM_TRACE` | off | Dalvikで処理できなかったメソッドを記録 |
| `LUNARIA_HEAP_POISON` / `_HEAP_SELFTEST` | off | bump allocationを`0xFF`で埋める／起動時にアロケーターを自己検証 |
| `LUNARIA_A64_PCPROF`（`_EVERY` `20000`） | off | ゲストのホットPCをスレッド単位でサンプリング |
| `LUNARIA_SCHED_DUMP`（`_STACK`） | off | *N*回のscheduler passごとにゲストスレッド状態を出力（スタック走査付き） |
| `LUNARIA_DUMP_LAST_SVC` | off | 終了時に直近のSVC履歴を表示 |

ランチャー`Activity`とアプリケーションクラスは`ANDROID_LAUNCH_ACTIVITY`と`ANDROID_APPLICATION_CLASS`で渡され、ネイティブエンジンはアプリ自身の`System.loadLibrary`から読み込まれます。

## リポジトリ構成

| パス | 役割 |
|---|---|
| `src/arm_exec.cpp`、`src/arm.c` / `.h` | dynarmicエンジン、SVC dispatch、JNI、EGL/GLES、スレッド処理、共通ARM実行ロック |
| `src/dvm/` | APKの`classes*.dex`を実行するDalvikインタプリタ（`dvm_media.c`にメディアキュー、`apk_signers.c`に署名ブロック解析） |
| `src/loader.c` | Application・ContentProvider・Activityの起動とフレームループ |
| `src/apk.c` | パッケージ展開、マニフェスト解析、キャッシュ、起動処理 |
| `src/linker/`、`src/jvm/`、`src/lib/` | Android ELF linker · JVM/JNIオブジェクトモデル · ホスト側実装（`guest.c`はゲスト内libc） |
| `src/luna_vulkan.c` | Vulkanブリッジ |
| `src/luna_overlay.c`、`src/luna_boot.c`、`src/luna_ime.c` | ホストメニューと合成 · 起動カード · IMEブリッジ |
| `src/webview_cdp.c`、`luna-browser/` | DevToolsクライアント · 内蔵WebViewエンジン（luna-ui + QuickJS） |
| `src/luna_unicode.c` | Unicode分類・正規化・IDNA2003 |
| `keymaps/`、`lunaria.conf.sample`、`runtime/` | タッチマップ · 設定サンプル · 生成されたホスト共有ライブラリ |
| `PROGRESS.md` | 互換性メモと現在の開発状況 |

## トラブルシューティング

<details>
<summary><strong>起動時のムービーが再生されない</strong></summary>

OpenH264が対応するのはConstrained Baselineのみで、Blade & Soul Revolutionなど多くの起動ムービーが使うMain／High（CABAC、Bフレーム）は未対応です。16アクセスユニットを試して映像が得られない場合、Lunariaは`MEDIA_ERROR_UNSUPPORTED`を通知し、対応エンジンが動画をスキップできるようにします。Blade & Soul Masiaのオープニングのように対応プロファイルの動画は、OpenH264があれば最後まで再生されます（`make fetch-openh264`）。確認には`LUNARIA_TRACE_MEDIA=1`を使います。

</details>

<details>
<summary><strong>「project file not found」と表示される</strong></summary>

OBB拡張ファイルが必要です。元のファイル名のままAPKと同じ場所に置いてください。

</details>

<details>
<summary><strong>ランタイム用スタブライブラリが見つからない</strong></summary>

```bash
make runtime/libmediandk.so runtime/libGLESv3.so
```

</details>

<details>
<summary><strong>画面が真っ黒、または起動途中で止まる</strong></summary>

split APKとOBBがすべて揃っているか確認し、次のコマンドでログを取得します。

```bash
LUNARIA_TRACE_EXC=1 LUNARIA_DUMP_LAST_SVC=1 ./lunaria game.apk 2>&1 | tee lunaria.log
```

Unreal Engine 5ではGPUによるOpenGL描画を推奨します。`LIBGL_ALWAYS_SOFTWARE=1`（llvmpipe）は起動が極端に遅くなり、フリーズに見えることがあります。

</details>

<details>
<summary><strong>描画が遅い</strong></summary>

llvmpipeは画面なしのテスト向けで、性能評価には向きません。GPU対応のEGL／OpenGLを有効にし、AArch64では単一エンジンで安定してから`LUNARIA_A64_ENGINES=4`を試してください。計測中は`LUNARIA_SLICE_DETAIL`と`LUNARIA_SVC_HISTO`を無効にします。

</details>

<details>
<summary><strong>クリックしても反応しない</strong></summary>

GLFWは`xdotool`や`XSendEvent`の疑似クリックを受け付けません。実際のウィンドウをクリックするか、`LUNARIA_TOUCH_TEST`／`LUNARIA_TOUCH_FIFO`でタップを注入してください。右クリックはホストメニュー専用です。

</details>

## 開発状況

Lunariaは**開発中の研究用ソフトウェア**で、未実装のAPI、タイトル固有の不具合、互換性を壊す変更があります。結果は特定環境での記録であり、保証ではありません。

- **Linux：** 検証環境では、描画されたフレームとGLFWウィンドウの表示が一致しています。
- **Apple Silicon搭載Mac：** `make dist-mac`で配布物を作れますが、描画できていてもウィンドウが黒いままになることがあります。Cross Worlds 5.04.12はオープニング動画、3Dイントロ、利用規約画面まで到達しました。
- **Windows：** `make dist`と`make dist-cross-win`（2026-11-09に再ビルド）でZIPを作成できます。Wine上ではARM32／ARM64のABIとEGL／GLESの初期化を確認済みで、Windows実機の表示と音声は未検証です。

配布用ビルドは`-O3`、`-DNDEBUG`、LTOで最適化し、dynarmicはRelease＋IPOで別途ビルドします（`-march=native`は不使用）。

**不具合報告や開発への参加を歓迎します。** タイトル名、エンジンのバージョン、ABI、最後に正常だった画面、ログ、可能ならフレーム画像を添えてください。

ライセンスは[Mozilla Public License 2.0](LICENSE)です。Project Lunaria © 2026 Yuichiro Nakada.
