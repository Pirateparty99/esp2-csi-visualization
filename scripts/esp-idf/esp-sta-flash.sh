#!/usr/bin/env bash

# ESP-IDF version 4.3 required by the ESP32 CSI Toolkit repo
export IDF_PATH="/home/esp-idf/v4.3.3"

# Set the Python venv version to one compatible for the specific ESP-IDF version (ex: Python 3.9 for ESP-IDF 4.3)
export LEGACY_PYTHON_BIN=python3.9 

# Set the board target for installing the board-specific toolchain(s) with ESP-IDF
export ESP_TARGET=esp32,esp32c6  # multiple targets

source ${IDF_PATH}/export.sh

# cd third_party/esp32-csi-toolkit/passive/
cd third_party/esp32-csi-toolkit/active_sta/
# cd third_party/esp32-csi-toolkit/active_ap/
idf.py flash