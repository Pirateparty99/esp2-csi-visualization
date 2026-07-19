#!/usr/bin/env python3
"""
UDP relay for the Pi AP node.

Listens on the Pi's AP-side interface and forwards each received packet
unchanged to the upstream server.
"""

import socket
import argparse

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen-port", type=int, default=5566,
                         help="Port ESP32s send to (default: 5566)")
    parser.add_argument("--bind", type=str, default="0.0.0.0")
    parser.add_argument("--upstream-ip", type=str, required=True,
                         help="Upstream server's real IP address on its actual network")
    parser.add_argument("--upstream-port", type=int, default=5566)
    args = parser.parse_args()

    recv_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    recv_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    recv_sock.bind((args.bind, args.listen_port))

    send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    print(f"[pi-relay] Listening on {args.bind}:{args.listen_port}")
    print(f"[pi-relay] Forwarding to upstream server at {args.upstream_ip}:{args.upstream_port}")
    print("[pi-relay] Ctrl+C to stop")

    packet_count = 0
    try:
        while True:
            data, addr = recv_sock.recvfrom(4096)
            packet_count += 1
            try:
                send_sock.sendto(data, (args.upstream_ip, args.upstream_port))
            except Exception as e:
                print(f"[pi-relay] Error forwarding to upstream server: {e}")
            if packet_count % 50 == 0:
                print(f"[pi-relay] Relayed {packet_count} packets so far (last from {addr})")
    except KeyboardInterrupt:
        print(f"\n[pi-relay] Stopped. Total packets relayed: {packet_count}")
    finally:
        recv_sock.close()
        send_sock.close()

if __name__ == "__main__":
    main()