# Firmware: building, flashing, configuration

Before running any visualization the ESPs need to be flashed with the CSI-emmitting firmware.

1. Configure AP/STA settings in the relevant `templates/esp32-csi-toolkit/<project>/sdkconfig.defaults`
2. Build/flash ESP32 firmware

## Build & flash

Each mode has a matching build and flash script. The flash scripts rebuild
first by default, so a stale binary is never silently reflashed; pass
`--skip-build` to flash the existing one.

```bash
./scripts/esp-idf/esp-sta-flash.sh                 # active_sta (rebuild + flash)
./scripts/esp-idf/esp-sta-flash.sh --skip-build    # flash existing binary
./scripts/esp-idf/esp-ap-flash.sh                  # active_ap
./scripts/esp-idf/esp-mesh-flash.sh                # wifi-mesh
```

Each prints the board's MAC address on completion — that value is what appears
as `node` in the JSON, and what goes in the config file.

Templates in `templates/esp32-csi-toolkit/` are copied over the upstream
`third_party/esp32-csi-toolkit/` checkout on every build, so edit the templates,
never `third_party/` directly.

> **Flashing gotcha:** `idf.py flash` occasionally reports success while writing
> an incomplete image, leaving the board in a boot loop. A good flash verifies
> **three** sections ("Hash of data verified." ×3). If a board loops or emits
> serial garbage, reflash before suspecting the firmware.

## Channel selection

CSI is frequency dependent: the channel response on one channel tells you
nothing about another. A baseline captured on channel 6 is meaningless if a node
later moves to channel 11. Node-to-node links also require every node to sit on
the *same* channel, since promiscuous capture only hears the current one.

**So do not enable dynamic channel switching.** Pick a quiet channel once at
deployment, pin it, and recalibrate. On the Pi, survey the band with:

```bash
sudo iw dev wlan0 scan | grep -E "^BSS|DS Parameter set|signal" 
# or, more readable:
nmcli -f SSID,CHAN,SIGNAL dev wifi list | sort -k2 -n
```

Prefer 1, 6, or 11 (non-overlapping on 2.4 GHz) and take whichever carries the
fewest/weakest neighbours. Pin it in the Pi's hotspot config so it cannot drift
between reboots, and set the same value in `WIFI_CHANNEL` for the firmware.
Re-run `--calibrate` after any channel change.
