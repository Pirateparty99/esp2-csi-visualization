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

import socket
import json
import time
import argparse
import numpy as np
from collections import defaultdict

STATIONS = {
    "192.168.4.2": (4.0, 0.0),
    "192.168.4.3": (4.0, 3.0),
    "192.168.4.4": (0.0, 3.0),
}

TRANSMITTERS = {
    "e4:65:b8:0f:3e:e5": (0.0, 0.0),
}

IP_TO_POS = STATIONS
MAC_TO_POS = {mac.lower(): pos for mac, pos in TRANSMITTERS.items()}

GRID_RESOLUTION = 0.1   # meters per pixel
ELLIPSE_WIDTH = 1.0     # lambda parameter (meters) - wider = smoother/coarser image


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
    """Return (tx_pos, rx_pos) for a packet, or None if unrecognized."""
    rx_pos = IP_TO_POS.get(source_ip)
    tx_mac = payload.get("mac", "").lower()
    tx_pos = MAC_TO_POS.get(tx_mac)
    if rx_pos is None or tx_pos is None:
        return None
    return (tx_pos, rx_pos)


def build_room_grid(room_width, room_height, resolution):
    xs = np.arange(0, room_width, resolution)
    ys = np.arange(0, room_height, resolution)
    return xs, ys


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
    """
    WtW = W.T @ W
    reg = alpha * np.eye(WtW.shape[0], dtype=np.float32)
    Wty = W.T @ deviations
    image = np.linalg.solve(WtW + reg, Wty)
    return image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=5566)
    parser.add_argument("--bind", type=str, default="0.0.0.0")
    parser.add_argument("--calibrate", action="store_true",
                         help="Record baseline amplitudes with an EMPTY room")
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

    if args.calibrate:
        print(f"[aggregator] CALIBRATING for {args.calibrate_seconds}s — keep the room EMPTY.")
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
        print(f"[aggregator] Loaded {len(baseline)} baseline links from rti_baseline.json")
    except FileNotFoundError:
        print("[aggregator] WARNING: no baseline found. Run with --calibrate first for meaningful results.")

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
                    deviations = np.array([
                        baseline[l] - float(np.mean(live_amps[l])) for l in links
                    ], dtype=np.float32)
                    W = rti_weight_matrix(links, xs, ys, ELLIPSE_WIDTH)
                    image = reconstruct_image(W, deviations)
                    grid = image.reshape(len(xs), len(ys))
                    # Print a crude ASCII heatmap for a quick sanity check
                    print(f"\n[aggregator] --- Room image ({len(links)} active links) ---")
                    normed = (grid - grid.min()) / (grid.ptp() + 1e-6)
                    chars = " .:-=+*#%@"
                    for row in normed.T[::-1]:
                        print("".join(chars[min(int(v * (len(chars) - 1)), len(chars) - 1)] for v in row))
                else:
                    print(f"[aggregator] Only {len(links)} active links with baseline - need >=3 for imaging.")
                live_amps.clear()
                last_image_time = time.time()
    except KeyboardInterrupt:
        print("\n[aggregator] Stopped.")


if __name__ == "__main__":
    main()