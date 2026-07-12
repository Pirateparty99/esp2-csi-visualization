# esp32-csi-visualization
This repo is intended to ingest CSI data from ESP32s to create a visualization of sensed objects. Very much a test/POC. Currently has one script that aims to map out a room and spit out a basic "heat map" of the room on the CLI every three seconds.  

This repo adds UDP forwarding to the ESP CSI Toolkit project with templates to set the WiFi name/password and destination host/port.

This repo also includes a script to setup the ESP IDF with version 4.3.3 to build the firmware.

# Setup

Before running any visualization the ESPs need to be flashed with the CSI-emmitting firmware.

1, Configure AP/STA settings
2. Build/flash ESP32 firmware



# Visualization(s)

## ESP32 Room Mapping/Sensing 

### Requirements
- 1 ESP32 configured as AP, 2+ ESP32s configured as STAs (Working on adding functionailty to have a  full mesh of nodes pinging each other)
- Each STA running ESP32-CSI-Tool firmware, sending CSI JSON over UDP to a central aggregator
- Physically measured (x, y) position (in meters) for every node, relative to a chosen room origin

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
| `transmitters` | Each transmitting node's MAC address and measured `(x, y)` position — devices whose frames get sniffed and reported by stations. For a single-AP setup, this is one entry (the AP's MAC). For a mesh setup, every mesh node needs an entry here **and** in `stations`, with matching positions, since each node both transmits and receives. |

Other tunable constants (top of `rti_aggregator.py`):

| Variable | Description |
|---|---|
| `GRID_RESOLUTION` | Meters per pixel in the reconstructed image (default `0.1`) |
| `ELLIPSE_WIDTH` | RTI ellipse width parameter in meters — higher = smoother/coarser, more noise-tolerant (default `1.0`) |

### Usage

**1. Point every ESP32's UDP target at the aggregator's IP, on the listen port (default 5566).**

**2. Calibrate with the room empty:**
```bash
python rti_aggregator.py --calibrate --calibrate-seconds 30 --room-width <W> --room-height <H>
```
Produces `rti_baseline.json`. Re-run this any time a node's position changes.

**3. Run live sensing:**
```bash
python rti_aggregator.py --room-width <W> --room-height <H>
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
Use `diagnose_links.py` to verify which (IP, MAC) link pairs are actually arriving before trusting calibration results:
```bash
python diagnose_links.py --count 30
```

### Known limitations
- With few nodes / a single-AP fan topology (all links sharing one transmitter), spatial resolution is coarse — reconstructs rough "something changed in this direction" rather than a precise position.
- Reconstructs *change from baseline* (presence/movement), not static room geometry (walls, furniture shape) — that's a fundamentally harder, unsolved problem with this approach.
- Resolution improves meaningfully with more nodes and, especially, with node-to-node (mesh) links rather than single-AP fan links, since crossing paths from multiple transmit points disambiguate position much better.

CSI Fields (In order of how the fields are sent in the JSON messages)
type,role,mac,rssi,rate,sig_mode,mcs,bandwidth,smoothing,not_sounding,aggregation,stbc,fec_coding,sgi,noise_floor,ampdu_cnt,channel,secondary_channel,local_timestamp,ant,sig_len,rx_state,real_time_set,real_timestamp,len,CSI_DATA