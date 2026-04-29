# Lab2 Figure 4 复现说明

本文档说明 `lab2` 最终结果如何用于复现 SIRD 论文 Figure 4，并给出可以放入毕业论文的场景描述、图解释和结论表述。

## 1. 数据来源

最终结果目录：

```text
/mnt/nasDisk_ds3617/sird/lab2/20260427/lab2_batch_profiles4_sched2_delay0_1_2_4_sampleonly_bdp150_20260427_143352/full_srr_delay0us
```

本地拷贝和重画图目录：

```text
outputs/sird-scenarios/lab2_figure4_full_srr_delay0us_20260427_143352/
```

新生成的 Figure 4 风格图：

```text
outputs/sird-scenarios/lab2_figure4_full_srr_delay0us_20260427_143352/plots/lab2_figure4_paper_style.png
outputs/sird-scenarios/lab2_figure4_full_srr_delay0us_20260427_143352/plots/lab2_figure4_paper_style.pdf
```

带 receiver 启动时刻标注的辅助图：

```text
outputs/sird-scenarios/lab2_figure4_full_srr_delay0us_20260427_143352/plots/lab2_figure4_with_receiver_starts.png
outputs/sird-scenarios/lab2_figure4_full_srr_delay0us_20260427_143352/plots/lab2_figure4_with_receiver_starts.pdf
```

本次重画使用的脚本：

```text
scripts/lab2_figure4_plot.py
```

实验元数据：

```text
profile=full
scheduler=srr
use_srr=1
delay_us=0
bdp_pkts=150
trace_credit_sample=1
trace_msg=0
trace_protocol_credit=0
```

运行参数摘要：

```text
durationSec=13.2
flowGapUs=4500000
sendIntervalUs=400
backloggedFlow=1
backlogDepthMsgs=2
activeReceiverCount=3
sampleUs=100
settleTailSec=0.1
```

绘图处理：

- 横轴从第一个 receiver 开始发送时刻算起，即原始仿真时间 `0.2s` 被平移为 `0s`。
- 三个 receiver 的启动时刻分别约为 `0s`、`4.5s`、`9.0s`。
- 纵轴使用 `BDP` 归一化 credit。
- 曲线使用 `100ms` moving average，和论文 Figure 4 caption 中的移动平均窗口一致。

## 2. 场景描述

该实验复现的是论文 Figure 4 中的 sender-congestion / sender-information 场景。拓扑中只有一个发送端和三个接收端。发送端以 full-rate/backlogged 的方式向三个接收端发送 `10MB` 大消息，三个接收端并不是同时开始接收流量，而是按固定间隔依次加入。

实验的重点不是比较 SRPT 和 SRR 的调度优劣，而是观察 sender-side feedback 是否能让接收端识别“发送端已经成为瓶颈”。当一个 sender 同时服务多个 receiver 时，如果 receiver 不了解 sender 端是否拥塞，就可能继续向该 sender 分配 credit。这样 credit 会被积累在 sender 侧，导致 receiver 侧可用 credit 下降。SIRD 的 sender feedback 机制通过 sender 标记拥塞信息，使 receiver 能够减少给拥塞 sender 的 credit，从而避免 credit 被困在 sender 端。

本实验比较两条曲线：

- `S_Thr = 0.5 x BDP`：开启 sender feedback。sender 侧 credit 超过阈值后会反馈拥塞信息。
- `S_Thr = infinity`：近似关闭 sender feedback。sender 侧阈值设为无穷大，因此 receiver 基本无法通过 sender-side signal 获知 sender 拥塞。

## 3. Figure 4 风格主图

![Lab2 Figure 4 paper style](../outputs/sird-scenarios/lab2_figure4_full_srr_delay0us_20260427_143352/plots/lab2_figure4_paper_style.png)

这张图采用双子图结构，尽量贴近论文 Figure 4 的表达方式：

- 左图 `(a)`：sender 端累积或持有的 credit，单位为 `BDP`。
- 右图 `(b)`：三个 receiver 侧对该 sender 仍可用的总 credit，单位为 `BDP`。
- 蓝色曲线：`S_Thr = 0.5 x BDP`，即开启 sender feedback。
- 红色曲线：`S_Thr = infinity`，即近似关闭 sender feedback。

### 3.1 左图表达什么

左图展示 credit 是否被困在 sender 端。

在开启 sender feedback 的蓝色曲线中，sender 侧 credit 在三个 receiver 陆续加入后仍保持在约 `0.42 BDP`。这说明 sender feedback 能够抑制 receiver 继续向拥塞 sender 分配过多 credit。

在关闭 sender feedback 的红色曲线中，sender 侧 credit 随 receiver 加入而阶梯式上升。第二个 receiver 加入后，sender-held credit 上升到约 `0.96 BDP`；第三个 receiver 加入后，最终接近 `1.85 BDP`。这说明没有 sender feedback 时，多个 receiver 会继续给同一个 sender 发放 credit，导致 credit 在 sender 端堆积。

该图支持的结论：

> Sender feedback prevents credits from accumulating at a congested sender. Without sender feedback, receiver credits are misallocated to the sender bottleneck and become trapped at the sender.

中文表述：

> Sender feedback 能够防止 credit 在拥塞 sender 端堆积。关闭 sender feedback 后，多个 receiver 无法感知 sender 侧瓶颈，仍然继续向该 sender 分配 credit，导致 sender-held credit 随 receiver 数量增加而逐步上升。

### 3.2 右图表达什么

右图展示三个 receiver 侧仍然可用的总 credit。

开启 sender feedback 时，receiver 侧可用 credit 在第三个 receiver 加入后仍保持在约 `3.07 BDP`。这表示 credit 没有大量被无效地转移到 sender 端，而是留在 receiver 侧等待更合理的分配。

关闭 sender feedback 时，receiver 侧可用 credit 随 receiver 加入而下降。最终只有约 `1.64 BDP` 的可用 credit 留在 receiver 侧。这个现象与左图互相印证：receiver 侧可用 credit 减少，是因为 credit 被分配到 sender 端并在那里积累。

该图支持的结论：

> Sender feedback preserves credit availability at receivers. When sender feedback is disabled, receiver-side available credit drops because credits are consumed by a sender-side bottleneck.

中文表述：

> Sender feedback 能保持 receiver 侧 credit 的可用性。关闭 sender feedback 后，receiver 侧可用 credit 明显下降，说明 credit 被错误地分配给已经拥塞的 sender，并在 sender 侧积累。

## 4. 带 receiver 启动标注的辅助图

![Lab2 Figure 4 with receiver starts](../outputs/sird-scenarios/lab2_figure4_full_srr_delay0us_20260427_143352/plots/lab2_figure4_with_receiver_starts.png)

这张图和主图使用相同数据，但额外用虚线标出了三个 receiver 的加入时刻：

- `R1`：`0s`
- `R2`：`4.5s`
- `R3`：`9.0s`

这张辅助图更适合在论文撰写或答辩解释时使用。它能清楚说明红色曲线的阶梯式变化来自 receiver 的依次加入：

- 第一个 receiver 开始后，系统只有一个接收端消耗 sender 能力，sender 侧 credit 没有明显堆积。
- 第二个 receiver 加入后，关闭 feedback 的红色曲线开始积累约 `1 BDP` credit。
- 第三个 receiver 加入后，关闭 feedback 的红色曲线进一步上升到接近 `2 BDP`。
- 开启 feedback 的蓝色曲线则始终被压在约 `0.5 BDP` 附近。

如果论文主图需要尽量贴近原文，建议使用无标注的 `lab2_figure4_paper_style.png`。如果论文需要更强解释性，或者答辩 PPT 中需要讲清楚曲线阶梯变化原因，可以使用这张带 receiver-start 标注的版本。

## 5. 数值摘要

重画脚本生成的 summary：

```text
outputs/sird-scenarios/lab2_figure4_full_srr_delay0us_20260427_143352/plots/lab2_figure4_summary.csv
```

关键数值如下：

| Case | Sender credit final | Sender credit mean after R3 | Receiver available final | Receiver available mean after R3 |
|---|---:|---:|---:|---:|
| `S_Thr = 0.5 x BDP` | 0.420 BDP | 0.423 BDP | 3.068 BDP | 3.065 BDP |
| `S_Thr = infinity` | 1.853 BDP | 1.841 BDP | 1.636 BDP | 1.647 BDP |

这个结果和论文 Figure 4 想表达的趋势一致：

- 开启 sender feedback 后，sender-held credit 被控制在约 `0.5 BDP` 以下。
- 关闭 sender feedback 后，sender-held credit 接近 `2 BDP`。
- 开启 sender feedback 后，receiver 侧保留更多可用 credit。
- 关闭 sender feedback 后，receiver 侧可用 credit 明显下降。

## 6. 为什么和原文 Figure 4 有差距

本复现图和原文 Figure 4 在趋势上是一致的：开启 sender feedback 后，sender 侧 credit 被限制在较低水平；关闭 sender feedback 后，credit 会随 receiver 数量增加而在 sender 端积累，同时 receiver 侧可用 credit 下降。但它不应被描述为和原文“逐点一致”或“逐像素一致”。差距主要来自以下几类原因。

### 6.1 复现环境不同

原文 Figure 4 来自论文作者的实现和实验环境，而本文使用的是 ns-3 中重新实现的 Homa/SIRD 逻辑。即使宏观机制相同，具体实现中的 packet scheduling、grant timing、credit bookkeeping、事件调度顺序和 trace 采样方式都可能不同。因此，两张图应该比较趋势和稳态水平，而不是比较每一个时间点的精确数值。

论文中可以这样写：

> 由于本文基于 ns-3 重新实现 SIRD/Homa 的关键机制，而非直接运行论文作者的原始代码，图中曲线与原文 Figure 4 不要求逐点一致。本文关注的是 sender feedback 是否能复现原文中的核心机制：限制 sender-held credit 并保持 receiver-side available credit。

### 6.2 参数和场景是按论文含义近似对齐

本实验使用 `BDP=150 packets`、`10MB` 大消息、三个 receiver 按 `4.5s` 间隔加入，并使用 `100ms` moving average。这些设置是为了贴合原文 Figure 4 的场景描述和图注。但论文未公开所有实现细节，例如具体的内部计时、grant 发放细节、消息补发节奏、每个 protocol event 的处理顺序等。因此本文只能做到机制和主要参数对齐，不能保证所有低层事件完全一致。

当前结果目录名中的 `delay0us` 指本批实验中 sender credit launch delay sweep 的 `0us` 配置，并不表示链路传播时延被改成 `0us`。该实验仍用于表达 Figure 4 的 sender feedback 对照关系。

### 6.3 当前结果使用 SRR receiver scheduling

当前最终结果目录是 `full_srr_delay0us`，即 receiver scheduling 使用 SRR 配置。原文 Figure 4 的核心并不是比较 SRPT 和 SRR，而是比较 `S_Thr = 0.5 x BDP` 与 `S_Thr = infinity`，即是否启用 sender feedback。因此，SRR 配置会影响某些细节时间点和 credit 分配轨迹，但不改变这张图要验证的主要机制。

在论文中应避免写成“SRR 导致 Figure 4 的结果”。更准确的表述是：

> 本实验在 SRR receiver scheduling 配置下复现 Figure 4 的 sender feedback 机制。由于 Figure 4 的核心变量是 sender feedback threshold，而非 receiver scheduling policy，本文主要解释 `S_Thr` 对 sender-held credit 和 receiver-available credit 的影响。

### 6.4 `S_Thr = infinity` 是工程近似

原文图例中的 `S_Thr = infinity` 表示 sender-side congestion threshold 被设为无穷大，从而关闭 sender feedback。在实现中，无穷大通常通过一个足够大的阈值近似，例如让 sender credit 在本实验范围内永远不会触发 sender feedback。因此，本文图中的 `S_Thr = infinity` 应理解为“近似关闭 sender feedback”，而不是数学意义上真正的无穷大。

这会带来一个写作注意点：可以说“近似关闭 sender feedback”，不要说“关闭了所有拥塞反馈”。尤其不要把它解释成关闭 ECN/CE。Figure 4 比较的是 sender-side feedback，而不是 core congestion 的 ECN 反馈。

### 6.5 采样和绘图处理不同

本图使用 `credit-sample.tr` 中的采样结果绘制，采样间隔为 `100us`，再进行 `100ms` moving average。原文 Figure 4 也使用移动平均，但原始数据来源、采样点、平滑窗口边界处理和绘图样式未必完全相同。因此，曲线转折处的斜率、短暂抖动、初始阶段的细节可能和原文不同。

例如本文图中曲线呈现较清楚的阶梯形，这是因为三个 receiver 按 `4.5s` 间隔加入，且采样和平滑后的 credit 状态比较稳定。原文中的曲线如果更平滑或局部过渡不同，不一定表示机制不一致，而可能来自实现和采样处理差异。

### 6.6 应该如何在论文里表述这种差距

建议使用以下表述：

> 本文复现结果与原文 Figure 4 在核心趋势上保持一致：启用 sender feedback 后，sender 侧累积 credit 被限制在约 `0.5 BDP` 附近，receiver 侧保留较高可用 credit；关闭 sender feedback 后，sender 侧 credit 随接收端数量增加而积累到接近 `2 BDP`，receiver 侧可用 credit 明显下降。由于本文基于 ns-3 重新实现协议机制，并且原文未公开所有低层实现细节，本文不追求曲线逐点一致，而是验证 sender feedback 所体现的控制机制是否能够复现。

更简短的写法：

> 复现图与原文 Figure 4 的数值细节存在差异，但主要趋势一致。差异主要来自仿真实现、参数近似、采样和平滑方式不同。本文关注的是机制复现，即 sender feedback 能否限制 sender-held credit 并保持 receiver-side available credit。

## 7. 可以直接写进毕业论文的段落

### 中文版本

为了复现 SIRD 论文中的 sender information 实验，本文构造了一个单发送端到三个接收端的 sender-congestion 场景。发送端持续向三个接收端发送 `10MB` 大消息，三个接收端以 `4.5s` 的间隔依次开始接收流量。该场景用于观察 receiver 是否能够通过 sender feedback 感知发送端瓶颈，并据此调整 credit 分配。

实验比较两种配置：第一种配置设置 `S_Thr = 0.5 x BDP`，即当 sender 侧累积 credit 超过 `0.5 BDP` 时触发 sender feedback；第二种配置设置 `S_Thr = infinity`，近似关闭 sender feedback。图中左侧展示 sender 侧累积 credit，右侧展示三个 receiver 侧对该 sender 的总可用 credit。所有 credit 均以 BDP 归一化，并使用 `100ms` moving average。

结果表明，开启 sender feedback 后，sender 侧 credit 在第三个 receiver 加入后仍保持在约 `0.42 BDP`，receiver 侧可用 credit 保持在约 `3.07 BDP`。相比之下，关闭 sender feedback 时，sender 侧 credit 随 receiver 加入阶梯式上升，最终达到约 `1.85 BDP`；receiver 侧可用 credit 则下降到约 `1.64 BDP`。这说明如果 receiver 无法获得 sender 侧拥塞信息，就会继续向已经成为瓶颈的 sender 分配 credit，导致 credit 在 sender 端堆积并降低 receiver 侧可用 credit。Sender feedback 能够抑制这种错误分配，使 credit 保持在 receiver 侧，从而更好地协调多个 receiver 对同一 sender 的请求。

### 图注建议

> Figure X: Sender information experiment under one-sender-three-receiver traffic. The sender transmits backlogged `10MB` messages to three time-staggered receivers. With `S_Thr = 0.5 x BDP`, sender feedback limits the credit accumulated at the sender and preserves receiver-side available credit. With `S_Thr = infinity`, sender feedback is effectively disabled, causing credits to accumulate at the sender and reducing available credits at receivers. Curves are smoothed with a `100ms` moving average.

中文图注：

> 图 X：单发送端到三个接收端场景下的 sender information 实验。发送端持续向三个按时间错开的接收端发送 `10MB` 消息。当 `S_Thr = 0.5 x BDP` 时，sender feedback 能限制 sender 侧 credit 积累，并保持 receiver 侧可用 credit；当 `S_Thr = infinity` 时，sender feedback 近似关闭，credit 会在 sender 侧堆积，receiver 侧可用 credit 随之下降。曲线使用 `100ms` moving average。

## 8. 论文中应强调的结论

可以把结论写成三点：

1. Figure 4 场景验证的是 sender-side information，而不是普通的 core congestion，也不是 SRPT/SRR 调度本身。

2. 当多个 receiver 同时向一个 sender 请求数据时，sender 可能成为瓶颈。如果没有 sender feedback，receiver 会继续向该 sender 分配 credit，使 credit 在 sender 侧积累。

3. Sender feedback 通过暴露 sender 侧拥塞状态，使 receiver 减少对拥塞 sender 的 credit 分配，从而避免 sender-held credit 过高，并保持 receiver-side credit availability。

更简洁的结论句：

> Sender feedback prevents receiver credits from being trapped at a sender bottleneck.

中文：

> Sender feedback 避免 receiver credit 被困在 sender 侧瓶颈处。

## 9. 写作注意事项

- 本结果目录是 `full_srr_delay0us`，即使用 SRR receiver scheduling 和 `delay_us=0` 的最终结果。
- 论文 Figure 4 的核心变量是 sender feedback threshold，即 `S_Thr = 0.5 x BDP` 与 `S_Thr = infinity` 的对比。
- 不要把这张图解释成 SRPT/SRR 调度性能对比。这里 SRR 是当前最终结果采用的 receiver scheduling 配置，但图的主旨仍是 sender information。
- 右图的 receiver available credit 下降不是坏的瞬时噪声，而是说明 credit 被转移并积累到 sender 端。
- 左右两图要一起解释：左图说明 credit 在 sender 端堆积，右图说明 receiver 侧可用 credit 因此减少。
- 本复现图在趋势上贴合论文 Figure 4，但不应表述为逐像素一致复现。更准确的说法是“复现了 Figure 4 的核心趋势和机制”。
