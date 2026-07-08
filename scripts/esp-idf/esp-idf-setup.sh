#!/usr/bin/env bash
set -euo pipefail

# Requires: ESP_PATH, ESP_IDF_VERSION (e.g. "v6.0.2" or "v4.3") to be set in the environment.
# Optional: ESP_TARGET (e.g. "esp32", "esp32,esp32s3") — chip target(s) to install
#           tools for. Defaults to "all" if unset, which installs tools for
#           every supported target.
# Optional: LEGACY_PYTHON_BIN — path/name of the Python interpreter to use
#           for the legacy (<5.0) install.sh flow. Defaults to "python3.9",
#           since old ESP-IDF versions pin dependency versions from their
#           era and often fail to build under modern Python (e.g. 3.12
#           removed distutils, which many old pinned packages' setup.py
#           relies on). If it isn't already installed, the script will try
#           to install it via the OS package manager.
ESP_TARGET="${ESP_TARGET:-all}"
LEGACY_PYTHON_BIN="${LEGACY_PYTHON_BIN:-python3.9}"

log() { printf '\033[1;34m[esp-build]\033[0m %s\n' "$1"; }
err() { printf '\033[1;31m[esp-build]\033[0m %s\n' "$1" >&2; }
have() { command -v "$1" >/dev/null 2>&1; }

# A venv records the exact path used to invoke it as its own interpreter
# symlink target — NOT the fully-resolved real binary underneath. So the
# python3/python shim used to bootstrap install.sh must live at a STABLE
# path that's never deleted; otherwise the venv created through it breaks
# permanently once that path goes away (its bin/python becomes a dangling
# symlink). This directory is created once and left in place indefinitely.
PYTHON_SHIM_DIR="${PYTHON_SHIM_DIR:-$HOME/.esp-idf-python-shim}"

# Ensures PYTHON_SHIM_DIR exists with python3/python symlinks pointing at
# LEGACY_PYTHON_BIN, and prints its path. Idempotent — safe to call
# repeatedly; only rewrites the symlinks if they're missing or stale.
ensure_python_shim() {
  local resolved_python
  resolved_python="$(command -v "${LEGACY_PYTHON_BIN}")" || {
    err "LEGACY_PYTHON_BIN '${LEGACY_PYTHON_BIN}' not found on PATH"
    exit 1
  }

  mkdir -p "${PYTHON_SHIM_DIR}"
  ln -sf "${resolved_python}" "${PYTHON_SHIM_DIR}/python3"
  ln -sf "${resolved_python}" "${PYTHON_SHIM_DIR}/python"
  echo "${PYTHON_SHIM_DIR}"
}

# Ensures LEGACY_PYTHON_BIN is installed, installing it via the detected OS
# package manager if it's missing. Currently only knows how to install
# python3.9 specifically (apt: deadsnakes PPA, dnf: package name
# python3.9, brew: python@3.9) — if LEGACY_PYTHON_BIN is set to something
# else and it's missing, this just errors out with instructions rather
# than guessing.
ensure_legacy_python() {
  if have "${LEGACY_PYTHON_BIN}"; then
    log "${LEGACY_PYTHON_BIN} already installed: $(command -v "${LEGACY_PYTHON_BIN}")"
    return 0
  fi

  if [ "${LEGACY_PYTHON_BIN}" != "python3.9" ]; then
    err "LEGACY_PYTHON_BIN '${LEGACY_PYTHON_BIN}' not found on PATH, and auto-install"
    err "is only implemented for python3.9. Install '${LEGACY_PYTHON_BIN}' manually and re-run."
    exit 1
  fi

  log "python3.9 not found — attempting to install it"

  local uname_s
  uname_s="$(uname -s 2>/dev/null || echo unknown)"

  case "$uname_s" in
    Linux)
      if have apt-get || have apt; then
        log "Installing python3.9 via APT (deadsnakes PPA)"
        sudo apt-get update
        sudo apt-get install -y software-properties-common
        sudo add-apt-repository -y ppa:deadsnakes/ppa
        sudo apt-get update
        sudo apt-get install -y python3.9 python3.9-venv python3.9-distutils
      elif have dnf; then
        log "Installing python3.9 via DNF"
        sudo dnf install -y python3.9
      elif have pacman; then
        err "Auto-install of python3.9 via pacman is not supported (not in official repos)."
        err "Install it manually, e.g. via an AUR helper (python39), and re-run."
        exit 1
      else
        err "No supported package manager found (apt, dnf, or pacman)."
        exit 1
      fi
      ;;
    Darwin)
      if have brew; then
        log "Installing python3.9 via Homebrew"
        brew install python@3.9
      else
        err "Homebrew not found. Install it from https://brew.sh and re-run,"
        err "or install python3.9 manually."
        exit 1
      fi
      ;;
    *)
      err "Unsupported or undetected OS: $uname_s. Install python3.9 manually and re-run."
      exit 1
      ;;
  esac

  if ! have "${LEGACY_PYTHON_BIN}"; then
    err "python3.9 installation appeared to succeed, but '${LEGACY_PYTHON_BIN}' still isn't on PATH."
    exit 1
  fi
  log "python3.9 installed: $(command -v "${LEGACY_PYTHON_BIN}")"
}

# Parses the major version number out of ESP_IDF_VERSION (handles "v6.0.2",
# "6.0.2", "v5", etc.) and prints it. Exits with an error if unparseable.
get_idf_major_version() {
  local version="$1" major
  major="${version#v}"
  major="${major%%.*}"
  if ! [[ "$major" =~ ^[0-9]+$ ]]; then
    err "Could not parse major version from ESP_IDF_VERSION='${version}'"
    exit 1
  fi
  echo "$major"
}

# Ensures the base ESP_PATH directory exists and is owned by the current
# user. Deliberately does NOT create the version-specific subdirectory —
# both install functions build into /tmp first (always user-writable,
# avoiding root-owned-directory permission clashes with unprivileged
# git/pip/etc.), then copy the finished result into ESP_PATH at the end.
ensure_base_dir() {
  sudo mkdir -p "${ESP_PATH}"
  sudo chown "${USER}:${USER}" "${ESP_PATH}"
}

# ---------------------------------------------------------------------------
# Modern path (>=5.0): install via EIM
# ---------------------------------------------------------------------------
install_with_eim() {
  log "ESP-IDF ${ESP_IDF_VERSION} >= v5.0 — installing via EIM (target: ${ESP_TARGET})"

  log "Installing EIM"
  sudo bash ./scripts/esp-idf/install-eim.sh

  log "Installing ESP-IDF ${ESP_IDF_VERSION} with EIM"
  eim install -i "${ESP_IDF_VERSION}" -p /tmp/esp-idf -t "${ESP_TARGET}"

  log "Copying installed ESP-IDF into ${ESP_PATH}"
  sudo cp -r "/tmp/esp-idf/${ESP_IDF_VERSION}" "${ESP_PATH}/${ESP_IDF_VERSION}"
  rm -rf "/tmp/esp-idf/${ESP_IDF_VERSION}"

  # Set ownership of ESP-IDF installation dir to current user, now that
  # the copy is complete
  sudo chown -R "${USER}:${USER}" "${ESP_PATH}/${ESP_IDF_VERSION}"

  # Re-apply execute permissions after chown
  sudo chmod -R +x scripts/esp-idf/

  # Copy the espressif folder from root's home to the current user's home
  # echo "Copying espressif folder to ${USER}'s home"
  # sudo cp -r /root/.espressif/ ~/

  activate_eim_env

  # Install ESP-IDF tools
  echo "Installing ESP-IDF tools"
  ${ESP_PATH}/${ESP_IDF_VERSION}/install.sh
  
}

# Sources the eim-generated activate_idf_*.sh script and runs a smoke test.
# NOTE on the two workarounds below:
# 1. `bash -c '...' bash` fakes $0 to "bash" so the script's own
#    is_sourced() check (which only allows $0 to look like a shell name)
#    passes correctly, even though we're sourcing it from inside this
#    non-shell-named wrapper script.
# 2. The sed fixes a known typo in the generated PATH entries, which
#    concatenate "$HOME" + ".espressif" without a "/" in between
#    (e.g. ".../user1.espressif/tools/..." instead of
#    ".../user1/.espressif/tools/...").
# Keep any idf.py/build commands INSIDE this same bash -c block --
# the PATH/function setup only exists within this subshell.
activate_eim_env() {
  log "Activating the ESP-IDF ${ESP_IDF_VERSION} virtual environment"

  local activate_script="$HOME/.espressif/tools/activate_idf_${ESP_IDF_VERSION}.sh"
  if [ ! -f "$activate_script" ]; then
    err "Activation script not found: $activate_script"
    exit 1
  fi

  bash -c '
    source "$1"
    export PATH="$(printf "%s" "$PATH" | sed -E "s#([^/])\.espressif/#\1/.espressif/#g")"
    idf.py --version
  ' bash "$activate_script"
}

# ---------------------------------------------------------------------------
# Legacy path (<5.0): eim is not supported for this version, so clone and
# bootstrap ESP-IDF directly with its own install.sh / export.sh scripts.
# ---------------------------------------------------------------------------
install_with_legacy() {
  log "ESP-IDF ${ESP_IDF_VERSION} < v5.0 — EIM does not support this version; using legacy install (target: ${ESP_TARGET})"

  ensure_legacy_python

  local idf_tmp_dir="/tmp/esp-idf-legacy/${ESP_IDF_VERSION}"
  local idf_final_dir="${ESP_PATH}/${ESP_IDF_VERSION}"

  mkdir -p "$(dirname "$idf_tmp_dir")"

  if [ -d "${idf_tmp_dir}/.git" ]; then
    log "Existing clone found at ${idf_tmp_dir}, skipping clone"
  else
    log "Cloning ESP-IDF ${ESP_IDF_VERSION}"
    git clone -b "${ESP_IDF_VERSION}" --recursive \
      https://github.com/espressif/esp-idf.git "${idf_tmp_dir}"
  fi

  run_legacy_install_sh "$idf_tmp_dir"

  log "Copying installed ESP-IDF into ${ESP_PATH}"
  sudo cp -r "${idf_tmp_dir}" "${idf_final_dir}"
  rm -rf "${idf_tmp_dir}"

  # Set ownership of ESP-IDF installation dir to current user, now that
  # the copy is complete
  sudo chown -R "${USER}:${USER}" "${idf_final_dir}"

  activate_legacy_env "$idf_final_dir"

  # Install ESP-IDF tools
  echo "Installing ESP-IDF tools"
  ${ESP_PATH}/${ESP_IDF_VERSION}/install.sh

}

# Runs the legacy install.sh, optionally forcing a specific Python
# interpreter. install.sh detects its interpreter by running `which
# python3`/`which python` — there's no clean env-var override for this in
# old ESP-IDF versions. So if LEGACY_PYTHON_BIN differs from the default,
# build a small shim directory with python3/python symlinks pointing at
# it, and put that at the front of PATH for just this command.
run_legacy_install_sh() {
  local idf_tmp_dir="$1"

  log "Running legacy install.sh (python: ${LEGACY_PYTHON_BIN})"
  (
    cd "$idf_tmp_dir"

    if [ "${LEGACY_PYTHON_BIN}" != "python3" ]; then
      shim_dir="$(ensure_python_shim)"
      export PATH="${shim_dir}:${PATH}"
      log "Using $(command -v "${LEGACY_PYTHON_BIN}") for install.sh (via persistent PATH shim at ${shim_dir})"
    fi

    ./install.sh "${ESP_TARGET}"
  )
}

# Sources the legacy export.sh script and runs a smoke test.
activate_legacy_env() {
  local idf_final_dir="$1"
  local export_script="${idf_final_dir}/export.sh"

  log "Activating the ESP-IDF ${ESP_IDF_VERSION} virtual environment (legacy export.sh)"

  if [ ! -f "$export_script" ]; then
    err "Legacy export.sh not found: $export_script"
    exit 1
  fi

  bash -c '
    if [ -d "$2" ]; then
      export PATH="$2:$PATH"
    fi
    source "$1"
    idf.py --version
  ' bash "$export_script" "$PYTHON_SHIM_DIR"
}

main() {
  local idf_major
  idf_major="$(get_idf_major_version "${ESP_IDF_VERSION}")"

  # Add execute permissions to ESP-IDF installation scripts
  sudo chmod -R +x scripts/esp-idf/

  ensure_base_dir

  if [ "$idf_major" -ge 5 ]; then
    install_with_eim
  else
    install_with_legacy
  fi

  log "Done."
}

main "$@"