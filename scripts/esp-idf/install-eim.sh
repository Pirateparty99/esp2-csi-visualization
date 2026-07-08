#!/usr/bin/env bash
#
# install_eim.sh
#
# Installs the Espressif Installation Manager (EIM) using the appropriate
# native package manager for the detected OS:
#   - macOS / Linux : Homebrew (brew)
#   - Debian/Ubuntu : APT
#   - Fedora/RHEL   : DNF
#   - Windows       : WinGet (via powershell.exe/winget.exe, e.g. from Git Bash/WSL-with-interop)
#
# Usage:
#   ./install_eim.sh            # installs the CLI-only version (default)
#   ./install_eim.sh --gui      # installs the GUI (includes CLI)
#
# Docs: https://docs.espressif.com/projects/idf-im-ui/en/latest/

set -euo pipefail

INSTALL_GUI=false
for arg in "$@"; do
  case "$arg" in
    --gui) INSTALL_GUI=true ;;
    -h|--help)
      echo "Usage: $0 [--gui]"
      echo "  --gui   Install the EIM GUI application (includes the CLI)."
      echo "          Default installs the CLI-only package."
      exit 0
      ;;
  esac
done

log()  { printf '\033[1;34m[install_eim]\033[0m %s\n' "$1"; }
err()  { printf '\033[1;31m[install_eim]\033[0m %s\n' "$1" >&2; }
have() { command -v "$1" >/dev/null 2>&1; }

# Normalizes `uname -m` output to one of: x86_64, arm64, unsupported.
# EIM's Linux repos (APT/DNF/Pacman) only publish x86_64 and arm64/aarch64 builds.
detect_linux_arch() {
  local raw_arch normalized
  raw_arch="$(uname -m)"
  case "$raw_arch" in
    x86_64|amd64)
      normalized="x86_64"
      ;;
    aarch64|arm64)
      normalized="arm64"
      ;;
    *)
      normalized="unsupported"
      ;;
  esac
  echo "$normalized"
}

install_with_brew() {
  log "Installing via Homebrew..."
  brew tap espressif/eim
  if [ "$INSTALL_GUI" = true ]; then
    brew install --cask eim-gui
  else
    brew install eim
  fi
}

install_with_apt() {
  log "Installing via APT..."
  echo "deb [trusted=yes] https://dl.espressif.com/dl/eim/apt/ stable main" \
    | sudo tee /etc/apt/sources.list.d/espressif.list >/dev/null
  sudo apt update
  if [ "$INSTALL_GUI" = true ]; then
    sudo apt install -y eim
  else
    sudo apt install -y eim-cli
  fi
}

install_with_dnf() {
  log "Installing via DNF..."
  sudo dnf install -y https://dl.espressif.com/dl/eim/rpm/eim-repo-latest.noarch.rpm
  if [ "$INSTALL_GUI" = true ]; then
    sudo dnf install -y eim
  else
    sudo dnf install -y eim-cli
  fi
}

install_with_pacman() {
  local arch="$1"
  log "Installing via Pacman (arch: $arch)..."
  if ! grep -q '\[eim\]' /etc/pacman.conf; then
    sudo tee -a /etc/pacman.conf <<'EOF'
[eim]
SigLevel = Optional TrustAll
Server = https://dl.espressif.com/dl/eim/pacman/$arch
EOF
  fi
  sudo pacman -Sy
  if [ "$INSTALL_GUI" = true ]; then
    sudo pacman -S --noconfirm eim
  else
    sudo pacman -S --noconfirm eim-cli
  fi
}

install_with_winget() {
  local winget_cmd="$1"
  log "Installing via WinGet..."
  if [ "$INSTALL_GUI" = true ]; then
    "$winget_cmd" install --id Espressif.EIM -e
  else
    "$winget_cmd" install --id Espressif.EIM-CLI -e
  fi
}

main() {
  local uname_s
  uname_s="$(uname -s 2>/dev/null || echo unknown)"

  case "$uname_s" in
    Darwin)
      if have brew; then
        install_with_brew
      else
        err "Homebrew not found. Install it from https://brew.sh and re-run this script."
        exit 1
      fi
      ;;
    Linux)
      local arch raw_arch
      raw_arch="$(uname -m)"
      arch="$(detect_linux_arch)"
      log "Detected Linux architecture: $raw_arch (normalized: $arch)"

      if [ "$arch" = "unsupported" ]; then
        err "Architecture '$raw_arch' is not supported by EIM's Linux packages."
        err "EIM's APT/DNF/Pacman repos only publish x86_64 and arm64/aarch64 builds."
        err "See https://docs.espressif.com/projects/idf-im-ui/en/latest/ for manual/offline install options."
        exit 1
      fi

      # Prefer the distro-native manager; fall back to Homebrew if available.
      if have apt-get || have apt; then
        install_with_apt
      elif have dnf; then
        install_with_dnf
      elif have pacman; then
        install_with_pacman "$arch"
      elif have brew; then
        install_with_brew
      else
        err "No supported package manager found (apt, dnf, pacman, or brew)."
        err "See https://docs.espressif.com/projects/idf-im-ui/en/latest/ for manual install options."
        exit 1
      fi
      ;;
    MINGW*|MSYS*|CYGWIN*)
      # Running inside Git Bash / MSYS on Windows.
      if have winget.exe; then
        install_with_winget winget.exe
      elif have winget; then
        install_with_winget winget
      else
        err "WinGet not found. Install 'App Installer' from the Microsoft Store, then re-run."
        exit 1
      fi
      ;;
    *)
      err "Unsupported or undetected OS: $uname_s"
      err "Please install EIM manually: https://docs.espressif.com/projects/idf-im-ui/en/latest/"
      exit 1
      ;;
  esac

  log "Done. Verify with: eim --help  (or launch the EIM GUI if installed)"
}

main "$@"