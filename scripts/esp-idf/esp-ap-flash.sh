#!/usr/bin/env bash
set -euo pipefail

SKIP_BUILD=0
PORT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-build) SKIP_BUILD=1; shift ;;
        -p|--port)    PORT="${2:-}"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--skip-build] [-p /dev/ttyUSB0]"
            echo "  --skip-build   flash the existing binary instead of rebuilding"
            echo "  -p, --port     serial port to flash. Required when several boards"
            echo "                 are attached, so the wrong one is never flashed."
            exit 0 ;;
        *) echo "Unknown argument: $1 (try --help)" >&2; exit 1 ;;
    esac
done

# With more than one board attached, idf.py's auto-detect silently picks the
# first. Flashing a multi-node deployment that way reflashes one board N times
# and leaves the rest on stale firmware, which then shows up much later as
# nodes that will not associate. Refuse to guess instead.
if [ -z "$PORT" ]; then
    PORTS="$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true)"
    COUNT="$(printf '%s
' "$PORTS" | grep -c . || true)"
    if [ "$COUNT" -gt 1 ]; then
        echo "ERROR: several serial ports detected; specify one with -p:" >&2
        printf '%s
' "$PORTS" | sed 's/^/  /' >&2
        echo "  e.g. $0 -p $(printf '%s
' "$PORTS" | head -n1)" >&2
        exit 1
    fi
    PORT="$(printf '%s
' "$PORTS" | head -n1)"
fi

if [ -z "$PORT" ]; then
    echo "ERROR: no serial port detected. Plug in a board or pass -p." >&2
    exit 1
fi

# ESP-IDF version 4.3 required by the ESP32 CSI Toolkit repo
export IDF_PATH="/home/esp-idf/v4.3.3"

# ESP-IDF version 4.3 required by the ESP32 CSI Toolkit repo
export ESP_IDF_VERSION="v4.3.3"

# Set the Python venv version to one compatible for the specific ESP-IDF version (ex: Python 3.9 for ESP-IDF 4.3)
export LEGACY_PYTHON_BIN=python3.9

# Set the board target for installing the board-specific toolchain(s) with ESP-IDF
export ESP_TARGET=esp32  # multiple targets does not work currently

source "${IDF_PATH}/export.sh"

PROJECT_DIR="third_party/esp32-csi-toolkit/active_ap"
FIRMWARE_BIN="${PROJECT_DIR}/build/active-ap.bin"

if [ "$SKIP_BUILD" = "1" ]; then
    if [ ! -f "$FIRMWARE_BIN" ]; then
        echo "ERROR: --skip-build given but ${FIRMWARE_BIN} doesn't exist." >&2
        echo "Run without --skip-build first (or run the build script)." >&2
        exit 1
    fi
    # Warn (don't block) if the binary looks older than the source it's
    # supposed to reflect - catches "forgot to rebuild after editing" cases.
    if [ "${PROJECT_DIR}/main/main.cc" -nt "$FIRMWARE_BIN" ]; then
        echo "WARNING: main.cc is newer than ${FIRMWARE_BIN} -- the binary" >&2
        echo "         may be stale. Re-run without --skip-build to rebuild first." >&2
    fi
    echo "Skipping build (--skip-build given), using existing ${FIRMWARE_BIN}"
else
    # Always rebuild before flashing, so a stale/previous binary (e.g. left
    # over from before this project's main.cc last compiled successfully)
    # never gets silently reflashed instead of current source.
    ./scripts/esp-idf/esp-csi-ap-build.sh
fi

cd "$PROJECT_DIR"

echo "Flashing from: $(pwd)  ->  $PORT"

idf.py -p "$PORT" flash

echo "Firmware flashed. Below is the board's MAC Address."

printf '=%.0s' {1..100}
echo ""
esptool.py -p "$PORT" read_mac | grep -m 1 "MAC:"
printf '=%.0s' {1..100}
echo ""
