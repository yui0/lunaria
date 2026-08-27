# Lunaria — Android-to-Linux loader

Android の X86_64/ARM32/64 JNI ライブラリ(主に Unity の `libunity.so`)を Linux 上で
dynarmic JIT によりエミュレート実行するローダー。

## ビルド

```sh
make dynarmic-build   # dynarmic A32 JIT (要: cmake, libboost-dev)
make                  # lunaria 本体 + runtime/*.so
make fetch-luna-ui    # エミュレータ UI / 起動画面が使う luna-ui ヘッダを取得
make fetch-libunity   # テスト用 APK を取得し test/ に ARM ライブラリを展開
```

`luna-ui` は `make` が初回に自動取得するため、通常 `fetch-luna-ui` を明示的に
実行する必要はない。

必要パッケージ (Ubuntu): `libglfw3-dev libegl1-mesa-dev libgles2-mesa-dev
libbsd-dev libunwind-dev libboost-dev zlib1g-dev`

## 実行

```sh
make test

bash lunaria-apk.sh UnitySampleGame.apk
bash lunaria-apk.sh test/dfu-mono-32bit.apk
bash lunaria-apk.sh test/btw-android.apk

#LD_LIBRARY_PATH=.:runtime ANDROID_PACKAGE_CODE_PATH=$PWD/test/dfu-mono-32bit.apk ./lunaria test/libunity.so
#LD_LIBRARY_PATH=.:runtime ./lunaria libunity.so
```

ヘッドレス環境では `Xvfb :99 & DISPLAY=:99 ...` で実行できる。

### 起動画面

大きなタイトルはランチャーの最後のメッセージから最初のフレームまでが長い。
Blade & Soul Revolution の場合、200MB のライブラリのリンク、1,610 個の静的
初期化子、4 ファイル計 28MB の dex コンパイルが先に走る。その間 Lunaria は
自前の起動画面を描画し、いま何を読み込んでいるかを表示する。月・その光輪・
明暗境界線はすべて luna-ui が描画する CSS アニメーション。

![Lunaria の起動画面](screenshot_bootscreen.png)

ゲストがサーフェスを取得した時点で画面は消える。`LUNARIA_SPLASH=0` で無効化、
`make splash-test` でタイトルを起動せずに単体描画できる。

### 環境変数

| 変数 | 既定値 | 意味 |
|------|--------|------|
| `LUNARIA_CALL_TICKS` | 2G | JNI 呼び出し 1 回あたりの tick 上限 |
| `LUNARIA_ONLOAD_TICKS` | 5G | JNI_OnLoad の tick 上限 |
| `LUNARIA_THREAD_TICKS` | 200M | ゲストスレッド 1 スライスの tick 上限 |
| `LUNARIA_TRACE_SVC` | off | SVC 呼び出しをログ (各 run 先頭 1000 件) |
| `LUNARIA_TRACE_BLOCKS` | 0 | JIT ブロック実行を N 件ログ |
| `LUNARIA_DUMP_LAST_SVC` | off | 各 run 終了時に直近 64 件の SVC をダンプ |
| `LUNARIA_TRACE_FUTEX` | off | futex WAIT の woken / timeout を各 64 件までログ |
| `LUNARIA_TRACE_JITINIT` | off | `mono_jit_init_version` (guest 0x2013407c) 突入を呼び出し元付きでログ |
| `GC_DONT_GC` | 1 (ローダが設定) | Boehm GC を無効化 (協調スレッドでは STW 不可) |
| `LUNARIA_SPLASH` | 1 | 起動画面。`0` でゲストが描くまでサーフェスに触れない |
| `LUNARIA_DVM` | 1 | Dalvik バイトコードエミュレータ。`0`=無効、`1`=ホストスタブが無いメソッドだけ dex を実行、`2`=dex を優先 |
| `LUNARIA_DVM_TRACE` | off | エミュレータが引き受けなかったメソッドを `[dvm] miss …` でログ |

K/M/G 接尾辞 (例 `LUNARIA_ONLOAD_TICKS=5G`) が利用できる。

## アーキテクチャ概要

- **フラット 4GB メモリ**: ゲスト 32bit 空間全体を 1 つの匿名 mmap
  (MAP_NORESERVE) で予約。dynarmic fastmem によりゲストのロード/ストアは
  ホスト直接アクセスにコンパイルされる。
- **SVC トランポリン**: 未解決インポートと JNI vtable は `SVC #n` +
  `BX LR` のトランポリンに解決され、ホスト側 `dispatch_svc()` が処理する。
  libc / EGL / GLES2 / zlib / 数学関数 / 時刻系をホストへパススルー。
- **協調スレッド**: `pthread_create` はレジスタコンテキスト+専用スタックを
  持つゲストスレッドを生成。メイン JIT が呼び出し中でも、待機系 SVC
  (cond_wait/sem_wait/usleep/yield) から **補助 JIT** 上でラウンドロビン実行。
- **再配置**: R_ARM_RELATIVE / 定義済みシンボルの JUMP_SLOT/GLOB_DAT/ABS32
  を処理し、非ゼロベース(libmono 等)のロードに対応。
- **暴走ガード**: NULL コール (pc<0x1000)、ゼロ命令スレッド、未定義命令
  パッチ上限により、wild jump 時の JIT キャッシュ無限成長 (OOM) を防止。
- **AssetManager ブリッジ** (`arm_exec.cpp` の `asset_bridge` / `try_asset_jni`):
  `getAssets().open(path)` を APK zip から直接読み出す `java.io.InputStream`
  に解決する。STORE / DEFLATE 両対応 (zlib raw inflate)。`InputStream.read`
  系・`Scanner.next` (boot.config 用) をホスト側で実装し、ストリームのカーソル
  を jobject ハンドルで管理する。汎用 JNI ディスパッチは Call*Method の引数を
  捨てるため、ここでは AAPCS に従い ARM レジスタ/スタックから引数 (open の
  パス, read のバッファ等) を直接 marshal する。

