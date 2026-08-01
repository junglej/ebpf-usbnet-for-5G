#!/usr/bin/env bash
# record.sh - run txdwell on the robot and store a CSV recording.
#
# Usage:
#   ./scripts/record.sh [duration_s] [outfile.csv] [-- extra txdwell args]
# Examples:
#   ./scripts/record.sh 60                       # 60 s, auto-named CSV, all ifaces
#   ./scripts/record.sh 60 data/run1.csv -i wwan0 -v
set -euo pipefail
cd "$(dirname "$0")/.."

DUR="${1:-30}"
OUT="${2:-data/txdwell_$(date +%Y%m%d_%H%M%S).csv}"
if [ $# -ge 2 ]; then shift 2; elif [ $# -ge 1 ]; then shift 1; fi

mkdir -p "$(dirname "$OUT")"
echo "recording ${DUR}s -> $OUT"
sudo ./txdwell -d "$DUR" -o "$OUT" "$@"
echo "saved: $OUT"
