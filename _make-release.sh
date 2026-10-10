#!/usr/bin/env bash
# _make-release.sh — GitHub Actions の代替ローカルリリーススクリプト (lunaria 用)
# 使い方:
#   ./_make-release.sh [OPTIONS] [TAG]     # TAGを指定 (省略時は YYYY.M.D)
#   ./_make-release.sh                     # Linux ネイティブ + Linux 対向アーキ (aarch64 ⇄ x86_64) クロス
#   ./_make-release.sh --native-only       # Linux ネイティブのみ (対向アーキのクロスをしない)
#   ./_make-release.sh --windows           # + Windows (mingw クロス, .zip)
#   ./_make-release.sh --macos             # Linux + macOS (.dmg; macOS ホスト上のみビルド可)
#   ./_make-release.sh --all-targets       # Linux + Windows + macOS すべて
#   ./_make-release.sh --push              # タグを自動的にリモートへプッシュ (確認省略)
#   ./_make-release.sh --no-upload         # ビルドのみ (GitHub Release を作らない)
#   ./_make-release.sh --sync-os           # リリース後に yui0/qBerryOS へアセットを同期
#
# ビルド成果物 (Makefile の `make dist` が作る。名前は lunaria-YYYY-MM-DD-<os>[-<arch>].*):
#   lunaria-*-linux-aarch64.tar.gz / lunaria-*-linux-x86_64.tar.gz
#                            : Linux (同梱 lib/runtime/fonts 付き)。ホストが aarch64 なら
#                              x86_64 を gcc クロス (crossbuild-essential-amd64) で作る
#   lunaria-*-windows.zip    : Windows x86_64 UCRT (Linux から `make dist-cross-win`)
#   lunaria-*-macos.dmg      : macOS (hdiutil/codesign が要るため macOS 上でのみ生成)
#
# 注意: ツリー直下の lunaria / runtime/ / libdl.so / libpthread.so はビルドのたびに
#       消して作り直す (アーキ違いのビルド成果物が混ざらないようにするため)。
#       syslib-arm64/ は ARM64 ゲスト用でアーキ非依存のため両方で共用する。
#
# macOS について:
#   Linux ホストでは .dmg を作れない。--macos 指定時、Linux ホストでは
#   リポジトリ直下 (または MACOS_DMG=/path/to.dmg) に Mac で作成済みの
#   lunaria-*-macos.dmg があればそれをリリースに含め、無ければ警告してスキップする。
#
# 環境変数:
#   GITHUB_TOKEN        — Release 作成用 (未設定時は ~/.config/gh/hosts.yml から取得)
#   QBERRYOS_TOKEN      — --sync-os 用 (未設定時は GITHUB_TOKEN)
#   SYNC_REPO           — --sync-os の同期先 (default: yui0/qBerryOS)
#   SYNC_TAG_PREFIX     — 同期先でのタグ接頭辞 (default: lunaria-)  例: lunaria-2026.10.9
#   WIN_DEPS 他         — scripts/dist-cross-win.sh が参照する変数をそのまま引き継ぐ
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ── 引数パース ─────────────────────────────────────────
BUILD_WINDOWS=false
BUILD_MACOS=false
AUTO_PUSH=false
SYNC_OS=false
UPLOAD=true
LINUX_CROSS=true
while [[ $# -gt 0 ]]; do
  case "${1:-}" in
    --windows)       BUILD_WINDOWS=true ;;
    --macos)         BUILD_MACOS=true ;;
    --all-targets)   BUILD_WINDOWS=true; BUILD_MACOS=true ;;
    --push)          AUTO_PUSH=true ;;
    --sync-os)       SYNC_OS=true ;;
    --no-upload)     UPLOAD=false ;;
    --native-only)   LINUX_CROSS=false ;;
    -h|--help)       sed -n '2,32p' "$0"; exit 0 ;;
    -*)              echo "不明なオプション: $1" >&2; exit 1 ;;
    *)               break ;;
  esac
  shift
done
TAG="${1:-$(date +%Y.%-m.%-d)}"
DIST_DATE="$(date +%Y-%m-%d)"   # Makefile の DIST_DATE と同じ書式 (成果物名の予測用)
HOST_OS="$(uname -s)"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# ── カラー出力 ─────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()    { echo -e "${CYAN}[INFO]${NC} $*"; }
success() { echo -e "${GREEN}[OK]${NC} $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $*"; }
die()     { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }

info "TAG: $TAG"
info "ホスト: $HOST_OS / $(uname -m)"
info "Windows ビルド: $BUILD_WINDOWS"
info "macOS ビルド:   $BUILD_MACOS"
info "自動プッシュ:   $AUTO_PUSH"
info "アップロード:   $UPLOAD"
info "qBerryOS 同期:  $SYNC_OS"

# ── ホスト / クロスのアーキ判定 ────────────────────────
HOST_ARCH="$(uname -m)"
CROSS_ARCH="" CROSS_TRIPLE="" APT_CROSS_ARCH=""
if [[ "$HOST_OS" == "Linux" ]]; then
  case "$HOST_ARCH" in
    x86_64)
      NATIVE_OPENH264=linux64
      CROSS_ARCH=aarch64; CROSS_TRIPLE=aarch64-linux-gnu; APT_CROSS_ARCH=arm64; CROSS_OPENH264=linux-arm64 ;;
    aarch64)
      NATIVE_OPENH264=linux-arm64
      CROSS_ARCH=x86_64;  CROSS_TRIPLE=x86_64-linux-gnu;  APT_CROSS_ARCH=amd64; CROSS_OPENH264=linux64 ;;
    *) die "未対応の Linux ホストアーキ: $HOST_ARCH (x86_64 / aarch64 のみ)" ;;
  esac
  [[ "$LINUX_CROSS" == "true" ]] && info "Linux クロス: $HOST_ARCH → $CROSS_ARCH ($CROSS_TRIPLE)"
fi

# クロスアーキ用の -dev パッケージ (Makefile が -lglfw -lEGL -lGLESv2 -lasound -lbsd -lunwind -lssl -lz を使う)
CROSS_DEV_PKGS=(libglfw3-dev libegl-dev libgles-dev libasound2-dev libbsd-dev
                libunwind-dev libssl-dev zlib1g-dev)

# ── ビルド依存パッケージ (Linux ホスト / apt) ─────────
# qfkey 版と同様、無ければ自動インストールする (Rust は不要)。
# Windows クロスには mingw-w64 + ninja + zip、tar の zstd 展開 (依存取得用) が要る。
if [[ "$HOST_OS" == "Linux" ]] && command -v apt-get &>/dev/null; then
  APT_PKGS=(
    build-essential nasm cmake ninja-build clang libclang-dev pkg-config
    libboost-dev libssl-dev libglfw3-dev libgles2-mesa-dev libegl1-mesa-dev
    libasound2-dev libbsd-dev libunwind-dev zlib1g-dev
    libxcb1-dev libxcb-shm0-dev libxcb-randr0-dev
    python3 curl jq zip unzip bzip2 xz-utils zstd git
  )
  [[ "$BUILD_WINDOWS" == "true" ]] && APT_PKGS+=(mingw-w64)
  [[ "$LINUX_CROSS" == "true" ]] && APT_PKGS+=("crossbuild-essential-$APT_CROSS_ARCH")
  APT_MISSING=()
  for p in "${APT_PKGS[@]}"; do
    dpkg -s "$p" &>/dev/null || APT_MISSING+=("$p")
  done
  if [[ ${#APT_MISSING[@]} -gt 0 ]]; then
    info "不足パッケージをインストール中: ${APT_MISSING[*]}"
    sudo apt-get update -qq
    sudo apt-get install -y --no-install-recommends "${APT_MISSING[@]}"
  fi

  # 対向アーキの -dev ライブラリ (multiarch)
  if [[ "$LINUX_CROSS" == "true" ]]; then
    if ! dpkg -s "libglfw3-dev:$APT_CROSS_ARCH" &>/dev/null || ! dpkg -s "libunwind-dev:$APT_CROSS_ARCH" &>/dev/null; then
      info "対向アーキ ($APT_CROSS_ARCH) の開発ライブラリをインストール中..."
      NATIVE_DPKG_ARCH="$(dpkg --print-architecture)"
      sudo dpkg --add-architecture "$APT_CROSS_ARCH" 2>/dev/null || true

      # 既存の apt ソースを native アーキに限定し、対向アーキ用のミラーを別ファイルで追加する。
      # (ports.ubuntu.com は amd64 を、archive.ubuntu.com は arm64 を配信しない → 404 回避)
      _CROSS_LIST="/etc/apt/sources.list.d/lunaria-cross-$APT_CROSS_ARCH.list"
      if [[ ! -f "$_CROSS_LIST" ]]; then
        _CODENAME="$(lsb_release -sc 2>/dev/null || echo noble)"
        [[ -f /etc/apt/sources.list ]] && \
          sudo sed -i "/^deb [^[]/ s|^deb |deb [arch=$NATIVE_DPKG_ARCH] |" /etc/apt/sources.list || true
        for _f in /etc/apt/sources.list.d/*.list; do
          [[ -f "$_f" ]] && \
            sudo sed -i "/^deb [^[]/ s|^deb |deb [arch=$NATIVE_DPKG_ARCH] |" "$_f" || true
        done
        for _f in /etc/apt/sources.list.d/*.sources; do
          [[ -f "$_f" ]] || continue
          grep -q "^Architectures:" "$_f" || sudo sed -i "/^Components:/a Architectures: $NATIVE_DPKG_ARCH" "$_f" || true
        done
        if [[ "$APT_CROSS_ARCH" == "arm64" ]]; then _MIRROR="http://ports.ubuntu.com/ubuntu-ports/"
        else _MIRROR="http://archive.ubuntu.com/ubuntu/"; fi
        sudo tee "$_CROSS_LIST" >/dev/null <<APT_EOF
deb [arch=$APT_CROSS_ARCH] $_MIRROR $_CODENAME main restricted universe multiverse
deb [arch=$APT_CROSS_ARCH] $_MIRROR $_CODENAME-updates main restricted universe multiverse
deb [arch=$APT_CROSS_ARCH] $_MIRROR $_CODENAME-security main restricted universe multiverse
APT_EOF
      fi
      sudo apt-get update -qq || true
      sudo apt-get install -y --no-install-recommends \
        "${CROSS_DEV_PKGS[@]/%/:$APT_CROSS_ARCH}" || \
        warn "対向アーキの -dev パッケージを一括インストールできませんでした。個別に再試行します。"
      for p in "${CROSS_DEV_PKGS[@]}"; do
        dpkg -s "$p:$APT_CROSS_ARCH" &>/dev/null || \
          sudo apt-get install -y --no-install-recommends "$p:$APT_CROSS_ARCH" || \
          warn "$p:$APT_CROSS_ARCH をインストールできませんでした"
      done
    fi
  fi
fi

check_cmd() { command -v "$1" &>/dev/null || die "$1 が見つかりません。インストールしてください。"; }
check_cmd make
check_cmd git
check_cmd cmake
check_cmd python3
if [[ "$UPLOAD" == "true" ]]; then check_cmd curl; check_cmd jq; fi
if [[ "$HOST_OS" == "Linux" && "$LINUX_CROSS" == "true" ]]; then
  for c in "$CROSS_TRIPLE-gcc" "$CROSS_TRIPLE-g++" "$CROSS_TRIPLE-objdump"; do check_cmd "$c"; done
  for l in glfw EGL GLESv2 asound bsd unwind ssl crypto z; do
    [[ -e "/usr/lib/$CROSS_TRIPLE/lib$l.so" ]] ||       die "対向アーキのライブラリが見つかりません: /usr/lib/$CROSS_TRIPLE/lib$l.so (対応する -dev:$APT_CROSS_ARCH パッケージを確認してください。--native-only でクロスを省略できます)"
  done
fi
if [[ "$BUILD_WINDOWS" == "true" ]]; then
  for c in x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ ninja zip curl bunzip2; do check_cmd "$c"; done
fi

# ── サブモジュール ─────────────────────────────────────
info "サブモジュールを更新中..."
git submodule update --init --recursive

# ── タグ (make dist は git describe でバージョンを決めるので先に作る) ──
if ! git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
  git tag "$TAG"
  info "ローカルタグ $TAG を作成しました"
fi

ARTIFACTS_DIR="$SCRIPT_DIR/_release_artifacts"
rm -rf "$ARTIFACTS_DIR"
mkdir -p "$ARTIFACTS_DIR"

# 古い成果物を消してから作り、完成品を ARTIFACTS_DIR へ移す
make dist-clean >/dev/null 2>&1 || rm -f lunaria-*.tar.gz lunaria-*.zip lunaria-*.dmg
collect() {  # collect <ファイル名>
  [[ -f "$1" ]] || die "成果物が見つかりません: $1"
  mv "$1" "$ARTIFACTS_DIR/"
  success "$1 完成"
}

# guest 側 AArch64 libm 等。無くても動く (SVC thunk 経由になるだけ) ので失敗は許容
build_syslib() { make syslib guestlib || warn "syslib/guestlib をビルドできませんでした。無しで同梱します。"; }

# ビルドツリー直下のアーキ依存成果物を消す (ネイティブ/クロスの混在防止)
reset_tree() { rm -rf lunaria lunaria.exe runtime libdl.so libpthread.so; }

# ─────────────────────────────────────────────────────────
# ① Linux / macOS ネイティブ (ホスト OS に対応する成果物)
# ─────────────────────────────────────────────────────────
case "$HOST_OS" in
  Linux)
    NAME="lunaria-$DIST_DATE-linux-$HOST_ARCH"
    info "ビルド中: $NAME.tar.gz (ネイティブ) ..."
    build_syslib
    reset_tree
    make -j"$JOBS" dist DIST_NAME="$NAME" OPENH264_ARCH="$NATIVE_OPENH264"
    collect "$NAME.tar.gz"
    ;;
  Darwin)
    if [[ "$BUILD_MACOS" == "true" ]]; then
      info "ビルド中: lunaria-$DIST_DATE-macos.dmg ..."
      make macos-deps
      build_syslib
      make -j"$JOBS" dist
      collect "lunaria-$DIST_DATE-macos.dmg"
    else
      warn "macOS ホストですが --macos が無いため macOS ビルドをスキップします。"
    fi
    ;;
  *) die "未対応のホスト OS: $HOST_OS (Linux / macOS のみ)" ;;
esac

# ─────────────────────────────────────────────────────────
# ①-B Linux クロス (aarch64 ホスト → x86_64 / x86_64 ホスト → aarch64)
# pkg-config はホスト用を指すので使わず、Makefile のライブラリ変数を直接渡す。
# ヘッダ (glfw/EGL/GLES/boost) は /usr/include にあり、アーキ固有のもの
# (libunwind 等) は gcc クロスが /usr/include/<triple> を自動で探す。
# ─────────────────────────────────────────────────────────
if [[ "$HOST_OS" == "Linux" && "$LINUX_CROSS" == "true" ]]; then
  NAME="lunaria-$DIST_DATE-linux-$CROSS_ARCH"
  info "ビルド中: $NAME.tar.gz (クロス: $CROSS_TRIPLE) ..."
  _GCC_CROSS_LIB="$(ls -d /usr/lib/gcc-cross/$CROSS_TRIPLE/* 2>/dev/null | sort -V | tail -1 || true)"
  reset_tree
  OBJDUMP="$CROSS_TRIPLE-objdump" \
  DIST_LIB_DIRS="/usr/lib/$CROSS_TRIPLE:/lib/$CROSS_TRIPLE:/usr/$CROSS_TRIPLE/lib:${_GCC_CROSS_LIB}" \
  make -j"$JOBS" dist \
    DIST_NAME="$NAME" DIST_ARCH="$CROSS_ARCH" OPENH264_ARCH="$CROSS_OPENH264" \
    CC="$CROSS_TRIPLE-gcc" CXX="$CROSS_TRIPLE-g++" \
    HOST_CPPFLAGS="" \
    HOST_WINDOW_LIBS="-lglfw" \
    HOST_LIBC_LIBS="-lbsd -lunwind" \
    DYNARMIC_CMAKE_FLAGS="-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=$CROSS_ARCH -DCMAKE_C_COMPILER=$CROSS_TRIPLE-gcc -DCMAKE_CXX_COMPILER=$CROSS_TRIPLE-g++ -DBoost_INCLUDE_DIR=/usr/include -DBoost_NO_BOOST_CMAKE=ON"
  collect "$NAME.tar.gz"
  reset_tree
fi

# ─────────────────────────────────────────────────────────
# ② Windows: Linux から mingw-w64 (UCRT) でクロスビルド
# ─────────────────────────────────────────────────────────
if [[ "$BUILD_WINDOWS" == "true" ]]; then
  if [[ "$HOST_OS" != "Linux" ]]; then
    warn "Windows クロスビルドは Linux ホストのみ対応です。スキップします。"
  else
    info "ビルド中: lunaria-$DIST_DATE-windows.zip (mingw クロス) ..."
    make dist-cross-win
    collect "lunaria-$DIST_DATE-windows.zip"
  fi
fi

# ─────────────────────────────────────────────────────────
# ③ macOS: Linux ホストでは作成済み .dmg を取り込むのみ
# ─────────────────────────────────────────────────────────
if [[ "$BUILD_MACOS" == "true" && "$HOST_OS" != "Darwin" ]]; then
  DMG="${MACOS_DMG:-}"
  if [[ -z "$DMG" ]]; then
    DMG="$(ls -t lunaria-*-macos.dmg 2>/dev/null | head -1 || true)"
  fi
  if [[ -n "$DMG" && -f "$DMG" ]]; then
    cp "$DMG" "$ARTIFACTS_DIR/$(basename "$DMG")"
    success "作成済みの macOS dmg を取り込みました: $(basename "$DMG")"
  else
    warn "macOS の .dmg は Linux では作れません (hdiutil/codesign が必要)。"
    warn "Mac で ./_make-release.sh --macos --no-upload を実行して lunaria-*-macos.dmg を作り、"
    warn "このディレクトリに置くか MACOS_DMG=/path/to.dmg を指定してください。macOS はスキップします。"
  fi
fi

# ─────────────────────────────────────────────────────────
RELEASE_FILES=()
for f in "$ARTIFACTS_DIR"/*.tar.gz "$ARTIFACTS_DIR"/*.zip "$ARTIFACTS_DIR"/*.dmg; do
  [[ -f "$f" ]] && RELEASE_FILES+=("$f")
done
[[ ${#RELEASE_FILES[@]} -eq 0 ]] && die "リリース対象のファイルがありません"

if [[ "$UPLOAD" != "true" ]]; then
  echo ""
  success "=== ビルド完了 (--no-upload) ==="
  ls -lh "${RELEASE_FILES[@]}"
  exit 0
fi

# ─────────────────────────────────────────────────────────
# ④ GitHub Release 作成 & アップロード (curl + GitHub REST API)
# ─────────────────────────────────────────────────────────
if [[ -z "${GITHUB_TOKEN:-}" ]]; then
  HOSTS_YML="$HOME/.config/gh/hosts.yml"
  if [[ -f "$HOSTS_YML" ]]; then
    GITHUB_TOKEN=$(grep -A2 'github.com' "$HOSTS_YML" | grep 'oauth_token' | awk '{print $2}' | tr -d '"' || true)
  fi
fi
[[ -z "${GITHUB_TOKEN:-}" ]] && die "GITHUB_TOKEN が未設定です。export GITHUB_TOKEN=ghp_xxx を実行してください。"

GH_REPO=$(git remote get-url origin | sed -E 's|.*github\.com[:/]||; s|\.git$||')
GH_API="https://api.github.com"
info "リポジトリ: $GH_REPO"

if [[ "$AUTO_PUSH" == "true" ]]; then
  info "タグ '$TAG' をリモートに自動プッシュします..."
  git push -f origin "$TAG"
else
  read -rp "タグ '$TAG' をリモートにプッシュしますか? [y/N]: " ans
  [[ "${ans,,}" == "y" ]] && git push -f origin "$TAG"
fi

RELEASE_BODY=$(cat <<'EOF'
## Lunaria

An Android runtime that runs an APK on the desktop: AArch64 and ARM32 guest
code on dynarmic, the platform's Java on its own bytecode interpreter, and GLES
straight through to the host.

### Linux
```bash
tar -xzf lunaria-*-linux-x86_64.tar.gz && cd lunaria-*-linux-x86_64   # or -aarch64
./lunaria /path/to/app.apk      # or .xapk / .apks
```
The archive carries its own shim libraries, host libraries and fonts; it uses
the running system's C library.

### macOS
Open the .dmg, drag `Lunaria.app` where you like, then run
`/Applications/Lunaria.app/Contents/MacOS/Lunaria /path/to/app.apk`.

### Windows
Unzip `lunaria-*-windows.zip` and run `lunaria.exe /path/to/app.apk`.
EOF
)

# upload_release <repo> <token> <tag> <name>  — 既存 Release があればアセットを差し替える
upload_release() {
  local repo="$1" token="$2" tag="$3" name="$4"
  local auth="Authorization: Bearer $token" resp id upload_url
  resp=$(curl -sL -H "$auth" "$GH_API/repos/$repo/releases/tags/$tag")
  id=$(jq -r '.id // empty' <<<"$resp")
  if [[ -z "$id" ]]; then
    resp=$(curl -sL -X POST -H "$auth" -H "Content-Type: application/json" \
      "$GH_API/repos/$repo/releases" \
      -d "$(jq -n --arg t "$tag" --arg n "$name" --arg b "$RELEASE_BODY" \
            '{tag_name:$t,name:$n,body:$b,generate_release_notes:true,draft:false,prerelease:false}')")
    id=$(jq -r '.id // empty' <<<"$resp")
    [[ -z "$id" ]] && die "$repo: Release 作成失敗: $(jq -r '.message // .' <<<"$resp")"
    info "$repo: Release $tag を作成 (id=$id)"
  else
    info "$repo: Release $tag は既存 (id=$id) — 同名アセットを差し替えます"
    local assets aid aname
    assets=$(curl -sL -H "$auth" "$GH_API/repos/$repo/releases/$id/assets?per_page=100")
    for f in "${RELEASE_FILES[@]}"; do
      aname="$(basename "$f")"
      aid=$(jq -r --arg n "$aname" '.[] | select(.name==$n) | .id' <<<"$assets")
      [[ -n "$aid" ]] && curl -sL -o /dev/null -X DELETE -H "$auth" \
        "$GH_API/repos/$repo/releases/assets/$aid"
    done
  fi
  upload_url=$(jq -r '.upload_url // empty' <<<"$resp" | sed 's/{.*}//')
  if [[ -z "$upload_url" ]]; then   # 既存の場合は resp が空なので取り直す
    upload_url="https://uploads.github.com/repos/$repo/releases/$id/assets"
  fi
  local f
  for f in "${RELEASE_FILES[@]}"; do
    info "  アップロード中: $(basename "$f") → $repo"
    curl -fsSL -X POST -H "$auth" -H "Content-Type: application/octet-stream" \
      "${upload_url}?name=$(basename "$f")" --data-binary @"$f" >/dev/null
  done
  success "$repo: ${#RELEASE_FILES[@]} ファイルをアップロード完了"
}

upload_release "$GH_REPO" "$GITHUB_TOKEN" "$TAG" "Lunaria $TAG"

# ─────────────────────────────────────────────────────────
# ⑤ qBerryOS 同期 (--sync-os 指定時のみ): 同じアセットを別リポジトリの Release にも載せる
# ─────────────────────────────────────────────────────────
if [[ "$SYNC_OS" == "true" ]]; then
  SYNC_REPO="${SYNC_REPO:-yui0/qBerryOS}"
  SYNC_TOKEN="${QBERRYOS_TOKEN:-$GITHUB_TOKEN}"
  info "$SYNC_REPO へリリースを同期します..."
  upload_release "$SYNC_REPO" "$SYNC_TOKEN" "${SYNC_TAG_PREFIX:-lunaria-}$TAG" "Lunaria $TAG"
fi

echo ""
success "=== リリース $TAG 完了 ==="
ls -lh "${RELEASE_FILES[@]}"
