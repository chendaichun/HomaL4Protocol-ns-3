# 基于 Homa 改造 SIRD 的论文写法建议

本文档回答两个问题：

1. 在论文里，如何把“我是在 Homa 上改出来的 SIRD”这件事讲清楚？
2. 如何利用已有的调用图、状态机图和伪代码，把方法章节组织得更像论文，而不是代码说明？

这份文档已经结合当前仓库代码和 `git` 中的 Homa 初版实现做了对照。可以确认：

- 当前 Homa/SIRD 实现来自原始 Homa 的逐步扩展，而不是另起炉灶的新协议栈；
- `git` 中可见的 Homa 初版提交为 `f13fe4b35` (`HomaL4Protocol files added`)；
- 初版实现是单文件 `src/internet/model/homa-l4-protocol.cc`，而不是现在拆分后的 `core/send/recv` 三文件结构。

---

## 1. 总体上应该怎么讲

你现在这部分不要讲成“我做了很多零散 patch”，而要讲成：

> 本文以原始 Homa 的接收端驱动传输框架为基础，在不改变其基本消息分片、接收端授权和重组主线的前提下，系统性地补充了 SIRD 所需的四类能力：  
> 1) 反馈信号承载与提取；  
> 2) 发送端 credit 请求与发送端拥塞反馈；  
> 3) 接收端基于 host-side/core-side 双约束的 credit 分配；  
> 4) 支撑核心网络反馈闭环的 ECN 队列与细粒度观测机制。  
> 因此，本文的实现不是用一个全新的协议替换 Homa，而是在原始 Homa 的 receiver-driven 底座上，将其扩展为同时感知发送端上行拥塞与核心共享链路拥塞的 SIRD 近似实现。

这段话很关键，因为它先把“继承关系”说清楚了。后面所有小节都要围绕这个主线展开。

你原来的写法信息很多，但有两个问题：

1. 太像“按代码文件逐条报修改”；
2. 还不够突出“原始 Homa 到 SIRD 的机制升级路径”。

论文里更好的方式是：

- 先讲“原始 Homa 的能力边界”；
- 再讲“为什么这些边界不足以表达 SIRD”；
- 最后讲“你分别补了哪些机制，补完之后协议行为如何改变”。

---

## 2. 推荐的章节组织方式

推荐把方法章节组织成下面五个小节：

### 2.1 实现基线：原始 Homa

这一小节只做两件事：

- 说明你的实现基线是 ns-3 中的 HomaL4Protocol；
- 简要概括原始 Homa 的核心特征：
  - receiver-driven；
  - 发送端按 `grantOffset` 消费授权；
  - 接收端根据消息剩余大小、busy 状态和 overcommit 规则发 `GRANT`；
  - 没有 SIRD 专用的发送端反馈和 core-side ECN 闭环。

这一节不要展开太多细节，控制在一段到两段。

### 2.2 总体改造目标

这一小节要回答：

> 为什么原始 Homa 不够？SIRD 想额外解决什么问题？

建议直接说：

- 原始 Homa 的调度重点是接收端独占下行链路的消息优先级管理；
- 但 SIRD 关注的是共享链路场景下，发送端 host-side 积压与 core-side 拥塞的联合控制；
- 因此，要把 Homa 从“只依据接收端局部状态发 `GRANT`”扩展为“同时感知发送端与核心网络状态的 credit control”。

### 2.3 协议对象与报文头扩展

这里讲：

- `HomaHeader` 增加 `feedbackFlags`
- `HomaL4Protocol::GetTypeId()` 增加 SIRD 相关参数
- `HomaL4Protocol::Receive()` 与发送路径读取 `ECN + CSN`

这是“输入信号从哪里来”的问题。

### 2.4 发送端与接收端控制逻辑改造

这里拆成两个子小节：

- 发送端：首轮显式授权、累计 credit 反馈、CSN 上报
- 接收端：双桶预算、tick 驱动授权、按 token 发 `GRANT`

这是全文最核心的部分。

### 2.5 队列与观测机制

这里讲：

- `SirdQueueDisc` 如何提供 CE 信号
- 为什么要加 `SirdGrantDecision / SirdBucketState / SirdPacketState`

这是“如何闭环”和“如何观察闭环”的问题。

---

## 3. 可直接放论文的方法描述（润色版）

下面这版可以直接作为论文中“实现与修改”小节的正文基础。你可以整体使用，也可以拆成多段放进各子节。

### 3.1 实现基线与总体思路

> 本文的 SIRD 实现并非从头重写一个新的传输协议栈，而是基于 ns-3 中已有的 HomaL4Protocol 逐步扩展得到。原始 Homa 的核心特点是 receiver-driven：发送端维护消息分片与待发送队列，接收端依据消息接收进度和调度策略决定何时发放新的 `GRANT`，从而控制发送端后续数据的推进。该实现已经具备消息分片、接收端授权、消息重组以及 `GRANT/RESEND/ACK` 等基本控制路径，为本文实现 SIRD 提供了合适的工程底座。  
> 然而，原始 Homa 的控制逻辑主要围绕接收端独占下行链路上的调度效率展开，并未显式建模发送端共享上行链路拥塞，也未提供与核心网络共享链路拥塞直接对应的 ECN 闭环。因此，本文在保留原始 Homa 基本消息生命周期和 receiver-driven 授权框架的前提下，对报文头字段、发送端状态机、接收端 credit 分配逻辑、观测 trace 以及交换机队列模块进行了系统扩展，使协议能够同时感知发送端 host-side 积压与核心链路拥塞，并据此实现 SIRD 的近似控制行为。

### 3.2 报文头与反馈信号扩展

> 首先，在反馈信号承载方面，原始 Homa 头部主要包含消息标识、优先级、分片偏移和授权偏移等字段，并不具备显式承载发送端拥塞反馈位的能力。为此，本文在 `HomaHeader` 中增加了 `feedbackFlags` 相关字段及访问接口，使数据包能够携带 SIRD 所需的发送端反馈信息。相应地，在发送与接收路径上，修改后的 `HomaL4Protocol` 进一步联合读取 IPv4 头中的 ECN 字段和 `feedbackFlags` 中的 CSN 标志，从而统一提取共享链路反馈信号。相比原始实现仅按 `DATA/BUSY` 与 `GRANT/RESEND/ACK` 类型分发 Homa 内部控制报文的逻辑，这一修改使协议从“仅处理 Homa 自身控制状态”扩展为“同时感知发送端和核心网络状态”的控制模型。

### 3.3 协议对象配置参数扩展

> 其次，在协议对象配置方面，本文在 `HomaL4Protocol::GetTypeId()` 中新增了一组 SIRD 相关属性，包括 `SirdEnabled`、`SirdCreditBudgetPkts`、`SirdUnschThresholdPkts`、`SirdEcnMdFactor`、`SirdEcnAiStep`、`SirdSenderMdFactor`、`SirdSenderAiStep`、`SirdEcnAlphaGain` 以及 `SirdSenderCsnThreshold` 等。它们分别对应全局 credit 预算、长消息起发阈值、ECN 控制环路参数以及发送端反馈控制环路参数。相较于原始 Homa 主要提供 `RttPackets`、`NumTotalPrioBands`、`NumUnschedPrioBands` 与 `OvercommitLevel` 等基础调度参数，修改后的实现已经将 SIRD 双环路控制所需的关键参数纳入统一的属性系统，从而支持后续实验中的统一配置与参数扫描。

### 3.4 发送端状态机扩展

> 第三，在发送端状态机方面，本文并未推翻 Homa 原有的消息分片与发送调度框架，而是在 `HomaOutboundMsg` 与 `HomaSendScheduler` 中补充了 SIRD 所需的“长消息首轮显式授权”和“发送端累计 credit 反馈”机制。原始 Homa 中，消息创建后会直接依据 BDP 初始化可发送窗口，随后发送端在 `grantOffset` 约束下持续选择可发送分片。而在修改后的实现中，当启用 SIRD 且消息长度超过 `SirdUnschThresholdPkts` 时，发送端不再默认直接发送完整 unscheduled 窗口，而是进入 `wait-for-first-grant` 状态，先发送一个零负载 DATA 包作为初始 credit 请求，随后等待接收端显式发放首个 `GRANT` 后才继续发送真实数据。  
> 此外，发送侧调度器进一步维护累计已授权但尚未消耗的 scheduled credit。当累计 credit 超过 `SirdSenderCsnThreshold` 时，发送端会在即将发出的 DATA 包中置位 `FEEDBACK_CSN`，以此向接收端报告自身可能已成为共享上行瓶颈。这样一来，发送端不再只是被动消费接收端授权的一方，而是开始承担“反馈自身 credit 积压状态”的角色，从而为 host-side 控制环路提供输入。

### 3.5 接收端 credit 发放与双桶预算

> 第四，在接收端 credit 发放逻辑方面，本文对原始 Homa 的接收调度器进行了最核心的扩展。原始 Homa 主要依据 overcommit 水平、消息剩余大小与 busy sender 状态决定是否发放 `GRANT`，其目标是提升接收端下行链路利用率和消息调度效率。相比之下，修改后的 `HomaRecvScheduler` 还进一步维护了 SIRD 所需的全局 outstanding credit 统计、每发送端在用 credit 统计、每发送端 host-side/core-side budget，以及最近观测到的 CSN 和 CE 状态。  
> 具体而言，接收端在收到 DATA 包后，首先回收一个已经发出但尚未归还的 credit 份额，并同步减少全局和 per-sender 的 in-use 计数；随后根据 CSN 和 ECN 两类反馈分别更新发送端的 host-side budget 与 core-side budget，并取两者的较小值作为下一轮发放 `GRANT` 时的有效 sender budget。在授权时序上，接收端不再简单地“有 grantable packet 就直接发 `GRANT`”，而是通过 `CreditTick` 驱动离散化的 credit 发放过程，并在 `IssuePendingGrants()` 中同时检查全局预算、per-sender 预算以及 grantable window 是否满足条件。每次成功发放授权时，接收端只暴露一个 credit token 对应的新增授权窗口，从而使“全局桶 + 每发送端桶”的记账过程与 SIRD 机制保持一致。  
> 因此，相比原始 Homa 主要解决“谁先被授予更多分片”的问题，本文的实现进一步扩展为“在发放每一笔授权之前，都先判断该 sender 是否仍具备 host-side 与 core-side 的剩余容量”。

### 3.6 观测与交换机队列支持

> 第五，在调度观测与交换机支持方面，本文在原始 Homa trace 体系之上新增了 `SirdGrantDecision`、`SirdBucketState` 和 `SirdPacketState` 等细粒度 trace，用于记录某次授权时的 sender budget、ECN EWMA、CSN 状态、每发送端当前在用 credit 以及全局在用 credit 等内部控制信息。这些 trace 使后续实验不再只能观察吞吐、时延或消息完成时间等外部结果，而能够直接回溯 SIRD 控制环路内部“为什么本轮没有授权”“为什么某个 sender 被减额”“为什么全局 budget 被耗尽”等机制性原因。  
> 同时，原始 Homa ns-3 实现本身并未提供与 SIRD 论文中共享链路 ECN 反馈直接对应的交换机队列模块。为此，本文额外实现了 `SirdQueueDisc`，采用 FIFO 服务与固定阈值 ECN 标记策略：当队列占用超过 `MarkThreshold` 时，对到达分组打上 CE 标记；仅当队列长度超过 `MaxSize` 时才执行丢包。这样，接收端在读取 IPv4 头部 ECN 字段时，才能获得与核心链路拥塞程度直接相关的反馈信号，从而使 core-side 控制环路真正闭合。

### 3.7 小结段

> 综合来看，本文并没有用一个全新的协议替换原始 Homa，而是沿着其既有的消息分片、接收端授权和调度框架，在报文头反馈字段、发送端 credit 请求与反馈、接收端双桶预算、细粒度授权时序、trace 输出以及交换机 ECN 队列等环节逐步补充 SIRD 所需能力。原始 Homa 提供了“接收端驱动消息传输”的工程底座，而本文的修改则将这一底座扩展为能够同时处理发送端上行拥塞和核心共享链路拥塞的 SIRD 近似实现。

---

## 4. 如何利用已有图和伪代码来讲

你现在手上已经有 4 类文档材料：

- [详细调用图](./homa_send_recv_call_graph_zh.md)
- [精简调用图](./homa_send_recv_call_graph_simple_zh.md)
- [论文风格 C++ 伪代码](./homa_send_recv_pseudocode_zh.md)
- [状态机图](./homa_send_recv_state_machine_zh.md)

论文里不要把它们并列堆上去，而要分工使用。

### 4.1 正文里最推荐放的图

如果你正文只能放两张图，建议是：

1. `Homa/SIRD 状态机图`
2. `SenderMain / ReceiverMain` 两段伪代码

原因很简单：

- 调用图更像“工程实现导览”，适合附录或汇报；
- 状态机图更适合回答“协议行为为什么会变”；
- 伪代码更适合回答“控制逻辑到底怎么运行”。

### 4.2 如果正文还能再放一张图

第三优先级放：

- [精简调用图](./homa_send_recv_call_graph_simple_zh.md)

它的作用不是解释全部细节，而是帮助读者建立：

- 发送端有哪些主要模块
- 接收端有哪些主要模块
- 控制包路径和数据路径分别怎么走

### 4.3 图和文字的搭配方式

推荐顺序如下：

#### 先用一段话总述

先在文字里讲：

> 本文并未改写 Homa 的基本 receiver-driven 主线，而是在其原有发送端消息管理、接收端授权以及消息重组机制上，分别加入反馈信号、发送端 credit 请求、接收端双桶预算和交换机 ECN 队列支持。

#### 再放状态机图

然后接一句：

> 图 X 展示了修改后协议在发送端和接收端的主要状态转移。与原始 Homa 相比，最关键的新状态包括发送端的 `WaitingFirstGrant` / `WaitingCredit` 以及接收端的 `CreditTick` 驱动授权过程。

#### 再放伪代码

接着过渡到算法：

> 为了更清晰地说明控制逻辑，算法 1 与算法 2 分别给出发送端与接收端的事件驱动主过程。前者描述消息从创建、等待显式授权、消费 scheduled credit 到完成确认的全过程；后者描述消息从首包到达、接收端 credit 回收、按预算发放 `GRANT`、直到 `ACK` 与 `RESEND` 的控制闭环。

#### 调用图放到附录或实现章节

详细调用图最适合放在：

- 实现章节末尾
- 附录
- 答辩汇报

因为它更偏“源码结构导览”，而不是“协议机制表达”。

---

## 5. 和原始 Homa 对照时，最值得强调的点

这部分最好不要泛泛地说“我加了很多东西”，而要说“初版没有什么，我补了什么”。

基于 `git show f13fe4b35` 可确认如下事实。

### 5.1 原始 Homa 头部没有 SIRD 反馈字段

在初版 `homa-header.h` 中，头部字段主要包括：

- `srcPort`
- `dstPort`
- `txMsgId`
- `flags`
- `prio`
- `msgSize`
- `pktOffset`
- `grantOffset`
- `payloadSize`
- `generation`

没有 `feedbackFlags` 之类的发送端反馈承载位。

因此，你可以明确写：

> 原始 Homa 头部只能表达 Homa 本身的消息控制信息，不能显式表达发送端拥塞反馈；本文新增 `feedbackFlags` 字段后，协议才能在数据路径中同时承载 host-side 反馈信号。

### 5.2 原始 Homa 的参数系统没有 SIRD 控制参数

初版 `HomaL4Protocol::GetTypeId()` 中主要只有：

- `RttPackets`
- `NumTotalPrioBands`
- `NumUnschedPrioBands`
- `OvercommitLevel`
- `InbndRtxTimeout`
- `OutbndRtxTimeout`
- `MaxRtxCnt`
- `OptimizeMemory`

没有：

- `SirdEnabled`
- `SirdCreditBudgetPkts`
- `SirdUnschThresholdPkts`
- `SirdEcnMdFactor`
- `SirdSenderMdFactor`
- `SirdSenderCsnThreshold`

所以你可以明确写：

> 原始 Homa 的可配置参数主要面向接收端授权与优先级控制；本文则进一步将 SIRD 的 sender/global budget、ECN/CSN 环路和长消息起发门限等参数纳入统一的协议配置对象中。

### 5.3 原始发送端没有“首轮显式授权”与“发送端 credit 反馈”

从初版代码可以看到：

- `HomaOutboundMsg::GetNextPktOffset()` 只检查 `pktOffset <= m_maxGrantedIdx`
- `HandleGrantOffset()` 只推进 `m_maxGrantedIdx` 和 `m_prio`
- 没有 `m_waitForFirstGrant`
- 没有零负载 DATA 请求首个 `GRANT`
- 没有 `GetAccumulatedCreditPkts()` / `FEEDBACK_CSN`

因此你可以直接写：

> 原始 Homa 中，发送端一旦拥有初始 BDP 窗口即可直接发送 unscheduled 数据；而在本文的 SIRD 改造中，长消息必须先请求首轮显式授权，并在后续数据发送中反馈本地累计 credit 积压状态。

### 5.4 原始接收端没有双桶预算控制

从初版接收端代码可以看到：

- `ReceiveDataPacket()` 收到一个新分片后，直接 `m_maxGrantableIdx++`
- 接收端主要围绕 `IsGrantable()`、`remainingBytes`、busy sender 和 overcommit 做决策
- 没有 sender budget / global budget
- 没有 host-side / net-side 双环路
- 没有 `CreditTick` 这套 token 化节拍控制

所以你可以明确写：

> 原始 Homa 中，接收端是否继续授权主要取决于消息本身是否仍可被授予更多分片；而在本文实现中，每一笔授权在发出之前都还要额外满足全局 budget、per-sender budget 与 token 化时序约束，因此授权逻辑已从“消息级调度”扩展为“预算受限的 credit control”。

### 5.5 原始 Homa 没有核心网络 ECN 闭环

这个点尤其要强调，因为它最能体现你不是只改了端系统。

你可以写：

> 即使在端主机逻辑中增加了 host-side 与 core-side 状态变量，如果交换机队列本身不产生与共享链路拥塞相对应的 CE 标记，则 net-side 控制环路仍然无法闭合。为此，本文额外实现 `SirdQueueDisc`，以固定阈值 ECN 标记方式向接收端提供核心拥塞反馈，从而将端系统扩展推进到端到端闭环仿真。

---

## 6. 你在论文里最该避免的讲法

### 不要这样讲

> 我在 Homa 上加了 feedbackFlags，加了参数，加了 trace，又改了发送端和接收端。

这太像改代码日志。

### 更好的讲法

> 本文沿着“反馈信号输入 - 发送端反馈 - 接收端预算决策 - 核心拥塞闭环”这一主线，对原始 Homa 进行了系统扩展。

因为这能让评审觉得你是在讲“机制设计”，而不是讲“代码 patch 列表”。

---

## 7. 一个更适合论文的方法章节结构

你可以直接照下面这个结构写：

### 3.X Implementation Based on Homa

#### 3.X.1 Baseline Homa framework

讲原始 Homa 的 receiver-driven 主线。

#### 3.X.2 Feedback signal extensions

讲 `feedbackFlags + ECN + CSN`。

#### 3.X.3 Sender-side modifications

讲首轮显式授权、累计 credit、CSN 反馈。

#### 3.X.4 Receiver-side credit control

讲双桶预算、`CreditTick`、`IssuePendingGrants()`。

#### 3.X.5 Queue and tracing support

讲 `SirdQueueDisc` 与 trace。

#### 3.X.6 Summary of differences from original Homa

用一个小表或者一段总结，把“原始 Homa vs SIRD 扩展版”收束一下。

---

## 8. 你可以直接放进论文的一段总结

最后如果你要一段收束全节的话，可以直接用下面这段：

> 总体而言，本文并未改变 Homa “由接收端驱动消息推进”的基本设计，而是在其原有消息分片、接收端授权和重组框架上，进一步引入了发送端反馈信号、接收端双桶预算、credit tick 驱动的细粒度授权时序以及交换机 ECN 队列支持。这样，原始 Homa 被扩展为一个能够同时感知 host-side 与 core-side 共享拥塞状态的 SIRD 近似实现。相比原始 Homa 主要解决接收端独占下行链路调度问题，本文实现则进一步将控制目标扩展到共享上行链路与核心网络资源的联合约束。

---

## 9. 建议你下一步怎么用这些文档

建议顺序：

1. 方法章节正文以本文件为主
2. 状态机图作为正文图
3. `SenderMain / ReceiverMain` 伪代码作为正文算法
4. 精简调用图放附录或实现说明
5. 详细调用图只在答辩或代码导览里使用

如果你愿意，我下一步可以继续直接生成一版“可直接粘进论文”的：

- 方法章节 LaTeX 风格分节文本
- 图题
- 伪代码标题
- “原始 Homa vs SIRD 扩展版”的对照表格草稿
