# RTT 异构下的 Credit 低效占用场景写法

本文档用于把 `bad2` RTT 异构实验写进论文或报告。它说明场景、问题、实验设置、结果解释、可直接使用的论文段落，以及写作时需要避免的过度结论。

## 1. 场景描述

本实验构造了一个两发送者、一接收者的 RTT 异构场景。拓扑包含两个 sender、两个交换机和一个 receiver：

```text
sender A -- switch0 -- switch1 -- receiver
sender B ----------- switch1 -- receiver
```

其中 sender A 和 receiver 不在同一个交换机下，A 的数据需要经过 `switch0 -> switch1` 的跨交换机链路；sender B 和 receiver 在同一个交换机 `switch1` 下。A 和 B 最终都通过 `switch1 -> receiver` 这条 receiver-facing bottleneck link 到达接收端。

实验通过增加 `switch0 -> switch1` 链路的额外传播时延来拉大 sender A 的 RTT，而 sender B 的 RTT 保持较短。这样可以隔离观察：当接收方对 A/B 近似均匀分配 credit budget 时，长 RTT sender A 获得的 credit 是否会因为周转慢而产生更低的数据吞吐，从而造成 credit 资源低效占用。

## 2. 要说明的问题

SIRD/Homa 的 receiver-driven credit 机制会在接收端为多个发送者分配 scheduled data 的发送信用。这个机制在 RTT 接近的情况下可以较好地协调多个 sender，但在 RTT 异构时可能出现一个问题：

> 如果接收方按 sender 近似均匀分配 credit，而不考虑不同 sender 的 RTT，分配给长 RTT sender 的 credit 会更慢返回数据。相同数量的 credit 在单位时间内产生的数据更少，造成 credit 资源的低效占用。短 RTT sender 即使可以更快周转，也无法完全补足被长 RTT sender 占用的 credit 空间，最终导致 receiver-facing bottleneck link 用不满。

因此，本实验要回答三个问题：

1. receiver 是否仍然给 A/B 近似均匀的 per-sender credit budget？
2. 随着 A 的 RTT 增加，A 的 goodput 是否显著下降？
3. B 是否能够完全补偿 A 的吞吐下降，使最后一跳仍保持满利用？

只有当下面三个现象同时出现时，才能支撑本实验的结论：

```text
sender budget A/B ~= 1.0
sender A goodput 随 RTT 增大而下降
aggregate goodput < receiver bottleneck capacity，且 receiver queue 不高
```

这说明最后一跳链路用不满不是因为拥塞排队，而是因为 credit 分配在 RTT 异构下产生了低效周转。

## 3. 实验设计

实验使用 `scratch/bad2.cc` 和 `scripts/bad2.sh`。重构后的 `bad2` 默认参数专门用于 RTT 异构实验：

```text
host link rate = 100 Gbps
inter-switch link rate = 100 Gbps
receiver bottleneck link = 100 Gbps
base per-link delay = 1 us
BDP_PKTS = 33.32
message size = 10 MB
backlog depth = 8 messages per sender
long-path extra delay = 0, 4, 8, 16, 32 us
```

两个 sender 都是 backlogged flow，持续向同一个 receiver 发送大消息。`long-path extra delay=0us` 是 RTT 同构近似基线；`4/8/16/32us` 是逐步增加 sender A RTT 的异构配置。

主要观测指标：

- `sender_budget_ratio_a_to_b`：receiver 分配给 A/B 的 per-sender credit budget 比例；
- `sender_a_goodput_gbps` 和 `sender_b_goodput_gbps`：A/B 各自完成数据形成的 goodput；
- `aggregate_goodput_gbps` 和 `aggregate_utilization`：最后一跳链路实际利用率；
- `receiver_queue_avg_pkts` 和 `receiver_queue_peak_pkts`：receiver-facing queue 是否有拥塞积压。

服务器结果目录：

```text
/mnt/nasDisk_ds3617/sird/bad2/20260429/bad2_rtt_sweep_smoke_bdp33p32_bottleneck100_20260429_163150
```

本地拷贝：

```text
outputs/sird-scenarios/bad2_rtt_sweep_smoke_bdp33p32_bottleneck100_20260429_163150/
```

关键文件：

```text
bad2_summary.csv
bad2_analysis.md
```

## 4. 实验结果

结果汇总如下：

| A 额外单向时延 | Aggregate goodput | Utilization | A goodput | B goodput | Sender budget A/B | Avg receiver queue | Peak receiver queue |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 us | 96.8 Gbps | 96.8% | 48.4 Gbps | 48.4 Gbps | 1.0 | 0.0 pkt | 0.0 pkt |
| 4 us | 82.4 Gbps | 82.4% | 21.2 Gbps | 61.2 Gbps | 1.0 | 0.0 pkt | 0.0 pkt |
| 8 us | 76.8 Gbps | 76.8% | 13.6 Gbps | 63.2 Gbps | 1.0 | 0.0 pkt | 0.0 pkt |
| 16 us | 71.6 Gbps | 71.6% | 7.6 Gbps | 64.0 Gbps | 1.0 | 0.0 pkt | 0.0 pkt |
| 32 us | 69.2 Gbps | 69.2% | 4.0 Gbps | 65.2 Gbps | 1.0 | 0.0 pkt | 0.0 pkt |

这组结果有三个关键信号。

第一，receiver 对 A/B 的 `senderBudgetPkts` 始终相同，`sender budget A/B = 1.0`。这说明 receiver 的 per-sender credit budget 分配没有随着 RTT 异构而倾斜，长 RTT sender A 和短 RTT sender B 获得了近似相同的 credit budget。

第二，随着 A 的额外 RTT 增加，A 的 goodput 快速下降。`extra delay=0us` 时，A 和 B 各自约 `48.4Gbps`，整体接近公平并且最后一跳利用率约 `96.8%`。当 A 的额外单向时延增加到 `32us` 时，A 的 goodput 下降到 `4.0Gbps`，而 B 只能提高到 `65.2Gbps`。

第三，aggregate goodput 明显低于 `100Gbps` receiver bottleneck，且 receiver-facing queue 始终为 `0`。这说明最后一跳不是因为拥塞排队而受限；相反，最后一跳没有足够数据可发，表现为 under-utilization。

## 5. 图和读图说明

本节给出适合论文展示的图。由于这个实验的论点是“RTT 异构相对 RTT 同构基线造成链路利用率下降”，正文中最好优先使用带基准线或对比含义明确的图，而不是只展示单条结果曲线。

建议正文主图使用下面三张：

1. `aggregate_utilization_baseline_comparison.png`：直接对比每个 RTT 配置与 `0us` 同构基线、`100Gbps` 理想瓶颈线的差距。
2. `goodput_stack_with_unused_capacity.png`：把 A/B goodput 与未利用容量堆叠在同一张图中，直观看到“B 补不回来”的容量空洞。
3. `normalized_goodput_vs_baseline.png`：把 A、B、aggregate 都归一化到 `0us` 基线，说明退化不是局部现象，而是 aggregate 层面的退化。

然后辅以 `sender_budget_ratio_vs_rtt.png` 和 `receiver_queue_vs_rtt.png`，分别证明 credit budget 仍均匀、最后一跳不是拥塞排队。

### 5.1 Aggregate utilization with baseline comparison

![Aggregate utilization baseline comparison](../outputs/sird-scenarios/bad2_rtt_sweep_smoke_bdp33p32_bottleneck100_20260429_163150/plots/aggregate_utilization_baseline_comparison.png)

这张图是最适合放在正文里的主图。它把每个 RTT 配置下的 receiver bottleneck utilization 画成柱状图，并同时标出两条基准线：

- `100% bottleneck`：理想情况下 `100Gbps` receiver-facing bottleneck 被完全利用；
- `0us baseline`：RTT 近似同构时的实测基线，利用率为 `96.8%`。

图中每个异构配置上方的 `-x pp` 表示相对 `0us` 基线损失了多少个百分点。结果显示：

- `4us` 额外单向时延时，利用率从 `96.8%` 降到 `82.4%`，相对基线损失约 `14.4` 个百分点；
- `8us` 时利用率降到 `76.8%`，损失约 `20.0` 个百分点；
- `16us` 时利用率降到 `71.6%`，损失约 `25.2` 个百分点；
- `32us` 时利用率降到 `69.2%`，损失约 `27.6` 个百分点。

这张图能说明的核心问题是：RTT 异构不是只改变了两个 sender 之间的公平性，而是让共享的 receiver-facing bottleneck 相对同构基线出现明显利用率损失。因为图中同时有 `100%` 理想线和 `0us` 实测基线，读者可以区分两件事：系统本身在同构 RTT 下能接近打满链路；而 RTT 异构引入后，链路利用率系统性下降。

论文图注可以写：

> Receiver bottleneck utilization compared with the homogeneous-RTT baseline. The `0us` case reaches `96.8%` utilization, while increasing sender A's extra path delay reduces utilization to `69.2%`, leaving a `27.6` percentage-point gap from the measured baseline.

中文图注可以写：

> 相对 RTT 同构基线的 receiver bottleneck 利用率。`0us` 基线达到 `96.8%`，而 sender A 的额外路径时延增加到 `32us` 后，利用率下降到 `69.2%`，相对实测基线损失 `27.6` 个百分点。

### 5.2 Goodput stack with unused capacity

![Goodput stack with unused capacity](../outputs/sird-scenarios/bad2_rtt_sweep_smoke_bdp33p32_bottleneck100_20260429_163150/plots/goodput_stack_with_unused_capacity.png)

这张图把 receiver bottleneck 的 `100Gbps` 容量拆成三部分：

- sender A goodput；
- sender B goodput；
- unused bottleneck capacity。

它的优点是非常直观：读者可以直接看到随着 A 的 RTT 增加，A 的蓝色部分快速变小，B 的部分有所增加，但顶部灰色的 unused capacity 逐渐变大。这说明短 RTT sender B 并没有完全补偿长 RTT sender A 的吞吐下降。

具体来说：

- `0us` 时，A 和 B 各约 `48.4Gbps`，未利用容量只有约 `3.2Gbps`；
- `32us` 时，A 只有 `4.0Gbps`，B 增至 `65.2Gbps`，但仍留下约 `30.8Gbps` 未利用容量；
- 如果 credit 能够根据 RTT 和周转效率充分重分配，B 理论上应当更接近填满 A 留出的容量，但当前结果中并没有发生。

这张图能说明的核心问题是：瓶颈链路的损失不是因为总需求不足。两个 sender 都是 backlogged flow，B 也确实提高了发送贡献；但是由于 receiver-side credit budget 仍然近似均匀，B 无法获得足够额外 credit 来填满最后一跳。

推荐正文写法：

> The stacked goodput view decomposes the `100Gbps` receiver bottleneck into sender A goodput, sender B goodput, and unused capacity. As sender A's RTT increases, A's contribution collapses, while sender B only partially compensates. The unused capacity grows to `30.8Gbps` at `32us`, showing that the bottleneck is not fully utilized despite persistent demand.

中文写法：

> 堆叠图将 `100Gbps` receiver bottleneck 拆分为 A 的吞吐、B 的吞吐和未利用容量。随着 A 的 RTT 增加，A 的吞吐快速下降；B 虽然有所提升，但只能部分补偿。在 `32us` 配置下，仍有约 `30.8Gbps` 的瓶颈容量未被利用，说明问题不是业务需求不足，而是 credit 周转和分配效率不足。

### 5.3 Normalized goodput relative to 0us baseline

![Normalized goodput vs baseline](../outputs/sird-scenarios/bad2_rtt_sweep_smoke_bdp33p32_bottleneck100_20260429_163150/plots/normalized_goodput_vs_baseline.png)

这张图把 A、B 和 aggregate goodput 都归一化到 `0us` 基线。`100%` 表示与 RTT 同构基线相同。它适合用来表达“退化比例”，而不是绝对 Gbps 数值。

图中的关键读法是：

- sender A 相对基线快速下降，在 `32us` 时只剩约 `8.3%`；
- sender B 相对基线上升到约 `134.7%`，说明短 RTT sender 确实拿到了一部分额外吞吐；
- aggregate 仍下降到约 `71.5%`，说明 B 的提升不足以抵消 A 的下降。

这张图能帮助论文避免一个误解：读者可能会认为“B 变快了，所以系统应该只是重新分配吞吐”。归一化图显示，B 的确变快，但 aggregate 仍然明显低于基线，因此这是总效率下降，而不只是公平性变化。

推荐正文写法：

> Normalizing goodput to the `0us` baseline shows that the RTT-heterogeneous cases reduce aggregate efficiency rather than merely redistributing throughput between senders. At `32us`, sender B reaches `134.7%` of its baseline goodput, but aggregate goodput still drops to `71.5%` of baseline because sender A falls to `8.3%`.

中文写法：

> 将 goodput 归一化到 `0us` 基线后可以看到，RTT 异构导致的是整体效率下降，而不只是 sender 间吞吐重新分配。在 `32us` 时，B 达到自身基线的 `134.7%`，但 A 只有基线的 `8.3%`，最终 aggregate 只有基线的 `71.5%`。

### 5.4 Aggregate utilization trend

![Aggregate utilization vs RTT](../outputs/sird-scenarios/bad2_rtt_sweep_smoke_bdp33p32_bottleneck100_20260429_163150/plots/aggregate_utilization_vs_rtt.png)

这张图展示 sender A 额外单向时延增加后，receiver-facing bottleneck link 的总体利用率如何变化。横轴是 A 路径上的额外单向时延，纵轴是 aggregate goodput 相对于 `100Gbps` receiver bottleneck 的利用率。

这张图最直接说明“最后一跳用不满”：

- `0us` 时利用率约为 `96.8%`，说明同构 RTT 下链路基本能被打满；
- `4us` 时利用率下降到 `82.4%`；
- `32us` 时利用率只有 `69.2%`。

它能支持的结论是：随着 sender A 的 RTT 增大，receiver bottleneck 的总体利用率持续下降。这说明问题不是单纯的 sender A 变慢，而是整个共享 receiver bottleneck 都没有被充分利用。

论文图注可以写：

> Receiver bottleneck utilization under RTT heterogeneity. As sender A's extra path delay increases, aggregate utilization drops from `96.8%` to `69.2%`, indicating that the receiver-facing bottleneck becomes under-utilized.

中文图注可以写：

> RTT 异构下 receiver bottleneck 的链路利用率。随着 sender A 的额外路径时延增加，最后一跳利用率从 `96.8%` 下降到 `69.2%`，说明 receiver-facing bottleneck 出现用不满。

如果论文已经放了 `aggregate_utilization_baseline_comparison.png`，这张趋势图可以作为补充材料；二者表达的数值相同，但 baseline comparison 图更利于读者直接看到相对基准的损失。

### 5.5 Per-sender goodput

![Sender goodput vs RTT](../outputs/sird-scenarios/bad2_rtt_sweep_smoke_bdp33p32_bottleneck100_20260429_163150/plots/sender_goodput_vs_rtt.png)

这张图展示 sender A、sender B 和 aggregate goodput 随 RTT 异构程度变化的趋势。它用于说明“谁的吞吐下降，以及短 RTT sender 是否能补偿”。

图中的关键现象是：

- `0us` 时 A/B 都约为 `48.4Gbps`，接近公平；
- 随着 A 的额外时延增加，A goodput 快速下降；
- B 的 goodput 会提高，但最高只到约 `65.2Gbps`；
- aggregate goodput 随 A 的下降同步下降，说明 B 没有完全补偿 A 的损失。

这张图能支持的结论是：长 RTT sender A 的 credit 周转变慢后，它单位时间内能产生的数据显著减少；短 RTT sender B 虽然更快，但没有获得足够额外 credit 来完全填补最后一跳容量。

推荐正文写法：

> Per-sender goodput reveals the source of under-utilization. Sender A's goodput drops sharply as its extra path delay increases, while sender B increases only to about `65Gbps`. Since B cannot fully compensate for A's lost throughput, aggregate goodput falls below the receiver bottleneck capacity.

中文写法：

> Per-sender goodput 表明链路用不满的来源：随着额外 RTT 增加，sender A 的 goodput 快速下降，而 sender B 只能提升到约 `65Gbps`，无法完全补偿 A 的吞吐损失。因此 aggregate goodput 明显低于 receiver bottleneck capacity。

如果论文空间有限，`goodput_stack_with_unused_capacity.png` 比这张图更适合作为正文图，因为它同时展示了 A/B 吞吐和未利用容量。这张 per-sender goodput 折线图更适合用于补充材料或实验细节说明。

### 5.6 Sender budget ratio

![Sender budget ratio vs RTT](../outputs/sird-scenarios/bad2_rtt_sweep_smoke_bdp33p32_bottleneck100_20260429_163150/plots/sender_budget_ratio_vs_rtt.png)

这张图展示 receiver 分配给 sender A 和 sender B 的 per-sender credit budget 比例。纵轴是 `sender_budget_a / sender_budget_b`。如果该值接近 `1.0`，说明 receiver 对 A/B 的 credit budget 分配近似均匀。

这张图非常关键，因为它支撑“credit 低效占用”的前提：

- 所有 RTT 配置下，sender budget A/B 都保持为 `1.0`；
- receiver 并没有因为 A 的 RTT 更长而减少 A 的 budget；
- 因此，A 仍持有与 B 相同的 credit budget，但 A 的 credit 周转更慢。

这张图能说明：aggregate utilization 下降不是因为 receiver 主动少给 A credit，也不是因为 A 没有被纳入调度，而是因为均匀 credit budget 在 RTT 异构下产生了不同的周转效率。

论文中可以这样解释：

> The sender budget ratio remains at `1.0` across all RTT settings, meaning that the receiver allocates equal per-sender credit budget to A and B. However, the same budget turns over more slowly on the long-RTT path, making the credit assigned to sender A less productive in terms of delivered bytes per unit time.

中文写法：

> Sender budget ratio 在所有 RTT 配置下都保持为 `1.0`，说明 receiver 对 A/B 分配了相同的 per-sender credit budget。然而，相同数量的 credit 在长 RTT 路径上周转更慢，单位时间内产生的数据更少，因此分配给 A 的 credit 形成了低效占用。

### 5.7 Receiver-facing queue

![Receiver queue vs RTT](../outputs/sird-scenarios/bad2_rtt_sweep_smoke_bdp33p32_bottleneck100_20260429_163150/plots/receiver_queue_vs_rtt.png)

这张图展示 receiver-facing queue 的平均值和峰值。它用于排除另一种解释：如果 queue 很高，那么 aggregate goodput 下降可能来自拥塞、排队或丢包；但如果 queue 始终为空，同时 aggregate utilization 下降，则说明最后一跳不是拥塞，而是没有足够数据可发。

当前结果中：

- avg receiver queue 始终为 `0`；
- peak receiver queue 始终为 `0`；
- 但 aggregate utilization 从 `96.8%` 降到 `69.2%`。

因此，这张图能说明：RTT 异构场景下的性能下降不是 queue buildup 或 receiver-side congestion，而是 receiver bottleneck under-utilization。

推荐正文写法：

> The receiver-facing queue remains empty throughout the sweep. Therefore, the reduced aggregate goodput is not caused by congestion or queue buildup. Instead, the bottleneck link is under-utilized because the credit assigned to the long-RTT sender produces data too slowly.

中文写法：

> Receiver-facing queue 在整个 sweep 中始终为空。因此，aggregate goodput 下降不是由拥塞排队造成的，而是因为分配给长 RTT sender 的 credit 周转慢，导致最后一跳没有足够数据可发。

### 5.8 推荐图组合

如果论文空间有限，建议使用三张主图加一张辅助图：

1. `aggregate_utilization_baseline_comparison.png`
   - 作为主结果图，证明 RTT 异构相对 `0us` 基线和 `100Gbps` 理想瓶颈都有明显利用率损失。

2. `goodput_stack_with_unused_capacity.png`
   - 证明 A 吞吐下降后，B 只能部分补偿，最后一跳出现明确的 unused capacity。

3. `normalized_goodput_vs_baseline.png`
   - 证明这是 aggregate efficiency 相对基线下降，而不只是 A/B 之间重新分配吞吐。

4. `sender_budget_ratio_vs_rtt.png`
   - 作为机制证据，证明 receiver 对 A/B 的 credit budget 仍近似均匀。

如果还可以放第五张，加入 `receiver_queue_vs_rtt.png`，用来排除“拥塞排队导致吞吐下降”的解释。若只能放一张图，优先放 `aggregate_utilization_baseline_comparison.png`；若能放两张，第二张放 `goodput_stack_with_unused_capacity.png`。

## 6. 如何解释结果

这个实验最重要的解释链条是：

```text
receiver 近似均匀分配 A/B credit budget
        ↓
A 的 RTT 更长，credit 周转时间更长
        ↓
A 单位时间内用同样 credit 产生的数据更少
        ↓
B 虽然 RTT 短，但无法完全获得 A 低效占用的 credit 空间
        ↓
aggregate goodput 下降，receiver-facing bottleneck link 用不满
```

结果中最有力的对照是 `0us` 和 `32us`：

- `0us`：A/B goodput 都是 `48.4Gbps`，总 goodput `96.8Gbps`，接近打满最后一跳；
- `32us`：A goodput 只有 `4.0Gbps`，B goodput 提高到 `65.2Gbps`，但总 goodput 只有 `69.2Gbps`；
- 两者的 `sender budget A/B` 都是 `1.0`；
- 两者的 receiver queue 都是 `0`。

这说明：问题不是 receiver bottleneck 过载，也不是 B 无数据可发，而是 credit 分配没有充分考虑 RTT 异构，导致给 A 的 credit 在单位时间内产生较低吞吐，形成低效占用。

## 7. 可以直接使用的论文段落

### 中文版本

我们进一步构造了一个 RTT 异构场景，用于分析 receiver-driven credit 在不同 RTT sender 之间分配 credit 时的效率问题。该场景包含两个发送端、两个交换机和一个接收端。sender A 位于远端交换机下，需要经过跨交换机链路到达接收端；sender B 与接收端位于同一交换机下。两个 sender 共享 `switch1 -> receiver` 这条 `100Gbps` receiver-facing bottleneck link。实验通过增加 sender A 路径上的额外传播时延来构造 RTT 异构，而 sender B 保持短 RTT。

实验结果显示，receiver 对两个 sender 的 per-sender credit budget 始终近似相同，`sender budget A/B` 保持为 `1.0`。然而，随着 sender A 的额外单向时延从 `0us` 增加到 `32us`，A 的 goodput 从 `48.4Gbps` 下降到 `4.0Gbps`。短 RTT 的 sender B 虽然从 `48.4Gbps` 提升到 `65.2Gbps`，但无法完全补偿 A 的吞吐下降，aggregate goodput 从 `96.8Gbps` 下降到 `69.2Gbps`。

同时，receiver-facing queue 的平均值和峰值均为 `0`，说明该场景下最后一跳链路并非因为拥塞排队受限，而是由于没有足够 scheduled data 到达而未被充分利用。这表明，在 RTT 异构场景中，均匀的 receiver-side credit budget 分配可能导致 credit 资源低效占用：分配给长 RTT sender 的 credit 周转更慢，单位时间内产生的数据更少，而短 RTT sender 又无法完全获得这部分低效占用的 credit 空间，最终造成 receiver bottleneck under-utilization。

### English version, if needed

We construct an RTT-heterogeneous scenario to study the efficiency of receiver-driven credit allocation across senders with different RTTs. The topology contains two senders, two switches, and one receiver. Sender A is attached to a remote switch and reaches the receiver through an inter-switch link, while sender B is co-located with the receiver under the same switch. Both senders share a `100Gbps` receiver-facing bottleneck link. We increase the extra propagation delay on sender A's path to create RTT heterogeneity while keeping sender B on a short-RTT path.

The results show that the receiver allocates approximately equal per-sender credit budgets to A and B, with the sender budget ratio remaining at `1.0`. However, as sender A's extra one-way delay increases from `0us` to `32us`, A's goodput drops from `48.4Gbps` to `4.0Gbps`. Sender B increases its goodput from `48.4Gbps` to `65.2Gbps`, but it cannot fully compensate for A's throughput loss. As a result, aggregate goodput drops from `96.8Gbps` to `69.2Gbps`.

The receiver-facing queue remains empty throughout the sweep, indicating that the bottleneck is not limited by congestion or queue buildup. Instead, the receiver bottleneck is under-utilized because credit assigned to the long-RTT sender turns over slowly and produces less data per unit time. This demonstrates that RTT-oblivious, uniform receiver-side credit allocation can inefficiently occupy credit resources and reduce bottleneck utilization under RTT heterogeneity.

## 8. 结论写法

可以把结论写成以下三点：

1. 在 RTT 同构时，两个 sender 各自获得约一半吞吐，receiver bottleneck 接近满利用。

2. 在 RTT 异构时，receiver 仍然给 A/B 近似相同的 sender credit budget，但长 RTT sender A 的 credit 周转更慢，goodput 显著下降。

3. 短 RTT sender B 不能完全补偿 A 的吞吐下降，导致 aggregate goodput 明显低于 receiver bottleneck capacity；同时 receiver queue 为空，说明这是 under-utilization，而不是拥塞。

一句话结论：

> RTT 异构会使均匀 receiver-side credit allocation 产生低效占用：长 RTT sender 持有的 credit 周转慢，单位时间内贡献的数据少，最终导致共享 receiver bottleneck link 用不满。

## 9. 写作注意事项

- 不要只说“长 RTT sender 吞吐更低”。这只是现象的一部分，真正要强调的是：receiver budget 仍然均匀，但 aggregate utilization 下降且 receiver queue 为空。
- 不要把该结果描述为 packet loss 或 queue overflow 问题。这里的关键是 under-utilization。
- `sender budget A/B = 1.0` 是支撑“均匀分配”的关键证据。
- `receiver queue = 0` 是支撑“不是拥塞”的关键证据。
- `aggregate utilization` 从 `96.8%` 降到 `69.2%` 是支撑“最后一跳用不满”的关键证据。
- 目前这是一组服务器 smoke sweep。若作为最终论文图，建议再跑更长 duration，并画出 aggregate utilization、A/B goodput、sender budget ratio、receiver queue 四类图。
