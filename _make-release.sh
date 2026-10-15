#!/usr/bin/env bash
# _make-release.sh — GitHub Actions の代替ローカルリリーススクリプト (lunaria 用)
# 使い方:
#   ./_make-release.sh [OPTIONS] [TAG]     # TAGを指定 (省略時は YYYY.M.D)
#   ./_make-release.sh                     # Linux ネイティブ + Linux 対向アーキ (aarch64 ⇄ x86_64) クロス
#   ./_make-release.sh --native-only       # Linux ネイティブのみ (対向アーキのクロスをしない)
#   bash _make-release.sh                  # clone 直後で Permission denied のとき (chmod +x でも可)
#   ※ Orange Pi 等で panfork Mesa があると amd64 の libegl が multiarch 同居できない。
#     その場合は .deb を .cross-sysroot-amd64/ に展開してクロスリンクする。
#   ./_make-release.sh --windows           # + Windows (mingw クロス, .zip)
#   ./_make-release.sh --macos             # Linux + macOS (.dmg; macOS ホスト上のみビルド可)
#   ./_make-release.sh --all-targets       # Linux + Windows + macOS すべて
#   ./_make-release.sh --push              # タグを自動的にリモートへプッシュ (確認省略)
#   ./_make-release.sh --no-upload         # ビルドのみ (GitHub Release を作らない)
#   ./_make-release.sh --sync-os           # リリース後に yui0/qBerryOS へアセットを同期
#
# ビルド成果物 (Makefile の `make dist` が作る。名前は lunaria-YYYY-MM-DD-<os>-<arch>.*):
#   lunaria-*-linux-aarch64.tar.gz / lunaria-*-linux-x86_64.tar.gz
#                            : Linux (同梱 lib/runtime/fonts 付き)。ホストが aarch64 なら
#                              x86_64 を gcc クロス (crossbuild-essential-amd64) で作る
#   lunaria-*-windows-x86_64.zip : Windows x86_64 UCRT (Linux から `make dist-cross-win`)
#   lunaria-*-macos-arm64.dmg    : macOS (hdiutil/codesign が要るため macOS 上でのみ生成)
#
# 注意: ツリー直下の lunaria / runtime/ / libdl.so / libpthread.so はビルドのたびに
#       消して作り直す (アーキ違いのビルド成果物が混ざらないようにするため)。
#       syslib-arm64/ は ARM64 ゲスト用でアーキ非依存のため両方で共用する。
#
# macOS について:
#   Linux ホストでは .dmg を作れない。--macos 指定時、Linux ホストでは
#   リポジトリ直下 (または MACOS_DMG=/path/to.dmg) に Mac で作成済みの
#   lunaria-*-macos-*.dmg があればそれをリリースに含め、無ければ警告してスキップする。
#
# 環境変数:
#   GITHUB_TOKEN        — Release 作成用 (未設定時は ~/.config/gh/hosts.yml から取得)
#   QBERRYOS_TOKEN      — --sync-os 用 (未設定時は GITHUB_TOKEN)
#   SYNC_REPO           — --sync-os の同期先 (default: yui0/qBerryOS)
#   SYNC_TAG_PREFIX     — 同期先でのタグ接頭辞 (default: lunaria-)  例: lunaria-2026.10.9
#   WIN_DEPS 他         — scripts/dist-cross-win.sh が参照する変数をそのまま引き継ぐ
#
# `sh _make-release.sh` (dash 等) でも動くよう、bash でなければ付け替える。
# この分岐だけ POSIX にする（pipefail / [[ は bash 専用）。
if [ -z "${BASH_VERSION:-}" ]; then
  exec bash "$0" "$@"
fi
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
# libEGL/libGLESv2 の DT_NEEDED。-dev 依存の再帰取得が欠けることがあるので明示する。
CROSS_RUNTIME_PKGS=(libglvnd0 libegl1 libgles2)
# panfork 等のカスタム Mesa がある board では Multi-Arch:same の Mesa が
# 対向アーキと同居できない。その場合 deb を展開した sysroot でリンクする。
CROSS_SYSROOT=""
CROSS_LIB_FLAGS=""

# 対向アーキの .so をシステム multiarch か sysroot から探す
# name は "EGL" / "GLdispatch" など。versioned soname (libGLdispatch.so.0) も探す。
cross_lib_path() {
  local name="$1" p dir
  local dirs=("/usr/lib/$CROSS_TRIPLE" "/lib/$CROSS_TRIPLE")
  if [[ -n "${CROSS_SYSROOT:-}" ]]; then
    dirs+=("$CROSS_SYSROOT/usr/lib/$CROSS_TRIPLE" "$CROSS_SYSROOT/lib/$CROSS_TRIPLE")
  fi
  for dir in "${dirs[@]}"; do
    [[ -d "$dir" ]] || continue
    for p in "$dir/lib${name}.so" "$dir/lib${name}.so."*; do
      [[ -e "$p" ]] && { echo "$p"; return 0; }
    done
  done
  return 1
}

# クロスリンク時、共有ライブラリの DT_NEEDED (libGLdispatch 等) を解決する -L / -rpath-link
cross_link_flags() {
  local dirs=() d flags="" rpath_link=""
  if [[ -n "$CROSS_SYSROOT" ]]; then
    dirs+=(
      "$CROSS_SYSROOT/usr/lib/$CROSS_TRIPLE"
      "$CROSS_SYSROOT/lib/$CROSS_TRIPLE"
      "$CROSS_SYSROOT/usr/lib"
      "$CROSS_SYSROOT/lib"
    )
  fi
  dirs+=("/usr/lib/$CROSS_TRIPLE" "/lib/$CROSS_TRIPLE" "/usr/$CROSS_TRIPLE/lib")
  for d in "${dirs[@]}"; do
    [[ -d "$d" ]] || continue
    flags+=" -L$d"
    rpath_link+="${rpath_link:+:}$d"
  done
  [[ -n "$rpath_link" ]] && flags+=" -Wl,-rpath-link,$rpath_link"
  # trim leading space
  echo "${flags# }"
}

# apt で入れられない対向アーキ -dev を、deb 展開 sysroot で賄う
# (Orange Pi + panfork Mesa など: arm64 カスタム Mesa と amd64 標準 Mesa が同居不可)
setup_cross_sysroot() {
  local arch="$1"
  local sysroot="$SCRIPT_DIR/.cross-sysroot-$arch"
  local tmpdir pkgs pkg
  info "対向アーキ ($arch) を sysroot に展開します (multiarch 同居不可時のフォールバック)..."
  tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/lunaria-cross-debs.XXXXXX")"
  # shellcheck disable=SC2064
  trap "rm -rf '$tmpdir'" RETURN
  rm -rf "$sysroot"
  mkdir -p "$sysroot"
  # 依存を再帰解決して download (インストールはしない → Mesa 衝突を回避)
  mapfile -t pkgs < <(
    apt-cache depends --recurse --no-recommends --no-suggests \
      --no-conflicts --no-breaks --no-replaces --no-enhances \
      "${CROSS_DEV_PKGS[@]/%/:$arch}" "${CROSS_RUNTIME_PKGS[@]/%/:$arch}" 2>/dev/null \
      | awk '/^[^[:space:]]/ { sub(/:.*$/, ""); print }' \
      | sed "s/\$/:$arch/" \
      | sort -u
  )
  # apt-cache が空振りしたときは最低限のパッケージ名だけ落とす
  if [[ ${#pkgs[@]} -eq 0 ]]; then
    pkgs=("${CROSS_DEV_PKGS[@]/%/:$arch}" "${CROSS_RUNTIME_PKGS[@]/%/:$arch}")
  fi
  (
    cd "$tmpdir"
    # 仮想パッケージ等で落ちる依存は飛ばす (リンクに不要なことが多い)
    for pkg in "${pkgs[@]}" "${CROSS_DEV_PKGS[@]/%/:$arch}" "${CROSS_RUNTIME_PKGS[@]/%/:$arch}"; do
      apt-get download "$pkg" 2>/dev/null || true
    done
    shopt -s nullglob
    set -- ./*.deb
    [[ -e "$1" ]] || die "対向アーキの .deb を download できませんでした"
    for deb in "$@"; do
      dpkg-deb -x "$deb" "$sysroot"
    done
  )
  # リンク用 .so の有無を確認 (glfw / EGL / GLESv2 は Mesa 衝突で欠ける典型。
  # GLdispatch は libEGL の DT_NEEDED — 欠けると undefined reference to __glDispatch*)
  CROSS_SYSROOT="$sysroot"
  for name in glfw EGL GLESv2 GLdispatch; do
    cross_lib_path "$name" >/dev/null \
      || die "sysroot に lib${name}.so がありません ($sysroot)。--native-only でクロスを省略できます"
  done
  CROSS_LIB_FLAGS="$(cross_link_flags)"
  success "クロス用 sysroot: $sysroot"
}

# 既存 sysroot に libglvnd (GLdispatch) だけ欠けている場合、その deb だけ足す
ensure_sysroot_glvnd() {
  local arch="$1" sysroot="$2" tmpdir pkg
  CROSS_SYSROOT="$sysroot"
  cross_lib_path GLdispatch >/dev/null && return 0
  info "sysroot に libGLdispatch が無いため libglvnd0:$arch を追加します..."
  tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/lunaria-glvnd.XXXXXX")"
  # shellcheck disable=SC2064
  trap "rm -rf '$tmpdir'" RETURN
  (
    cd "$tmpdir"
    for pkg in "${CROSS_RUNTIME_PKGS[@]/%/:$arch}"; do
      apt-get download "$pkg" 2>/dev/null || true
    done
    shopt -s nullglob
    for deb in ./*.deb; do
      dpkg-deb -x "$deb" "$sysroot"
    done
  )
  cross_lib_path GLdispatch >/dev/null \
    || die "sysroot に libGLdispatch を追加できませんでした ($sysroot)。apt で libglvnd0:$arch が取れるか確認するか、--native-only でクロスを省略できます"
}

# ── ビルド依存パッケージ (Linux ホスト / apt) ─────────
# qfkey 版と同様、無ければ自動インストールする (Rust は不要)。
# Windows クロスには UCRT 対応 MinGW + ninja + zip、tar の zstd 展開 (依存取得用) が要る。
# Ubuntu apt の mingw-w64 (GCC 13) は -mcrtdll=ucrt 非対応なので使わず、後段で llvm-mingw を入れる。
if [[ "$HOST_OS" == "Linux" ]] && command -v apt-get &>/dev/null; then
  APT_PKGS=(
    build-essential nasm cmake ninja-build clang libclang-dev pkg-config
    libboost-dev libssl-dev libglfw3-dev libgles2-mesa-dev libegl1-mesa-dev
    libasound2-dev libbsd-dev libunwind-dev zlib1g-dev libvulkan-dev
    libxcb1-dev libxcb-shm0-dev libxcb-randr0-dev
    python3 curl jq zip unzip bzip2 xz-utils zstd git
  )
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

  # Ubuntu apt MinGW lacks -mcrtdll=ucrt; install llvm-mingw (UCRT) when needed.
  # Host arch matters: aarch64 (Orange Pi 等) には *-ubuntu-22.04-aarch64 を入れる。
  # x86_64 用バイナリを PATH に置くと Exec format error で
  # "cross compiler failed to link a trivial program" になる。
  if [[ "$BUILD_WINDOWS" == "true" ]]; then
    _need_llvm_mingw=true
    if command -v x86_64-w64-mingw32-gcc &>/dev/null \
        && x86_64-w64-mingw32-gcc --version &>/dev/null; then
      if printf 'int main(void){return 0;}\n' \
          | x86_64-w64-mingw32-gcc -mcrtdll=ucrt -x c -fsyntax-only - &>/dev/null; then
        _need_llvm_mingw=false
      else
        _probe=$(mktemp -d)
        printf 'int main(void){return 0;}\n' >"$_probe/t.c"
        if x86_64-w64-mingw32-gcc "$_probe/t.c" -o "$_probe/t.exe" &>/dev/null \
            && x86_64-w64-mingw32-objdump -p "$_probe/t.exe" | grep -Eiq 'ucrtbase|api-ms-win-crt-'; then
          _need_llvm_mingw=false
        fi
        rm -rf "$_probe"
      fi
    fi
    if [[ "$_need_llvm_mingw" == "true" ]]; then
      _ver=20261006
      case "$(uname -m)" in
        x86_64)        _llvm_host=x86_64 ;;
        aarch64|arm64) _llvm_host=aarch64 ;;
        *) die "Windows クロス用 llvm-mingw は x86_64 / aarch64 ホストのみ対応です (今: $(uname -m))" ;;
      esac
      _prefix="/opt/llvm-mingw-${_ver}-ucrt-ubuntu-22.04-${_llvm_host}"
      _gcc="$_prefix/bin/x86_64-w64-mingw32-gcc"
      # -x だけでは他アーキの ELF を弾けないので、実際に --version が通るか見る
      if [[ ! -x "$_gcc" ]] || ! "$_gcc" --version &>/dev/null; then
        info "UCRT 用 llvm-mingw ($_llvm_host ホスト) をインストール中..."
        _archive="llvm-mingw-${_ver}-ucrt-ubuntu-22.04-${_llvm_host}.tar.xz"
        curl -fsSL -o "/tmp/$_archive" \
          "https://github.com/mstorsjo/llvm-mingw/releases/download/${_ver}/$_archive"
        sudo rm -rf "$_prefix"
        sudo tar -xJf "/tmp/$_archive" -C /opt
        rm -f "/tmp/$_archive"
        "$_gcc" --version &>/dev/null \
          || die "llvm-mingw の gcc を実行できません ($_gcc)。アーカイブとホストアーキを確認してください"
      fi
      sudo ln -sfn "$_prefix" /opt/llvm-mingw
      export PATH="/opt/llvm-mingw/bin:$PATH"
    fi
  fi

  # 対向アーキの -dev ライブラリ (multiarch)
  if [[ "$LINUX_CROSS" == "true" ]]; then
    if ! dpkg -s "libglfw3-dev:$APT_CROSS_ARCH" &>/dev/null || ! dpkg -s "libunwind-dev:$APT_CROSS_ARCH" &>/dev/null; then
      info "対向アーキ ($APT_CROSS_ARCH) の開発ライブラリをインストール中..."
      NATIVE_DPKG_ARCH="$(dpkg --print-architecture)"
      sudo dpkg --add-architecture "$APT_CROSS_ARCH" 2>/dev/null || true

      # 既存の apt ソースを native アーキに限定し、対向アーキ用のミラーを別ファイルで追加する。
      # (ports.ubuntu.com は amd64 を、archive.ubuntu.com は arm64 を配信しない → 404 回避)
      # 既に amd64-cross.list 等で対向アーキミラーがある場合は二重登録しない。
      _CROSS_LIST="/etc/apt/sources.list.d/lunaria-cross-$APT_CROSS_ARCH.list"
      _HAS_CROSS_MIRROR=false
      for _f in /etc/apt/sources.list /etc/apt/sources.list.d/*.list; do
        [[ -f "$_f" ]] || continue
        if grep -qE "^deb[[:space:]]+\[arch=$APT_CROSS_ARCH\]" "$_f"; then
          _HAS_CROSS_MIRROR=true
          break
        fi
      done
      if [[ "$_HAS_CROSS_MIRROR" == "false" && ! -f "$_CROSS_LIST" ]]; then
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
      elif [[ "$_HAS_CROSS_MIRROR" == "true" ]]; then
        info "対向アーキ ($APT_CROSS_ARCH) の apt ミラーは既にあるため追加しません"
        # 手動の amd64-cross.list 等と lunaria-cross-*.list が二重だと apt が大量警告する
        if [[ -f "$_CROSS_LIST" ]]; then
          for _f in /etc/apt/sources.list /etc/apt/sources.list.d/*.list; do
            [[ -f "$_f" && "$_f" != "$_CROSS_LIST" ]] || continue
            if grep -qE "^deb[[:space:]]+\[arch=$APT_CROSS_ARCH\]" "$_f"; then
              warn "重複ミラー $_CROSS_LIST を削除します ($_f を利用)"
              sudo rm -f "$_CROSS_LIST"
              break
            fi
          done
        fi
      fi
      sudo apt-get update -qq || true
      sudo apt-get install -y --no-install-recommends \
        "${CROSS_DEV_PKGS[@]/%/:$APT_CROSS_ARCH}" \
        "${CROSS_RUNTIME_PKGS[@]/%/:$APT_CROSS_ARCH}" || \
        warn "対向アーキの -dev パッケージを一括インストールできませんでした。個別に再試行します。"
      for p in "${CROSS_DEV_PKGS[@]}" "${CROSS_RUNTIME_PKGS[@]}"; do
        dpkg -s "$p:$APT_CROSS_ARCH" &>/dev/null || \
          sudo apt-get install -y --no-install-recommends "$p:$APT_CROSS_ARCH" || \
          warn "$p:$APT_CROSS_ARCH をインストールできませんでした"
      done
    fi
    # multiarch で GLFW/EGL が入らない場合 (panfork Mesa 等) は sysroot へフォールバック
    if ! cross_lib_path glfw >/dev/null || ! cross_lib_path EGL >/dev/null || ! cross_lib_path GLESv2 >/dev/null; then
      _EXISTING_SYSROOT="$SCRIPT_DIR/.cross-sysroot-$APT_CROSS_ARCH"
      if [[ -e "$_EXISTING_SYSROOT/usr/lib/$CROSS_TRIPLE/libglfw.so" \
         && -e "$_EXISTING_SYSROOT/usr/lib/$CROSS_TRIPLE/libEGL.so" \
         && -e "$_EXISTING_SYSROOT/usr/lib/$CROSS_TRIPLE/libGLESv2.so" ]]; then
        CROSS_SYSROOT="$_EXISTING_SYSROOT"
        ensure_sysroot_glvnd "$APT_CROSS_ARCH" "$CROSS_SYSROOT"
        CROSS_LIB_FLAGS="$(cross_link_flags)"
        info "既存のクロス用 sysroot を再利用: $CROSS_SYSROOT"
      else
        warn "対向アーキの GLFW/EGL/GLES が multiarch で入りません (カスタム Mesa PPA との衝突が典型です)"
        setup_cross_sysroot "$APT_CROSS_ARCH"
      fi
    else
      # multiarch でも DT_NEEDED (libGLdispatch.so.0) 解決に -rpath-link が要る
      CROSS_LIB_FLAGS="$(cross_link_flags)"
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
  for l in glfw EGL GLESv2 GLdispatch asound bsd unwind ssl crypto z; do
    cross_lib_path "$l" >/dev/null || \
      die "対向アーキのライブラリが見つかりません: lib${l}.so (対応する -dev:$APT_CROSS_ARCH / libglvnd0:$APT_CROSS_ARCH を確認するか、--native-only でクロスを省略できます)"
  done
  # 未設定なら multiarch パス向けに -L/-rpath-link を組み立てる
  [[ -z "$CROSS_LIB_FLAGS" ]] && CROSS_LIB_FLAGS="$(cross_link_flags)"
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
      NAME="lunaria-$DIST_DATE-macos-$HOST_ARCH"
      info "ビルド中: $NAME.dmg ..."
      make macos-deps
      build_syslib
      make -j"$JOBS" dist DIST_NAME="$NAME"
      collect "$NAME.dmg"
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
  _CROSS_LPATH="/usr/lib/$CROSS_TRIPLE:/lib/$CROSS_TRIPLE:/usr/$CROSS_TRIPLE/lib"
  [[ -n "$_GCC_CROSS_LIB" ]] && _CROSS_LPATH="$_CROSS_LPATH:$_GCC_CROSS_LIB"
  if [[ -n "$CROSS_SYSROOT" ]]; then
    _CROSS_LPATH="$CROSS_SYSROOT/usr/lib/$CROSS_TRIPLE:$CROSS_SYSROOT/lib/$CROSS_TRIPLE:$_CROSS_LPATH"
    info "クロスリンクに sysroot を使用: $CROSS_SYSROOT"
  fi
  reset_tree
  [[ -z "$CROSS_LIB_FLAGS" ]] && CROSS_LIB_FLAGS="$(cross_link_flags)"
  _MAKE_CROSS_ARGS=(
    dist
    "DIST_NAME=$NAME" "DIST_ARCH=$CROSS_ARCH" "OPENH264_ARCH=$CROSS_OPENH264"
    "CC=$CROSS_TRIPLE-gcc" "CXX=$CROSS_TRIPLE-g++"
    "HOST_CPPFLAGS="
    "HOST_GL_LIBS=$CROSS_LIB_FLAGS -lEGL -lGLESv2"
    "HOST_WINDOW_LIBS=$CROSS_LIB_FLAGS -lglfw"
    "HOST_LIBC_LIBS=$CROSS_LIB_FLAGS -lbsd -lunwind"
    "LDFLAGS=$CROSS_LIB_FLAGS"
    "DYNARMIC_CMAKE_FLAGS=-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=$CROSS_ARCH -DCMAKE_C_COMPILER=$CROSS_TRIPLE-gcc -DCMAKE_CXX_COMPILER=$CROSS_TRIPLE-g++ -DBoost_INCLUDE_DIR=/usr/include -DBoost_NO_BOOST_CMAKE=ON"
  )
  OBJDUMP="$CROSS_TRIPLE-objdump" \
  LIBRARY_PATH="${CROSS_SYSROOT:+$CROSS_SYSROOT/usr/lib/$CROSS_TRIPLE:$CROSS_SYSROOT/lib/$CROSS_TRIPLE:}${LIBRARY_PATH:-}" \
  DIST_LIB_DIRS="$_CROSS_LPATH" \
  make -j"$JOBS" "${_MAKE_CROSS_ARGS[@]}"
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
    NAME="lunaria-$DIST_DATE-windows-x86_64"
    info "ビルド中: $NAME.zip (mingw クロス) ..."
    # Guest AArch64 syslib はホスト非依存。先に作って dist-cross-win で再利用する。
    build_syslib
    make dist-cross-win
    collect "$NAME.zip"
  fi
fi

# ─────────────────────────────────────────────────────────
# ③ macOS: Linux ホストでは作成済み .dmg を取り込むのみ
# ─────────────────────────────────────────────────────────
if [[ "$BUILD_MACOS" == "true" && "$HOST_OS" != "Darwin" ]]; then
  DMG="${MACOS_DMG:-}"
  if [[ -z "$DMG" ]]; then
    DMG="$(ls -t lunaria-*-macos-*.dmg lunaria-*-macos.dmg 2>/dev/null | head -1 || true)"
  fi
  if [[ -n "$DMG" && -f "$DMG" ]]; then
    cp "$DMG" "$ARTIFACTS_DIR/$(basename "$DMG")"
    success "作成済みの macOS dmg を取り込みました: $(basename "$DMG")"
  else
    warn "macOS の .dmg は Linux では作れません (hdiutil/codesign が必要)。"
    warn "Mac で ./_make-release.sh --macos --no-upload を実行して lunaria-*-macos-*.dmg を作り、"
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

### Install (Linux / macOS)

```bash
curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | bash
```

### Linux (manual)
```bash
tar -xzf lunaria-*-linux-x86_64.tar.gz && cd lunaria-*-linux-x86_64   # or -aarch64
./lunaria /path/to/app.apk      # or .xapk / .apks
```
The archive carries its own shim libraries, host libraries and fonts; it uses
the running system's C library.

### macOS (manual)
Open the .dmg, drag `Lunaria.app` where you like, then run
`/Applications/Lunaria.app/Contents/MacOS/Lunaria /path/to/app.apk`.

### Windows
Unzip `lunaria-*-windows-x86_64.zip` and run `lunaria.exe /path/to/app.apk`.
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
