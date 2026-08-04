# txdwell — Aether 论文 driver-queue 观测在 RM500Q 上的复现

用 eBPF(CO-RE)在 **Quectel RM500Q-GL**(usbnet/qmi_wwan)上复现
**Aether (SIGCOMM'26,《Forewarned is Forearmed》)** 的核心观测:
**driver 层 TX 队列的 per-packet dwell time 与队列长度是上行拥塞的
即时信号,且 driver 队列与模组空口缓冲构成"单一逻辑队列"**。
并用 MobileInsight(DIAG)与 gNB 日志从模组侧、网络侧交叉验证,
形成三源互证的完整复现链路。

测试床:ocudu gNB(USRP X310,n78 TDD 20 MHz,PLMN 001-01)+ open5gs
(host 或 docker 均可),UE 在 `robot@192.168.5.2`(Ubuntu 22.04,
kernel 6.8.0-134),UE IP `10.45.1.2/30`,网关 `10.45.1.1`。

## 复现链路(三个证据源)

```
            UE (robot)                                   network
 ┌──────────────────────────────┐
 │ app → qdisc → usbnet → USB → │      ┌─────────────┐     ┌────────┐
 │                ▲             │      │ RM500Q 内部  │     │ ocudu  │
 │   ① txdwell (eBPF kprobes)   │      │ UL buffer   │────▶│ gNB    │
 │   usbnet_start_xmit          │      │  ②DIAG:     │ air │ ③MAC 日志│
 │   tx_complete                │      │  0xB883 TB  │     │ SBSR   │
 │   → dwell + in-flight        │      │  0xB873 BSR │     │        │
 └──────────────────────────────┘      └─────────────┘     └────────┘
```

- **① driver 侧(eBPF)**:enqueue = `usbnet_start_xmit`(qmi_wwan 在 6.8
  无 `tx_fixup`,skb 指针无损),dequeue = `tx_complete`(`urb->context`
  关联 skb)。`tx_complete` 是通用名,**必须按地址挂载**:`/proc/kallsyms`
  中带 `[usbnet]` 后缀的符号 + libbpf kprobe_multi(`opts.addrs`)。
  dwell = 两探针时间戳之差;in-flight = 在飞 skb 计数。
- **② 模组侧(DIAG/MobileInsight)**:robot 上
  `~/Documents/mobileinsight-core`(venv)经 `/dev/ttyUSB0` 采集:
  - `5G_NR_MAC_UL_Physical_Channel_Schedule_Report` (**0xB883**):
    逐 TB 空口事件。v2.11 记录格式自带解码器解不出(只给 Raw Hex),
    本仓库 `scripts/decode_b883.py` 负责解码(布局与位打包见 docstring,
    已经多重校验:RNTI 恒定、slot∈[0,20)@30 kHz、num_rbs=51=满 20 MHz
    载波、TB size 吻合流量、retx 占比 ≈ gNB 侧 NOK)。
  - `5G_NR_L2_UL_BSR` (**0xB873**):**UE 侧 BSR**——逐 TTI 的触发原因
    (HIGH_DATA_ARRIVAL/周期)、SHORT/LONG、逐 LCG BSR index(与 gNB
    SBSR 同标度)。
  - `5G_NR_MAC_UL_TB_Stats` (0xB881,累计计数)、`5G_NR_LL1_FW_Serving_FTL`。
- **③ gNB 侧**:ocudu MAC 日志逐条解 UE 上报的
  `SBSR: lcg=0 bs=N`(docker 部署时完整日志在容器卷
  `/tmp/gnb_ocudu_x300.log`,`docker logs` 只是子集)。

**时间对齐**:txdwell 用 `CLOCK_MONOTONIC` ns;mi2log 解码字典里的
`timestamp` 是 datetime(UTC)。用在 robot 上采的
`time.monotonic_ns()` ↔ `time.time_ns()` 偏移换算(同次开机有效;
注意 `/proc/uptime` 含休眠时间,不等于 monotonic)。
回放时的 `msg.timestamp` 是回放时刻,**不要**用作时间轴。

## 主要结果(与论文对照)

| 论文观测 | 本复现(RM500Q) | 判定 |
|---|---|---|
| Obs 2:dequeue 时机 = 模组消费时机,dwell 是"空口子 RTT" | 空闲 dwell 0.02–0.15 ms;有会话时单包 ~6.9 ms,12 µs 间隔两包完成间隔 6.95 ms、后者 dwell ~13.9 ms(教科书 FIFO,消费节奏=UL grant 周期) | 一致 |
| Obs 3:driver 队列与模组缓冲"单一逻辑队列",Fig. 4b R=0.96 | **corr(driver 消费速率, 模组逐 TB 空口速率) R=0.964**;字节闭环 air/DEQ=1.015(TB 含协议开销);BSR 顶格 ↔ driver 队列顶满 ↔ dwell 平台,同起同落 | 一致(关键结论) |
| Fig. 2c:拥塞时 cwnd 滞后 1.6 s 才降 | 更强:**cwnd 全程不降**(20+ s 严重拥塞,CUBIC 240→575 还涨)——深缓冲让 ACK 系 CCA 完全无感 | 一致且更极端 |
| §4.3:USB 的 skb_buffer 最大 4(PCIe 128) | 实测 6.8 usbnet in-flight 上限 **~64** | 参数差异(版本相关,不影响 dwell 法) |
| (隐含)driver dwell ≈ 空口排队全部 | RM500Q 内部 UL 缓冲很深:RTT ~850 ms 时 driver dwell 仅 ~190 ms——**driver dwell 是空口排队下界** | 定量差异(值得注意) |

另外观测到的模组行为细节:过载时 dwell **双峰**(~2.25 ms 快路径
vs ~180–205 ms 窄带),窄带是模组 USB 流控的时间尺度,直接暴露在
driver dwell 分布里。

## 实测数字汇总

**(a) 端到端吞吐(UE→10.45.1.1)**

| 指标 | 数值 | 说明 |
|---|---|---|
| TCP 上行 goodput | 发送端 3.0–5.1 Mbps,接收端 2.4–3.7 Mbps | iperf3 3.19.1,8–45 s 多次 |
| UDP 上行(不超容量) | 9.0 Mbps 全程无损 | 单发 ~1100 pkt/s × 1 KB |
| 空口 UL 容量(实测) | ~5.5 Mbps(8-01 多次)– ~10.5 Mbps(8-02) | 同一小区不同时段;由 0xB883 逐 TB 速率直接读出 |
| ping RTT | 空闲 28–47 ms;TCP 加载后 ~500–640 ms(`ss` 采样),峰值 ~850 ms | TCP 下排队几乎全在模组内部缓冲(driver dwell ~0.1 ms);UDP 过载时 driver 队列另承担 ~190 ms(溢出阀) |
| UL 空口重传率 | ~3.4 %(逐 TB 统计) | 与 gNB 侧 UL NOK 一致 |

**(b) 队列各层(driver/模组,过载工况 `data/overload.*`)**

| 层 | 行为 |
|---|---|
| driver in-flight | 顶格 ~64 包(p50=40),6.8 usbnet 上限 |
| driver dwell | p50=2.41 ms,p90=177 ms,p99=190 ms(双峰) |
| driver 消费速率 vs 模组空口速率 | **R=0.964**;字节闭环 air/DEQ=1.015 |
| UE BSR(两端) | 97% 顶格 31(=报告值 >150 KB 的量化上桶),空闲为 0;中间值只在过渡瞬间 |
| 模组内部缓冲深度 | RTT(~850 ms)− driver dwell(~190 ms)⇒ 模组内部还压着数百毫秒 |

**(c) 队列各层(不超容量工况 `data/aligned.*`)**:dwell p50=0.09 ms、
p99=0.2 ms、in-flight≈0、BSR 24–31 中间值跟随、R=0.943、
goodput 9.03 Mbps 无损。

## 端到端链路逐段分析

```
app udp_seq_sender/iperf3  ── 供给 9.0–14 Mbps(UDP)/ 2.4–5 Mbps(TCP)
  │
qdisc(fq_codel, qlen 1000)── 仅当 driver 满才丢(过载时 ~11.5k 包根本没进驱动)
  │
usbnet driver queue        ── in-flight ≤64;不超容量 dwell ~0.1 ms,
  │(URB 提交→tx_complete)     超容量双峰 2.4 ms / ~190 ms(流控时间尺度)
  │
USB bulk-OUT 服务           ── 空闲即时;低速时 ~6.9 ms/包的 grant 周期;
  │                          满载 ~830–1250 URB/s(≈7–10 Mbps)
  │
modem 内部 UL 缓冲           ── SDAP/PDCP/RLC,深:>150 KB 且开关式
  │(对 host 不可见,仅 BSR     (0↔31);TCP 下能藏 ~600 ms 排队
  │  与 0xB883 可见)
  │
空口 UL(PUSCH)            ── 容量 ~5.5–10.5 Mbps(随无线环境);
  │                          retx ~3.4%;MCS 7–13,51 RB×14 sym 满载波
  │
gNB→UPF→ogstun(GTP-U)    ── 无可观测附加排队;RTT 构成证明队列几乎全在模组
```

RTT 分解(过载):e2e RTT ~850 ms ≈ driver dwell ~190 ms + 模组内部
~600 ms + 空口/回传 ~30–50 ms。**排队瓶颈的主体在模组内部,driver
队列是它的溢出阀**——这正是论文"单一逻辑队列"的物理图景,但在
RM500Q 上两段的"容积比"和论文测试的 USB 模组(浅缓冲)不同。

## 与论文机制的深入对照

- **§4.3 为什么选 dwell 而不是队长**:我们的数据直接背书——队长是
  开关式的(in-flight 0 或顶格 64、BSR 0 或 31),几乎不含梯度信息;
  dwell 连续可变(0.02 ms→190 ms,跨 4 个数量级),是唯一可用的
  拥塞度量。
- **§4.4 瓶颈检测器**:dwell 突增=上行瓶颈成立的证据(加压同秒内
  起跳);停压即落可判定瓶颈转移。逻辑上直接可用。
- **§4.5 Little's law 容量估计(Cu = γ·Q/D)**:在 RM500Q 上**不能
  直接套 driver 段的 Q/D**——usbnet 是多 URB 并行+流控的非单一
  FIFO,且 dwell 双峰:用快路径(64×1028 B/2.4 ms ≈ 27 Mbps)高估,
  用慢路径(64×1028 B/190 ms ≈ 2.8 Mbps)低估,真值 ~10 Mbps 在
  两者之间。论文的估计器成立的前提是"driver 队列=瓶颈队列本体",
  而本机瓶颈主体在模组内部。要用 §4.5,应改用 BSR(模组内部占位)
  或快路径 dwell 的统计下沿——这是把 Aether 移植到深缓冲 USB 模组
  上需要修正的点。
- **部署方式(方法论差异)**:论文是改驱动 + EXPORT_SYMBOL + 内核
  模块;本复现用**纯 eBPF kprobe(按地址挂载),零内核改动**,拿到
  同样的观测。对"不改固件/驱动"的论点构成加强。
- **主要定量差异**:USB 队列上限(64 vs 论文 4)、模组内部深缓冲
  (driver dwell 是空口排队下界,论文 USB 模组是浅缓冲近似相等)。
  两者都不动摇 dwell 作为信号的有效性,但影响用它做定量估计的方式。

**标准数据集**(`data/`,采集窗口已对齐:DIAG 58 s ⊇ txdwell 50 s。
**流量全部由 iperf3 3.19.1 产生**(udp_seq_sender.py 是 gNB 底层
UL_PKT 分析专用,不用于本复现):

- `aligned.*` — `iperf3 -u -b 8M -t 30`(低于容量):实测 8.00 Mbps
  0% 丢包;driver 队列全程为零(dwell≈0、in-flight≈0),**BSR 稳定在
  ~22–23 的中间水位**(两端一致,健康稳态的样本),
  **R=1.000**,air/DEQ=1.015。
- `overload.*` — `iperf3 -u -b 20M -w 64M -t 30`(超容量;需先把
  robot 的 `net.core.wmem_max` 调到 128 MB,否则 SNDBUF 背压把发送端
  限在 ~9.7 Mbps 堆不出过载):实测供给 20.0 Mbps、接收 11.0 Mbps、
  **41% 丢包**;BSR 两端顶格 31、in-flight 顶格 64、dwell 170–200 ms
  带、driver 消费速率绕空口速率锯齿波动,停压各层同步排空,
  R=0.909,air/DEQ=1.011。
- 队列填充的因果链(过载每次复现):加压 → **UE BSR 先动**(0xB873
  触发 HIGH_DATA_ARRIVAL,~1 s 内 0→31)→ gNB 见 bs=31 → **USB 流控
  反压,driver 队列顶满**(in-flight→64)→ dwell 抬升 → 空口顶到
  容量平台 → 停压各层一起排空。

## TDD 时隙配比实验(2026-08-02,DL:UL 6:3 → 2:7)

把 docker gNB 的 `tdd_ul_dl_cfg` 从 6DL:3UL 改为 **2DL:7UL**(备份:
`configs/gnb_rf_x310_tdd_n78_20mhz_docker.yml.bak_6d3u`)。注意:严格的
1:8 被配置校验拒绝(CSI-RS 要求 ≥2 个起始 DL 时隙,或 1 DL + 特殊时隙
9 DL 符号 + dmrs-AdditionalPosition=2),2:7 是合法的最接近值。

| 指标 | 6DL:3UL(原) | 2DL:7UL(新) | 变化 |
|---|---|---|---|
| UDP 上行容量 | ~9–10.5 Mbps | **~16.6 Mbps**(2024 pkt/s × 1028 B,~40 Mbps 冲击下平台+30% 丢包) | ×1.65 |
| TCP 上行(单流) | 3.0–5.1 Mbps | **12.4 Mbps**(接收端 11.0) | ×2.4–4 |
| TCP 上行(4 流) | — | 12.6 Mbps(与单流持平,单流已饱) | — |
| TCP 下 driver 队列 | 过载时顶格(64) | **依旧很薄**:dwell p50=0.17 ms、in-flight ~3(TCP 自限在容量 75% 处) | — |
| iperf3 UDP 模式 | 控制信道在 UE→容器路径上必崩 | **已根治**:5gc 容器 ogstun 有多个 /16 地址,UDP 回包源地址被路由选成 10.45.0.1,connected UDP 客户端丢弃非对端回包;修复见下 | — |

**iperf3 UDP 故障根治(2026-08-02)**:tcpdump(nsenter 进容器 netns)抓到
服务端 UDP 回包源地址为 `10.45.0.1` 而非 `10.45.1.1`,iperf3 UDP 客户端
(connected socket)直接丢弃。修复落在 ocudu 仓库:
`docker/open5gs/routes_ue_src.py`(为每个 /24 加 prefsrc 路由)+
`open5gs_entrypoint.sh` 尾部延迟 8s 调用(UPF 启动会刷 ogstun 路由)+
Dockerfile COPY。已重建 5gc 镜像并全量重部署验证(`ip route get
10.45.1.2 → src 10.45.1.1`)。修复后 iperf3 全功能可用:
UDP 10M → 8.34 Mbps(0% 丢包),TCP UL 7.69/6.11 Mbps,
TCP DL(-R) 26.1/24.6 Mbps(6DL:3UL,2026-08-02)。
`使用说明.md` §4.1 已同步记录。

结论:UL 时隙从 30% 提到 70%,空口 UL 容量 ~×1.65;TCP 因拥塞控制
自限在容量以下,吃到了比容量比例更大的提升;12 Mbps 的 TCP 仍不足以
让 driver 队列堆积——要复现过载形态需 ~17 Mbps 以上持续供给。
iperf3 UDP 控制信道在空口路径上始终报 `unable to read from stream
socket`(host→容器直连正常),原因未查明,UDP 容量测试请用
`udp_seq_sender.py` 多路并发 + 容器内 `udp_seq_server.py`。

## TCP 为什么低于 UDP:bufferbloat 实测分解(2026-08-03,tcp1 数据集)

同一小区同一时刻:UDP 上行 ~9.7 Mbps(SNDBUF 背压限速),TCP 上行
7.59 Mbps。用三源仪器完整分解(eBPF + DIAG + ss):

- **driver 层零排队**:driver dwell 全程 0.1–0.2 ms,in-flight ≈0——
  TCP 的包被模组瞬时收走,usbnet 不积压。
- **模组内部持续深排队**:0xB873 BSR 97% 顶格 31。注意 Short BSR 是
  量化桶,31 只表示 **>150 KB**,不代表模组缓冲已达物理上限(其绝对
  容量未知);但 RTT(~500–640 ms)× 吞吐(~7.6 Mbps)反推全链路在飞
  ~0.5 MB,与模组侧深排队自洽。BSR 值的"开关式"分布(只有 0 和 31,
  中间值仅出现在过渡瞬间)是 work-conserving 深队列特征。
- **RTT 膨胀**:ss 采样 RTT 从 ~150 ms 涨到 ~500–640 ms;cwnd
  108→488 钉住;unacked ~370 段 ≈ 500 KB;delivery_rate ~5.4–7.7 Mbps,
  pacing_rate ~12 Mbps。
- **空口重传升高**:0xB883 逐 TB retx 占比 9.8 %(UDP 洪泛实验 3.4 %)。

结论:TCP 吞吐 ≈ 在飞字节/RTT ≈ 500 KB / 0.5 s ≈ 8 Mbps——**不是空口
变差,是深缓冲把 RTT 吹大、TCP 被自己的反馈环限住**(再加 pacing 与
偶发丢包 ssthresh=30)。这正是 Aether 针对的 bufferbloat 场景。

**对论文模型的边界发现**:亚容量 TCP 负载下 driver dwell(0.1 ms)对
模组内部 150 KB 积压完全无感——USB 流控要等模组队列更深(~190 ms
dwell 带)才启用。即"driver 队列≡模组队列"在这颗 RM500Q 上只在
**饱和点以上**成立;要让 Aether 的瓶颈检测在此类深缓冲 USB 模组上
工作,信号应改用 BSR(0xB873 实时可得)或快路径 dwell 统计。

## RM500Q USB 稳定性

这颗 RM500Q 在 **USB 10 Gbps(SSP Gen2)** 链路上不稳定:任何持续
bulk-OUT 突发在秒级内触发 `-EPROTO`(`qmi_wwan: Unexpected error -71`),
模组吞包但不上空口(gNB 只见 BSR=0+padding),只有 USB 重新枚举能恢复。
已排除:过热(AT+QTEMP 26–31 °C)、信号(RSRP −81 dBm/SINR ~28 dB)、
LPM(xHCI 本就拒绝 U1/U2)、RAN 部署方式(host/docker 同样复现)。

**修复(已写入模组 NVRAM,重启保持)**:

```
AT+QCFG="usbspeed","20"     # 强制 USB 2.0 High-Speed(480 Mbps)
AT+CFUN=1,1                 # 重启模组生效;回退: "30" + 同样重启
```

修复后 10 Mbps 持续冲击 30049/30049 URB 完成、零错误。480 Mbps 相对
20 MHz 小区无瓶颈。固件 RM500QGLABR13A02M4G(R13A02,较旧)。

## 仓库结构

- `src/txdwell.bpf.c` — BPF 侧:两个 kprobe(kprobe.multi SEC)、
  skb hash、ringbuf、配置 map(ifindex 过滤/ENQ 开关)、统计 counters。
- `src/txdwell.c` — 用户态 loader:kallsyms 解析 `[usbnet]` 符号地址、
  kprobe_multi 按地址挂载、ringbuf→CSV、1 s STATS、SIGINT 汇总。
- `src/txdwell.h` — 共享事件/配置定义。
- `Makefile` — 生成 `vmlinux.h`(bpftool;目标机 usbcore 内建于
  vmlinux,`struct urb` 可直接 CO-RE)、clang `-target bpf`、静态
  libbpf(`LIBBPF_DIR ?= ../libbpf`,v1.4.5;系统 0.5.0 太旧)。
- `scripts/setup_robot.sh` — robot 一键幂等装依赖+libbpf+构建。
- `scripts/record.sh` — 定时录制 CSV。
- `scripts/plot.py` — dwell + in-flight 曲线(Fig. 4a 风格)。
- `scripts/decode_b883.py` — 0xB883 v2.11 Raw Hex → 逐 PUSCH TB CSV。
- `scripts/plot_aligned.py` — 四面板对齐图(dwell / in-flight / 双速率 /
  双端 BSR),参数化数据集名,例如
  `python3 scripts/plot_aligned.py overload --off <offset>`。
- `data/` — 标准对齐数据集(`aligned.*`、`overload.*`)。
- robot 侧采集/分析脚本在 robot 仓库
  `~/Documents/mobileinsight-core/bsr_exp/`(capture_ue_bsr.py、
  mi_extract_ue_bsr.py、mi_b883_raw.py、mi_dump.py)。
- 论文原文:`sigcomm26-final420(2).pdf`、`paper.txt`。

## 复现步骤

1. 基站: `cd ~/Documents/ocudu && ./start_docker.sh`(或
   `WITH_IPERF=1 ./start_host.sh`);确认 UE 注册、ping 10.45.1.1 通。
2. robot 构建: `cd ~/usbnet-ebpf && ./scripts/setup_robot.sh`。
3. 观测: `sudo ./txdwell -i wwan0 -d 60 -o /tmp/run.csv`
   (CSV: `DEQ,ts_ns,ifindex,len,dwell_ns,inflight`,ts 为
   CLOCK_MONOTONIC ns)。
4. 模组侧(与 3 同时):停 ModemManager(**注意:NM 会随之删除
   wwan0 的 IP/路由,必须手工补回**,否则流量静默走 WiFi):
   ```
   systemctl stop ModemManager && chmod 666 /dev/ttyUSB0
   ip addr replace 10.45.1.2/30 dev wwan0 && ip link set wwan0 up mtu 1400
   ip route replace 10.45.1.0/30 dev wwan0 scope link
   timeout 68 ~/Documents/mobileinsight-core/venv/bin/python \
     ~/Documents/mobileinsight-core/bsr_exp/capture_ue_bsr.py \
     /dev/ttyUSB0 115200 /tmp/run.mi2log
   ```
   发压用 iperf3 3.19.1(robot `~/Documents/iperf3.new`,server 在
   5gc 容器):不超容量 `iperf3 -u -b 8M -t 30`;超容量先
   `sudo sysctl -w net.core.wmem_max=134217728`(运行时生效,重启失效)
   再 `iperf3 -u -b 20M -w 64M -t 30`。
5. 分析:robot 上用 `bsr_exp/` 的提取脚本出 UE BSR / 0xB883 raw;
   本地 `scripts/decode_b883.py` 解逐 TB 事件,`scripts/plot.py`
   或对齐出图脚本出曲线;时钟偏移在 robot 上
   `python3 -c 'import time; print(time.monotonic_ns(), time.time_ns())'`
   采样换算。

## 已知坑(都踩过)

- 停 ModemManager → NM 删 wwan0 路由(见上)。
- iperf3 3.9 server `select failed: Bad file descriptor`(每测必崩
  退出)→ 用 3.19.1 静态版。
- `ss -tin dst <gw>` 会同时匹配 iperf 控制连接和数据连接,采 cwnd
  要按本地端口区分。
- DIAG 采集窗口要 ≥ txdwell 窗口,否则空口序列"断头"(曾误判为
  模组停发,gNB PUSCH 日志证伪)。
- kprobe_multi 走 fprobe,`/sys/kernel/tracing/kprobe_profile` 不显示
  命中,以 STATS 计数为准。
