# 监控实验(三源采集)—— 6DL:3UL,2026-08-03 15:27–15:31

采集:txdwell(eBPF,55 s)+ DIAG 0xB883/0xB873(75 s)+ gNB SBSR,
时钟偏移随 run 采样(`<name>_offset.txt`)。出图:

```
../../.venv/bin/python ../../scripts/plot_aligned.py udp_overload --off 1784793311.278624 --data .
../../.venv/bin/python ../../scripts/plot_aligned.py tcp_cubic   --off 1784793311.2127557 --data .
```

## udp_overload(iperf3 -u -b 20M -w 64M -t 30)

供给 20.00 Mbps,接收 9.73 Mbps,丢包 38.6%(30 s 全程过载)。
图:`udp_overload.png`,**R=0.844**,字节闭环 air/DEQ=1.013。

- dwell:几乎全部钉在 ~200 ms 慢路径带(模组 USB 流控时间尺度)
- in-flight:顶格 64,停压后同步排空
- driver DEQ 速率绕空口速率(~10 Mbps)锯齿波动
- BSR 两端(0xB873 与 gNB SBSR)全程顶格 31,同起同落

## tcp_cubic(iperf3 -t 30,单流 cubic)

发送 7.72 Mbps,接收 6.72 Mbps,0 重传。
图:`tcp_cubic.png`,R=0.566(TCP 突发致 bin 内错位),字节闭环 1.018。

- dwell:绝大多数 ~0.1 ms;一小簇 170–200 ms 并带出逐级下降的
  阶梯(TCP 突发在 driver 队列 FIFO 排空:队首等最久)
- in-flight:0–5 之间,典型 ≤4——TCP 自限,driver 队列从不满
- 速率 ~7 Mbps,driver 侧突发锯齿,空口侧平稳
- BSR 两端同样顶格 31:限制 TCP 的不是模组缓冲(已满),是 TCP 自己

## 对比结论(与基线互证)

BSR 在两种工况下都是满的,但 driver 队列形态完全不同:UDP 硬灌把
driver 队列钉死(过载形态),TCP 因拥塞控制自限只有小突发,速率停在
容量(≈11 Mbps,见 baseline_summary.md)以下。这正是 Aether 想改的点:
有 driver 层即时信号,TCP 类 CCA 本可贴着容量跑。

## 文件清单

- `baseline_json/` — 基线 20 轮 iperf3 JSON;`baseline_summary.md` 统计
- `<name>.csv` — txdwell 记录;`<name>.png` — 四面板对齐图
- `<name>_ue_bsr.csv` — UE 侧 BSR(0xB873 提取)
- `<name>_b883_raw.csv` → `b883_pusch_<name>.csv` — 0xB883 raw → 逐 TB
- `<name>_gnb_sbsr.txt` — gNB MAC 日志 SBSR 行(iperf 窗口 ±15/25 s)
- `<name>.mi2log` — DIAG 原始采集;`<name>_iperf.json` — 当轮 iperf3
- `<name>_{offset,iperf_start,iperf_end}.txt` — 时钟/窗口;`*_log` — 运行日志
