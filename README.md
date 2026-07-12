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

### Configuration
Edit the top of `rti_aggregator.py`:

| Variable | Description |
|---|---|
| `STATIONS` | Dict of `{"station_ip": (x, y)}` — each receiving node's IP and measured position |
| `TRANSMITTERS` | Dict of `{"mac_address": (x, y)}` — each transmitting node's MAC and measured position (for a single-AP setup, this is one entry: the AP's MAC) |
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
