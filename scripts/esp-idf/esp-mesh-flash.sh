#!/usr/bin/env bash
set -euo pipefail

SKIP_BUILD=0
if [ "${1:-}" = "--skip-build" ]; then
    SKIP_BUILD=1
fi

# ESP-IDF version 4.3 required by the ESP32 CSI Toolkit repo
export IDF_PATH="/home/esp-idf/v4.3.3"

# Set the Python venv version to one compatible for the specific ESP-IDF version (ex: Python 3.9 for ESP-IDF 4.3)
export LEGACY_PYTHON_BIN=python3.9

# Set the board target for installing the board-specific toolchain(s) with ESP-IDF
export ESP_TARGET=esp32,esp32c3  # multiple targets

# cd third_party/esp32-csi-toolkit/wifi-mesh/

echo "Flashing from: $(pwd)"

if [ "$SKIP_BUILD" = "1" ]; then
    if [ ! -f build/wifi-mesh.bin ]; then
        echo "ERROR: --skip-build given but build/wifi-mesh.bin doesn't exist." >&2
        echo "Run without --skip-build first (or run the build script)." >&2
        exit 1
    fi
    # Warn (don't block) if the binary looks older than the source it's
    # supposed to reflect - catches "forgot to rebuild after editing" cases.
    if [ main/main.cc -nt build/wifi-mesh.bin ]; then
        echo "WARNING: main.cc is newer than build/wifi-mesh.bin -- the binary" >&2
        echo "         may be stale. Re-run without --skip-build to rebuild first." >&2
    fi
    echo "Skipping build (--skip-build given), using existing build/wifi-mesh.bin"
else
    # Always rebuild before flashing, so a stale/previous binary (e.g. left
    # over from before this project's main.cc last compiled successfully)
    # never gets silently reflashed instead of current source.
    scripts/esp-idf/esp-csi-mesh-build.sh
fi

source "${IDF_PATH}/export.sh"


idf.py flash

echo "Firmware flashed. Below is the board's MAC Address."

printf '=%.0s' {1..100}
echo ""
esptool.py read_mac | grep -m 1 "MAC:"
printf '=%.0s' {1..100}