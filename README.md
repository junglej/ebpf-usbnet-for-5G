# txdwell — usbnet TX queue dwell-time / queue-length observer

eBPF (CO-RE) tool reproducing the core observation of **Aether (SIGCOMM'26)**:
for a USB cellular modem driven by `usbnet` (Quectel RM500Q / `qmi_wwan`),
measure the **driver-level TX queue dwell time** and **queue length**
(in-flight packets) as an uplink congestion signal.

## Hook design (verified against Linux 6.8 sources)

`drivers/net/usb/usbnet.c` @ v6.8:

- **enqueue** = `usbnet_start_xmit(struct sk_buff *skb, struct net_device *net)`
  — the `ndo_start_xmit` of every usbnet minidriver. `qmi_wwan` sets
  `.ndo_start_xmit = usbnet_start_xmit` and, importantly, **has no
  `tx_fixup` in 6.8**, so the `skb` pointer seen at function entry is exactly
  the pointer handed to the URB below.
- **dequeue** = `static void tx_complete(struct urb *urb)` — the TX URB
  completion callback. The skb is recovered from `urb->context`:
  `usb_fill_bulk_urb(urb, dev->udev, dev->out, skb->data, skb->len,
  tx_complete, skb)`; `tx_complete` starts with
  `struct sk_buff *skb = (struct sk_buff *) urb->context;`.
- **dwell time** = `tx_complete` entry − `usbnet_start_xmit` entry, correlated
  per skb pointer. **Queue length** = in-flight count (enqueue +1 /
  completion −1), also kept in bytes.

Attach method: `tx_complete` is a generic name (other modules have their
own), so both probes are attached **by address** with libbpf
`bpf_program__attach_kprobe_multi_opts()` (`opts.addrs`), after resolving
`usbnet_start_xmit [usbnet]` / `tx_complete [usbnet]` in `/proc/kallsyms`
(root required). Never attach by name.

`struct urb` note: on the target (Ubuntu 22.04, kernel 6.8.0-134-generic)
usbcore is built into vmlinux — there is **no** `/sys/kernel/btf/usbcore` —
so `struct urb` is in the vmlinux BTF and CO-RE reads of `urb->context`
work with the plain generated `vmlinux.h`.

Known behaviors / caveats:

- If the device is runtime-suspended (`EVENT_DEV_ASLEEP`), usbnet anchors
  the URB (`usb_anchor_urb(&dev->deferred)`) and submits it at resume; dwell
  then legitimately includes the sleep. Rare under load.
- If `usb_submit_urb()` fails synchronously, the skb is freed without a
  completion; that entry leaks in `skb_map` and inflight stays +1. This
  essentially does not happen in steady state; watch the counters.
- TX completions via `usb_unlink_urb()` (tx timeout, halt, disconnect)
  still run `tx_complete`, so they are accounted.
- Skbs already in flight when the tool starts have no enqueue record;
  their completions bump the `miss` counter once.
- Drivers *with* a `tx_fixup` (e.g. cdc_ncm) may replace the skb after our
  enqueue probe; those would show up as `miss`. Not the case for qmi_wwan.

## Files

- `src/txdwell.bpf.c` — BPF side: kprobes, skb hash, ringbuf (256 KB),
  config map, atomic stats counters (in-flight pkts/bytes, totals, misses).
- `src/txdwell.c` — userspace loader: kallsyms resolution, kprobe_multi
  attach by address, ringbuf → CSV, 1 s `STATS` lines, SIGINT summary.
- `src/txdwell.h` — shared event/config/stat definitions.
- `Makefile` — builds `vmlinux.h` (bpftool BTF dump), the BPF object
  (clang `-target bpf`, CO-RE) and the loader (static libbpf).
- `scripts/setup_robot.sh` — idempotent robot setup: apt deps
  (clang, libelf-dev, zlib1g-dev), libbpf v1.4.5 source build into
  `~/libbpf`, `vmlinux.h`, `make`.
- `scripts/record.sh` — timed recording to CSV (runs on the robot).
- `scripts/plot.py` — dwell time + in-flight depth over time from a CSV
  (Aether Fig. 4a style). Runs locally; robot needs no python.

## Build (on the robot)

```sh
git clone <this repo> ~/usbnet-ebpf        # or rsync it over
cd ~/usbnet-ebpf
./scripts/setup_robot.sh                   # sudo password if asked
```

The Makefile expects libbpf at `../libbpf` (matches `~/libbpf` when the
repo is `~/usbnet-ebpf`); override with `make LIBBPF_DIR=/path/to/libbpf`.
The system libbpf (0.5.0) is too old and is not used.

## Run (on the robot, root)

```sh
sudo ./txdwell                    # all usbnet ifaces, DEQ + STATS, stdout
sudo ./txdwell -i wwan0 -v        # filter interface, also emit ENQ events
sudo ./txdwell -d 60 -o run.csv   # timed recording to file
./scripts/record.sh 60 data/run1.csv -i wwan0
```

CSV columns:

```
ENQ,ts_ns,ifindex,len,0,inflight
DEQ,ts_ns,ifindex,len,dwell_ns,inflight
STATS,ts_ns,inflight_pkts,inflight_bytes,enq_total,deq_total,miss,insert_fail,rb_lost
```

`ts_ns` is `CLOCK_MONOTONIC` (same clock as `bpf_ktime_get_ns`).

Then locally: `python3 scripts/plot.py data/run1.csv`.

## Status

Validated on the robot (kernel 6.8.0-134-generic, 2026-07-30):

- Build OK (clang 14 + libbpf v1.4.5 static, CO-RE against vmlinux BTF).
- Both kprobes attach **by address** via kprobe_multi; no verifier errors.
- No traffic → counters all zero, as expected. (fprobe-based attaches do
  not create tracefs kprobe events, so `/sys/kernel/tracing/kprobe_profile`
  does not list them; use the STATS counters as the hit indicator.)
- RM500Q present (`2c7c:0800`, qmi_wwan bound, `wwan0`), but NetworkManager
  is stuck in `connecting (prepare)` — **no data session**, and without one
  the modem does not service the bulk-OUT endpoint, so submitted URBs just
  sit pending (observed: ENQ without DEQ, inflight grows).
- ENQ path validated with real packets on `wwan0` (NM control traffic):
  ifindex filter works, lengths/inflight tracked.
- DEQ path validated via the unlink flush on `ip link set wwan0 down`:
  every pending URB completes through `tx_complete` (-ECONNRESET), dwell
  times matched each packet's exact queue residency (max 4.21 s),
  `urb->context` correlation 5/5, inflight drained 4→3→2→1→0. Pre-observer
  in-flight skbs correctly accounted as `miss`.

### Data session blocker (diagnosed 2026-07-30)

The modem cannot register to any network in its current environment:

- SIM is a **test SIM**: IMSI `001011234567895` → home PLMN **001-01**
  (the test PLMN the private base station must broadcast).
- Full band scan (`AT+COPS=?`) sees only Hong Kong commercial networks
  (CMHK 454-12, CSL 454-00/454-19, "3" 454-03, SmarTone 454-06), all
  reported **forbidden** for this SIM; serving cell `LIMSRV`
  (emergency-only). PLMN 001-01 is **not on air** → private base station
  is off or out of range.
- `qmicli --wds-start-network="apn='internet',ip-type=4"` →
  `CallFailed, call end reason: generic-no-service ([cm] no-service)`.
- Conclusion: dwell-time curves under real uplink load require the
  private base station (PLMN 001-01) to be powered on first. Everything
  else (tool chain, ENQ/DEQ correlation, dwell math, CSV/plot) is ready
  and validated.

Host changes made during bring-up (robot): `libqmi-utils` installed via
apt; ModemManager service restarted; wwan0 brought up/down during tests
(restored to DOWN, no IP); no routes touched (default stays on wlo1,
192.168.5.0/24 on enp100s0).

## Bring-up checklist for a real data session

1. Power on the private base station broadcasting PLMN **001-01** (the
   test SIM's home network; commercial HK networks reject this SIM).
   Confirm with `mmcli -m 0` → state `registered`, or
   `AT+COPS?` → `+COPS: 0,2,"00101"` / `AT+CEREG?` → `0,1`.
2. Establish the session: NetworkManager/ModemManager
   (`nmcli connection up internet`), or
   `qmicli -d /dev/cdc-wdm0 --wds-start-network="apn='internet',ip-type=4" --client-no-release-cid`
   then `--wds-get-current-settings` and configure wwan0 accordingly
   (set `raw_ip` first if needed, interface must be down to change it).
2. Start observer: `sudo ./txdwell -i wwan0 -v -d 60 -o /tmp/t.csv`
   (or `./scripts/record.sh 60 data/run1.csv -i wwan0`).
3. Generate uplink traffic: `ping -I wwan0 <gw>` or iperf3 uplink.
4. Check the `summary:` line: `miss` ~0, `enq ≈ deq + inflight`.
5. Copy CSV back, `python3 scripts/plot.py data/run1.csv` for the
   Fig. 4a-style dwell / queue-length curves.

## Experimental results (2026-07-31, private 5G on air)

Setup: ocudu gNB (USRP X310, n78 TDD 20 MHz) + open5gs on the gNB host
(`~/Documents/ocudu/start_host.sh`), UE = RM500Q on the robot,
PLMN 001-01, UE IP `10.45.1.2/30`, gateway `10.45.1.1`. Traffic:
`udp_seq_sender.py` (1000 B datagrams) and iperf3 from the UE.

Observed with txdwell on `wwan0` (healthy modem, `data/run3.csv` +
live probes):

- **Idle baseline**: dwell ≈ 0.02–0.15 ms, in-flight 0 — the modem
  consumes packets immediately, no driver queue.
- **Grant-cycle regime**: with a live session the modem services the
  bulk-OUT endpoint roughly once per UL scheduling cycle: single packets
  show dwell ≈ 6.9 ms; two packets 12 µs apart complete ~6.95 ms apart
  and the second shows dwell ≈ 13.9 ms — textbook FIFO: driver queue
  dwell time directly exposes the modem's consumption (grant) timing,
  which is Aether's Observation 2/3 on the USB host interface.
- **Sustained overload** (`data/run5.csv`, `data/run5.png`): 10 Mbps UDP
  flood → dwell jumps from ~0.02 ms to a 200–680 ms sawtooth, in-flight
  oscillates ~30–90 packets for the whole flood. The queue sawtooth is
  the driver queue repeatedly draining/refilling under modem
  backpressure — the Fig. 4a shape, here driven extreme because the
  modem's data path stalled (see below).

### RM500Q data-path instability — ROOT-CAUSED AND FIXED (2026-07-31)

Root cause: **USB 10 Gbps (SuperSpeedPlus, Gen 2) link instability**
between the RM500Q and the robot's Intel Alder Lake PCH xHCI. At 10 Gbps
any sustained bulk-OUT burst triggers -EPROTO (`Unexpected error -71`)
within ~1 s; the modem keeps consuming errored URBs but drops them
internally (gNB sees BSR=0 + padding). Ruled out: thermals
(AT+QTEMP = 26–31 °C at wedge), RF signal (RSRP −81 dBm / SINR ~28 dB),
autosuspend/LPM (off; xHCI already refuses U1/U2), raw_ip/WDA settings,
RAN deployment mode (identical on host and docker gNB).

Fix (persistent in modem NVRAM, applied 2026-07-31):

```
AT+QCFG="usbspeed","20"      # was "30"; forces USB 2.0 High-Speed (480 Mbps)
AT+CFUN=1,1                  # reboot module to apply
```

Afterwards the modem enumerates on the USB2 hub (`3-1`, speed 480) and
the wedge is **gone**: 10 Mbps sustained flood = 30049/30049 URBs
completed, zero TX errors, ping healthy throughout. 480 Mbps is ~10×
above what this 20 MHz n78 cell can do; no practical throughput loss.
Revert with `AT+QCFG="usbspeed","30"` + `AT+CFUN=1,1` (MM stopped).
For a hardware-level cure at 10 Gbps (cable/connector retest), the
previous instability notes were: recovery needed a USB re-enumeration
(`echo 4-1 > /sys/bus/usb/drivers/usb/{unbind,bind}` with MM stopped);
a QMI `--wds-reset` alone was not sufficient.

### Results after the fix (physiological regimes, 2026-07-31)

- `data/run8.csv` — 10 Mbps single-sender flood (real rate ~6.8 Mbps ≈
  cell UL capacity): dwell p50 ~0.14 ms, p99 ~182 ms, queue stays
  shallow; effective consumption ~830 URB/s ≈ 6.8 Mbps goodput.
- `data/run10.csv` — dual-sender ~13 Mbps (above capacity): sustained
  queue, in-flight oscillating ~15–64, dwell strongly **bimodal**:
  ~2.25 ms median fast path vs a tight **~180–205 ms slow band**. The
  200 ms band is a modem-internal flow-control timescale exposed
  directly in the driver queue dwell distribution — another instance of
  the paper's core claim (modem behavior legible in driver queuing).
  Offered-minus-consumed excess dropped at the qdisc (~11.5k packets
  never entered the driver): backpressure propagated correctly.

Traffic note: iperf3 3.9's server crashes per-test here
(`select failed: Bad file descriptor`); the repo-free
`udp_seq_sender.py` / `udp_seq_server.py` pair (from
`~/Documents/ocudu/scripts/`, copied to the robot as `~/udp_seq_sender.py`)
is the reliable load generator.

Local (gNB host) changes this session: temporary NOPASSWD sudoers entry
(removed after gNB start), gNB + supervised iperf3 server left running.
Robot changes this session: ModemManager restarts, one QMI `--wds-reset`,
two USB re-enumerations of the modem, `udp_seq_sender.py` copied to
`~`. txdwell sources + binary in `~/usbnet-ebpf`.

### Follow-up runs (docker gNB + rate ladder, 2026-07-31)

- **Docker gNB vs host gNB**: identical dwell signatures (`data/run6.csv`,
  median 426 ms vs 415 ms host). The wedge is modem-side, independent of
  RAN deployment mode.
- **Rate ladder** (`data/run7.csv`, 2 → 4 → 8 Mbps in one recording):
  the modem wedged **already at 2 Mbps** — dwell went 0.14 ms → 311 ms
  within the first second of load; all subsequent rates show the same
  wedged regime: effective USB service rate ~143 URB/s (~1.1 Mbps goodput
  ceiling), dwell ~420 ms median (max ~1 s), in-flight oscillating 30–90.
  No self-recovery after load stops (still wedged after 14 s drain).
- Healthy-regime reference (from probes on 2026-07-31): at ping-level
  rates the modem sustains indefinitely with grant-cycle dwell ~6.9 ms.
  The physiological multi-Mbps regime (dwell ~7–50 ms under real air
  backpressure) is **not reachable on this unit** until the USB data-path
  instability is fixed — the wedge onset sits between ~1 pkt/s and
  250 pkt/s sustained.
- Interpretation vs the paper: the driver-queue signal tracks modem
  backpressure faithfully across 4 orders of magnitude (0.02 ms idle →
  ~7 ms grant-cycle → ~400+ ms congestion/stall), which is exactly the
  property Aether relies on for its bottleneck detector (§4.4: dwell
  spike = uplink bottleneck).

## Cross-validation vs the paper (2026-08-01, docker gNB, USB2 mode)

iperf3 fixed: built 3.19.1 static (`--without-openssl --enable-static-bin`),
deployed as `~/iperf3.new` (robot) and `/usr/local/bin/iperf3.new`
(open5gs_5gc container). Stock 3.9's per-test server crash is gone; TCP UL
≈3 Mbps / UDP fine. Use it for all iperf3 work here.

run12 (TCP UL + 8 s UDP injection, `data/run12.csv` + `data/cwnd12.log`,
plot `data/run12_cc.png`):

- Driver dwell: 0.2 ms → ~190 ms plateau within ~2–3 s of the congestion
  event, back to ~0.2 ms after it drains. Instant response confirmed.
- **TCP cwnd never decreased** through 20+ s of severe congestion
  (CUBIC plateaued at ~240 segs, later grew to 575): the deep buffers
  hide loss entirely, so the ACK-based CCA is not 1.6 s late (paper's
  Fig. 2c case) but *completely oblivious* here — strengthens the
  paper's motivation.
- RTT reached ~850 ms while driver dwell plateaued at ~190 ms → on the
  RM500Q the modem-internal UL buffer holds several hundred ms of data
  below the URB-completion point. Driver dwell is a *lower bound* of
  air-side queuing on this modem, unlike the paper's USB modem whose
  driver queue ≡ modem queue ("single logical queue", max 4 skbs).

run15 (10 Mbps UDP flood + concurrent DIAG capture, `data/run15.csv` +
`data/run15_tb.csv`, plot `data/run15_joint.png`):

- MobileInsight OnlineMonitor on `/dev/ttyUSB0` (DIAG), log types
  `5G_NR_MAC_UL_TB_Stats` (0xB881, 5 ms cadence cumulative counters) +
  `5G_NR_LL1_FW_Serving_FTL`. Per-bin modem air UL rate from
  `TB New Tx Bytes` deltas tracks the driver DEQ rate through ramp and
  plateau (~5.5 Mbps cell UL capacity): **R = 0.81** over loaded 0.5 s
  bins — direct modem-side confirmation that the driver queue mirrors
  modem service dynamics (the paper's Observation 3).
- Time alignment: txdwell ts = CLOCK_MONOTONIC ns; mi2log decoded dict
  `timestamp` is a datetime (UTC). Convert with a per-boot offset
  (sample `/proc/uptime` + `date -u` on the robot). NOTE: replayed
  `msg.timestamp` is replay-time, useless; always use the dict field.
- DIAG capture gotchas: stopping ModemManager makes NM remove wwan0's
  IP/routes — re-apply `ip addr replace 10.45.1.2/30 dev wwan0` +
  `ip route replace 10.45.1.0/30 dev wwan0 scope link` before sending
  traffic, or it silently egresses via WiFi. `0xB883`
  (UL_Physical_Channel_Schedule_Report, would carry BSR) decodes as
  `Records: null` with only Raw Hex on this firmware (v2.11 record
  format unsupported by the bundled decoder) — BSR path left as future
  work (decode Raw Hex or QLog).
