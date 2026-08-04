#!/usr/bin/env python3
"""UDP echo-RTT probe, wire-compatible with udp_seq_sender.py.

Sends "{seq}||{send_timestamp_ns}" + 'a' padding packets (same format as
udp_seq_sender.py, so the gNB RLC UL_PKT inspector still parses them and
each probe can be correlated with gNB-side UL_PKT logs by seq), receives
the echoes from udp_seq_echo_server.py on the same socket, and computes
per-packet RTT entirely on the local (UE) clock -- no NTP/PTP needed.

Under UL-only load the DL direction is idle, so RTT inflation above the
idle baseline is essentially the UL queue (driver + modem buffer + air).

Run on the UE (robot):
    python3 udp_rtt_probe.py 10.45.1.1 -n 300 -i 0.1 --csv /tmp/rtt.csv

On exit prints loss and RTT statistics.
"""

import argparse
import select
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
    ap = argparse.ArgumentParser(description="UDP echo-RTT probe ({seq}||{ts} format)")
    ap.add_argument("dst", help="destination IP (echo server, e.g. 10.45.1.1)")
    ap.add_argument("-p", "--port", type=int, default=5000, help="destination UDP port (default 5000)")
    ap.add_argument("-n", "--count", type=int, default=100, help="number of probes (default 100)")
    ap.add_argument("-i", "--interval", type=float, default=0.1, help="interval between probes in seconds (default 0.1)")
    ap.add_argument("-s", "--size", type=int, default=1000, help="payload size in bytes (default 1000)")
    ap.add_argument("--csv", help="append per-packet records to this CSV file")
    ap.add_argument("--drain", type=float, default=3.0, help="seconds to wait for late echoes after last send (default 3)")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setblocking(False)
    sock.connect((args.dst, args.port))  # connected: only accept echoes from the server

    csv_f = open(args.csv, "a", buffering=1) if args.csv else None
    if csv_f:
        csv_f.write("seq,size,send_ts_ns,recv_ts_ns,rtt_ms\n")

    rtts = {}      # seq -> rtt_ms (computed on CLOCK_MONOTONIC, immune to
                   # the realtime clock steps seen on this testbed)
    send_mono = {}  # seq -> time.monotonic_ns() at send
    sent = 0

    def pump(deadline):
        while True:
            now = time.monotonic()
            if now >= deadline:
                return
            r, _, _ = select.select([sock], [], [], deadline - now)
            if not r:
                return
            try:
                data = sock.recv(65535)
            except BlockingIOError:
                continue
            recv_mono = time.monotonic_ns()
            recv_ts = time.time_ns()
            parsed = parse_payload(data)
            if parsed is None:
                continue
            seq, send_ts = parsed
            if seq not in rtts and seq in send_mono:
                rtts[seq] = (recv_mono - send_mono[seq]) / 1e6
                if csv_f:
                    csv_f.write(f"{seq},{len(data)},{send_ts},{recv_ts},{rtts[seq]:.6f}\n")

    print(f"probing {args.dst}:{args.port} n={args.count} interval={args.interval}s size={args.size}B")
    try:
        t0 = time.monotonic()
        while sent < args.count:
            header = f"{sent}||{time.time_ns()}".encode()
            if len(header) > args.size:
                print(f"error: header ({len(header)}B) larger than packet size", file=sys.stderr)
                return 1
            send_mono[sent] = time.monotonic_ns()
            sock.send(header + b"a" * (args.size - len(header)))
            sent += 1
            pump(t0 + sent * args.interval)
        pump(time.monotonic() + args.drain)
    except KeyboardInterrupt:
        pass

    n = len(rtts)
    lost = sent - n
    print(f"\nsent={sent} echoed={n} lost={lost} ({100.0 * lost / max(sent, 1):.1f}%)")
    if n:
        vals = sorted(rtts.values())
        avg = sum(vals) / n
        print(f"rtt_ms: min={vals[0]:.3f} avg={avg:.3f} p50={vals[n // 2]:.3f} "
              f"p99={vals[min(n - 1, int(n * 0.99))]:.3f} max={vals[-1]:.3f}")
    if csv_f:
        csv_f.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
