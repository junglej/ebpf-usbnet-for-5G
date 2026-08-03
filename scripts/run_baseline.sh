#!/usr/bin/env bash
# run_baseline.sh - ablation baseline on the robot: 10x UDP + 10x TCP, 10 s each,
# no monitoring. Saves iperf3 JSON per round to the given output dir (on robot).
# Usage on robot: ./run_baseline.sh <outdir>
set -uo pipefail

OUT="${1:?usage: run_baseline.sh <outdir>}"
IPERF="$HOME/Documents/iperf3.new"
SERVER="10.45.1.1"
ROUNDS=10
DUR=10
GAP=5

mkdir -p "$OUT"
echo robot | sudo -S sysctl -w net.core.wmem_max=134217728 >/dev/null

echo "=== UDP baseline: ${ROUNDS}x ${DUR}s, -u -b 20M -w 64M ==="
for i in $(seq 1 "$ROUNDS"); do
  echo "--- UDP round $i/$(date +%H:%M:%S) ---"
  "$IPERF" -c "$SERVER" -u -b 20M -w 64M -t "$DUR" -J > "$OUT/udp_$i.json" 2>&1
  tail -c 200 "$OUT/udp_$i.json" | grep -q '"sum_received"' \
    && echo "udp_$i OK" || echo "udp_$i FAILED"
  sleep "$GAP"
done

echo "=== TCP baseline: ${ROUNDS}x ${DUR}s, default cubic ==="
for i in $(seq 1 "$ROUNDS"); do
  echo "--- TCP round $i/$(date +%H:%M:%S) ---"
  "$IPERF" -c "$SERVER" -t "$DUR" -J > "$OUT/tcp_$i.json" 2>&1
  tail -c 200 "$OUT/tcp_$i.json" | grep -q '"sum_received"' \
    && echo "tcp_$i OK" || echo "tcp_$i FAILED"
  sleep "$GAP"
done

echo "ALL DONE"
