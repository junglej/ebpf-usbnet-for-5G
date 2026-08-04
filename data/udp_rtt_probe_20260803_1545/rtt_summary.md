# UDP echo-RTT 探针实验 —— 6DL:3UL,2026-08-03

方法:`scripts/udp_rtt_probe.py`(robot)→ `scripts/udp_seq_echo_server.py`
(5gc 容器,ogstun 10.45.1.1:5000,收包立即原样回声)。探针格式与
`udp_seq_sender.py` 一致(`{seq}||{send_ts_ns}` + 'a' 填充,1000 B,
10 pkt/s),gNB RLC UL_PKT 日志可按 seq 对应每个探针包。
**RTT 全部用 UE 本机 CLOCK_MONOTONIC 计算,无需时钟同步**;
负载仅为 UL 方向,DL 空闲,故 RTT 超出空闲基线的部分 ≈ UL 排队
(driver + 模组缓冲 + 空口)。

## 结果(每种工况 250 探针 @ 10 pkt/s)

| 工况 | iperf3 并发负载 | 回声丢失 | RTT min | RTT p50 | RTT p99 | RTT max |
|---|---|---|---|---|---|---|
| 空闲 | — | 0.0 % | 25.1 ms | 45.1 ms | 64.8 ms | 83.1 ms |
| TCP cubic(-t 30) | 发送 7.79 / 接收 6.84 Mbps,0 retx | 0.0 % | 151.3 ms | **491.0 ms** | 631.1 ms | 648.8 ms |
| UDP 过载(-u -b 20M -w 64M) | 供给 20.0 / 接收 9.30 Mbps,38.8% 丢 | **41.6 %** | 334.7 ms | **410.0 ms** | 522.7 ms | 529.9 ms |

读法:

- **TCP 负载**:零丢失,RTT p50 ≈ 490 ms——探针包排在 TCP 堆起来的模组
  深缓冲后面,与 `ss` 采的 500–640 ms、BSR 顶格 31 互证。bufferbloat
  主体在模组内部(同窗口 driver dwell ~0.1 ms)。
- **UDP 过载**:模组缓冲钉满,探针包与数据包同比例被丢(41.6% ≈ iperf
  的 38.8%);幸存的探针看到 ~410 ms 排队(driver ~190 ms + 模组内部,
  队列满时排队时延反而被"丢包截断"在缓冲容量上限)。
- 空闲 p50 45 ms 与历史 ping 28–47 ms 一致,方法学自洽。

## 注意:测试床时钟跳变

第一轮测量出现 ±46 s 的虚假 RTT:robot(和 host)的 realtime 时钟在
测量窗口内发生跳步(两机当日还同步跳过 +6h44m,与 PTP/NTP 环境有关,
待用 `使用说明.md` §0 的 linuxptp 配置排查)。探针已改为 MONOTONIC
计时免疫该问题;本目录 CSV 均为修复后的干净重测。

## 文件

- `rtt_idle.csv` / `rtt_tcp.csv` / `rtt_udp_overload.csv` —
  seq,size,send_ts_ns(Realtime,供 gNB 对齐),recv_ts_ns,rtt_ms(MONOTONIC)
- `rtt_{tcp,udp}_iperf.json` — 并发 iperf3 结果
