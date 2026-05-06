# sim1 原文对齐设计

## 目标

将当前 ns-3 `sim1` 从“单向消息注入的近似复现”改造为更贴近原文公开模拟器的大规模实验版本，使其在工作负载、流量语义、统计窗口和主结果提取口径上与原文保持一致，并对仍无法完全同构的部分单独标注工程边界：

1. 工作负载类型：使用 `W3/google_rpc`、`W4/facebook_hadoop`、`W5/web_search`。
2. 流量语义：采用 `request/response` RPC 模型，其中 `request` 大小服从 workload 分布，`response` 固定为 `20B`。
3. 负载与时间窗口：保留原文的负载点、`start_at=10.0`、以及仅统计最后 `10%` 窗口的规则。
4. 统计口径：将 `goodput`、`ToR queue`、`slowdown` 等结果语义按原文风格重整，避免现有脚本把非业务字节或隐藏队列混入主结果。

## 当前实现与原文的主要差异

当前 `scratch/sim1.cc` 采用 `MsgGeneratorApp` 在所有主机之间生成背景流量。每个应用直接发送一条服从 workload 分布的 Homa message，接收端只负责接收，不在应用层返回固定小响应。该实现保留了：

- workload 大小分布；
- 指数到达间隔；
- 均匀随机目标选择；
- paper-style 统计窗口脚本。

但它仍不同于原文公开模拟器的关键点在于：

1. 当前是“单向消息流”，原文是“请求 + 固定小响应”的 RPC 流量。
2. 当前 `goodput` 统计基于所有完成消息累计字节，未区分 request 与 response，也未显式对齐原文的业务吞吐语义。
3. 当前 `incast` 是在背景流量外额外叠加一个 overlay，语义上接近原文的 `incast_workload_ratio`，但实现方式尚未完全统一。

## 设计选择

采用 **方案 B**：新增一个独立的 RPC workload app，而不是直接把现有 `MsgGeneratorApp` 改造成 RPC 工具。

原因：

1. `MsgGeneratorApp` 当前职责清晰，是单向消息生成器；硬改会使其承担双重语义。
2. 新 app 可以更直接表达“客户端发送 request，服务端回 `20B` response”的原文模型。
3. `sim1` 的改造可以局限在应用层与分析层，不干扰现有 Homa/SIRD 协议逻辑。

## 拟新增/修改的模块

### 1. 新增 RPC 工作负载应用

新增一个应用层模块 `RpcWorkloadApp`，文件名为 `rpc-workload-app.{cc,h}`，职责如下：

- 在客户端侧：
  - 按 workload CDF 抽样生成 request 大小；
  - 按指数分布决定下一次 request 发送时间；
  - 按均匀随机选择目标主机；
  - 发送 request。
- 在服务端侧：
  - 收到 request 后立即返回固定 `20B` response；
  - 默认不引入额外服务时间，与当前公开配置中极小服务时间的简化目标保持一致。

该模块应支持：

- 设置 request workload 分布；
- 设置平均 request 大小；
- 设置 offered load；
- 设置 response 大小；
- 设置远端主机列表；
- 标记当前消息类型，以便 trace 区分 request 与 response。

### 2. sim1 场景装配逻辑

`scratch/sim1.cc` 将不再把背景流量直接绑定到 `MsgGeneratorApp`，而是改为：

- 在所有主机上安装 `RpcWorkloadApp`；
- 用 workload 文件配置 request 大小分布；
- 用固定 `20B` 配置 response；
- 保持原有 `balanced`、`core`、`incast` 三种拓扑/负载组织方式；
- 保持原有 ToR / Spine / Host 链路参数；
- 保持现有 Homa/SIRD 协议参数注入方式。

其中：

- `balanced`：全局背景 RPC 流量；
- `core`：全局背景 RPC 流量，但 ToR-Spine 链路容量按现有设计收紧；
- `incast`：保留 overlay 机制，但在文档和参数语义中明确解释为与原文 `incast_workload_ratio` 对齐的近似实现。

### 3. trace 与统计口径

为了与原文表述更一致，需要将 trace 和分析语义分成两层：

1. **协议完成字节**
   - 用于保留现有调试和总传输观察能力。
2. **论文主结果字节**
   - 优先以 request 完成字节作为主业务量；
   - response 的 `20B` 可单独统计，但不应在主 goodput 结果中产生误导性影响。

具体要求：

- `msg.tr` 中能够区分 request / response，或通过端口/标记可靠区分；
- `goodput.tr` 中至少保留一组“论文口径”的 request goodput；
- `sim1_analyze.py` 需要明确区分：
  - 采样均值；
  - 统计窗口内累计业务字节换算的 run-average；
  - 仅 request 的 goodput。

### 4. ToR queue 语义

论文主结果中的 ToR queue 继续采用严格队列语义：

- `torQueueIncludeDevice=0`
- 优先使用 `deviceQueue=1p`

原因是原文关注的是交换机队列/链路瓶颈处的显式排队，而不是网卡设备队列中的隐藏缓存。这样可避免此前 `deviceQueue` 过大导致 qdisc 队列被低估的问题。

## 兼容性与保守性

本次改造不打算：

1. 重写 Homa/SIRD 协议内部逻辑；
2. 大范围重构现有 trace 文件格式；
3. 推翻已有 `sim1_run_paper50.sh` 的时间窗口推导脚本。

本次改造只在以下范围内动刀：

- `sim1` 的应用层流量生成与组织；
- `sim1` 的 trace 字段；
- `sim1` 的分析脚本；
- 必要的文档更新。

## 风险与处理策略

### 风险 1：事件数进一步增加

引入 response 后，消息数量与协议事件数会增加，尤其在 `google_rpc` 下可能进一步加剧仿真耗时。

处理策略：

- 保留现有 split/paper50 脚本结构；
- 先用小规模 case 做功能验证；
- 再在服务器上启动 paper-style 全量运行。

### 风险 2：goodput 定义变化导致历史结果不可直接比较

旧结果中的 goodput 混合了当前实现下的单向业务字节，新结果会更强调 request 业务吞吐。

处理策略：

- 新分析输出显式区分字段名；
- 文档中说明“旧结果是近似口径，新结果是原文对齐口径”。

### 风险 3：incast 仍非原文逐行复刻

即使总体语义接近，当前 ns-3 `incast` overlay 与原公开模拟器的脚本化配置仍可能不完全等价。

处理策略：

- 在论文和实验文档中明确说明；
- 先优先保证 `balanced/core` 的流量语义与统计口径严格对齐；
- `incast` 作为在 ns-3 框架下的近似对齐场景单独说明。

## 实现完成后的预期结论表述

完成本次改造后，可以更准确地写成：

> 我们在 ns-3/Homa 框架下复用了原文相同的 workload 分布、负载点与统计窗口，并将应用层流量组织改造成与原公开模拟器一致的 request/response RPC 语义；其中 request 大小服从 W3/W4/W5 分布，response 固定为 20B。与此同时，论文中的 goodput 与 ToR queue 结果均按原文风格的统计口径提取，从而使结果对比更具可解释性。

同时仍应保留一条边界说明：

> 由于底层模拟平台与协议实现框架不同，ns-3/Homa 版本与原公开模拟器在内部事件调度、协议栈细节及个别场景组织上仍存在工程差异，因此本文更准确的表述应为“尽量对齐原文工作负载与统计方法的复现”，而非逐行同构复刻。
