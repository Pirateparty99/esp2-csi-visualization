# ESP32 Room Mapping / Sensing (RTI) 

## Requirements
- An access point (a Raspberry Pi, or an ESP32 flashed with `active_ap`) plus 2+ ESP32s running `active_sta`
- Each node running this repo's firmware, sending CSI JSON over UDP to a central aggregator
- `CSI_PROMISCUOUS` enabled if you want node-to-node links as well as node-to-AP links
- Physically measured (x, y) position (in meters) for every node, relative to a chosen room origin
- Every node on the same, pinned WiFi channel (see [Channel selection](firmware.md#channel-selection))

The reference deployment is 6 ESP32s placed in the room. Additional boards kept
on USB at a workstation are useful for flashing and serial monitoring, but they
are bench equipment rather than part of the sensing geometry — exclude them from
the node config, since a node sitting on a desk contributes links that do not
cross the monitored area.

## Measuring node positions

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

## Configuration

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

### Rooms that are not rectangles

`--room-width` / `--room-height` define a bounding rectangle. For an L-shape,
an alcove, a cut corner, or any other non-rectangular floor, add an optional
`room_polygon` to the config: the vertices of the floor outline, in order,
in the same coordinate frame as the node positions.

```json
{
  "room_polygon": [[0,0], [7,0], [7,5], [4,5], [4,8], [0,8]]
}
```

That example is a 7×8 m bounding box with a 3×3 m bite taken out of the
top-right corner. Set `--room-width 7 --room-height 8` to match the bounding
box; pixels outside the outline are **excluded from the reconstruction**, not
merely hidden, so no attenuation is attributed to space that cannot contain
anything. They render blank.

Any simple polygon works, including concave ones. Omit `room_polygon` entirely
and the whole rectangle is treated as floor, which is the previous behaviour.

Walls and fixed furniture inside the outline do **not** need modelling — RTI
reconstructs *change* from the empty-room baseline, so anything static is
cancelled by calibration.

Other tunable constants (top of `visualizations/rti-aggregator.py`):

| Variable | Description |
|---|---|
| `GRID_RESOLUTION` | Meters per pixel in the reconstructed image (default `0.1`) |
| `ELLIPSE_WIDTH` | RTI ellipse width parameter in meters — higher = smoother/coarser, more noise-tolerant (default `1.0`) |

## Usage

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

## CLI flags
| Flag | Default | Description |
|---|---|---|
| `--port` | `5566` | UDP port to listen on |
| `--bind` | `0.0.0.0` | Bind address |
| `--calibrate` | off | Run in calibration mode instead of live sensing |
| `--calibrate-seconds` | `20` | Duration of calibration capture |
| `--window-seconds` | `5.0` | Seconds averaged per frame. Noise in the mean falls as 1/√samples, so longer windows detect smaller changes but respond more slowly |
| `--z-threshold` | `3.0` | Standard errors a link must move to count as real. Below this the frame reports no significant change instead of rendering |
| `--alpha` | `0.1` | Regularization, as a fraction of the data term. Higher is smoother and more conservative; lower fits measurements more closely and amplifies noise. Cannot be 0 — see below |
| `--max-link-cv` | `0.15` | Drop links whose calibrated amplitude varies by more than this fraction of their mean |
| `--room-width` | `4.0` | Grid width in meters |
| `--room-height` | `3.0` | Grid height in meters |

### Sizing the grid to your nodes

`--room-width` and `--room-height` define the reconstruction grid, and **every
node must fall inside it**. The defaults (4.0 × 3.0) are smaller than most real
deployments, so they almost always need setting.

Take the largest `x` and largest `y` in your node config and round up:

```bash
# nodes spanning 6.0m x 7.1m
python visualizations/rti-aggregator.py --room-width 7 --room-height 8
```

The script refuses to run if any node lies outside, and tells you what to pass:

```
[aggregator] ERROR: 5 node position(s) fall outside the 4.0m x 3.0m room:
[aggregator]   b4:bf:e9:60:b5:24 at (4.4, 7.1)
[aggregator] Node positions span 6.0m x 7.1m. Pass at least
             --room-width 6.0 --room-height 7.1, or correct the positions.
```

This used to fail silently, and the failure was easy to mistake for a working
system: off-grid nodes still contribute rows to the weight matrix, but their
ellipses land mostly outside it, so the output becomes smeared diagonal streaks
that look like structure and track nothing.

Use the **same** dimensions for `--calibrate` and for live runs. A baseline
captured on one grid does not apply to another.

## Diagnostics
Use `tests/diagnose_links.py` to verify which (IP, MAC) link pairs are actually arriving before trusting calibration results:
```bash
python tests/diagnose_links.py --count 30
```

## Sensitivity and noise

CSI amplitude from an ESP32 is noisy, and the noise is close in size to the
effect being measured. Measured on this deployment with nothing moving, a
link's window mean wandered by about **0.45 on a mean of ~20** — roughly 2%.
Changes caused by a person are often the same order.

**Deviations are reported in sigma, not raw amplitude.** Each link's change is
divided by its own standard error from calibration. Links differ in how noisy
they are, so raw amplitude cannot be thresholded — a quiet link moving slightly
and a noisy link idling look identical. Calibration flags links too unstable to
contribute:

```
[aggregator] NOTE: 2/20 links vary by >15% at rest; they will contribute little.
```

**Frames below `--z-threshold` are not rendered:**

```
[aggregator] no significant change (peak 2.5 sigma < 3.0, 19 links)
```

This matters because the image is scaled to its own data. Rescaling every frame
to its own range turns arbitrarily small deviations into a confident-looking
picture, so an idle room and an occupied one look equally dramatic. Saying
"nothing detected" is more useful than a vivid image of noise.

### Why regularization cannot be zero

Links share endpoints — with 6 nodes, every link touches two of the same six
positions — so the rows of the weight matrix are linearly dependent and `W Wᵀ`
is rank deficient. Measured on this deployment its condition number is **3×10¹⁸**
unregularized, which is numerically singular: the reconstruction is then
dominated by floating-point error rather than by the measurements, and shows up
as large room-spanning shapes that flip between frames.

`--alpha` is a *fraction* of the data term, not an absolute value, because the
magnitude of `W Wᵀ` depends on grid size, resolution and ellipse width. At the
default `0.1` the condition number drops to ~83.

If images look wild and unstable, raise it (`--alpha 0.3`). If everything looks
flat and featureless, lower it (`--alpha 0.03`).

### Reading the output

What to expect at this scale, and what not to:

- **Presence detection works.** Peak sigma should sit below the threshold when
  the room is still and rise clearly when someone moves.
- **Localization is weak.** With ~20 links against 5600 pixels the problem is
  badly under-determined, so the reconstruction smears along the link ellipses.
  Expect a rough "something changed over here", not a person-shaped blob.
- **Diagonal streaks are the ellipses themselves**, not an object. Seeing the
  same diagonal repeatedly means one or two links dominate the solution.

Signs something is actually wrong, rather than merely coarse:

- The same link dominates every frame **and flips sign** between frames — that
  is an unstable link, not a detection.
- Peak sigma sits near the threshold regardless of whether anyone is present.
- The strongest links are not the ones whose paths you are crossing.

First things to try: `--window-seconds 10` for more averaging, a longer
`--calibrate-seconds`, and re-checking that the node positions in the config
match reality. Wrong positions produce confident, meaningless images.

## Known limitations
- With few nodes / a single-AP fan topology (all links sharing one transmitter), spatial resolution is coarse — reconstructs rough "something changed in this direction" rather than a precise position.
- Reconstructs *change from baseline* (presence/movement), not static room geometry (walls, furniture shape) — that's a fundamentally harder, unsolved problem with this approach.
- Resolution improves meaningfully with more nodes and, especially, with node-to-node links rather than single-AP fan links, since crossing paths from multiple transmit points disambiguate position much better. Enable `CSI_PROMISCUOUS` to get them.
- Even with promiscuous capture you get whichever links happen to carry traffic, not a guaranteed set. Full all-pairs coverage (N×(N−1)/2 links) would need a scheduled round-robin where each node broadcasts in its own slot — not implemented.
- Sensitivity is marginal at 6 nodes: the per-link change from a person is close to the link's resting noise. See [Sensitivity and noise](#sensitivity-and-noise).
- The baseline drifts as WiFi conditions change over minutes to hours, so an old baseline reads as spurious change. Recalibrate at the start of a session, and after any channel change.
