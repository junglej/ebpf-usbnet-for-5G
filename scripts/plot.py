#!/usr/bin/env python3
"""Plot usbnet TX dwell time and in-flight queue depth from a txdwell CSV.

Style follows Aether (SIGCOMM'26) Fig. 4a: per-packet driver queue dwell
time over time, with driver queue length (in-flight packets) on a second
panel sharing the time axis.

Run locally (the robot does not need python/matplotlib):

    python3 scripts/plot.py data/run1.csv -o run1.png
"""
import argparse
import csv
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("csv", help="txdwell CSV file")
    ap.add_argument("-o", "--output", default=None,
                    help="output PNG (default: <csv>.png)")
    ap.add_argument("--log", action="store_true",
                    help="log scale for dwell time")
    args = ap.parse_args()

    t0 = None
    ts, dwell_ms, inflight = [], [], []
    with open(args.csv, newline="") as f:
        for row in csv.reader(f):
            if not row or row[0].startswith("#") or row[0] != "DEQ":
                continue
            t = int(row[1]) / 1e9
            if t0 is None:
                t0 = t
            ts.append(t - t0)
            dwell_ms.append(int(row[4]) / 1e6)
            inflight.append(int(row[5]))

    if not ts:
        sys.exit(f"no DEQ events found in {args.csv}")

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, (ax1, ax2) = plt.subplots(
        2, 1, figsize=(10, 6), sharex=True,
        gridspec_kw={"height_ratios": [2, 1]})

    ax1.scatter(ts, dwell_ms, s=3, alpha=0.5, c="tab:blue")
    ax1.set_ylabel("TX queue dwell time (ms)")
    if args.log:
        ax1.set_yscale("log")
    ax1.grid(True, alpha=0.3)
    ax1.set_title(f"usbnet driver TX queue dwell ({args.csv})")

    ax2.step(ts, inflight, where="post", c="tab:red", lw=0.8)
    ax2.set_ylabel("in-flight packets")
    ax2.set_xlabel("time (s)")
    ax2.grid(True, alpha=0.3)

    fig.tight_layout()
    out = args.output or (args.csv.rsplit(".", 1)[0] + ".png")
    fig.savefig(out, dpi=150)
    print(f"saved: {out}  ({len(ts)} DEQ events, "
          f"dwell median {sorted(dwell_ms)[len(dwell_ms)//2]:.3f} ms, "
          f"max {max(dwell_ms):.3f} ms)")


if __name__ == "__main__":
    main()
