# Lab1 Figure 3 复现实验说明

本文档整理 `lab1_o` 最终结果，用于毕业论文中描述 SIRD 论文 Figure 3 的复现实验。内容包括场景、绘图结果、读图解释、关键数值、结论和可直接引用的论文段落。

## 1. 实验目标

该实验目标是复现 SIRD 论文 Figure 3 的 receiver congestion / incast 场景。原文 Figure 3 关注两个问题：

1. 对于很小的 `8B` request，SIRD 在接收端被 incast 背景流量打满时，是否仍能保持接近 unloaded baseline 的低延迟。
2. 对于较大的 `500KB` request，SIRD 的 receiver-side scheduling policy 是否能影响 request latency，特别是 SRPT 是否能优先完成较小消息，而 SRR 是否体现更公平但更慢的调度行为。

本地实验使用 `scratch/lab1_o.cc` 和 `scripts/lab1_o_sweep.sh`，采用 request/reply latency 语义：客户端发送 probe request，服务端返回最小 reply，客户端在收到 reply 后记录端到端 request/reply latency。这一点与原文“client-observed end-to-end latency”的 Figure 3 语义一致。

## 2. 场景描述

实验拓扑是一个接收端瓶颈 incast 场景：

- 6 个背景发送端向同一个 receiver 发送 `10MB` 大消息；
- 背景发送端速率为 `17Gbps`，总 offered load 约为 `102Gbps`，用于饱和 `100Gbps` receiver downlink；
- 第 7 个发送端周期性发送 probe request；
- probe request 分两种大小：`8B` 和 `500KB`；
- 记录 probe request 的端到端 request/reply latency；
- 比较 unloaded baseline 和 incast 下的 latency CDF；
- 对 `500KB` probe，额外比较两种 receiver scheduling policy：SRPT 和 SRR。

本次最终结果来自修正后的重跑版本：

```text
/mnt/nasDisk_ds3617/sird/lab1_o/20260429/lab1_o_fig3_rerun_interval8b100us_probe2000_long17_qsample100us_20260429_163840/probe2000
```

本地拷贝位置：

```text
outputs/sird-scenarios/lab1_o_fig3_rerun_interval8b100us_probe2000_long17_qsample100us_20260429
```

本次每条曲线包含 `2000` 个 probe samples。与旧版不同的是，本次将 `8B probe` 的发送间隔修正为 `100us`，避免 probe 自身过密发送对左图 `8B` latency CDF 的干扰。

## 3. 绘图输出

为贴近原文 Figure 3，我重新生成了 paper-style CDF 图。绘图脚本为：

```text
scripts/lab1_o_fig3_paper_plot.py
```

生成目录为：

```text
outputs/sird-scenarios/lab1_o_fig3_rerun_interval8b100us_probe2000_long17_qsample100us_20260429/paper_fig3
```

主要输出：

```text
lab1_o_fig3_paper_style.png
lab1_o_fig3_paper_style.pdf
lab1_o_fig3_8B_cdf.png
lab1_o_fig3_500KB_cdf.png
lab1_o_fig3_paper_summary.csv
```

推荐论文使用 `PDF` 版本；Markdown 预览使用 `PNG` 版本。

## 4. Figure 3 风格复现图

![Lab1 Figure 3 paper-style reproduction](../outputs/sird-scenarios/lab1_o_fig3_rerun_interval8b100us_probe2000_long17_qsample100us_20260429/paper_fig3/lab1_o_fig3_paper_style.png)

这张图按原文 Figure 3 的结构组织：

- 左图：`8B` request latency CDF，比较 `Unloaded` 和 `Incast`；
- 右图：`500KB` request latency CDF，比较 `Unloaded`、`Incast-SRPT` 和 `Incast-SRR`；
- 横轴是 request/reply latency，单位为微秒；
- 纵轴是 CDF；
- 左图横轴范围设为 `0-50us`；
- 右图横轴范围设为 `0-800us`。

这张图的核心用途是说明：在 receiver downlink 被 6 个 `10MB` 背景流饱和时，SIRD 对小 request 和中等 request 的延迟控制能力。

## 5. 左图：8B request under incast

![8B request latency CDF zoomed view](../outputs/sird-scenarios/lab1_o_fig3_rerun_interval8b100us_probe2000_long17_qsample100us_20260429/paper_fig3/lab1_o_fig3_8B_cdf_zoom.png)

上图是 `8B` request 的放大视图，用于展示 `Unloaded` 和 `Incast` 之间非常小的延迟差异。由于两条曲线在原始 `0-50us` 横轴下几乎重合，因此这里将横轴缩放到 `18us` 附近，并直接标注 `p50`，以便读者看清两者的相对位置关系。

关键数值：

| Case | Samples | p50 | p99 | Mean |
|---|---:|---:|---:|---:|
| Unloaded 8B | 2000 | 18.019us | 18.019us | 18.019us |
| Incast 8B | 2000 | 18.108us | 18.180us | 18.107us |

这张图说明：

- unloaded baseline 的 8B request latency 约为 `18us`；
- incast 下 8B request 的 p50 仅增加到 `18.108us`，p99 为 `18.180us`；
- 即使 receiver downlink 被 6 个背景大流饱和，小 request 只增加了约 `0.1-0.2us` 的延迟；
- 这说明 SIRD 在 receiver congestion 场景下没有让小 request 被大量 queueing delay 淹没。

论文中可以这样解释：

> 对于 `8B` request，incast 背景流量只带来极小的额外延迟。unloaded baseline 的 p50/p99 均为 `18.019us`，而 incast 下 p50 为 `18.108us`、p99 为 `18.180us`。这表明在接收端链路被饱和时，SIRD 仍能将小消息延迟保持在几乎与 unloaded 相同的水平。

这里需要特别说明：旧版结果中，`8B probe` 采用了过密的发送间隔，导致 probe 自身对测量结果产生干扰，使左图出现明显右移。本次重跑将 `8B probe` 间隔修正为 `100us` 后，左图重新回到与原文 Figure 3 更一致的形态，即 `Incast` 曲线仅略高于 `Unloaded`。

需要注意的是，`8B` request 属于 unscheduled short request，主要体现 SIRD/Homa-style short-message fast path 在接收端拥塞下的低延迟表现。它不是用来说明 receiver scheduling policy 的优劣；调度策略的差异主要体现在右图的 `500KB` request。

## 6. 右图：500KB request under SRPT and SRR

![500KB request latency CDF](../outputs/sird-scenarios/lab1_o_fig3_rerun_interval8b100us_probe2000_long17_qsample100us_20260429/paper_fig3/lab1_o_fig3_500KB_cdf.png)

右图比较 `500KB` request 在 unloaded、incast-SRPT 和 incast-SRR 下的 latency CDF。

关键数值：

| Case | Samples | p50 | p99 | Mean |
|---|---:|---:|---:|---:|
| Unloaded 500KB | 2000 | 77.760us | 77.760us | 77.760us |
| Incast 500KB SRPT | 2000 | 78.164us | 104.6418us | 78.947us |
| Incast 500KB SRR | 2000 | 322.591us | 325.831us | 322.099us |

这张图说明：

- `500KB` request 在 unloaded 下延迟约为 `77.76us`；
- 在 incast-SRPT 下，p50 仍接近 unloaded，仅为 `78.164us`；
- incast-SRPT 的 p99 为 `104.6418us`，尾部有一定增加，但总体仍接近 unloaded；
- 在 incast-SRR 下，p50/p99 都约为 `322-326us`，显著高于 SRPT；
- 这说明 receiver scheduling policy 会直接影响 scheduled message 的 latency。

SRPT 和 SRR 的含义：

- SRPT：优先给剩余字节数更少的消息发 credit，因此 `500KB` request 会优先于 `10MB` 背景消息完成；
- SRR：按 sender 近似 round-robin 发 credit，更公平，但不会特别偏向较小的 `500KB` request，因此 `500KB` request latency 明显更高。

论文中可以这样解释：

> 对于 `500KB` request，SRPT 调度使其在 incast 下仍接近 unloaded latency：p50 从 `77.760us` 增加到 `78.164us`，p99 为 `104.6418us`。相比之下，SRR 调度下 p50 达到 `322.591us`，p99 达到 `325.831us`。这说明 SIRD 的 receiver-side credit scheduling 可以表达不同策略：SRPT 更有利于降低小/中等消息延迟，而 SRR 更偏向公平共享，因此会牺牲 `500KB` request 的完成时间。

## 7. 与原文 Figure 3 的对应关系

原文 Figure 3 的设置是：6 个发送端 open-loop 发送 `10MB` messages 形成 incast，第 7 个发送端周期性发送 `8B` 或 `500KB` requests，并记录 latency CDF。本地 `lab1_o` 的最终结果按这个结构实现：

| 原文 Figure 3 元素 | 本地 lab1_o 对应 |
|---|---|
| 6 个 incast senders | 6 个 background long-flow senders |
| 10MB messages | `LONG_MSG_SIZE_BYTES=10000000` |
| 17Gbps each | `LONG_SENDER_RATE_GBPS=17.0` |
| 7th sender sends probe requests | probe sender sends request/reply probes |
| 8B requests | `shortMsgSizeBytes=8` |
| 500KB requests | `shortMsgSizeBytes=500000` |
| Unloaded baseline | `enableBackgroundTraffic=0` |
| Incast | `enableBackgroundTraffic=1` |
| SRPT / SRR | `useSrrScheduling=0/1` |
| Latency CDF | request/reply latency CDF |

复现效果上，左图和右图的趋势与原文一致：

- 8B request 在 incast 下只出现小幅延迟增加；
- 500KB request 在 SRPT 下接近 unloaded；
- 500KB request 在 SRR 下明显更慢；
- 图形结构、曲线命名、坐标范围与原文 Figure 3 保持一致。

但需要在论文中避免过度表述为“完全复刻”。更严谨的说法是：

> 本实验复现了 Figure 3 的核心场景和趋势，但运行环境是 ns-3 中的 `lab1_o` 实现，而非原文 CloudLab/Caladan 实机环境。因此本文将其作为 Figure 3 的 reproduction-style validation，用于验证实现中的 receiver congestion 行为是否符合论文趋势。

## 8. 为什么与原文图形仍有差距

虽然本次结果已经复现了 Figure 3 的核心趋势，但图形形态与原文仍存在明显差距，主要体现在以下几个方面。

第一，运行环境不同。原文 Figure 3 基于 CloudLab/Caladan 实机环境，真实系统中会叠加主机软件栈、NIC 调度、计时器抖动、DMA/中断、缓存扰动和背景噪声等因素，因此 latency 分布通常更宽，CDF 曲线也更平滑。相比之下，`lab1_o` 运行在 ns-3 中，链路、主机和事件调度更理想化，所以 unloaded 曲线往往接近一条竖线，incast 曲线也会比原文更陡。

第二，实现语义虽然对齐，但不可能与原文一字不差。本文已经按照论文描述构造了“6 个 `10MB` 背景发送端 + 第 7 个 probe sender”的 receiver-side incast 场景，也保留了 `17Gbps`、`8B`、`500KB`、SRPT 和 SRR 等关键要素；但 `lab1_o` 毕竟是 ns-3 中的 SIRD/Homa 风格实现，而不是原文系统本身。因此，即使趋势一致，绝对时延数值、尾部形态和曲线平滑程度仍会存在偏差。

第三，`8B` 左图的旧版偏差主要来自实验配置问题，而不是协议结论本身有误。旧版实验中 `8B probe` 的发送间隔设置过小，导致 probe 自身形成了额外干扰，使得左图的 `Incast` 曲线被人为拉宽。修正为 `100us` 间隔后，`8B` 的结果重新回到“仅略高于 unloaded”的形态，说明此前最大的偏差来自测量方法，而不是 SIRD 在该场景下失效。

第四，当前结果仍然比原文更“紧”。这主要是因为本次仿真中的拓扑、发送模式和链路参数是固定的，且随机性较弱；同时每条曲线只统计 `2000` 个 probe sample。在这种条件下，CDF 更容易表现为集中、陡峭的分布，而不像实机测量那样自然带有更宽的抖动和长尾。

因此，本文更合适的表述不是“完全复刻原图”，而是“在 ns-3 环境中复现了 Figure 3 的核心场景与主要趋势，但由于实验平台和系统实现不同，曲线形态仍与原文存在可解释的差异”。对于毕业论文，这种表述既准确，也更经得起推敲。

## 9. 可以直接放入毕业论文的段落

本文使用 `lab1_o` 场景复现 SIRD 论文 Figure 3 的 receiver congestion 实验。该实验由 6 个背景发送端向同一接收端发送 `10MB` 大消息，每个背景发送端速率为 `17Gbps`，从而使接收端 `100Gbps` downlink 进入饱和状态。第 7 个发送端周期性发送 probe request，分别测试 `8B` 和 `500KB` 两种 request size，并在客户端记录端到端 request/reply latency。

图中左侧展示 `8B` request 的 latency CDF。unloaded baseline 的 p50/p99 均为 `18.019us`；在 incast 背景流量下，p50 为 `18.108us`，p99 为 `18.180us`。这说明即使接收端链路被大消息 incast 饱和，SIRD 仍能将小 request 的额外延迟控制在极低水平。

图中右侧展示 `500KB` request 在不同 receiver scheduling policy 下的 latency CDF。SRPT 调度下，`500KB` request 的 p50 为 `78.164us`，接近 unloaded baseline 的 `77.760us`；而 SRR 调度下 p50 增加到 `322.591us`。这是因为 SRPT 优先调度剩余字节数更少的消息，使 `500KB` request 能够优先于 `10MB` 背景消息完成；而 SRR 以更公平的方式在发送端之间分配 credit，因此显著增加了 `500KB` request 的完成时间。

因此，该实验说明了两个结论：第一，SIRD 在 receiver congestion 场景下能够避免小消息被大流排队延迟严重影响；第二，SIRD 的 receiver-side credit scheduling 可以表达不同策略，SRPT 能显著降低中等大小消息的 latency，而 SRR 则体现公平性与低延迟之间的权衡。

## 10. 图注建议

中文图注：

> 图 X：`lab1_o` 对 SIRD 论文 Figure 3 的复现。6 个背景发送端以 `17Gbps` 向同一接收端发送 `10MB` 大消息，形成 receiver-side incast；第 7 个发送端周期性发送 probe request 并记录 request/reply latency。左图为 `8B` request 的 latency CDF，比较 unloaded 和 incast；右图为 `500KB` request 的 latency CDF，比较 unloaded、Incast-SRPT 和 Incast-SRR。

英文图注：

> Figure X: Reproduction-style result for SIRD Figure 3 using `lab1_o`. Six background senders transmit `10MB` messages to one receiver at `17Gbps` each, forming receiver-side incast. A seventh sender periodically issues probe requests and records request/reply latency. Left: latency CDF for `8B` requests under unloaded and incast conditions. Right: latency CDF for `500KB` requests under unloaded, Incast-SRPT, and Incast-SRR configurations.

## 11. 写作注意事项

- 建议称为“复现 Figure 3 的核心趋势”或“reproduction-style validation”，不要写成完全复刻原文实机实验。
- 左图重点不是调度策略，而是小 request 在 receiver incast 下仍保持低延迟。
- 右图重点是 receiver scheduling policy：SRPT 接近 unloaded，SRR 延迟明显增加。
- 如果论文只放一张图，建议使用 combined 图 `lab1_o_fig3_paper_style.pdf`。
- 如果论文排版需要左右分开，可以使用 `lab1_o_fig3_8B_cdf.png` 和 `lab1_o_fig3_500KB_cdf.png`。
