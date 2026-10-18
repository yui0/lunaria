#!/usr/bin/env bash
# Lunaria — Linux / macOS curl+bash installer
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | bash
#   curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | bash -s -- --version 2026.10.15
#   curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | sudo bash -s -- --prefix /usr/local
#   curl -fsSL https://raw.githubusercontent.com/yui0/lunaria/master/install.sh | bash -s -- --uninstall
#
# Options:
#   --version <tag>   Release tag (default: latest)
#   --prefix  <dir>   Install prefix (default: /usr/local as root, else ~/.local)
#   --app-dir <dir>   macOS .app destination (default: /Applications as root, else ~/Applications)
#   --uninstall       Remove the installed binary / app (keep config)
#
# Environment overrides:
#   LUNARIA_VERSION, LUNARIA_PREFIX, LUNARIA_APP_DIR, LUNARIA_REPO
set -euo pipefail

REPO="${LUNARIA_REPO:-yui0/lunaria}"
GH_API="https://api.github.com"
VERSION="${LUNARIA_VERSION:-latest}"
PREFIX="${LUNARIA_PREFIX:-}"
APP_DIR="${LUNARIA_APP_DIR:-}"
DO_UNINSTALL=0
_TMP=""

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --version)    VERSION="${2:?--version requires a value}"; shift 2 ;;
    --version=*)  VERSION="${1#*=}"; shift ;;
    --prefix)     PREFIX="${2:?--prefix requires a value}"; shift 2 ;;
    --prefix=*)   PREFIX="${1#*=}"; shift ;;
    --app-dir)    APP_DIR="${2:?--app-dir requires a value}"; shift 2 ;;
    --app-dir=*)  APP_DIR="${1#*=}"; shift ;;
    --uninstall)  DO_UNINSTALL=1; shift ;;
    -h|--help)    sed -n '2,20p' "$0"; exit 0 ;;
    *) die "Unknown argument: $1" ;;
  esac
done

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "Command not found: $1"
}

detect_os() {
  case "$(uname -s)" in
    Linux*)  echo "linux" ;;
    Darwin*) echo "macos" ;;
    *) die "Unsupported OS: $(uname -s) (supported: Linux / macOS)" ;;
  esac
}

detect_arch() {
  local m
  m="$(uname -m)"
  case "$m" in
    x86_64|amd64)  echo "x86_64" ;;
    aarch64|arm64) echo "aarch64" ;;
    *) die "Unsupported architecture: $m (supported: x86_64 / aarch64)" ;;
  esac
}

# Release asset arch names differ slightly from uname on macOS.
asset_arch() {
  local os="$1" arch="$2"
  if [ "$os" = "macos" ] && [ "$arch" = "aarch64" ]; then
    echo "arm64"
  else
    echo "$arch"
  fi
}

default_prefix() {
  if [ "$(id -u)" -eq 0 ]; then
    echo "/usr/local"
  else
    echo "${HOME}/.local"
  fi
}

default_app_dir() {
  if [ "$(id -u)" -eq 0 ]; then
    echo "/Applications"
  else
    echo "${HOME}/Applications"
  fi
}

ensure_writable() {
  local dir="$1"
  mkdir -p "$dir" 2>/dev/null || true
  if [ ! -d "$dir" ] || [ ! -w "$dir" ]; then
    die "Cannot write to ${dir}. Re-run with sudo, or pass --prefix / --app-dir under your home directory."
  fi
}

download() {
  local url="$1" out="$2"
  if command -v curl >/dev/null 2>&1; then
    curl -fL --proto '=https' --tlsv1.2 --retry 3 -o "$out" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget --https-only -O "$out" "$url"
  else
    die "curl or wget is required"
  fi
}

# Resolve browser_download_url for the matching release asset via GitHub API.
# Asset names look like: lunaria-2026-10-15-linux-x86_64.tar.gz
resolve_asset_url() {
  local os="$1" arch="$2" version="$3"
  local aarch api_url json pattern
  aarch="$(asset_arch "$os" "$arch")"
  if [ "$version" = "latest" ]; then
    api_url="${GH_API}/repos/${REPO}/releases/latest"
  else
    api_url="${GH_API}/repos/${REPO}/releases/tags/${version}"
  fi

  case "$os" in
    linux) pattern="lunaria-.*-linux-${aarch}\\.tar\\.gz" ;;
    macos) pattern="lunaria-.*-macos-${aarch}\\.dmg" ;;
    *) die "Unsupported OS for assets: $os" ;;
  esac

  log "Querying release metadata: ${api_url}"
  json="$(curl -fsSL -H 'Accept: application/vnd.github+json' "$api_url")" \
    || die "Failed to fetch release metadata for ${REPO} (${version})"

  if command -v python3 >/dev/null 2>&1; then
    printf '%s' "$json" | python3 -c "
import json, re, sys
data = json.load(sys.stdin)
pat = re.compile(r'^${pattern}\$')
for a in data.get('assets') or []:
    name = a.get('name') or ''
    if pat.match(name):
        print(a['browser_download_url'])
        print(name)
        sys.exit(0)
sys.exit(1)
" || die "No matching asset for ${os}-${aarch} in release ${version}. See https://github.com/${REPO}/releases"
  elif command -v jq >/dev/null 2>&1; then
    local name url
    name="$(printf '%s' "$json" | jq -r --arg re "^${pattern}\$" '.assets[] | select(.name | test($re)) | .name' | head -n1)"
    url="$(printf '%s' "$json" | jq -r --arg re "^${pattern}\$" '.assets[] | select(.name | test($re)) | .browser_download_url' | head -n1)"
    [ -n "$url" ] && [ "$url" != "null" ] || die "No matching asset for ${os}-${aarch} in release ${version}"
    printf '%s\n%s\n' "$url" "$name"
  else
    die "python3 or jq is required to resolve the release asset"
  fi
}

install_linux() {
  local prefix="$1" archive="$2"
  local share_dir bin_dir stage src

  require_cmd tar
  share_dir="${prefix}/lib/lunaria"
  bin_dir="${prefix}/bin"

  ensure_writable "$prefix"
  mkdir -p "$share_dir" "$bin_dir" "$_TMP/extract"
  tar -xzf "$archive" -C "$_TMP/extract"

  src="$(find "$_TMP/extract" -mindepth 1 -maxdepth 1 -type d -name 'lunaria-*' | head -n1)"
  [ -n "$src" ] || src="$_TMP/extract"
  [ -x "${src}/lunaria" ] || die "lunaria binary not found in the archive"

  log "Installing tree → ${share_dir}"
  # Replace previous install atomically enough for a single-user machine.
  rm -rf "${share_dir}.new"
  mkdir -p "${share_dir}.new"
  cp -a "${src}/." "${share_dir}.new/"
  rm -rf "${share_dir}.old"
  if [ -d "$share_dir" ]; then
    mv "$share_dir" "${share_dir}.old"
  fi
  mv "${share_dir}.new" "$share_dir"
  rm -rf "${share_dir}.old"

  chmod 0755 "${share_dir}/lunaria"
  ln -sfn "${share_dir}/lunaria" "${bin_dir}/lunaria"
  log "Symlink: ${bin_dir}/lunaria → ${share_dir}/lunaria"
  if [ -f "${share_dir}/scripts/lunaria-server.sh" ]; then
    chmod 0755 "${share_dir}/scripts/lunaria-server.sh"
    ln -sfn "${share_dir}/scripts/lunaria-server.sh" "${bin_dir}/lunaria-server"
    log "Symlink: ${bin_dir}/lunaria-server → ${share_dir}/scripts/lunaria-server.sh"
  fi
  if [ -f "${share_dir}/systemd/lunaria@.service" ] && [ -d /etc/systemd/system ]; then
    if [ -w /etc/systemd/system ]; then
      cp -f "${share_dir}/systemd/lunaria@.service" /etc/systemd/system/lunaria@.service
      log "Installed systemd unit: lunaria@.service"
    else
      warn "systemd unit present at ${share_dir}/systemd/lunaria@.service (install as root to enable)"
    fi
  fi

  case ":${PATH}:" in
    *":${bin_dir}:"*) ;;
    *)
      warn "${bin_dir} is not on PATH. Add it, for example:"
      warn "  echo 'export PATH=\"${bin_dir}:\$PATH\"' >> ~/.bashrc"
      ;;
  esac

  cat <<EOF

  Installed : ${share_dir}/lunaria
  Command   : ${bin_dir}/lunaria

  Run:
    lunaria /path/to/app.apk
    lunaria                  # graphical launcher
    lunaria-server 1         # headless launcher over qfkey (needs qberry RDP)

EOF
}

attach_dmg() {
  local dmg="$1" line mp
  # Prefer plist output so the mount point is unambiguous.
  if hdiutil attach -nobrowse -readonly -plist "$dmg" >"$_TMP/attach.plist" 2>/dev/null \
     && command -v python3 >/dev/null 2>&1; then
    mp="$(ATTACH_PLIST="$_TMP/attach.plist" python3 - <<'PY'
import os, plistlib, sys
with open(os.environ["ATTACH_PLIST"], "rb") as f:
    data = plistlib.load(f)
for ent in data.get("system-entities") or []:
    point = ent.get("mount-point")
    if point:
        print(point)
        sys.exit(0)
sys.exit(1)
PY
)" && [ -n "$mp" ] && [ -d "$mp" ] && { printf '%s\n' "$mp"; return; }
  fi
  line="$(hdiutil attach -nobrowse -readonly "$dmg" | tail -n1)" || die "Failed to mount DMG"
  mp="$(printf '%s\n' "$line" | awk '{print $NF}')"
  [ -n "$mp" ] && [ -d "$mp" ] || die "Could not determine DMG mount point"
  printf '%s\n' "$mp"
}

install_macos() {
  local prefix="$1" app_dir="$2" dmg="$3"
  local mount app_src app_dst bin_dir bin_path

  require_cmd hdiutil
  ensure_writable "$app_dir"
  mkdir -p "$app_dir" "$prefix/bin" "$_TMP"

  log "Mounting DMG..."
  mount="$(attach_dmg "$dmg")"
  # shellcheck disable=SC2064
  trap 'hdiutil detach "'"$mount"'" -quiet 2>/dev/null || true; rm -rf "$_TMP"' EXIT

  app_src="$(find "$mount" -maxdepth 2 -type d -name 'Lunaria.app' | head -n1)"
  [ -n "$app_src" ] || die "Lunaria.app not found inside the DMG"
  app_dst="${app_dir}/Lunaria.app"

  log "Installing app → ${app_dst}"
  rm -rf "${app_dst}.new"
  mkdir -p "${app_dst}.new"
  # ditto preserves resource forks / codesign better than cp -R when available.
  if command -v ditto >/dev/null 2>&1; then
    ditto "$app_src" "${app_dst}.new"
  else
    cp -R "$app_src/." "${app_dst}.new/"
  fi
  rm -rf "${app_dst}.old"
  if [ -d "$app_dst" ]; then
    mv "$app_dst" "${app_dst}.old"
  fi
  mv "${app_dst}.new" "$app_dst"
  rm -rf "${app_dst}.old"

  bin_dir="${prefix}/bin"
  bin_path="${app_dst}/Contents/MacOS/Lunaria"
  [ -x "$bin_path" ] || die "Missing executable: ${bin_path}"
  mkdir -p "$bin_dir"
  ln -sfn "$bin_path" "${bin_dir}/lunaria"
  log "Symlink: ${bin_dir}/lunaria → ${bin_path}"

  hdiutil detach "$mount" -quiet 2>/dev/null || true
  trap 'rm -rf "$_TMP"' EXIT

  case ":${PATH}:" in
    *":${bin_dir}:"*) ;;
    *)
      warn "${bin_dir} is not on PATH. Add it, for example:"
      warn "  echo 'export PATH=\"${bin_dir}:\$PATH\"' >> ~/.zshrc"
      ;;
  esac

  cat <<EOF

  App       : ${app_dst}
  Command   : ${bin_dir}/lunaria

  Run:
    lunaria /path/to/app.apk
    open -a Lunaria
    # or: ${app_dst}/Contents/MacOS/Lunaria /path/to/app.apk

EOF
}

uninstall() {
  local os prefix app_dir
  os="$(detect_os)"
  prefix="${PREFIX:-$(default_prefix)}"
  app_dir="${APP_DIR:-$(default_app_dir)}"

  log "Uninstalling Lunaria from ${prefix}"
  rm -f "${prefix}/bin/lunaria"
  if [ "$os" = "macos" ]; then
    rm -rf "${app_dir}/Lunaria.app"
    log "Removed ${app_dir}/Lunaria.app"
  else
    rm -rf "${prefix}/lib/lunaria"
    log "Removed ${prefix}/lib/lunaria"
  fi
  log "Uninstall complete (user config under ~/.config / lunaria.conf was kept)"
}

install_main() {
  require_cmd uname
  require_cmd curl
  require_cmd mktemp
  require_cmd find

  local os arch prefix app_dir url name archive
  os="$(detect_os)"
  arch="$(detect_arch)"
  prefix="${PREFIX:-$(default_prefix)}"
  app_dir="${APP_DIR:-$(default_app_dir)}"

  if [ "$os" = "macos" ] && [ "$arch" = "x86_64" ]; then
    die "No macOS x86_64 release asset is published yet (Apple Silicon only). See https://github.com/${REPO}/releases"
  fi

  log "Target: ${os}/$(asset_arch "$os" "$arch")  version: ${VERSION}  prefix: ${prefix}"

  _TMP="$(mktemp -d "${TMPDIR:-/tmp}/lunaria-install.XXXXXX")"
  trap 'rm -rf "$_TMP"' EXIT

  # resolve_asset_url prints: url\nname
  {
    read -r url
    read -r name
  } < <(resolve_asset_url "$os" "$arch" "$VERSION")

  archive="${_TMP}/${name}"
  log "Downloading 📦 ${name}"
  log "  ${url}"
  download "$url" "$archive"

  if [ "$os" = "macos" ]; then
    install_macos "$prefix" "$app_dir" "$archive"
  else
    install_linux "$prefix" "$archive"
  fi

  log "Install complete 🎉"
}

if [ "$DO_UNINSTALL" = "1" ]; then
  uninstall
else
  install_main
fi
