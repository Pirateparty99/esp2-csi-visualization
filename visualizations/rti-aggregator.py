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
    reg = alpha * np.eye(num_links, dtype=np.float32)
    WWt = W @ W.T
    return W.T @ np.linalg.solve(WWt + reg, deviations)


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

        for link, amps in link_amplitudes.items():
            baseline[link] = float(np.mean(amps)) if amps else 0.0
        print(f"[aggregator] Calibration complete. {len(baseline)} links baselined.")
        with open("rti_baseline.json", "w") as f:
            json.dump({f"{k[0]}|{k[1]}": v for k, v in baseline.items()}, f, indent=2)
        print("[aggregator] Saved baseline to rti_baseline.json")
        return

    # Load baseline if it exists
    try:
        with open("rti_baseline.json", "r") as f:
            raw = json.load(f)
        for k, v in raw.items():
            tx_str, rx_str = k.split("|")
            baseline[(eval(tx_str), eval(rx_str))] = v
        print(
            f"[aggregator] Loaded {len(baseline)} baseline links from rti_baseline.json"
        )
    except FileNotFoundError:
        print(
            "[aggregator] WARNING: no baseline found. Run with --calibrate first for meaningful results."
        )

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

            if time.time() - last_image_time > 3.0:
                links = [l for l in live_amps.keys() if l in baseline]
                if len(links) >= 3:
                    deviations = np.array(
                        [baseline[l] - float(np.mean(live_amps[l])) for l in links],
                        dtype=np.float32,
                    )
                    W = rti_weight_matrix(links, xs, ys, ELLIPSE_WIDTH)
                    W = W * room_mask  # zero the columns outside the room
                    image = reconstruct_image(W, deviations)
                    grid = image.reshape(len(xs), len(ys))
                    # Print a crude ASCII heatmap for a quick sanity check
                    print(
                        f"\n[aggregator] --- Room image ({len(links)} active links) ---"
                    )
                    print(
                        f"[aggregator] raw deviation stats: min={grid.min():.4f} max={grid.max():.4f} "
                        f"std={grid.std():.4f} range={grid.ptp():.4f}"
                    )
                    print(
                        f"[aggregator] link deviations: "
                        + ", ".join(f"{l}={d:.3f}" for l, d in zip(links, deviations))
                    )
                    mask_grid = room_mask.reshape(len(xs), len(ys))
                    # Scale against in-room pixels only; masked ones are
                    # identically zero and would otherwise skew the range.
                    inside_vals = grid[mask_grid]
                    lo = float(inside_vals.min()) if inside_vals.size else 0.0
                    span = float(inside_vals.ptp()) if inside_vals.size else 0.0
                    normed = (grid - lo) / (span + 1e-6)
                    chars = " .:-=+*#%@"
                    for row, mrow in zip(normed.T[::-1], mask_grid.T[::-1]):
                        print(
                            "".join(
                                chars[min(int(v * (len(chars) - 1)), len(chars) - 1)]
                                if inside else " "
                                for v, inside in zip(row, mrow)
                            )
                        )
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
