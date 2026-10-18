#!/usr/bin/env bash
# Headless lunaria instance for a shared qberry (qfkey) RDP host.
#
# Usage:
#   lunaria-server.sh [instance-id] [-- lunaria-args...]
#   LUNARIA_DATA_ROOT=/var/lib/lunaria/1 lunaria-server.sh 1
#
# Requires a running qberry with rdp_modes != 0 (lunaria bridge on the
# qfkey-lunaria socket).  Pass --start-qberry to launch one in the background
# when the socket is missing.
set -euo pipefail

INSTANCE="${1:-default}"
if [[ "${1:-}" == --* ]]; then
  INSTANCE=default
else
  shift || true
fi

START_QBERRY=0
LUNARIA_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --start-qberry) START_QBERRY=1; shift ;;
    --) shift; LUNARIA_ARGS+=("$@"); break ;;
    *) LUNARIA_ARGS+=("$1"); shift ;;
  esac
done

log()  { printf '==> %s\n' "$*" >&2; }
warn() { printf '[!] %s\n' "$*" >&2; }
die()  { printf '[x] %s\n' "$*" >&2; exit 1; }

sock_path() {
  if [[ -n "${LUNARIA_QFKEY:-}" && "${LUNARIA_QFKEY}" == /* ]]; then
    printf '%s\n' "$LUNARIA_QFKEY"
  else
    printf '/tmp/qfkey-lunaria.sock\n'
  fi
}

qfkey_up() {
  local sock
  sock="$(sock_path)"
  [[ -S "$sock" ]] || return 1
  # A live listener accepts a connect (and we drop it immediately).
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$sock" <<'PY' 2>/dev/null
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(0.5)
try:
    s.connect(sys.argv[1])
except Exception:
    sys.exit(1)
finally:
    s.close()
PY
  else
    # Best-effort: socket inode exists.
    return 0
  fi
}

maybe_start_qberry() {
  qfkey_up && return 0
  [[ "$START_QBERRY" -eq 1 ]] || {
    warn "qfkey socket not ready: $(sock_path)"
    warn "Start qberry with RDP (e.g. qberry-cli -b --rdp-modes 1), or pass --start-qberry."
    return 1
  }
  local bin=""
  for c in qberry-cli qberry qkey; do
    if command -v "$c" >/dev/null 2>&1; then bin="$c"; break; fi
  done
  [[ -n "$bin" ]] || die "qberry-cli not found on PATH"
  log "starting $bin -b --rdp-modes 1"
  "$bin" -b --rdp-modes 1 </dev/null >/tmp/qberry-lunaria-server.log 2>&1 &
  local i=0
  while (( i < 50 )); do
    qfkey_up && return 0
    sleep 0.1
    i=$((i + 1))
  done
  die "qberry started but qfkey socket did not appear ($(sock_path)); see /tmp/qberry-lunaria-server.log"
}

resolve_lunaria() {
  if [[ -n "${LUNARIA_BIN:-}" && -x "$LUNARIA_BIN" ]]; then
    printf '%s\n' "$LUNARIA_BIN"; return
  fi
  if command -v lunaria >/dev/null 2>&1; then
    command -v lunaria; return
  fi
  local here
  here="$(cd "$(dirname "$0")/.." && pwd)"
  if [[ -x "$here/lunaria" ]]; then
    printf '%s\n' "$here/lunaria"; return
  fi
  die "lunaria binary not found (set LUNARIA_BIN or add lunaria to PATH)"
}

maybe_start_qberry || true

export LUNARIA_PBUFFER="${LUNARIA_PBUFFER:-1}"
export LUNARIA_INSTANCE="${LUNARIA_INSTANCE:-$INSTANCE}"
if [[ -z "${LUNARIA_DATA_ROOT:-}" ]]; then
  if [[ "$(id -u)" -eq 0 ]]; then
    export LUNARIA_DATA_ROOT="/var/lib/lunaria/${INSTANCE}"
  else
    export LUNARIA_DATA_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}/lunaria/${INSTANCE}"
  fi
fi
mkdir -p "$LUNARIA_DATA_ROOT"

BIN="$(resolve_lunaria)"
log "instance=${LUNARIA_INSTANCE} data=${LUNARIA_DATA_ROOT} pbuffer=${LUNARIA_PBUFFER}"
log "exec: $BIN ${LUNARIA_ARGS[*]:-}"
# No APK argument → graphical (headless) launcher waits for a package.
cd "$LUNARIA_DATA_ROOT"
exec "$BIN" "${LUNARIA_ARGS[@]}"
