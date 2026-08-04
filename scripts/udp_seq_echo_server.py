#!/usr/bin/env python3
"""UDP echo server for udp_rtt_probe.py / udp_seq_sender.py.

Listens on a UDP port; for every received packet immediately sends the
payload back unchanged to the source address (echo). Packets carrying the
custom "{seq}||{send_timestamp_ns}" prefix are also logged one per line.

Run where UE traffic lands (5gc container, ogstun 10.45.1.1):
    python3 udp_seq_echo_server.py              # listen on 0.0.0.0:5000
    python3 udp_seq_echo_server.py -p 5000 --quiet
"""

import argparse
import socket
import sys
import time


def parse_payload(data: bytes):
    sep = data.find(b"||")
    if sep <= 0:
        return None
    try:
        seq = int(data[:sep])
    except ValueError:
        return None
    end = sep + 2
    while end < len(data) and data[end:end + 1].isdigit():
        end += 1
    try:
        ts = int(data[sep + 2:end])
    except ValueError:
        return None
    return seq, ts


def main():
    ap = argparse.ArgumentParser(description="UDP echo server ({seq}||{ts} aware)")
    ap.add_argument("-p", "--port", type=int, default=5000, help="UDP port to listen on (default 5000)")
    ap.add_argument("-b", "--bind", default="0.0.0.0", help="bind address (default 0.0.0.0)")
    ap.add_argument("--quiet", action="store_true", help="only print the final summary")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    print(f"echo server listening on {args.bind}:{args.port} ...", flush=True)

    n_echo = n_parsed = 0
    try:
        while True:
            data, addr = sock.recvfrom(65535)
            sock.sendto(data, addr)  # echo back immediately, payload unchanged
            n_echo += 1
            parsed = parse_payload(data)
            if parsed is not None:
                n_parsed += 1
                if not args.quiet:
                    print(f"seq={parsed[0]} size={len(data)} send_ts={parsed[1]} "
                          f"recv_ts={time.time_ns()} from={addr[0]}:{addr[1]}", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        print(f"\nechoed={n_echo} parsed={n_parsed}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
