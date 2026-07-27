# esp32-csi-visualization

Ingests WiFi Channel State Information (CSI) from a set of ESP32s and turns it
into a visualization of sensed objects — currently a coarse ASCII "heat map" of
a room, printed to the CLI every three seconds. Very much a test/POC.

On top of the [ESP32-CSI-Tool](https://github.com/StevenMHernandez/ESP32-CSI-Tool)
this repo adds UDP forwarding, config templates for WiFi and destination
host/port, node-level attribution of every reading, and setup scripts for
ESP-IDF v4.3.3.

```
                      deployment nodes (6)
        ┌─────┬─────┬─────┬─────┬─────┬─────┐
        │ ESP │ ESP │ ESP │ ESP │ ESP │ ESP │
        └──┬──┴──┬──┴──┬──┴──┬──┴──┬──┴──┬──┘
           └─────┴─────┴──┬──┴─────┴─────┘
                          │
                          │   CSI JSON over UDP :5566
                          ▼
            ┌───────────────────────────┐
            │  Raspberry Pi             │
            │  AP + DHCP                │
            │  192.168.4.1   channel 6  │
            └─────────────┬─────────────┘
                          │  optional relay :5566
                          ▼
              visualizations/rti-aggregator.py
```

## Documentation

| Document | Contents |
|---|---|
| [Architecture](docs/architecture.md) | Topology, the two link types, firmware-mode comparison |
| [Firmware](docs/firmware.md) | Building, flashing, config options, channel selection |
| [CSI pipeline](docs/csi-pipeline.md) | State machine from radio to heatmap, where readings drop, JSON format |
| [RTI sensing](docs/rti-sensing.md) | Measuring node positions, calibration, running the aggregator |

## Quickstart

**1. Flash the nodes.** `active_sta` is the recommended mode; the script
rebuilds first so a stale binary is never silently flashed.

```bash
./scripts/esp-idf/esp-sta-flash.sh
```

Set the WiFi SSID/password and UDP target in
`templates/esp32-csi-toolkit/active_sta/sdkconfig.defaults` beforehand. Edit
templates, never `third_party/` — the templates are copied over that checkout on
every build.

**2. Record each node's MAC and measured `(x, y)` position** in
`templates/rti-nodes-config.json`. See
[Measuring node positions](docs/rti-sensing.md#measuring-node-positions).

**3. Calibrate with the room empty:**

```bash
python visualizations/rti-aggregator.py --calibrate --calibrate-seconds 30 --room-width <W> --room-height <H>
```

**4. Run live sensing:**

```bash
python visualizations/rti-aggregator.py --room-width <W> --room-height <H>
```

To check what is actually arriving before trusting a calibration:

```bash
python tests/diagnose_links.py --count 30
```
