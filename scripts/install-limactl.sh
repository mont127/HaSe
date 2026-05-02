#!/usr/bin/env sh
set -eu

usage() {
    cat <<'EOF'
Usage:
  scripts/install-limactl.sh [--check]

Installs Lima, which provides limactl, for the HaSe Linux VM prototype.

Options:
  --check   Only check whether limactl is available; do not install.
  -h, --help

Supported install paths on macOS:
  - Homebrew: brew install lima
  - MacPorts: sudo port install lima
EOF
}

log() {
    printf '[install-limactl] %s\n' "$*"
}

fail() {
    printf '[install-limactl] error: %s\n' "$*" >&2
    exit 1
}

have() {
    command -v "$1" >/dev/null 2>&1
}

find_limactl() {
    if have limactl; then
        command -v limactl
        return 0
    fi
    for candidate in /opt/homebrew/bin/limactl /usr/local/bin/limactl; do
        if [ -x "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

print_limactl_version() {
    limactl_bin="$(find_limactl)" || return 1
    log "found limactl at ${limactl_bin}"
    "$limactl_bin" --version
}

check_only=0
case "${1:-}" in
    --check)
        check_only=1
        shift
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    "")
        ;;
    *)
        usage >&2
        fail "unknown argument: $1"
        ;;
esac

if [ "$#" -ne 0 ]; then
    usage >&2
    fail "too many arguments"
fi

if print_limactl_version; then
    exit 0
fi

if [ "$check_only" -eq 1 ]; then
    fail "limactl is not installed"
fi

os_name="$(uname -s)"
if [ "$os_name" != "Darwin" ]; then
    fail "automatic install is only implemented for macOS; install Lima for ${os_name} manually"
fi

if have brew; then
    log "installing Lima with Homebrew"
    brew install lima
elif have port; then
    log "installing Lima with MacPorts"
    sudo port install lima
else
    cat >&2 <<'EOF'
[install-limactl] no supported macOS package manager was found.

Install Homebrew, then rerun this script:
  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
  scripts/install-limactl.sh

Or install Lima manually from:
  https://lima-vm.io/
EOF
    exit 1
fi

if ! print_limactl_version; then
    fail "Lima install completed, but limactl is still not on PATH"
fi

log "limactl is ready for HaSe"
