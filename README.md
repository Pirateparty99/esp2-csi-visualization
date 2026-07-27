# esp32-csi-visualization
This repo is intended to ingest CSI data from ESP32s to create a visualization of sensed objects. Very much a test/POC. Currently has one script that aims to map out a room and spit out a basic "heat map" of the room on the CLI every three seconds.  

This repo adds UDP forwarding to the ESP CSI Toolkit project with templates to set the WiFi name/password and destination host/port.

This repo also includes a script to setup the ESP IDF with version 4.3.3 to build the firmware.

# Architecture

A Raspberry Pi runs the access point and every ESP32 associates to it. Each node
captures CSI, serializes it to JSON, and sends it over UDP to the Pi on port
5566. The Pi optionally relays that stream on to a workstation for analysis.

```
      deployment nodes (6)                  debug nodes (2)
   ┌─────┬─────┬─────┬─────┬─────┬─────┐   ┌─────┬─────┐
   │ ESP │ ESP │ ESP │ ESP │ ESP │ ESP │   │ ESP │ ESP │
   └──┬──┴──┬──┴──┬──┴──┬──┴──┬──┴──┬──┘   └──┬──┴──┬──┘
      └─────┴─────┴──┬──┴─────┴─────┘         │     │ └──── USB serial ──┐
                     │                        └─────┘                    │
                     │   all 8 nodes: CSI JSON over UDP :5566            │
                     └────────────────┬───────────────────────────────┐  │
                                      ▼                               │  │
                        ┌───────────────────────────┐                 │  │
                        │  Raspberry Pi             │                 │  │
                        │  AP + DHCP                │                 │  │
                        │  192.168.4.1   channel 6  │                 │  │
                        └─────────────┬─────────────┘                 │  │
                                      │  optional relay :5566         │  │
                                      ▼                               ▼  ▼
                            visualizations/rti-aggregator.py      workstation
                                                                (flash + monitor)
```

Two link types are measured, distinguished by the `node` and `mac` fields in
each reading (see [CSI JSON format](#csi-json-format)):

```
  node = observer,  mac = Pi AP     ->  node <-> AP link    (primary, always present)
  node = observer,  mac = peer node ->  node <-> node link  (requires CSI_PROMISCUOUS)
```

`node <-> AP` links form a star: every node measures its own path to the Pi.
Those alone give poor tomographic coverage, since all paths radiate from one
point. `node <-> node` links cross the room and disambiguate position far
better, which is why `CSI_PROMISCUOUS` is on by default.

## Firmware modes

| Mode | Project | When to use |
|---|---|---|
| **active_sta** *(recommended)* | `active_sta` | All nodes within range of the Pi. Stable link geometry, no relay, no root election. With `CSI_PROMISCUOUS` it yields both link types. |
| active_ap | `active_ap` | An ESP32 acts as the AP instead of the Pi. |
| wifi-mesh | `wifi-mesh` | Only when some nodes cannot reach the Pi directly and need multi-hop relay. |

**On mesh:** it works and delivers CSI end to end, but ESP-MESH picks its own
parent/child tree and rearranges it on re-parenting and root re-election. The
root role is not stable across reboots. Since the measured links *are* the tree,
the sensing baseline moves whenever the topology changes, which invalidates
calibration. Prefer `active_sta` unless multi-hop is genuinely required.

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

# Setup

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


# Visualization(s)

## ESP32 Room Mapping/Sensing 

### Requirements
- An access point (a Raspberry Pi, or an ESP32 flashed with `active_ap`) plus 2+ ESP32s running `active_sta`
- Each node running this repo's firmware, sending CSI JSON over UDP to a central aggregator
- `CSI_PROMISCUOUS` enabled if you want node-to-node links as well as node-to-AP links
- Physically measured (x, y) position (in meters) for every node, relative to a chosen room origin
- Every node on the same, pinned WiFi channel (see [Channel selection](#channel-selection))

The reference deployment is 8 ESP32s: 6 placed in the room, and 2 kept on USB at
the workstation as debug nodes for flashing and serial monitoring.

### Measuring node positions

The script models your room as a flat 2D floor plan — every node needs an `(x, y)` coordinate in **meters**, where `x` and `y` are just distances along two perpendicular directions on the floor. Height (how far off the ground a node sits) isn't used; only where it is on the floor plan matters.

**1. Pick an origin `(0, 0)`.** Any fixed corner of the room works — the easiest choice is whichever corner is most convenient to measure from repeatedly (e.g. the corner nearest an outlet, or nearest your AP). Mark it (tape, sticky note) so you can re-measure from the exact same spot later if a node moves.

**2. Pick your axes.** From that corner:
- `x` = distance along one wall (pick a direction, e.g. "left to right" facing into the room)
- `y` = distance along the *other* wall, perpendicular to `x`

It doesn't matter which wall is `x` vs `y`, as long as you're consistent for every node.

**3. Measure each node's position.** For every ESP32 (and the AP), measure straight-line distance from the origin corner along the `x` wall, and separately along the `y` wall — basically "how far right, how far in," like grid coordinates on a floor plan. A tape measure or laser distance measurer works fine; accuracy to ~5-10cm is plenty.

**Example:** a 4m × 3m room, origin at the front-left corner:
```
(0,3) ---------------------- (4,3)
  |                             |
  |         node C (2,1.5)      |
  |                             |
(0,0) ---------------------- (4,0)
        ^ origin corner
```
Node at `(2.0, 1.5)` sits 2 meters along the x-wall and 1.5 meters along the y-wall from the origin corner — roughly the middle of the room.

**Important:** `--room-width` and `--room-height` (used when running the script) must describe the same room, measured from the same origin, as your node positions — e.g. a 4m × 3m room needs `--room-width 4 --room-height 3`, and every node's `x` should fall within `[0, 4]` and every `y` within `[0, 3]`.

**If you move a node**, re-measure its position, update it in your config file, and re-run `--calibrate` — a stale baseline measured against old positions will produce meaningless results.

### Configuration

Node positions/MACs/IPs are kept in `templates/rti-nodes-config.json`, **not committed to the repo** (it's gitignored), so real hardware identifiers don't end up in version control. On first run, if this file doesn't exist yet, the script automatically copies `templates/rti-nodes-config.example.json` into place and pauses so you can fill it in before continuing.

Format:
```json
{
  "stations": {
    "192.168.4.2": [4.0, 0.0]
  },
  "transmitters": {
    "aa:bb:cc:dd:ee:ff": [0.0, 0.0]
  }
}
```

| Section | Description |
|---|---|
| `stations` | Each receiving node's IP and measured `(x, y)` position — nodes that report their own captured CSI back to the aggregator |
| `transmitters` | Each transmitting node's MAC address and measured `(x, y)` position — devices whose frames get sniffed and reported by stations. For a plain single-AP setup this is one entry (the AP's MAC). With `CSI_PROMISCUOUS` (or a mesh), every node needs an entry here **and** in `stations`, with matching positions, since each node both transmits and receives. |

Other tunable constants (top of `visualizations/rti-aggregator.py`):

| Variable | Description |
|---|---|
| `GRID_RESOLUTION` | Meters per pixel in the reconstructed image (default `0.1`) |
| `ELLIPSE_WIDTH` | RTI ellipse width parameter in meters — higher = smoother/coarser, more noise-tolerant (default `1.0`) |

### Usage

**1. Point every ESP32's UDP target at the aggregator's IP, on the listen port (default 5566).**

**2. Calibrate with the room empty:**
```bash
python visualizations/rti-aggregator.py --calibrate --calibrate-seconds 30 --room-width <W> --room-height <H>
```
Produces `rti_baseline.json`. Re-run this any time a node's position changes.

**3. Run live sensing:**
```bash
python visualizations/rti-aggregator.py --room-width <W> --room-height <H>
```
Prints a coarse ASCII heatmap of signal-attenuation change every 3 seconds. Denser characters indicate a likely change (presence/movement) relative to the empty-room baseline.

### CLI flags
| Flag | Default | Description |
|---|---|---|
| `--port` | `5566` | UDP port to listen on |
| `--bind` | `0.0.0.0` | Bind address |
| `--calibrate` | off | Run in calibration mode instead of live sensing |
| `--calibrate-seconds` | `20` | Duration of calibration capture |
| `--room-width` | `4.0` | Room width in meters |
| `--room-height` | `3.0` | Room height in meters |

### Diagnostics
Use `tests/diagnose_links.py` to verify which (IP, MAC) link pairs are actually arriving before trusting calibration results:
```bash
python tests/diagnose_links.py --count 30
```

### Known limitations
- With few nodes / a single-AP fan topology (all links sharing one transmitter), spatial resolution is coarse — reconstructs rough "something changed in this direction" rather than a precise position.
- Reconstructs *change from baseline* (presence/movement), not static room geometry (walls, furniture shape) — that's a fundamentally harder, unsolved problem with this approach.
- Resolution improves meaningfully with more nodes and, especially, with node-to-node links rather than single-AP fan links, since crossing paths from multiple transmit points disambiguate position much better. Enable `CSI_PROMISCUOUS` to get them.
- Even with promiscuous capture you get whichever links happen to carry traffic, not a guaranteed set. Full all-pairs coverage (N×(N−1)/2 links) would need a scheduled round-robin where each node broadcasts in its own slot — not implemented.

## CSI JSON format

Each UDP datagram is one reading:

```json
{"type":"CSI_DATA","node":"70:4b:ca:27:17:a0","mac":"b4:bf:e9:60:4a:7d","rssi":-54,"len":384,"csi":[66,32,4,...]}
```

| Field | Description |
|---|---|
| `type` | Always `CSI_DATA`. |
| `node` | MAC of the node that **observed** this reading (its STA interface). |
| `mac` | MAC of the peer that **transmitted** the measured frame. |
| `rssi` | Received signal strength of that frame, in dBm. |
| `len` | Length of the CSI buffer as reported by the driver. |
| `csi` | Up to 128 signed values (interleaved I/Q pairs). |

`node` and `mac` together name a **directed link**: `node` heard a frame from
`mac`. Both are needed — `mac` alone identifies the transmitter, not the
reporter, and the datagram's source IP is no help either, since on a mesh every
reading arrives from the root regardless of which node captured it.

> Readings produced before the `node` field was added lack it entirely.
> Collectors should skip those rather than misattribute them, and any node still
> sending them needs reflashing.

The serial output (`SEND_CSI_TO_SERIAL`) is a separate, wider CSV format
inherited from the upstream toolkit and does **not** match the JSON above:

```
type,role,mac,rssi,rate,sig_mode,mcs,bandwidth,smoothing,not_sounding,aggregation,stbc,fec_coding,sgi,noise_floor,ampdu_cnt,channel,secondary_channel,local_timestamp,ant,sig_len,rx_state,real_time_set,real_timestamp,len,CSI_DATA
```