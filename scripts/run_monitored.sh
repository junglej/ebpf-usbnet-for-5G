#!/usr/bin/env bash
# run_monitored.sh - run ONE monitored dataset on the robot:
# txdwell (eBPF) + DIAG (0xB883/0xB873) + clock offset + iperf3 load.
# Usage on robot: ./run_monitored.sh <name> <outdir> -- <iperf3 args...>
# Example: ./run_monitored.sh udp_overload /tmp/mon -- -u -b 20M -w 64M -t 30
set -uo pipefail

NAME="${1:?name}"; OUT="${2:?outdir}"; shift 2
[ "${1:-}" = "--" ] && shift
IPERF_ARGS=("$@")

IPERF="$HOME/Documents/iperf3.new"
MI="$HOME/Documents/mobileinsight-core"
PY="$MI/venv/bin/python"
TXDWELL="$HOME/usbnet-ebpf/txdwell"
SERVER="10.45.1.1"

mkdir -p "$OUT"
echo robot | sudo -S true || { echo "sudo failed"; exit 1; }
sudo sysctl -w net.core.wmem_max=134217728 >/dev/null

# ModemManager must be stopped for DIAG; NM removes wwan0 IP/routes, put them back.
sudo systemctl stop ModemManager 2>/dev/null
sleep 1
sudo chmod 666 /dev/ttyUSB0
sudo ip addr replace 10.45.1.2/30 dev wwan0
sudo ip link set wwan0 up mtu 1400
sudo ip route replace 10.45.1.0/30 dev wwan0 scope link
ping -c1 -W3 "$SERVER" >/dev/null || { echo "WARN: ping $SERVER failed"; }

# clock offset monotonic->epoch (valid for this boot)
python3 -c 'import time; m=time.monotonic_ns(); e=time.time_ns(); print((e-m)/1e9)' \
  > "$OUT/${NAME}_offset.txt"

echo "[$NAME] starting DIAG capture (75 s) + txdwell (55 s)"
timeout 75 "$PY" "$MI/bsr_exp/capture_ue_bsr.py" /dev/ttyUSB0 115200 \
  "$OUT/${NAME}.mi2log" > "$OUT/${NAME}_capture.log" 2>&1 &
CAP_PID=$!
sleep 4
# txdwell opens ./txdwell.bpf.o relative to CWD -> run from the repo dir;
# pipe the password directly (cached sudo creds are unreliable in bg subshells)
(cd "$(dirname "$TXDWELL")" && echo robot | sudo -S ./txdwell -i wwan0 -d 55 \
  -o "$OUT/${NAME}.csv" > "$OUT/${NAME}_txdwell.log" 2>&1) &
TX_PID=$!
sleep 5

echo "[$NAME] iperf3 ${IPERF_ARGS[*]} start: $(date +%H:%M:%S)"
date +%s.%N > "$OUT/${NAME}_iperf_start.txt"
"$IPERF" -c "$SERVER" "${IPERF_ARGS[@]}" -J > "$OUT/${NAME}_iperf.json" 2>&1 \
  && echo "[$NAME] iperf3 OK" || echo "[$NAME] iperf3 FAILED"
date +%s.%N > "$OUT/${NAME}_iperf_end.txt"

wait "$TX_PID" 2>/dev/null
wait "$CAP_PID" 2>/dev/null
echo "[$NAME] capture done, extracting..."

"$PY" "$MI/bsr_exp/mi_extract_ue_bsr.py" "$OUT/${NAME}.mi2log" \
  "$OUT/${NAME}_ue_bsr.csv" 2>>"$OUT/${NAME}_extract.log"
"$PY" "$MI/bsr_exp/mi_b883_raw.py" "$OUT/${NAME}.mi2log" \
  > "$OUT/${NAME}_b883_raw.csv" 2>>"$OUT/${NAME}_extract.log"

echo "[$NAME] DONE $(ls -la "$OUT/${NAME}"* | wc -l) files"
