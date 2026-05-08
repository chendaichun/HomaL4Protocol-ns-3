# sim1 统一写作主文档（中文）

本文档作为当前 `sim1` 的统一主文档，合并并吸收以下几份阶段性文档中的核心内容：

- `sim1_paper_alignment_zh.md`
- `sim1_paper_comparison_zh.md`
- `sim1_paper_goodput_reanalysis_zh.md`
- `sim1_status_report_zh.md`
- `sim1_full_report_zh.md`

从论文写作的角度看，后续应优先维护这一份文档；其余文档可以保留为历史记录，但不再作为主说明文档引用。

本文档重点回答四个问题：

1. 论文里这一节应该叫什么标题；
2. `sim1` 场景应该怎么描述；
3. 我们在 application 层到底做了什么工作；
4. 现在写论文时，这一节应该按什么结构展开。

---

## 1. 这一节在论文里叫什么

不建议正文标题直接写成“sim1”，因为这是内部代号，不是对读者友好的学术表述。

### 推荐标题

**大规模多工作负载场景下的吞吐、排队与尾时延评估**

这个标题最稳妥，原因有三点：

- 它点明了这是一个大规模场景，而不是单链路或单机制 toy case；
- 它把 `sim1` 的三个核心指标直接说清楚：`goodput`、`ToR queue`、`slowdown`；
- 它和原文实验组织方式基本一致，便于后面承接图表和对比分析。

### 可选标题

如果你想更强调“对齐原文复现”，也可以用：

**对齐复现实验**

但如果这是论文正文实验章节的小节标题，我仍然更推荐上一种，因为它更自然。

---

## 2. sim1 场景是什么

`sim1` 的本质不是单个瓶颈实验，而是一个**大规模 leaf-spine 数据中心背景业务实验**。它要回答的问题是：

> 在大规模多工作负载、多种拥塞形态下，SIRD 近似实现能否在吞吐、排队和尾时延上呈现与原文一致的总体趋势与接近的量级。

### 2.1 拓扑

当前 `sim1` 使用两层 leaf-spine 拓扑，可描述为：

- `144` 个主机；
- `9` 个 ToR；
- 每个 ToR 下 `16` 个 host；
- `4` 个 Spine；
- host-ToR 链路速率为 `100Gbps`；
- 默认 ToR-Spine 链路速率为 `400Gbps`；
- 在 `core` 场景下，将 ToR-Spine 链路收紧为 `200Gbps`，人为制造更强的核心层瓶颈。

如果论文里只写一段，建议写成：

> 我们采用一个 `144` 主机的两层 leaf-spine 数据中心拓扑，包含 `9` 个 ToR 和 `4` 个 Spine。host-ToR 链路速率为 `100Gbps`；在 `core` 场景中，进一步将 ToR-Spine 链路从默认的 `400Gbps` 收紧至 `200Gbps`，以形成更明显的共享核心链路瓶颈。

### 2.2 工作负载

`sim1` 使用三类 workload：

- `google_rpc`
- `facebook_hadoop`
- `web_search`

它们分别对应原文公开配置中的 `W3 / W4 / W5` 消息大小分布。

### 2.3 三类流量形态

`sim1` 中的三类 traffic config 不是三个不同协议，而是三种不同的网络压力形态。

#### `balanced`

`balanced` 表示全网流量注入较均衡，没有刻意强化某个局部热点，更适合观察协议在“整体均衡负载”下的完成吞吐和排队表现。

论文里可写成：

> `balanced` 场景用于近似均衡流量注入条件，以观察协议在不存在显式局部热点时的整体吞吐与排队行为。

#### `core`

`core` 通过降低 ToR-Spine 带宽，使共享核心链路更容易过载，用于观察协议在核心层瓶颈下的表现。

论文里可写成：

> `core` 场景通过压缩 ToR-Spine 链路容量，使共享核心链路更容易形成拥塞，从而考察协议在核心瓶颈下的吞吐、排队与尾时延表现。

#### `incast`

`incast` 在背景流量基础上叠加面向同一接收端的聚集流量，用于模拟典型接收端热点。

论文里可写成：

> `incast` 场景在背景流量之上叠加指向同一接收端的聚集突发，以模拟典型接收端热点，并考察协议在接收端下行压力下的排队和尾部时延行为。

---

## 3. sim1 主要比较什么指标

这一组实验建议始终围绕原文最核心的三类指标展开：

1. `goodput`
2. `ToR queue`
3. `slowdown`

从原文和公开项目配置的角度看，更准确的对应关系是：

- `50% offered load` 下的 `99th percentile slowdown`
- 跨负载扫点后的 `maximum goodput`
- 跨负载扫点后的 `maximum ToR queuing`

因此，论文叙述时不要把 `sim1` 讲成“机制细节实验”，而要讲成：

> **大规模场景下，以吞吐、交换机排队和尾时延为核心观测指标的总体对比实验。**

---

## 4. 我们在 application 层做了什么工作

这一部分是当前 `sim1` 写作中最需要讲清楚的内容，因为它决定了我们是不是在测“和原文同一种业务”。

### 4.1 为什么一定要改 application

之前的 `sim1` 虽然已经开始对齐原文的 workload 名称、时间窗口和统计方式，但底层业务语义仍然存在一个关键偏差：

- 原文公开模拟器更接近 **RPC request/response** 工作负载；

这两者看起来都在“发消息”，但它们并不是同一种业务模型。它们会直接影响：

- 网络中实际流动的数据结构；
- 完成事件的统计对象；
- `goodput` 的可解释性；
- `queue` 和 `slowdown` 的结果对比意义。

所以，application 层的改造不是可有可无的小修补，而是让 `sim1` 从“名字上像原文”变成“业务语义上更接近原文”的必要工作。

### 4.2 新增了什么

为此，我们新增了 `RpcWorkloadApp`：

- [src/applications/model/rpc-workload-app.h](/Volumes/macos/codex-ns3/src/applications/model/rpc-workload-app.h)
- [src/applications/model/rpc-workload-app.cc](/Volumes/macos/codex-ns3/src/applications/model/rpc-workload-app.cc)

它的核心行为是：

1. 每个 host 同时扮演 client 和 server；
2. client 按 workload 分布生成 `request`；
3. `request` 大小服从 `W3 / W4 / W5` 分布；
4. `request` 到达间隔服从指数分布；
5. 目标主机按均匀随机选择；
6. server 收到 `request` 后立即返回固定 `20B` 的 `response`。

从语义上看，这一步完成的是：

> **把背景业务从单向消息流改造成 request/response RPC 流量模型。**

### 4.3 这项工作落在了哪些文件里

除了新增 `RpcWorkloadApp` 本身，还涉及以下文件：

- [scratch/sim1.cc](/Volumes/macos/codex-ns3/scratch/sim1.cc)
- [scripts/sim1_analyze.py](/Volumes/macos/codex-ns3/scripts/sim1_analyze.py)
- [src/applications/wscript](/Volumes/macos/codex-ns3/src/applications/wscript)

各文件的职责可以概括为：

- `rpc-workload-app.{cc,h}`：定义 RPC 请求/响应应用本身；
- `sim1.cc`：把新应用真正接入大规模拓扑和实验场景；
- `sim1_analyze.py`：把新的 trace 字段转换成论文可用指标。

### 4.4 这次 application 改造具体带来了什么语义变化

#### 1. 背景流量从单向消息改成 RPC 请求-响应

现在背景业务不再是“每个 host 一直往外发单向大消息”，而是：

- 先发 `request`
- 再由对端返回固定小 `response`

这让 `sim1` 的业务形态更接近原文公开模拟器。

#### 2. `msg.tr` 可以区分 request 和 response

当前 trace 中已经显式标注：

- `kind=request`
- `kind=response`
- `kind=other`

这使得后续分析不再把所有完成事件混成一种“消息完成”。

#### 3. `goodput` 主口径改成 request-only

当前主字段：

- `goodputGbps`
- `aggregateGoodputGbps`
- `perHostGoodputGbps`

都按 **request-only** 语义解释。

同时保留辅助字段：

- `responseGoodputGbps`
- `totalTransportGoodputGbps`

这样做的作用是：论文主结果里的 `goodput` 不再把固定 `20B` 响应也混进去，从而更接近原文语义。

### 4.5 这项 application 工作在论文里怎么说

这一段建议你不要写得太散。最稳妥的写法是先说明“为什么要改”，再说明“改成什么”，最后说明“这让什么结果变得可比”。

可以直接写成：

> 为提高与原文公开模拟器在业务语义上的一致性，本文对 `sim1` 的 application 层进行了专门改造。原始 `sim1` 更接近单向背景消息模型，而原文公开实验更接近 request/response RPC 工作负载。为此，本文新增 `RpcWorkloadApp`，使每个主机同时扮演 client 与 server：client 按 `W3/W4/W5` 分布生成 request，请求到达间隔服从指数分布，目标主机均匀随机选择；server 在收到 request 后立即返回固定 `20B` 的 response。基于这一改造，`sim1` 的背景业务从单向消息注入收紧为更贴近原文的 RPC 模型，并进一步支撑了 request-only goodput 统计口径，使吞吐、排队和尾时延结果具有更明确的对比语义。

---

## 5. 当前与原文对齐了什么

### 5.1 已经尽量收紧并对齐的部分

目前已经尽量向原文收紧的部分包括：

1. workload 类型：`google_rpc / facebook_hadoop / web_search`
2. request 大小分布：对应 `W3 / W4 / W5`
3. request 到达分布：指数分布
4. target 选择方式：均匀随机
5. response 大小：固定 `20B`
6. paper-style 时间窗口：`start_at = 10.0`
7. paper-style 统计窗口：仅统计最后 `10%`
8. 主 goodput 口径：request-only
9. 主 ToR queue 口径：strict queue，且当前主跑法采用 `deviceQueue=1p`

### 5.2 仍然不是“原项目逐行同构”的部分

仍需谨慎说明的部分包括：

1. 底层模拟框架仍是 `ns-3 + Homa/SIRD` 改造版，而不是原项目自身模拟器；
2. 协议内部实现细节、调度器细节、事件组织方式仍可能和原项目不同；
3. `incast` 的组织方式仍是我们这边的工程近似，而不是对原项目配置的逐行搬运；
4. 最终数值能否逐项重合，仍需依赖后续完整重跑验证。

因此，论文里更严谨的表述应当是：

> 本文在 workload 语义、时间窗口和主统计口径上尽量对齐原文，但由于底层模拟框架和部分场景组织方式仍存在工程差异，当前结果应视为“对齐复现”而非“原项目逐行同构执行”。

---

## 6. 论文里这一节建议怎么写

建议固定成三段式结构：

1. 先写场景；
2. 再写实现工作；
3. 最后写指标与结果组织方式。

### 6.1 第一段：场景

可直接写成：

> 我们在一个 `144` 主机的两层 leaf-spine 数据中心拓扑上，对齐复现原文 `sim1` 的大规模多工作负载实验。实验包含 `google_rpc`、`facebook_hadoop` 和 `web_search` 三类 workload，以及 `balanced`、`core` 和 `incast` 三类流量形态，用于比较协议在不同瓶颈条件下的吞吐、交换机排队与尾时延表现。

### 6.2 第二段：你的实现工作

这一段建议专门强调 application 层改造，因为这是你这次为“原文对齐”做出的最关键工作之一。

可直接写成：

> 与早期基于单向背景消息的近似实现不同，本文进一步将 `sim1` 的 application 层改造为 request/response RPC 工作负载。具体而言，我们新增 `RpcWorkloadApp`，使每个 host 同时扮演 client 和 server：client 按 `W3/W4/W5` 分布生成 request，请求到达间隔服从指数分布，目标主机均匀随机选择；server 在收到 request 后立即返回固定 `20B` 的 response。该改造使 `sim1` 的业务模型从单向消息注入收紧为更贴近原文公开模拟器的 RPC 模型。

### 6.3 第三段：统计口径

这一段用来说明为什么结果现在更可比。

可直接写成：

> 在统计方法上，我们保留原文式的时间窗口配置，即业务从 `start_at=10.0` 开始注入，结果仅统计最后 `10%` 的业务窗口；同时将主 `goodput` 统计口径调整为 request-only，并采用更贴近交换机排队语义的 ToR queue 观测方式。基于这些改动，本文后续主要报告 `goodput`、`ToR queue` 和 `slowdown` 三类指标，并据此分析实现结果与原文结论的一致性和偏差来源。

---

## 7. 一个可以直接放进论文里的合并版表述

如果你现在想先落一版论文正文，可以直接用下面这段作为基础版本：

> 我们在一个 `144` 主机的两层 leaf-spine 数据中心拓扑上，对齐复现原文 `sim1` 的大规模多工作负载实验。该拓扑包含 `9` 个 ToR、`4` 个 Spine，host-ToR 链路速率为 `100Gbps`；在 `core` 场景下，进一步将 ToR-Spine 链路从默认的 `400Gbps` 收紧至 `200Gbps`，以构造更明显的共享核心链路瓶颈。实验包含 `google_rpc`、`facebook_hadoop` 和 `web_search` 三类 workload，以及 `balanced`、`core` 和 `incast` 三类流量形态，用于评估协议在不同瓶颈条件下的吞吐、交换机排队与尾时延表现。为提高与原文公开模拟器在业务语义上的一致性，本文进一步改造了 `sim1` 的 application 层：新增 `RpcWorkloadApp`，使每个主机同时扮演 client 和 server，由 client 按 `W3/W4/W5` 分布生成 request，server 在收到 request 后立即返回固定 `20B` 的 response，从而将原有单向背景消息流改造为 request/response RPC 模型。在统计方法上，我们保留原文式的时间窗口配置，即业务从 `start_at=10.0` 开始注入，仅统计最后 `10%` 的业务窗口，并将主 `goodput` 统计口径调整为 request-only。基于这些改动，本文后续主要比较 `goodput`、`ToR queue` 和 `slowdown` 三类指标，以分析当前实现与原文结果的一致性与偏差来源。

---

## 8. 目前这份主文档和其他 sim1 文档的关系

从现在开始，建议把这份文档视为：

- `sim1` 的主说明文档；
- 论文写作时的首选引用文本；
- 合并多个阶段性 `sim1_*` 文档后的统一版本。

其余 `sim1` 文档建议视为：

- 历史记录；
- 阶段性分析；
- 某次 run 的状态说明；
- 某个单独问题的补充讨论。

如果后续还要继续补内容，建议优先往这份文档里加，而不是再新开一份新的 `sim1_*` 说明。

---

## 9. 当前最简结论

如果只用一句话概括这份文档想帮你在论文里讲清楚什么，那就是：

> `sim1` 这一节应被写成一个“大规模多工作负载下的吞吐、排队与尾时延评估”实验；而你在其中最重要的工程工作之一，是把原先的单向背景消息模型改造成更贴近原文公开模拟器的 request/response RPC application，并据此把 `goodput` 和队列结果重新收紧到更可比的语义上。
