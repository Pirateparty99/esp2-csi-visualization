#!/usr/bin/env bash

# Resolve the script's own directory before we cd elsewhere, so the
# templates/ path stays correct regardless of build function's cwd.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export SCRIPT_DIR

PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
export PROJECT_ROOT

# Optional: LEGACY_PYTHON_BIN — must match whatever Python interpreter was
# used to build the venv during legacy install (defaults to "python3.9",
# matching the install script's default). Old ESP-IDF's export.sh
# re-derives which venv to activate from whatever python3/python resolve
# to on PATH *at the moment it's sourced* — it does not remember what was
# used at install time. So this same interpreter has to be made resolvable
# again here, not just during install.
LEGACY_PYTHON_BIN="${LEGACY_PYTHON_BIN:-python3.9}"
export LEGACY_PYTHON_BIN
PYTHON_SHIM_DIR="${PYTHON_SHIM_DIR:-$HOME/.esp-idf-python-shim}"
export PYTHON_SHIM_DIR

err() { printf '\033[1;31m[esp-build]\033[0m %s\n' "$1" >&2; }
have() { command -v "$1" >/dev/null 2>&1; }
export -f err
export -f have

# Copies templates/sdkconfig.defaults from the project root (two
# directories up from this script), translating it into the format
# ESP-IDF's Kconfig actually reads (CONFIG_ prefix, y/n booleans), then
# clears any stale sdkconfig and runs `idf.py reconfigure` so the new
# defaults take effect. Replaces the old `idf.py menuconfig` step.
copy_sdkconfig_template() {
    local template="${PROJECT_ROOT}/templates/esp32-csi-toolkit/active_sta/sdkconfig.defaults"

    if [ ! -f "$template" ]; then
        err "Template config not found at: $template"
        exit 1
    fi

    rm -f sdkconfig

    grep -v '^[[:space:]]*$' "$template" \
        | sed -E 's/^/CONFIG_/; s/=true$/=y/; s/=false$/=n/' \
        > sdkconfig.defaults

    echo "Generated sdkconfig.defaults from template at $template"

    idf.py reconfigure
}
export -f copy_sdkconfig_template

# Prints a build-success message and how to flash the just-built firmware.
# Exported so it's callable from inside the bash -c subshells below (a
# plain function definition wouldn't be visible to a child bash process
# otherwise).
print_build_success() {
    echo ""
    echo "======================================================"
    echo "Build successful!"
    echo "======================================================"
    echo ""
    echo "Project directory: $(pwd)"
    echo ""

    local ports port uname_s example_port
    ports="$(ls /dev/ttyUSB* /dev/ttyACM* /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART* 2>/dev/null || true)"
    uname_s="$(uname -s 2>/dev/null || echo unknown)"

    if [ -n "$ports" ]; then
        port="$(echo "$ports" | head -n1)"
        echo "Detected device on: $port"
        if [ "$(echo "$ports" | wc -l)" -gt 1 ]; then
            echo "(Other candidates also found — using the first. Full list:)"
            echo "$ports" | sed 's/^/  /'
        fi
        echo ""
        echo "To flash this build, run:"
        echo "  idf.py -p $port flash"
        echo ""
        echo "To flash and open the serial monitor in one step, run:"
        echo "  idf.py -p $port flash monitor"
    else
        echo "No device detected on any serial port."
        echo "Plug in your ESP32 and check again with:"
        if [ "$uname_s" = "Darwin" ]; then
            example_port="/dev/cu.usbserial-1420"
            echo "  ls /dev/cu.*"
        else
            example_port="/dev/ttyUSB0"
            echo "  ls /dev/ttyUSB* /dev/ttyACM*"
        fi
        echo ""
        echo "Once connected, it will typically show up as something like:"
        echo "  $example_port"
        echo ""
        echo "Then flash with:"
        echo "  idf.py -p $example_port flash"
    fi
    echo ""
}
export -f print_build_success

# EIM-installed ESP-IDF build function
eim-esp-build() {
    bash -c '
        set -euo pipefail

        activate_script="$HOME/.espressif/tools/activate_idf_${ESP_IDF_VERSION}.sh"
        if [ ! -f "$activate_script" ]; then
            echo "Activation script not found: $activate_script" >&2
            exit 1
        fi

        source "$activate_script"

        # Fix a known typo in the generated PATH entries (missing "/"
        # between "$HOME" and ".espressif").
        export PATH="$(printf "%s" "$PATH" | sed -E "s#([^/])\.espressif/#\1/.espressif/#g")"

        echo "Configuring ESP-IDF to connect to ESP32"

        copy_sdkconfig_template
        idf.py set-target esp32

        # Build the firmware
        echo "Building firmware"
        idf.py build

        print_build_success
    ' bash
}

# Legacy (<5.0) ESP-IDF build function
legacy-esp-build () {
    # legacy export.sh needs IDF_PATH set — derive it from ESP_PATH/ESP_IDF_VERSION
    # (matching the layout the install script produces) unless it's already
    # been set explicitly. Using ${VAR:-} throughout so this stays safe under
    # `set -u` even when ESP_PATH/IDF_PATH aren't set at all.
    if [ -z "${IDF_PATH:-}" ]; then
        if [ -z "${ESP_PATH:-}" ]; then
            err "Neither IDF_PATH nor ESP_PATH is set. Set one of them (or export"
            err "IDF_PATH directly) before running legacy-esp-build."
            exit 1
        fi
        export IDF_PATH="${ESP_PATH}/${ESP_IDF_VERSION}"
    fi

    if [ ! -f "${IDF_PATH}/export.sh" ]; then
        err "export.sh not found at '${IDF_PATH}/export.sh'."
        err "Check that ESP_PATH and ESP_IDF_VERSION are set correctly, or set IDF_PATH directly."
        exit 1
    fi

    bash -c '
        set -euo pipefail

        # Re-apply the same persistent Python shim used during install (see
        # esp-build.sh). export.sh derives which venv to activate from
        # whatever python3/python resolve to right now, and the venv itself
        # only works when invoked via the exact path it was created
        # through — so this must be the SAME stable shim directory used at
        # install time, not a fresh throwaway one.
        if [ "${LEGACY_PYTHON_BIN}" != "python3" ]; then
            shim_dir="${PYTHON_SHIM_DIR:-$HOME/.esp-idf-python-shim}"
            if [ ! -e "${shim_dir}/python3" ]; then
                echo "Expected Python shim not found at ${shim_dir}/python3." >&2
                echo "Re-run the install script first so it can be created." >&2
                exit 1
            fi
            export PATH="${shim_dir}:${PATH}"
        fi

        source "${IDF_PATH}/export.sh"

        echo "Configuring ESP-IDF to connect to ESP32"

        idf.py set-target esp32
        copy_sdkconfig_template

        # Build the firmware
        echo "Building firmware"
        idf.py build

        print_build_success
    ' bash
}

# Function for grabbing the ESP-IDF major release version for version check
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

# Copies our modified template files over the freshly-cloned third-party
# toolkit repo. Runs every build (not just on first clone) so re-running
# the script always re-applies the overlay, even after a `git pull` or
# re-clone of third_party/esp32-csi-toolkit.
apply_toolkit_overlay() {
    local template_root="${1}"
    local repo_root="${2}"

    local files=(
        "_components/csi_component.h"
        "_components/csi_udp_sender.h"
        "active_sta/main/main.cc"
        "active_sta/main/Kconfig.projbuild"
    )

    for rel_path in "${files[@]}"; do
        local src="${template_root}/esp32-csi-toolkit/${rel_path}"
        local dest="${repo_root}/${rel_path}"

        if [ ! -f "$src" ]; then
            err "Overlay template not found: $src"
            exit 1
        fi

        mkdir -p "$(dirname "$dest")"
        cp "$src" "$dest"
        echo "Applied overlay: ${rel_path}"
    done
}
export -f apply_toolkit_overlay

main() {
    echo "Sourcing ESP-IDF version ${ESP_IDF_VERSION}"

    set -euo pipefail

    local idf_major
    idf_major="$(get_idf_major_version "${ESP_IDF_VERSION}")"

    # ESP-CSI Toolkit firmware build script

   # Clone repo if it does not exist
    if [ ! -d "third_party/esp32-csi-toolkit" ]; then
        echo "Cloning the ESP32 CSI Toolkit repo"
        git clone https://github.com/StevenMHernandez/ESP32-CSI-Tool "third_party/esp32-csi-toolkit"
    else
        echo "ESP32 CSI Toolkit repo already cloned, continuing"
    fi

    # Apply overlay every run, so template edits always take effect
    # without needing to delete/re-clone third_party/esp32-csi-toolkit.
    apply_toolkit_overlay "${PROJECT_ROOT}/templates" "third_party/esp32-csi-toolkit"

    cd "third_party/esp32-csi-toolkit/active_sta" # UDP forwarding requires a real STA connection with an IP

    # Run ESP build based on installed ESP-IDF version
    if [ "$idf_major" -ge 5 ]; then
        eim-esp-build
    else
        legacy-esp-build
    fi
}

main "$@"