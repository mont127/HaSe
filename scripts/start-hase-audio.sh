#!/bin/sh
# Start a macOS PulseAudio server for the HaSe Linux VM.
set -eu

PORT="${HASE_PULSE_PORT:-4713}"
PATH="/opt/homebrew/bin:/usr/local/bin:/opt/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

usage() {
  cat <<EOF
Usage:
  scripts/start-hase-audio.sh [--check] [--install] [--stop]

Starts PulseAudio on macOS and opens the native Pulse protocol for the HaSe VM.
The guest uses: PULSE_SERVER=tcp:host.lima.internal:${PORT}

Options:
  --check    Only check whether PulseAudio is running and listening.
  --install  Install PulseAudio first with Homebrew or MacPorts if missing.
  --stop     Unload the HaSe TCP listener module.
  -h, --help
EOF
}

log() {
  printf '[hase-audio] %s\n' "$*"
}

find_pulseaudio() {
  command -v pulseaudio 2>/dev/null || true
}

find_pactl() {
  command -v pactl 2>/dev/null || true
}

install_pulseaudio() {
  if command -v brew >/dev/null 2>&1; then
    log 'installing PulseAudio with Homebrew'
    brew install pulseaudio
    return 0
  fi
  if command -v port >/dev/null 2>&1; then
    log 'installing PulseAudio with MacPorts'
    sudo port install pulseaudio
    return 0
  fi
  log 'error: PulseAudio is missing and neither brew nor port is available'
  return 1
}

module_ids() {
  pactl list modules short 2>/dev/null | awk -v port="port=${PORT}" '
    $2 == "module-native-protocol-tcp" && index($0, port) { print $1 }
  '
}

check_status() {
  pulse="$(find_pulseaudio)"
  ctl="$(find_pactl)"
  if [ -z "$pulse" ] || [ -z "$ctl" ]; then
    log 'error: PulseAudio tools are not installed'
    return 1
  fi
  if ! pulseaudio --check >/dev/null 2>&1; then
    log 'error: PulseAudio daemon is not running'
    return 1
  fi
  ids="$(module_ids | tr '\n' ' ')"
  if [ -z "$ids" ]; then
    log "error: no HaSe TCP listener is loaded on port ${PORT}"
    return 1
  fi
  log "PulseAudio is ready on port ${PORT} (module ${ids})"
  pactl info | sed -n '1,8p'
}

stop_listener() {
  ctl="$(find_pactl)"
  [ -n "$ctl" ] || { log 'error: pactl is not installed'; return 1; }
  ids="$(module_ids || true)"
  if [ -z "$ids" ]; then
    log "no HaSe audio listener loaded on port ${PORT}"
    return 0
  fi
  for id in $ids; do
    pactl unload-module "$id" || true
    log "unloaded PulseAudio module ${id}"
  done
}

install=0
check=0
stop=0
for arg in "$@"; do
  case "$arg" in
    --install) install=1 ;;
    --check) check=1 ;;
    --stop) stop=1 ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done

if [ "$stop" = 1 ]; then
  stop_listener
  exit $?
fi

if [ -z "$(find_pulseaudio)" ] || [ -z "$(find_pactl)" ]; then
  if [ "$install" = 1 ]; then
    install_pulseaudio
  else
    log 'error: PulseAudio is not installed'
    log 'run: scripts/start-hase-audio.sh --install'
    exit 1
  fi
fi

if [ "$check" = 1 ]; then
  check_status
  exit $?
fi

if ! pulseaudio --check >/dev/null 2>&1; then
  log 'starting PulseAudio daemon'
  if ! pulseaudio --start --exit-idle-time=-1 >/tmp/hase-pulseaudio-start.log 2>&1; then
    log 'standard daemon start failed; retrying with explicit daemon mode'
    pulseaudio -D --exit-idle-time=-1 --log-target=file:/tmp/hase-pulseaudio.log
  fi
fi

if [ -z "$(module_ids | head -n1)" ]; then
  acl='auth-ip-acl=127.0.0.1;10.0.0.0/8;172.16.0.0/12;192.168.0.0/16'
  module="$(pactl load-module module-native-protocol-tcp "port=${PORT}" listen=0.0.0.0 "$acl")"
  log "loaded PulseAudio TCP listener module ${module} on port ${PORT}"
else
  log "PulseAudio TCP listener already active on port ${PORT}"
fi

check_status
