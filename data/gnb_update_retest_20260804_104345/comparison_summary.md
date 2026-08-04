# gNB 新代码对照重测 —— 6DL:3UL,2026-08-04 10:44–10:57

与 2026-08-03 数据集(`../baseline6d3u_udp_tcp_20260803_150323/`)同流程对照:
基线 10×UDP + 10×TCP(10 s,去极值平均)、echo-RTT 三工况、
三源监控 udp_overload / tcp_cubic。

## 总体对比(旧 → 新)

| 指标 | 08-03(旧代码) | 08-04(新代码) | 变化 |
|---|---|---|---|
| UDP 基线接收 | 9.73 Mbps | 7.09 Mbps | −27% |
| UDP 丢包 | 11.9 % | 13.2 % | ≈ |
| TCP 基线接收 | 6.82 Mbps | 5.76 Mbps | −16% |
| 空口 UL 平台(0xB883) | ~10–10.5 Mbps | ~7.5–8 Mbps | −25% |
| echo RTT 空闲 p50 | 45.1 ms | 35.2 ms | **改善** |
| echo RTT TCP p50 | 491 ms | 622 ms | 变差 |
| echo RTT UDP 过载 p50 | 410 ms(41.6% 丢) | 579 ms(42.8% 丢) | 变差 |
| MCS 均值(过载) | 18.4 | 15.4 | 更低 |
| UL 空口重传 | 5.1 % | 4.9 % | ≈ |
| 调度结构(num_rbs) | 均值 44.6,满 51 | 均值 45.0,满 51 | **不变** |
| R(DEQ vs air) | UDP 0.844 / TCP 0.566 | UDP 0.686 / TCP 0.839 | 噪声级波动 |
| 字节闭环 air/DEQ | 1.013 / 1.018 | 1.015 / 1.018 | 不变 |

## 判读:吞吐下降由空口 MCS 走低解释,调度行为无变化

- 新代码下**调度结构完全没变**:grant 仍满 51 RB(20 MHz 载波)、
  重传率 ~5% 持平、BSR 开关式顶格、队列形态(过载 dwell ~200 ms 带 +
  in-flight 64;TCP 薄队列)逐项复现。
- 变化集中在 **MCS:18.4 → 15.4**(TB 均值 2201 B → 1666 B)。今日 gNB
  侧 CQI=12、PUSCH SNR ~21.6 dB(历史记录 SINR ~28 dB),无线条件
  今天更差 ⇒ 容量 ~11 → ~8 Mbps ⇒ TCP/RTT 随之等比变化
  (RTT 膨胀 = 同样的模组缓冲字节数 ÷ 更低的排空速率)。
- **没有看到吞吐优化**;但也看不到新代码造成回归的证据——两天差异的
  解释变量是 MCS。空闲 RTT 45→35 ms 是唯一的改善项。

**要严格归因必须 A/B**:同一无线条件下(前后脚)跑旧 build 与新 build
的 6 分钟基线脚本各一遍即可(`scripts/run_baseline.sh` 一条命令)。

## 文件

- `baseline/` — 20 轮 iperf3 JSON(udp_2 因已知 UDP 控制信道问题无效,n=9)
- `rtt_{idle,tcp,udp_overload}.csv` + `rtt_{tcp,udp}_iperf.json`
- `mon/` — 三源监控两组 + 四面板图(`udp_overload.png` / `tcp_cubic.png`)
