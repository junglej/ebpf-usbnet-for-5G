#!/usr/bin/env python3
"""Plot the canonical 4-panel aligned figure for a txdwell + DIAG + gNB dataset.

Panels: (1) driver dwell per packet (txdwell DEQ), (2) driver in-flight
packets, (3) driver DEQ rate vs modem air new-TB rate (0xB883, 0.5 s bins),
(4) UE BSR from DIAG (0xB873, line) vs from gNB MAC log (SBSR, dots).

Inputs (all under data/ by default):
  <name>.csv              txdwell recording (DEQ rows)
  <name>_ue_bsr.csv       UE-side BSR extracted from mi2log (epoch,...,bsr_index)
  b883_pusch_<name>.csv   per-PUSCH-TB records from scripts/decode_b883.py
  <name>_gnb_sbsr.txt     gNB MAC log lines containing 'SBSR: lcg=N bs=M'

Clock model: txdwell ts = CLOCK_MONOTONIC ns on the robot, converted with
--off (monotonic->epoch offset sampled on the robot via
`python3 -c 'import time; print(time.monotonic_ns(), time.time_ns())'`).

Usage:
  python3 scripts/plot_aligned.py aligned --off 1784793310.014
  python3 scripts/plot_aligned.py overload --off 1784793310.014
"""
import argparse
import datetime
import re
import statistics
import math
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("name", help="dataset prefix: aligned | overload | ...")
    ap.add_argument("--off", type=float, required=True,
                    help="monotonic->epoch offset (seconds)")
    ap.add_argument("--data", default="data", help="data directory")
    ap.add_argument("--xmax", type=float, default=62, help="x axis limit (s)")
    args = ap.parse_args()
    d = f"{args.data}/{args.name}"

    ue = []
    for line in open(f"{d}_ue_bsr.csv"):
        if line.startswith("epoch"):
            continue
        p = line.strip().split(",")
        ue.append((float(p[0]), int(p[6])))

    gnb = []
    for line in open(f"{d}_gnb_sbsr.txt"):
        m = re.match(r"(\S+) .*SBSR: lcg=\d+ bs=(\d+)", line)
        if m:
            gnb.append((datetime.datetime.strptime(
                m.group(1)[:26], "%Y-%m-%dT%H:%M:%S.%f")
                .replace(tzinfo=datetime.timezone.utc).timestamp(),
                int(m.group(2))))

    drows = []
    for line in open(f"{d}.csv"):
        if line.startswith("#") or line.startswith("STATS"):
            continue
        p = line.strip().split(",")
        if len(p) == 6 and p[0] == "DEQ":
            drows.append((int(p[1]) / 1e9 + args.off, int(p[3]),
                          int(p[4]) / 1e6, int(p[5])))

    tb = []
    for line in open(f"{args.data}/b883_pusch_{args.name}.csv"):
        if line.startswith("epoch"):
            continue
        p = line.strip().split(",")
        tb.append((float(p[0]), int(p[7]), int(p[9])))

    t0 = drows[0][0]
    BW = 0.5

    def bof(ts):
        return int((ts - t0) / BW)

    deqb, airb, retb = defaultdict(int), defaultdict(int), defaultdict(int)
    ub, gb = defaultdict(list), defaultdict(list)
    for ts, l, _, _ in drows:
        deqb[bof(ts)] += l
    for ts, s, tt in tb:
        (airb if tt == 0 else retb)[bof(ts)] += s
    for ts, v in ue:
        ub[bof(ts)].append(v)
    for ts, v in gnb:
        gb[bof(ts)].append(v)

    bs = sorted(set(list(deqb) + list(airb) + list(ub) + list(gb)))
    bs = [b for b in bs if -6 <= b * BW <= args.xmax]
    tt_ = [b * BW for b in bs]
    deq_r = [deqb[b] * 8 / BW / 1e6 for b in bs]
    air_r = [airb[b] * 8 / BW / 1e6 for b in bs]
    ux = [b * BW for b in bs if ub.get(b)]
    uy = [statistics.median(ub[b]) for b in bs if ub.get(b)]
    gx = [b * BW for b in bs if gb.get(b)]
    gy = [statistics.median(gb[b]) for b in bs if gb.get(b)]
    dt = [ts - t0 for ts, _, _, _ in drows]
    dd = [dw for _, _, dw, _ in drows]
    di = [i for _, _, _, i in drows]

    fig, ax = plt.subplots(4, 1, figsize=(11, 11), sharex=True)
    ax[0].scatter(dt, dd, s=1.5)
    ax[0].set_ylabel("driver dwell (ms)")
    ax[0].set_ylim(-5, 220)
    ax[1].step(dt, di, lw=.8, color="tab:red")
    ax[1].set_ylabel("driver in-flight (pkts)")
    ax[2].plot(tt_, deq_r, label="driver DEQ rate", lw=1.4)
    ax[2].plot(tt_, air_r, label="modem air new-TB (0xB883)", lw=1.4)
    ax[2].set_ylabel("Mbps")
    ax[2].legend(loc="upper right")
    ax[3].scatter(gx, gy, s=16, label="UE BSR seen by gNB (SBSR)",
                  color="tab:purple", zorder=2)
    ax[3].plot(ux, uy, label="UE BSR from DIAG (0xB873)",
               color="tab:orange", lw=1.2, alpha=.85, zorder=3)
    ax[3].set_ylabel("BSR index\n(31 = >150KB)")
    ax[3].set_ylim(-1, 32)
    ax[3].legend(loc="center right")
    ax[3].set_xlabel("time (s)")
    for a in ax:
        a.grid(alpha=.3)
        a.set_xlim(-3, args.xmax)
    ax[0].set_title(f"{args.name}: eBPF driver queue / modem air (DIAG 0xB883) / BSR both ends")
    fig.tight_layout()
    out = f"{d}.png"
    fig.savefig(out, dpi=130)

    # summary stats
    xs, ys = [], []
    for b in airb:
        if airb[b] > 0 and deqb[b] > 0:
            xs.append(deqb[b] * 8 / BW / 1e6)
            ys.append(airb[b] * 8 / BW / 1e6)
    mx = sum(xs) / len(xs)
    my = sum(ys) / len(ys)
    cov = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sx = math.sqrt(sum((x - mx) ** 2 for x in xs))
    sy = math.sqrt(sum((y - my) ** 2 for y in ys))
    tb_end = tb[-1][0]
    deq_end = drows[-1][0]
    deq_in = sum(l for ts, l, _, _ in drows if ts <= tb_end)
    air_in = sum(s for ts, s, t2 in tb if t2 == 0)
    print(f"saved {out}")
    print(f"DIAG coverage past last DEQ: {(tb_end - deq_end):.2f}s")
    print(f"corr(driver DEQ, modem air): R={cov / (sx * sy):.3f} over {len(xs)} bins")
    print(f"byte closure air/DEQ = {air_in / deq_in:.3f}")


if __name__ == "__main__":
    main()
