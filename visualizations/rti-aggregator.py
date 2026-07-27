#!/usr/bin/env python3
"""
Multi-node CSI aggregator + basic Radio Tomographic Imaging (RTI).

Requires:
  - Multiple ESP32 nodes, each with a KNOWN fixed (x, y) position, all
    sending CSI JSON packets (ESP32-CSI-Tool format) to this aggregator's
    UDP port.
  - Nodes configured to periodically transmit to each other (not just to
    a router), so that pairs of nodes form distinct (TX, RX) links whose
    paths cross the room at different angles.

Each incoming packet is identified as a link by:
  - RX = the source IP the UDP packet arrived from
  - TX = the "mac" field inside the JSON payload (whoever's frame was sniffed)

Usage:
  1. Edit NODES below with each node's IP and physical position.
  2. Run with --calibrate first, with the room EMPTY, to record a baseline
     per link (10-30s is usually enough).
  3. Run normally afterward; it will print a coarse attenuation grid
     periodically (or dump JSON you can feed into a heatmap visualizer).
"""

import os
import shutil
import socket
import json
import time
import argparse
import numpy as np
from collections import defaultdict

# ---------------------------------------------------------------------------
# Node positions are loaded from an external JSON file (default:
# templates/rti-nodes-config.json), NOT hardcoded here, so MAC addresses/IPs
# can be kept out of version control. Add rti-nodes-config.json to your
# .gitignore.
#
# Expected format:
# {
#   "stations": {
#     "aa:bb:cc:dd:ee:01": [4.0, 0.0],
#     "aa:bb:cc:dd:ee:02": [4.0, 3.0]
#   },
#   "transmitters": {
#     "a1:b2:c3:d4:e5:00": [0.0, 0.0],
#     "00:e4:d4:c3:b2:a1": [4.0, 0.0]
#   }
# }
# ---------------------------------------------------------------------------
NODES_CONFIG_PATH = "templates/rti-nodes-config.json"


def load_nodes_config(path):
    example_path = path.replace(".json", ".example.json")

    if not os.path.exists(path):
        if os.path.exists(example_path):
            shutil.copy(example_path, path)
            print(
                f"\n[setup] '{path}' didn't exist yet — copied it from "
                f"'{example_path}'."
            )
            print(
                f"[setup] Open '{path}' now and fill in your real IPs, "
                f"MAC addresses, and measured positions."
            )
            input("[setup] Press Enter once you've saved your edits to continue... ")
        else:
            raise FileNotFoundError(
                f"\n\nNode config file '{path}' not found, and no example "
                f"template '{example_path}' exists to copy from.\n"
                f"Create '{path}' manually (see the comment block above "
                f"load_nodes_config in this script for the expected format), "
                f"and add it to your .gitignore to keep MAC addresses out of "
                f"the repo.\n"
            )

    with open(path, "r") as f:
        raw = json.load(f)
    # Station keys are MACs now (matching the "node" field). IP keys from
    # older configs still load, so both can coexist during a migration.
    stations = {key.lower(): tuple(pos) for key, pos in raw.get("stations", {}).items()}
    transmitters = {
        mac.lower(): tuple(pos) for mac, pos in raw.get("transmitters", {}).items()
    }
    # Optional outline of the floor for non-rectangular rooms. A list of
    # [x, y] vertices in the same coordinate frame as the node positions,
    # traced around the wall in order. Omit it and the whole bounding
    # rectangle is treated as floor.
    room_polygon = [tuple(pt) for pt in raw.get("room_polygon", [])]
    if room_polygon and len(room_polygon) < 3:
        raise ValueError(
            f"'room_polygon' in {path} needs at least 3 vertices, got "
            f"{len(room_polygon)}."
        )
    return stations, transmitters, room_polygon


STATIONS, TRANSMITTERS, ROOM_POLYGON = load_nodes_config(NODES_CONFIG_PATH)
IP_TO_POS = STATIONS
MAC_TO_POS = TRANSMITTERS

GRID_RESOLUTION = 0.1  # meters per pixel
ELLIPSE_WIDTH = 1.0  # lambda parameter (meters) - wider = smoother/coarser image


def amplitude_from_csi(csi_list):
    """Convert raw CSI int list (I/Q interleaved) into a scalar amplitude summary."""
    arr = np.asarray(csi_list, dtype=np.float32)
    if arr.size == 0:
        return 0.0
    if arr.size % 2 == 0:
        iq = arr.reshape(-1, 2)
        amp = np.sqrt(iq[:, 0] ** 2 + iq[:, 1] ** 2)
    else:
        amp = np.abs(arr)
    return float(np.mean(amp))


def identify_link(source_ip, payload):
    """Return (tx_pos, rx_pos) for a packet, or None if unrecognized.

    The receiver is identified by the payload's "node" field, not the source
    IP. Source IP does not work: when the collector sits behind a relay every
    datagram arrives from the relay's address, and nodes take DHCP leases
    rather than fixed addresses anyway. "node" is the capturing node's own MAC
    and is carried in the reading itself.

    Falls back to the source IP so configs predating the "node" field, and any
    node not yet reflashed, still resolve.
    """
    node_mac = payload.get("node", "").lower()
    rx_pos = STATIONS.get(node_mac) if node_mac else None
    if rx_pos is None:
        rx_pos = STATIONS.get(source_ip)

    tx_mac = payload.get("mac", "").lower()
    tx_pos = MAC_TO_POS.get(tx_mac)

    if rx_pos is None or tx_pos is None:
        return None
    if rx_pos == tx_pos:
        # A node hearing its own transmissions is a zero-length link and
        # carries no spatial information.
        return None
    return (tx_pos, rx_pos)


def build_room_grid(room_width, room_height, resolution):
    xs = np.arange(0, room_width, resolution)
    ys = np.arange(0, room_height, resolution)
    return xs, ys


def build_room_mask(xs, ys, polygon):
    """Boolean mask over the grid: True where a pixel is inside the room.

    Rooms are rarely rectangles. The grid stays a rectangle -- it is just an
    array -- and this marks which of its pixels are actually floor. Pixels
    outside are excluded from the reconstruction rather than merely hidden,
    so no signal is attributed to space that cannot contain anything.

    Ray casting, so any simple polygon works: L-shapes, alcoves, cut corners.
    No extra dependency.
    """
    px, py = np.meshgrid(xs, ys, indexing="ij")
    px = px.ravel()
    py = py.ravel()

    if not polygon:
        return np.ones(px.size, dtype=bool)

    inside = np.zeros(px.size, dtype=bool)
    n = len(polygon)
    for i in range(n):
        x1, y1 = polygon[i]
        x2, y2 = polygon[(i + 1) % n]
        # Does a ray to +x from the pixel cross this edge?
        straddles = (y1 > py) != (y2 > py)
        with np.errstate(divide="ignore", invalid="ignore"):
            x_cross = (x2 - x1) * (py - y1) / (y2 - y1 + 1e-12) + x1
        inside ^= straddles & (px < x_cross)
    return inside


def locate_change(grid, xs, ys, mask_grid):
    """Summarize where the attenuation sits: (peak_xy, centroid_xy, spread_m).

    Only positive values count. RTI models a body as *attenuating* a link, so
    negative pixels are the reconstruction's undershoot and including them
    drags the centroid toward nothing physical.

    Spread is the intensity-weighted RMS distance from the centroid. It is the
    honest part of the answer: with ~20 links the reconstruction smears along
    the link ellipses, so a spread comparable to the room size means "a change
    somewhere in this direction", not a located object.
    """
    px, py = np.meshgrid(xs, ys, indexing="ij")
    vals = np.where(mask_grid & (grid > 0), grid, 0.0)
    total = float(vals.sum())
    if total <= 0:
        return None, None, None

    peak_idx = np.unravel_index(np.argmax(vals), vals.shape)
    peak = (float(px[peak_idx]), float(py[peak_idx]))

    cx = float((vals * px).sum() / total)
    cy = float((vals * py).sum() / total)

    d2 = (px - cx) ** 2 + (py - cy) ** 2
    spread = float(np.sqrt((vals * d2).sum() / total))
    return peak, (cx, cy), spread


def describe_direction(cx, cy, room_width, room_height):
    """Plain-language bearing of a point relative to the room centre."""
    mx, my = room_width / 2.0, room_height / 2.0
    dx, dy = cx - mx, cy - my
    # Within a quarter of the room of centre, a bearing is not meaningful.
    if abs(dx) < room_width * 0.125 and abs(dy) < room_height * 0.125:
        return "centre"
    parts = []
    if dy > room_height * 0.125:
        parts.append("far")
    elif dy < -room_height * 0.125:
        parts.append("near")
    if dx > room_width * 0.125:
        parts.append("right")
    elif dx < -room_width * 0.125:
        parts.append("left")
    return "-".join(parts) if parts else "centre"


def rti_weight_matrix(links, xs, ys, ellipse_width):
    """
    Build the ellipse-model weight matrix W (num_links x num_pixels).
    Classic RTI model (Wilson & Patwari): a pixel contributes to a link's
    attenuation if it lies within an ellipse of the given width around the
    TX-RX line.
    """
    px, py = np.meshgrid(xs, ys, indexing="ij")
    px = px.ravel()
    py = py.ravel()
    num_pixels = px.size

    W = np.zeros((len(links), num_pixels), dtype=np.float32)
    for i, (tx, rx) in enumerate(links):
        x1, y1 = tx
        x2, y2 = rx
        d_tx_rx = np.hypot(x2 - x1, y2 - y1)
        if d_tx_rx < 1e-6:
            continue
        d1 = np.hypot(px - x1, py - y1)
        d2 = np.hypot(px - x2, py - y2)
        in_ellipse = (d1 + d2) < (d_tx_rx + ellipse_width)
        weight = np.where(in_ellipse, 1.0 / np.sqrt(max(d_tx_rx, 0.1)), 0.0)
        W[i, :] = weight
    return W


def reconstruct_image(W, deviations, alpha=1.0):
    """
    Solve regularized least squares: image = (W^T W + alpha*I)^-1 W^T y
    Returns a flat pixel array (reshape by caller using grid dims).

    Uses the dual (kernel) form:

        (W^T W + aI)^-1 W^T  ==  W^T (W W^T + aI)^-1

    These are exactly equal by the push-through identity, but the left side
    inverts a num_pixels x num_pixels matrix and the right a
    num_links x num_links one. RTI always has far more pixels than links --
    a 7x8m room at 0.1m is 5600 pixels against ~20 links -- so the primal
    form solves a 5600x5600 system per frame, measured at 116s. The dual is
    a 20x20 solve and returns in well under a millisecond, which is what
    makes a 3s refresh possible at all.
    """
    num_links = W.shape[0]
    WWt = W @ W.T

    # alpha is a FRACTION of the data term, not an absolute value.
    #
    # An absolute alpha cannot work: the magnitude of W W^T depends on grid
    # size, resolution and ellipse width, so a constant that regularizes one
    # room barely touches another. Measured on a 7x8m grid at 0.1m, the
    # diagonal averages ~279, so the previous hardcoded alpha=1.0 supplied
    # 0.4% of the data scale -- essentially unregularized.
    #
    # That matters because W W^T is rank deficient here. Links share
    # endpoints, so rows are linearly dependent and the unregularized solve is
    # singular. Ill-conditioning of that kind is what turns small measurement
    # noise into large, room-spanning, sign-flipping structure.
    scale = float(np.mean(np.diag(WWt)))
    reg = max(alpha * scale, 1e-6) * np.eye(num_links, dtype=np.float32)
    return W.T @ np.linalg.solve(WWt + reg, deviations)


def collect_peak_zs(sock, baseline, seconds, window_seconds, label):
    """Collect one peak-sigma value per window over `seconds`.

    This is the quantity the live imager thresholds on, so measuring its
    distribution under known conditions is what tells you whether the setup
    can separate occupied from empty at all.
    """
    peaks = []
    live = defaultdict(list)
    sock.settimeout(1.0)
    start = time.time()
    last = start
    while time.time() - start < seconds:
        try:
            data, addr = sock.recvfrom(4096)
        except socket.timeout:
            pass
        else:
            try:
                payload = json.loads(data.decode("utf-8", errors="ignore"))
            except Exception:
                payload = None
            if payload:
                link = identify_link(addr[0], payload)
                if link is not None:
                    live[link].append(amplitude_from_csi(payload.get("csi", [])))

        if time.time() - last >= window_seconds:
            zs = []
            for l, vals in live.items():
                if l not in baseline or len(vals) < 5:
                    continue
                base_mean, base_std, _ = baseline[l]
                if base_std <= 0:
                    continue
                sem = base_std / np.sqrt(len(vals))
                if sem > 1e-9:
                    zs.append(abs((base_mean - float(np.mean(vals))) / sem))
            if zs:
                peaks.append(max(zs))
                print(f"  [{label}] window {len(peaks):2d}: peak {max(zs):6.1f} sigma")
            live.clear()
            last = time.time()
    return np.array(peaks, dtype=np.float64)


def run_evaluation(sock, baseline, args):
    """A/B test: is occupied actually separable from empty?

    Tuning thresholds by watching the live output is guesswork -- the imager
    always renders something, and a picture is persuasive whether or not it
    means anything. This measures both conditions and reports whether their
    distributions actually separate.
    """
    print("\n" + "=" * 70)
    print("EVALUATION: measuring whether occupancy is detectable at all")
    print("=" * 70)

    input(f"\n[1/2] LEAVE the room, then press Enter to record {args.evaluate_seconds}s of EMPTY... ")
    print(f"[aggregator] recording EMPTY for {args.evaluate_seconds}s...")
    empty = collect_peak_zs(sock, baseline, args.evaluate_seconds, args.window_seconds, "empty")

    input(f"\n[2/2] STAND STILL inside the room, then press Enter to record {args.evaluate_seconds}s of OCCUPIED... ")
    print(f"[aggregator] recording OCCUPIED for {args.evaluate_seconds}s...")
    occupied = collect_peak_zs(sock, baseline, args.evaluate_seconds, args.window_seconds, "occupied")

    print("\n" + "=" * 70)
    if empty.size < 3 or occupied.size < 3:
        print(f"Not enough windows (empty={empty.size}, occupied={occupied.size}).")
        print("Increase --evaluate-seconds or lower --window-seconds.")
        return

    print(f"EMPTY    n={empty.size:3d}  median={np.median(empty):6.1f}  "
          f"mean={empty.mean():6.1f}  max={empty.max():6.1f}")
    print(f"OCCUPIED n={occupied.size:3d}  median={np.median(occupied):6.1f}  "
          f"mean={occupied.mean():6.1f}  min={occupied.min():6.1f}")

    # AUC via the Mann-Whitney U identity: the probability that a random
    # occupied window scores above a random empty one. 0.5 is chance, 1.0 is
    # perfect separation. Preferred over comparing means because it needs no
    # assumption about the shape of either distribution.
    wins = sum((o > e) + 0.5 * (o == e) for o in occupied for e in empty)
    auc = wins / (occupied.size * empty.size)

    # Best achievable threshold, by accuracy, over the observed values.
    candidates = np.unique(np.concatenate([empty, occupied]))
    best_t, best_acc = None, -1.0
    for t in candidates:
        acc = ((empty < t).sum() + (occupied >= t).sum()) / (empty.size + occupied.size)
        if acc > best_acc:
            best_acc, best_t = acc, t

    print(f"\nseparation (AUC)      : {auc:.2f}   (0.5 = indistinguishable, 1.0 = perfect)")
    print(f"best threshold        : {best_t:.1f} sigma -> {best_acc:.0%} accuracy")
    print(f"current --z-threshold : {args.z_threshold:.1f} sigma")
    fp = (empty >= args.z_threshold).mean()
    fn = (occupied < args.z_threshold).mean()
    print(f"  at the current setting: {fp:.0%} false alarms, {fn:.0%} missed detections")

    # The distributions usually overlap, so there is no threshold that is
    # simply "correct" -- only different trades. Report the ends of that trade
    # so the choice can be made against how the output will be used.
    quiet_t = float(empty.max()) + 0.1          # no false alarm on this sample
    quiet_miss = (occupied < quiet_t).mean()
    sens_t = float(occupied.min()) - 0.1        # no miss on this sample
    sens_fp = (empty >= sens_t).mean()
    print("\noperating points:")
    print(f"  fewest false alarms : --z-threshold {quiet_t:.1f} "
          f"-> {quiet_miss:.0%} missed")
    print(f"  fewest misses       : --z-threshold {max(sens_t, 0.1):.1f} "
          f"-> {sens_fp:.0%} false alarms")
    overlap_lo, overlap_hi = float(occupied.min()), float(empty.max())
    if overlap_lo < overlap_hi:
        print(f"  distributions overlap between {overlap_lo:.1f} and "
              f"{overlap_hi:.1f} sigma; no threshold separates them cleanly")

    if min(empty.size, occupied.size) < 20:
        print(
            f"\nNOTE: only {min(empty.size, occupied.size)} windows per condition. "
            f"AUC is coarse at this sample size -- raise --evaluate-seconds "
            f"before treating small differences as meaningful."
        )
    # A wide occupied spread means detectability depends strongly on where the
    # person stood, which is a coverage property of the node layout.
    if occupied.size >= 5 and occupied.std() > occupied.mean() * 0.6:
        print(
            "NOTE: occupied readings vary widely, so some positions register "
            "far more strongly than others. Re-run --evaluate standing in "
            "different spots to find the weak areas in the node layout."
        )

    print()
    if auc >= 0.9:
        print("VERDICT: occupancy is clearly detectable.")
    elif auc >= 0.75:
        print("VERDICT: detectable but marginal. Expect intermittent misses.")
    elif auc >= 0.6:
        print("VERDICT: weak. Barely better than chance -- treat any image with suspicion.")
    else:
        print("VERDICT: NOT detectable. The imager is showing noise.")
        print("  Check node positions are accurate and the baseline is fresh,")
        print("  then try --window-seconds 10 and recalibrating.")
    if best_t is not None and abs(best_t - args.z_threshold) > 0.5:
        print(f"  Consider --z-threshold {best_t:.1f}")
    print("=" * 70)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=5566)
    parser.add_argument("--bind", type=str, default="0.0.0.0")
    parser.add_argument(
        "--calibrate",
        action="store_true",
        help="Record baseline amplitudes with an EMPTY room",
    )
    parser.add_argument("--calibrate-seconds", type=int, default=20)
    parser.add_argument(
        "--window-seconds",
        type=float,
        default=5.0,
        help="Seconds of readings averaged per frame. Noise in the mean falls "
        "as 1/sqrt(samples), so longer windows detect smaller changes but "
        "respond more slowly.",
    )
    parser.add_argument(
        "--evaluate",
        action="store_true",
        help="A/B test empty vs occupied and report whether they separate",
    )
    parser.add_argument("--evaluate-seconds", type=int, default=60)
    parser.add_argument(
        "--alpha",
        type=float,
        default=0.1,
        help="Regularization, as a fraction of the data term. Higher gives "
        "smoother, more conservative images; lower fits the measurements more "
        "closely and amplifies noise. W W^T is rank deficient with shared-"
        "endpoint links, so this cannot be 0.",
    )
    parser.add_argument(
        "--max-link-cv",
        type=float,
        default=0.15,
        help="Exclude links whose calibrated amplitude varies by more than "
        "this fraction of their mean. Unstable links dominate the solution "
        "without carrying information.",
    )
    parser.add_argument(
        "--z-threshold",
        type=float,
        default=3.0,
        help="Standard errors a link must move before it counts as a real "
        "change. Below this the frame is reported as no significant change "
        "rather than rendered.",
    )
    parser.add_argument("--room-width", type=float, default=4.0, help="meters")
    parser.add_argument("--room-height", type=float, default=3.0, help="meters")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.port))
    print(f"[aggregator] Listening on {args.bind}:{args.port}")

    link_amplitudes = defaultdict(list)  # (tx_pos, rx_pos) -> [amplitude,...]
    baseline = {}

    xs, ys = build_room_grid(args.room_width, args.room_height, GRID_RESOLUTION)

    # Node positions must lie inside the grid. Nothing previously checked
    # this, and the failure is silent and misleading: links anchored outside
    # the grid still contribute rows to the weight matrix, but their ellipses
    # fall largely off it, so the reconstruction shows smeared diagonal
    # streaks that look like structure and track nothing.
    outside = {
        mac: pos
        for mac, pos in {**STATIONS, **TRANSMITTERS}.items()
        if pos[0] > args.room_width or pos[1] > args.room_height
        or pos[0] < 0 or pos[1] < 0
    }
    if outside:
        print(
            f"\n[aggregator] ERROR: {len(outside)} node position(s) fall outside "
            f"the {args.room_width}m x {args.room_height}m room:"
        )
        for mac, pos in sorted(outside.items()):
            print(f"[aggregator]   {mac} at {pos}")
        need_w = max(p[0] for p in {**STATIONS, **TRANSMITTERS}.values())
        need_h = max(p[1] for p in {**STATIONS, **TRANSMITTERS}.values())
        print(
            f"[aggregator] Node positions span {need_w:.1f}m x {need_h:.1f}m. "
            f"Pass at least --room-width {need_w:.1f} --room-height {need_h:.1f}, "
            f"or correct the positions in {NODES_CONFIG_PATH}."
        )
        raise SystemExit(1)

    room_mask = build_room_mask(xs, ys, ROOM_POLYGON)

    if ROOM_POLYGON:

        print(

            f"[aggregator] Room outline: {len(ROOM_POLYGON)} vertices, "

            f"{int(room_mask.sum())}/{room_mask.size} pixels inside"

        )

    if args.calibrate:
        print(
            f"[aggregator] CALIBRATING for {args.calibrate_seconds}s — keep the room EMPTY."
        )
        end_time = time.time() + args.calibrate_seconds
        sock.settimeout(1.0)
        while time.time() < end_time:
            try:
                data, addr = sock.recvfrom(4096)
            except socket.timeout:
                continue
            try:
                payload = json.loads(data.decode("utf-8", errors="ignore"))
            except Exception:
                continue
            link = identify_link(addr[0], payload)
            if link is None:
                continue
            amp = amplitude_from_csi(payload.get("csi", []))
            link_amplitudes[link].append(amp)

        # Store spread alongside the mean. Without it there is no way to tell
        # a real change from this link's ordinary jitter, and CSI amplitude
        # is noisy enough that the difference matters: measured here, a link's
        # 5s window mean wanders by ~0.45 on a mean of ~20 with nothing moving.
        for link, amps in link_amplitudes.items():
            if amps:
                baseline[link] = (float(np.mean(amps)), float(np.std(amps)), len(amps))
            else:
                baseline[link] = (0.0, 0.0, 0)
        print(f"[aggregator] Calibration complete. {len(baseline)} links baselined.")
        noisy = [k for k, v in baseline.items() if v[0] > 0 and v[1] / v[0] > 0.15]
        if noisy:
            print(
                f"[aggregator] NOTE: {len(noisy)}/{len(baseline)} links vary by "
                f">15% at rest; they will contribute little."
            )
        with open("rti_baseline.json", "w") as f:
            json.dump({f"{k[0]}|{k[1]}": list(v) for k, v in baseline.items()}, f, indent=2)
        print("[aggregator] Saved baseline to rti_baseline.json")
        return

    # Load baseline if it exists
    try:
        with open("rti_baseline.json", "r") as f:
            raw = json.load(f)
        for k, v in raw.items():
            tx_str, rx_str = k.split("|")
            # Older baselines stored a bare mean; treat their spread as
            # unknown so they still load.
            if isinstance(v, (int, float)):
                v = (float(v), 0.0, 0)
            baseline[(eval(tx_str), eval(rx_str))] = tuple(v)
        print(
            f"[aggregator] Loaded {len(baseline)} baseline links from rti_baseline.json"
        )
    except FileNotFoundError:
        print(
            "[aggregator] WARNING: no baseline found. Run with --calibrate first for meaningful results."
        )

    # Drop links too noisy to contribute. They are not merely unhelpful:
    # an unstable link presents large deviations every frame, and with a
    # rank-deficient system those dominate the reconstruction.
    unstable = {
        l for l, (mean, std, _) in baseline.items()
        if mean > 0 and std / mean > args.max_link_cv
    }
    if unstable:
        for l in unstable:
            del baseline[l]
        print(
            f"[aggregator] Excluded {len(unstable)} link(s) varying >"
            f"{args.max_link_cv:.0%} at rest; {len(baseline)} remain."
        )

    if args.evaluate:
        run_evaluation(sock, baseline, args)
        return

    sock.settimeout(1.0)
    last_image_time = time.time()
    live_amps = defaultdict(list)

    print("[aggregator] Running. Printing a coarse image every 3s.")
    try:
        while True:
            try:
                data, addr = sock.recvfrom(4096)
            except socket.timeout:
                pass
            else:
                try:
                    payload = json.loads(data.decode("utf-8", errors="ignore"))
                except Exception:
                    payload = None
                if payload:
                    link = identify_link(addr[0], payload)
                    if link is not None:
                        amp = amplitude_from_csi(payload.get("csi", []))
                        live_amps[link].append(amp)

            if time.time() - last_image_time > args.window_seconds:
                # Need enough samples for a window mean to mean anything.
                links = [
                    l for l in live_amps.keys() if l in baseline and len(live_amps[l]) >= 5
                ]
                if len(links) >= 3:
                    # Deviation in standard errors, not raw amplitude.
                    #
                    # Raw amplitude cannot be thresholded: links differ in
                    # how noisy they are, so a quiet link moving slightly and
                    # a noisy link idling look identical. Dividing by each
                    # link's own standard error puts them on one scale and
                    # makes "is this bigger than the jitter" answerable.
                    zs = []
                    for l in links:
                        base_mean, base_std, _ = baseline[l]
                        vals = live_amps[l]
                        live_mean = float(np.mean(vals))
                        # Standard error of this window's mean.
                        sem = base_std / np.sqrt(len(vals)) if base_std > 0 else 0.0
                        if sem <= 1e-9:
                            zs.append(0.0)
                        else:
                            zs.append((base_mean - live_mean) / sem)
                    deviations = np.array(zs, dtype=np.float32)
                    peak_z = float(np.max(np.abs(deviations))) if len(deviations) else 0.0
                    if peak_z < args.z_threshold:
                        # Everything is within its resting jitter. Rendering
                        # here would auto-scale noise into a confident-looking
                        # picture, which is worse than saying nothing.
                        print(
                            f"\n[aggregator] no significant change "
                            f"(peak {peak_z:.1f} sigma < {args.z_threshold:.1f}, "
                            f"{len(links)} links)"
                        )
                        live_amps.clear()
                        last_image_time = time.time()
                        continue

                    W = rti_weight_matrix(links, xs, ys, ELLIPSE_WIDTH)
                    W = W * room_mask  # zero the columns outside the room
                    image = reconstruct_image(W, deviations, alpha=args.alpha)
                    grid = image.reshape(len(xs), len(ys))
                    # Print a crude ASCII heatmap for a quick sanity check
                    print(
                        f"\n[aggregator] --- Room image ({len(links)} active links) ---"
                    )
                    print(
                        f"[aggregator] raw deviation stats: min={grid.min():.4f} max={grid.max():.4f} "
                        f"std={grid.std():.4f} range={grid.ptp():.4f}"
                    )
                    strongest = sorted(
                        zip(links, deviations), key=lambda p: -abs(p[1])
                    )[:5]
                    print(
                        "[aggregator] strongest links (sigma): "
                        + ", ".join(f"{l}={d:+.1f}" for l, d in strongest)
                    )
                    mask_grid = room_mask.reshape(len(xs), len(ys))

                    peak, centroid, spread = locate_change(
                        grid, xs, ys, mask_grid
                    )
                    if centroid is not None:
                        bearing = describe_direction(
                            centroid[0], centroid[1], args.room_width, args.room_height
                        )
                        # Spread against room scale decides how much to claim.
                        room_scale = np.hypot(args.room_width, args.room_height)
                        if spread < room_scale * 0.15:
                            quality = "LOCALIZED"
                        elif spread < room_scale * 0.30:
                            quality = "approximate"
                        else:
                            quality = "direction only"
                        print(
                            f"[aggregator] >>> PRESENCE DETECTED  "
                            f"peak {peak_z:.1f} sigma | "
                            f"centre of change ~({centroid[0]:.1f}, {centroid[1]:.1f})m "
                            f"[{bearing}] | strongest ({peak[0]:.1f}, {peak[1]:.1f})m | "
                            f"spread {spread:.1f}m -> {quality}"
                        )
                    else:
                        print(
                            f"[aggregator] >>> change detected (peak {peak_z:.1f} sigma) "
                            f"but no positive attenuation to localize"
                        )

                    # Fixed scale, anchored at zero. Rescaling each frame to
                    # its own min/max was the other half of "the image changes
                    # constantly": it stretches whatever is present to full
                    # contrast, so an idle room and an occupied one look
                    # equally dramatic. Tying the ramp to a fixed multiple of
                    # the peak instead means a weak frame renders weakly.
                    scale = max(abs(float(grid[mask_grid].min())),
                                abs(float(grid[mask_grid].max())), 1e-9)
                    chars = " .:-=+*#%@"
                    for row, mrow in zip(grid.T[::-1], mask_grid.T[::-1]):
                        out = []
                        for v, inside in zip(row, mrow):
                            if not inside:
                                out.append(" ")
                                continue
                            frac = max(0.0, float(v) / scale)  # attenuation only
                            out.append(
                                chars[min(int(frac * (len(chars) - 1)), len(chars) - 1)]
                            )
                        print("".join(out))
                else:
                    print(
                        f"[aggregator] Only {len(links)} active links with baseline - need >=3 for imaging."
                    )
                live_amps.clear()
                last_image_time = time.time()
    except KeyboardInterrupt:
        print("\n[aggregator] Stopped.")


if __name__ == "__main__":
    main()
