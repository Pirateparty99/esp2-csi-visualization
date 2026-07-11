#!/usr/bin/env bash
set -euo pipefail

# Export vars to set ESP-IDF verison and installation path
export ESP_PATH="/opt/esp-idf"

# ESP-IDF version 4.3 required by the ESP32 CSI Toolkit repo
export ESP_IDF_VERSION="v4.3.3"

# Set the Python venv version to one compatible for the specific ESP-IDF version (ex: Python 3.9 for ESP-IDF 4.3)
export LEGACY_PYTHON_BIN=python3.9 

# Set the board target for installing the board-specific toolchain(s) with ESP-IDF
export ESP_TARGET=esp32,esp32c3  # multiple targets


# Install the ESP-IDF if it is not found locally
if [ ! -d $ESP_PATH/$ESP_IDF_VERSION  ]; then
    echo "ESP-IDF not found, installing the ESP-IDF before building ESP32 firmware"
    bash scripts/esp-idf/esp-idf-setup.sh
else
    echo "ESP-IDF version ${ESP_IDF_VERSION} found, continuing"
fi

echo "Building ESP32 CSI Toolkit Active_STA firmware..."
bash scripts/esp-idf/esp-csi-sta-build.sh

echo "Done."