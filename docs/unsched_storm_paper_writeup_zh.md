# Unscheduled Short-message Storm 场景写法

本文档用于把 `unsched_storm` 实验写进论文或报告。它包含场景描述、问题定义、实验设置、结果表达、结论和写作注意事项。

## 1. 场景描述

本实验构造了一个多发送者到单接收者的短消息聚合场景。拓扑包含 `N` 个短消息发送端、一个可选的长流发送端、一个交换机和一个接收端。所有发送端都通过同一个交换机向同一个接收端发送流量，链路速率为 `100Gbps`，单段链路时延为 `1us`。

短消息大小设为 `8192B`。在默认 SIRD/Homa 配置下，该消息大小低于 unscheduled threshold，因此短消息可以不等待接收端 grant，直接以 unscheduled data 的形式发送。实验同时加入一条 `10MB` 的背景长流，用于观察短消息风暴对 scheduled traffic 的旁路影响。

这个场景刻意强调一种边界条件：receiver-driven credit 能够控制 scheduled data，但不能在短消息发送前控制已经被允许进入 unscheduled path 的数据。当许多发送端同时向同一个接收端发送低于阈值的短消息时，这些 unscheduled packets 会在接收端方向的交换机出口队列聚合。如果聚合速率超过接收端链路速率，receiver-facing queue 会快速增长，并在 lossy network 中产生 packet drop。

## 2. 要说明的问题

SIRD/Homa 的核心机制依赖接收端通过 grant/credit 控制 scheduled traffic，从而避免网络内部形成持久队列。然而，低于 unscheduled threshold 的短消息不需要等待 receiver credit。这个设计有利于小 RPC 的低延迟，但也带来一个潜在弱点：

> 当大量短消息同时发往同一个接收端时，unscheduled traffic 可能绕过 receiver-driven credit control，在接收端方向形成瞬时过载，导致队列溢出和丢包。

因此，本实验要回答的问题是：

1. 聚合 unscheduled short messages 是否会打满 receiver-facing queue？
2. 这种现象是否会导致 packet drop 和短消息完成率下降？
3. 如果把同样的短消息强制进入 scheduled path，是否能消除队列溢出？
4. 这种修复是否需要付出排队延迟增加的代价？

## 3. 实验设计

实验比较两种模式：

- `unsched`：使用正常 unscheduled threshold。`8192B` 短消息低于阈值，因此主要走 unscheduled path。
- `scheduled`：把 unscheduled threshold 降为 `1` packet，使同样的 `8192B` 短消息基本进入 scheduled path，作为 receiver-credit-controlled 对照组。

实验 sweep 参数如下：

```text
senders = 8, 16, 32
aggregate short-message load = 60, 90, 110, 130, 160, 200 Gbps
link rate = 100 Gbps
link delay = 1 us
short message size = 8192 B
long background flow = enabled, 10 MB, 17 Gbps
qdisc max size = 1000 packets
simulation duration = 5 ms
```

主要观测指标：

- receiver-facing queue peak；
- receiver-side packet drops；
- short-message completion ratio；
- short-message p99 FCT；
- background long-flow FCT。

结果目录：

```text
/mnt/nasDisk_ds3617/sird/unsched_storm/20260429/unsched_storm_sweep_delay1us_n8_16_32_load60_200_20260429_153215
```

本地拷贝：

```text
outputs/sird-scenarios/unsched_storm_server_sweep_delay1us_n8_16_32_load60_200_20260429_153215/plots/
```

## 4. 结果描述

### 4.1 Queue 和 drop

实验结果显示，`unsched` 模式在 `60Gbps` 和 `90Gbps` 下没有产生 drop，短消息全部完成。但从 `110Gbps` 开始，所有 sender 数量配置下的 receiver-facing queue 都达到 `1000p` 上限，并产生大量 packet drop。

相比之下，`scheduled` 模式在所有负载点和 sender 数量下都没有产生 receiver-side drop，receiver-facing queue peak 始终为 `0p`。

可以在论文中这样描述：

> 当 aggregate short-message load 不超过接收端链路容量时，unscheduled traffic 没有造成明显队列积压。然而，一旦 offered load 超过接收端 `100Gbps` 链路容量，unscheduled short messages 会迅速填满 receiver-facing queue。在 `110Gbps` 及更高负载下，`unsched` 模式的队列峰值均达到 `1000` packets，并伴随大量 packet drops。相同负载下，强制进入 scheduled path 的对照组没有出现队列积压和丢包，说明 receiver credit 能够控制 scheduled traffic，但无法预先约束已进入 unscheduled path 的短消息。

关键结果：

| Senders | Load | Mode | Peak queue | Drops | Short completion |
|---:|---:|---|---:|---:|---:|
| 8 | 200Gbps | unsched | 1000p | 47679 | 11.3% |
| 16 | 200Gbps | unsched | 1000p | 47895 | 7.6% |
| 32 | 200Gbps | unsched | 1000p | 48053 | 5.5% |
| 8 | 200Gbps | scheduled | 0p | 0 | 100% |
| 16 | 200Gbps | scheduled | 0p | 0 | 100% |
| 32 | 200Gbps | scheduled | 0p | 0 | 100% |

### 4.2 Short-message completion ratio

在 `unsched` 模式下，短消息完成率随着负载升高明显下降。以 `200Gbps` 为例，`N=8` 时短消息完成率为 `11.3%`，`N=16` 时为 `7.6%`，`N=32` 时进一步下降到 `5.5%`。

这说明在 lossy network 中，unscheduled short-message storm 不只是造成临时排队，还会造成大量消息无法在仿真窗口内完成。这个现象符合预期：一旦 receiver-facing queue 溢出，后续短消息包会被 drop，短消息完成率会快速下降。

可以在论文中这样写：

> The loss behavior is reflected directly in message completion. At `200Gbps`, only `11.3%`, `7.6%`, and `5.5%` of short messages complete for `N=8`, `16`, and `32`, respectively. In contrast, all short messages complete in the scheduled baseline. This confirms that the observed queue buildup is not merely a transient buffering artifact; it translates into application-visible message loss or non-completion.

如果全文是中文，可以改写为：

> 丢包最终会反映到消息完成率上。在 `200Gbps` 负载下，`N=8/16/32` 的 `unsched` 模式短消息完成率分别只有 `11.3%`、`7.6%` 和 `5.5%`。而在 `scheduled` 对照组中，所有短消息均能完成。这说明 receiver-facing queue 的增长不是单纯的瞬时缓存现象，而会直接转化为应用可见的消息丢失或未完成。

### 4.3 FCT 结果的正确解释

需要特别注意：`unsched` 高负载下的 short-message p99 FCT 大约稳定在 `240us` 左右，但这个数字不能解释为 `unsched` 延迟更好。原因是大量短消息已经没有完成，FCT 统计只覆盖完成的消息，存在明显 survivor bias。

因此论文中不应该单独用 `unsched` 的 p99 FCT 证明其低延迟。正确表达应当是：

> Although the completed short messages in the unscheduled case show sub-millisecond FCT, this metric is biased by survival: most short messages are dropped or remain incomplete under overload. Therefore, completion ratio and receiver-side drops are the primary indicators for this experiment.

中文写法：

> 虽然 `unsched` 模式下已完成短消息的 p99 FCT 仍处在亚毫秒级，但该指标存在幸存者偏差，因为高负载下大部分短消息已经丢包或未完成。因此，本实验不能用已完成消息的 FCT 来说明 `unsched` 更优，而应主要依据 completion ratio、receiver-side drops 和 queue occupancy 判断系统行为。

### 4.4 Scheduled 对照组的代价

`scheduled` 模式完全消除了 receiver-facing queue buildup 和 drop，但代价是短消息延迟随着负载上升而明显增加。在 `200Gbps` 下，`scheduled` 模式的 short-message p99 FCT 约为 `5.94ms`。这说明把短消息强制进入 scheduled path 并不是免费的优化；它用 receiver-side control 换来了可靠完成和无丢包，但牺牲了短消息低延迟。

可以在论文中这样表达：

> The scheduled baseline eliminates receiver-side drops, but it does so by serializing short messages through the receiver credit path. As load increases, short-message latency rises substantially; at `200Gbps`, p99 FCT reaches about `5.94ms`. This highlights the tradeoff: unscheduled transmission improves low-load latency, while scheduled control prevents overload-induced loss at the cost of queueing delay.

中文写法：

> `scheduled` 对照组能够完全避免 receiver-facing queue 溢出和 packet drop，但它通过 receiver credit path 对短消息进行调度控制，因此高负载下短消息排队延迟显著增加。在 `200Gbps` 负载下，`scheduled` 模式的 short-message p99 FCT 约为 `5.94ms`。这体现了一个明确权衡：unscheduled path 有利于低负载短消息延迟，而 scheduled path 能避免过载丢包，但会引入更高排队延迟。

## 5. 图和读图说明

本节把实验图直接放进文档，并说明每张图表达什么、能够支持什么结论。论文中最建议使用前两张图作为主证据，第三张图说明代价，第四张图作为补充时序证据，第五张图只在讨论背景长流时使用。

### 5.1 Receiver-facing queue peak

![Peak receiver queue vs load](../outputs/sird-scenarios/unsched_storm_server_sweep_delay1us_n8_16_32_load60_200_20260429_153215/plots/peak_receiver_queue_vs_load.png)

这张图展示不同 offered load 下 receiver-facing queue 的峰值。横轴是聚合短消息负载，纵轴是接收端方向交换机出口队列的最大 packet 数。不同曲线对应不同 sender 数量和 `unsched/scheduled` 模式。

这张图最直接说明了问题是否发生：

- 在 `60Gbps` 和 `90Gbps` 下，`unsched` 和 `scheduled` 都没有明显 receiver queue 积压。
- 从 `110Gbps` 开始，`unsched` 的 receiver-facing queue 直接达到 `1000p` 上限。
- `scheduled` 在所有负载下 queue peak 都保持为 `0p`。

可以从这张图得到的结论是：当聚合短消息负载超过接收端 `100Gbps` 链路容量后，unscheduled short messages 会绕过 receiver credit 并在接收端方向聚合，导致 receiver-facing queue 被打满。相同流量如果进入 scheduled path，则 receiver credit 能够避免该队列积压。

论文图注可以写：

> Receiver-facing queue peak under unscheduled short-message storms. Once the aggregate short-message load exceeds the receiver link capacity, the unscheduled path fills the receiver-facing queue to the `1000`-packet limit, while the scheduled baseline keeps the queue empty.

中文图注可以写：

> Unscheduled 短消息风暴下的 receiver-facing queue 峰值。当聚合短消息负载超过接收端链路容量后，`unsched` 模式将接收端方向队列打满到 `1000p`；而 `scheduled` 对照组始终没有形成队列积压。

### 5.2 Receiver-side packet drops

![Receiver drops vs load](../outputs/sird-scenarios/unsched_storm_server_sweep_delay1us_n8_16_32_load60_200_20260429_153215/plots/receiver_drops_vs_load.png)

这张图展示 receiver-facing queue 发生溢出后产生的 packet drop 数量。它和上一张 queue peak 图配合使用：queue peak 说明队列被打满，drop 图说明队列打满后确实造成了丢包，而不是只出现了暂时缓存。

这张图表达的核心现象是：

- `unsched` 在 `110Gbps` 开始出现 drop。
- 随着负载从 `110Gbps` 增加到 `200Gbps`，drop 数量持续上升。
- `scheduled` 在所有 sender 数量和负载下 drop 都是 `0`。
- 在 `200Gbps` 下，`unsched` 的 drop 约为 `4.8e4` packets，且 `N=8/16/32` 都出现同一量级的严重丢包。

这张图可以说明：unscheduled short-message storm 在 lossy network 中会造成真实 packet loss。该实验不是只观察到队列上升，而是观察到 receiver-side overflow 直接转化为丢包。

论文正文可以这样接在 queue 图之后：

> The queue buildup leads to actual packet loss. Starting at `110Gbps`, the unscheduled cases incur receiver-side drops, and the number of dropped packets increases with load. At `200Gbps`, all sender configurations drop around `4.8e4` packets. The scheduled baseline has zero drops across the entire sweep.

中文写法：

> 队列打满后会进一步转化为真实丢包。从 `110Gbps` 开始，`unsched` 模式出现 receiver-side drops，并且 drop 数量随 offered load 增加而上升。在 `200Gbps` 负载下，三种 sender 数量配置均产生约 `4.8e4` 个 packet drops。相比之下，`scheduled` 对照组在整个 sweep 中没有出现丢包。

### 5.3 Short-message p99 FCT

![Short-message p99 FCT vs load](../outputs/sird-scenarios/unsched_storm_server_sweep_delay1us_n8_16_32_load60_200_20260429_153215/plots/short_p99_fct_vs_load.png)

这张图展示短消息 p99 FCT 随负载变化的趋势。它需要谨慎解释，因为在 `unsched` 高负载下，大量短消息没有完成，图中 `unsched` 的 FCT 只统计了幸存完成的那一小部分消息。

这张图可以支持两个结论：

1. 在低负载下，unscheduled path 保持短消息低延迟，这是它的设计优势。
2. 在高负载下，scheduled path 虽然避免了 drop，但短消息 p99 FCT 明显上升，`200Gbps` 时约为 `5.94ms`。

这张图不能支持的结论是：

- 不能说 `unsched` 在高负载下仍然延迟更低。
- 因为 `unsched` 高负载下绝大多数短消息已经 drop 或未完成，完成消息的 FCT 存在 survivor bias。

因此论文中应把这张图作为“scheduled path 的代价”来解释，而不是作为 `unsched` 延迟优势的主证据。

推荐写法：

> The p99 FCT of the scheduled baseline increases with load because short messages are serialized by receiver-side credit control. At `200Gbps`, scheduled p99 FCT reaches about `5.94ms`. The unscheduled p99 FCT under overload should not be interpreted as better latency, because most short messages are dropped or incomplete; the FCT statistic only covers surviving messages.

中文写法：

> `scheduled` 对照组的 p99 FCT 随负载升高而明显增加，这是因为短消息被纳入 receiver credit path 后需要排队等待调度。在 `200Gbps` 下，`scheduled` 的 p99 FCT 约为 `5.94ms`。需要注意的是，高负载下 `unsched` 的 FCT 只统计了少量完成消息，存在幸存者偏差，不能据此认为 `unsched` 延迟更好。

### 5.4 Highest-load queue time series

![Receiver queue time series at highest load](../outputs/sird-scenarios/unsched_storm_server_sweep_delay1us_n8_16_32_load60_200_20260429_153215/plots/receiver_queue_timeseries_highest_load.png)

这张图展示最高负载 `200Gbps` 下 receiver-facing queue 随时间变化的过程。相比第一张 peak 图，它不是只给一个最大值，而是展示队列如何在仿真过程中上升、触顶并维持在高水位。

这张图适合作为补充证据，用来说明：

- queue overflow 不是单个采样点的偶然峰值；
- 在 overload 窗口内，receiver-facing queue 会持续处于满队列或接近满队列状态；
- `unsched` 的失效机制是持续的接收端方向过载，而不是测量噪声。

论文中可以简短写：

> The time-series view at `200Gbps` confirms that the receiver-facing queue remains saturated during the overload period, rather than exhibiting an isolated transient spike.

中文写法：

> `200Gbps` 下的队列时序进一步说明，receiver-facing queue 不是偶然出现单点峰值，而是在过载期间持续触顶或接近触顶。这说明 unscheduled storm 造成的是持续性的接收端方向过载。

### 5.5 Background long-flow p99 FCT

![Long-flow p99 FCT vs load](../outputs/sird-scenarios/unsched_storm_server_sweep_delay1us_n8_16_32_load60_200_20260429_153215/plots/long_p99_fct_vs_load.png)

这张图展示背景 `10MB` 长流的 p99 FCT。它不是本实验的主证据，但可以用于讨论短消息调度策略对背景 scheduled traffic 的影响。

从当前结果看，长流在高负载下也会受到短消息流量影响。需要注意的是，本实验中的长流主要用于提供背景 scheduled traffic，核心结论仍然来自 receiver queue、drop 和短消息完成率。若论文篇幅有限，可以不放这张图，或者只在讨论部分使用。

推荐解释方式：

> The background long flow is included to verify that the short-message storm coexists with scheduled traffic. Its FCT is not the primary metric in this experiment. The main conclusion relies on receiver queue occupancy, packet drops, and short-message completion ratio.

中文写法：

> 背景长流用于说明短消息风暴与 scheduled traffic 同时存在，但它不是本实验的主要评价指标。本文的核心结论应主要依据 receiver-facing queue、receiver-side drops 和短消息完成率。

### 5.6 推荐图组合

如果论文空间有限，建议使用以下组合：

1. 主图 A：`peak_receiver_queue_vs_load.png`
   - 证明 `unsched` 会打满 receiver queue，而 `scheduled` 不会。

2. 主图 B：`receiver_drops_vs_load.png`
   - 证明 queue overflow 会转化为真实 packet drop。

3. 辅图 C：`short_p99_fct_vs_load.png`
   - 说明 scheduled control 的代价是高负载下短消息排队延迟上升。

如果只能放两张图，就放 queue peak 和 drops；FCT 代价可以用正文数字说明。

## 6. 可以直接使用的论文段落

### 中文版本

我们进一步构造了一个 unscheduled short-message storm 场景，用于分析 receiver-driven credit 在短消息路径上的边界条件。该场景包含多个发送端和一个接收端，所有发送端通过同一个交换机向同一接收端发送 `8192B` 短消息。由于该消息大小低于默认 unscheduled threshold，短消息可以不等待接收端 grant 而直接发送。我们同时加入一条 `10MB` 背景长流，以观察短消息风暴对 scheduled traffic 的影响。

实验比较两种配置：`unsched` 使用默认 unscheduled threshold，使 `8192B` 短消息走 unscheduled path；`scheduled` 将 unscheduled threshold 降至 `1` packet，使相同短消息基本进入 receiver-credit-controlled scheduled path。我们 sweep 发送端数量 `N=8,16,32`，以及聚合短消息负载 `60-200Gbps`。

结果显示，当聚合短消息负载不超过接收端 `100Gbps` 链路容量时，`unsched` 模式没有产生明显丢包。但从 `110Gbps` 开始，`unsched` 模式在所有发送端数量下都将 receiver-facing queue 打满到 `1000` packets，并产生大量 packet drops。在 `200Gbps` 负载下，`N=8/16/32` 的 `unsched` 短消息完成率分别只有 `11.3%`、`7.6%` 和 `5.5%`。相比之下，`scheduled` 对照组在所有负载下都没有出现 receiver-side drop，短消息完成率保持 `100%`。

这一结果说明，SIRD/Homa 的 receiver-driven credit 能够有效控制 scheduled data，但无法在发送前约束已经进入 unscheduled path 的短消息。当大量低于阈值的短消息同时发往同一个接收端时，它们会在 receiver-facing egress queue 聚合，导致队列溢出和 packet drop。另一方面，强制短消息进入 scheduled path 可以避免该问题，但会显著增加高负载下的短消息排队延迟；在 `200Gbps` 负载下，`scheduled` 的 short-message p99 FCT 约为 `5.94ms`。因此，该实验揭示了 unscheduled path 的低延迟收益与过载保护能力之间的权衡。

### English version, if needed

We construct an unscheduled short-message storm scenario to expose a boundary condition of receiver-driven credit control. The topology consists of multiple senders and one receiver connected through a single switch. Each sender transmits `8192B` short messages to the same receiver. Since these messages are below the default unscheduled threshold, they can be sent without waiting for receiver grants. A `10MB` background long flow is also enabled to observe collateral effects on scheduled traffic.

We compare two configurations. In `unsched`, the default unscheduled threshold is used, so the `8192B` messages mostly follow the unscheduled path. In `scheduled`, the unscheduled threshold is reduced to one packet, forcing the same messages into the receiver-credit-controlled scheduled path. We sweep the number of short-message senders (`N=8,16,32`) and the aggregate offered load (`60-200Gbps`).

The results show that the unscheduled path behaves well below the receiver link capacity, but fails sharply once the aggregate short-message load exceeds `100Gbps`. Starting from `110Gbps`, the receiver-facing queue reaches the `1000`-packet limit in all sender configurations and incurs substantial packet drops. At `200Gbps`, only `11.3%`, `7.6%`, and `5.5%` of short messages complete for `N=8`, `16`, and `32`, respectively. In contrast, the scheduled baseline has zero receiver-side drops and completes all short messages across all tested loads.

These results show that receiver-driven credit effectively controls scheduled data, but cannot pre-control short messages that are already admitted into the unscheduled path. Aggregated unscheduled traffic can therefore overflow the receiver-facing queue in a lossy network. Forcing short messages into the scheduled path avoids drops, but increases queueing delay; at `200Gbps`, scheduled short-message p99 FCT reaches about `5.94ms`. The experiment highlights the tradeoff between low-latency unscheduled transmission and overload protection.

## 7. 结论写法

可以把结论写成以下三点：

1. SIRD/Homa 的 receiver-driven credit 对 scheduled traffic 有效，但它不控制已进入 unscheduled path 的短消息。

2. 在多发送者单接收者场景中，低于 unscheduled threshold 的短消息会在 receiver-facing queue 聚合。当 aggregate offered load 超过接收端链路容量时，该队列会被打满并产生 packet drop。

3. 将短消息强制纳入 scheduled path 可以消除该丢包问题，但代价是高负载下短消息 FCT 显著增加。因此，unscheduled threshold 的选择本质上是在低负载延迟和过载保护之间折中。

## 8. 写作注意事项

- 不要把该实验表述为“SIRD 整体失效”。更准确的说法是：该实验揭示了 unscheduled path 在多发送者聚合短消息下的边界条件。
- 不要用 `unsched` 高负载下完成消息的 p99 FCT 说明其延迟更好，因为该指标存在 survivor bias。
- 应把 completion ratio、receiver-side drops 和 receiver-facing queue peak 作为主指标。
- `scheduled` 不是替代协议，而是一个对照组，用于说明 receiver credit 控制可以避免该类 receiver-side overflow。
- 如果论文空间有限，优先展示 `peak_receiver_queue_vs_load.png` 和 `receiver_drops_vs_load.png`，再用正文说明 `scheduled` 的 FCT 代价。
