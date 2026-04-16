# SIRD 代码说明

## 1. 核心类一共有几个，它们分别做什么

如果只看 `src/internet/model/homa-l4-protocol.h` 这个文件，那么和 SIRD 主体实现直接相关的核心类一共是 5 个：

1. `HomaL4Protocol`
2. `HomaOutboundMsg`
3. `HomaSendScheduler`
4. `HomaInboundMsg`
5. `HomaRecvScheduler`

除此之外，还有 2 个非常重要的辅助类：

1. `HomaHeader`
2. `PfifoHomaQueueDisc`

因此可以有两种说法：

- 如果只按“协议主逻辑”统计，是 5 个核心类。
- 如果把“协议头格式”和“优先级队列”也算进去，那么与 SIRD 密切相关的主要类是 7 个。

## 1.8 后文如何标出“你的修改”

为了后面写论文时不把“原始机制”和“你的代码实现”混在一起，后文统一按下面方式区分：

- `原始机制`：指原始 Homa 或你参考论文中的描述
- `你的修改`：指你在当前代码中主动加入、改写或替代的实现
- `当前代码实现`：指这份仓库现在真正执行的逻辑
- `与原文差异`：指当前实现和原文不完全一致的地方

### 1.1 `HomaL4Protocol`

这是整个 Homa 协议在 ns-3 中的四层总控类。

它主要负责：

- 把 Homa 注册为 ns-3 的一个传输层协议
- 暴露协议参数，包括所有 SIRD 参数
- 接收应用层发下来的消息
- 接收网络层送上来的数据包
- 根据包类型把处理任务分派给发送侧或接收侧调度器
- 把已经构造好的数据包统一下发给 IP 层

可以把它理解为整个协议实现的总入口和总调度者。

### 1.2 `HomaOutboundMsg`

这是发送端“一条消息”的状态对象。

它记录的信息包括：

- 源地址、目的地址和端口
- 整条消息被切成多少个分片
- 哪些分片还在待发送队列中
- 当前已经被授权发送到哪个分片
- 是否正在等待第一个 `GRANT`
- 是否已经发送过首个零负载请求包

它的核心作用是：把“应用层的一条消息”转换成“发送端可调度的若干数据包状态”。

### 1.3 `HomaSendScheduler`

这是发送端调度器。

它主要负责：

- 接收新的待发送消息
- 给每条消息分配 `txMsgId`
- 在多条待发送消息之间选择下一条应该发送的消息
- 决定当前发的是首个零负载请求包，还是真实数据包
- 在收到 `GRANT`、`RESEND`、`ACK` 后推进发送端状态

它的核心作用是：决定“此时此刻发送端真正要发出去的那个包”。

### 1.4 `HomaInboundMsg`

这是接收端“一条消息”的状态对象。

它记录的信息包括：

- 这条消息来自谁、发给谁
- 对应哪个 `txMsgId`
- 一共有多少个分片
- 哪些分片已经收到
- 当前还剩多少字节没有收到
- 当前最多允许继续授权到哪里
- 当前已经实际授权到哪里

它的核心作用是：把接收端看到的一串分片组织成“一条正在接收和重组的消息”。

### 1.5 `HomaRecvScheduler`

这是接收端调度器，也是 SIRD 的核心实现位置。

它主要负责：

- 接收 `DATA` 和 `BUSY`
- 更新每条入站消息的接收进度
- 维护活跃消息队列
- 维护每个发送方的 SIRD 预算
- 根据 ECN 和 `CSN` 调整预算
- 决定什么时候发送 `GRANT`
- 决定给哪个发送方、哪条消息继续发信用

它的核心作用是：以接收端为中心，控制发送端还能继续注入多少数据。

### 1.6 `HomaHeader`

这个类定义 Homa 的包头格式。

它保存了 SIRD 所需要的重要字段，例如：

- `txMsgId`
- `pktOffset`
- `grantOffset`
- `prio`
- `flags`
- `feedbackFlags`

其中最关键的是 `feedbackFlags` 里的 `FEEDBACK_CSN`，它用来把发送端主机拥塞状态带回接收端。

### 1.7 `PfifoHomaQueueDisc`

这是交换机或网卡上的优先级队列。

它负责：

- 从 IP 的 DS 字段提取优先级
- 把包放进不同优先级队列
- 始终优先从更高优先级队列出队

它本身不做 SIRD 决策，但它决定了接收端授予的优先级能否真正体现在网络中的发送顺序上。

---

## 2. 从“一个包被接收”的流程来分析函数

这一部分按接收路径分析。SIRD 的本质是接收端驱动，因此接收路径是理解 SIRD 的关键。

## 2.1 `HomaL4Protocol::Receive`

这是所有入站 Homa 数据包的统一入口。

这个函数做的事情可以概括为：

1. 检查协议号，确认这是 Homa。
2. 从包中取出 `HomaHeader`。
3. 校验头部记录的载荷长度和实际长度是否一致。
4. 根据五元组查找对应端点。
5. 根据 `flags` 判断包的类型。
6. 把不同类型的包交给不同的调度器处理。

其中关键分流是：

- `DATA` 和 `BUSY` 交给 `HomaRecvScheduler::ReceivePacket`
- `GRANT`、`RESEND`、`ACK` 交给 `HomaSendScheduler::CtrlPktRecvdForOutboundMsg`

这个函数本身不直接执行 SIRD 算法，但它决定了接收端控制逻辑从哪里开始进入。

## 2.2 `HomaRecvScheduler::ReceivePacket`

这是接收端调度器处理入站包的第一层入口，也是 SIRD 最重要的入口之一。

当收到的是 `DATA` 时，这个函数会完成以下几件关键工作：

1. 读取发送端带回来的 `CSN` 标记，更新 `m_sirdSenderCsnState`
2. 读取 IPv4 头中的 ECN 状态，判断是否被标记为 `CE`
3. 更新 `m_sirdSenderCeState`
4. 更新接收端维护的 CE 统计量和平滑值
5. 如果这个 `DATA` 包有真实负载，则回收一个已经借出的信用
6. 调用 `ReceiveDataPacket`，把该包并入某条入站消息
7. 调用 `SendAppropriateGrants`，重新判断现在该给谁发 `GRANT`

这一步体现了 SIRD 的核心思想：接收端先观察拥塞信号，再做后续授权决策。

## 2.3 `HomaRecvScheduler::ReceiveDataPacket`

这个函数负责把当前收到的包，归并到某条具体的入站消息中。

它分两种情况：

- 如果消息已经存在，就找到对应的 `HomaInboundMsg`，调用其 `ReceiveDataPacket`
- 如果消息第一次出现，就创建新的 `HomaInboundMsg`

然后它继续判断：

- 如果整条消息已经收齐，则调用 `ForwardUp`
- 如果还没收齐，则调用 `ScheduleMsgAtIdx`，把该消息重新放回活跃消息队列

这一步的意义在于：SIRD 不只是控制发送方，还必须知道每条消息当前接收到了什么程度，才能决定接下来继续发多少信用。

## 2.4 `HomaRecvScheduler::GetInboundMsg`

这个函数负责根据以下信息查找一条活跃的入站消息：

- 源地址
- 目的地址
- 源端口
- 目的端口
- `txMsgId`

它的意义是：SIRD 不是针对“某个孤立的包”进行控制，而是针对“某个发送方的某条消息”进行控制，所以必须先把包准确归属到正确的消息状态上。

## 2.5 `HomaInboundMsg::ReceiveDataPacket`

这是接收端单条消息状态真正发生变化的地方。

这里要分两种情况理解。

### 情况一：零负载 `DATA`

如果 `p->GetSize() == 0`，说明这不是一个真正携带数据的分片，而是一个“首轮信用请求包”。

这在 SIRD 中非常关键。原因是：

- 长消息在发送端一开始不能直接发送真实数据
- 但接收端必须先知道“有一条长消息到来了”
- 所以发送端先发一个零负载 `DATA`
- 接收端看到后建立消息状态，再开始显式发放 `GRANT`

也就是说，这种零负载 `DATA` 不是为了传数据，而是为了触发接收端建模和授权。

你的修改：

- 这里引入了“零负载 `DATA` 作为长消息启动请求”的做法
- 这是你为了让长消息在 SIRD 下先建模、后授权而加入的启动机制

### 情况二：真实 `DATA`

如果这是一个真实载荷的数据包，那么这个函数会：

- 记录该偏移的分片已经收到
- 更新剩余字节数 `m_remainingBytes`
- 将 `m_maxGrantableIdx` 往前推进

这里的关键含义是：每收到一个真正的数据分片，接收端就知道发送端完成了一部分传输，因此后续可以考虑再发新的信用。

## 2.6 `HomaInboundMsg::HomaInboundMsg`

当接收端第一次看到一条消息时，会调用这个构造函数初始化该消息状态。

它会初始化：

- 消息总大小
- 分片总数
- 接收缓存
- 每个分片是否收到
- 当前最大已授权位置
- 当前最大可授权位置

SIRD 相关的关键逻辑在这里：

- 如果启用了 SIRD，且消息长度超过 `sirdUnschThresholdPkts`
- 则把该消息视为“长消息”
- 对长消息采用更保守的初始授权方式

此时会出现如下状态特征：

- `m_maxGrantedIdx = 0`
- `m_maxGrantableIdx = 1`

这说明接收端不会像原始 Homa 那样一开始就放出较大的初始窗口，而是只给非常有限的启动空间，后续靠显式信用逐步推进。

你的修改：

- 这里把长消息的初始授权窗口收得很紧
- 核心目的就是配合后面的显式 credit 控制

## 2.7 `HomaRecvScheduler::ScheduleMsgAtIdx`

这个函数决定一条活跃消息在接收端活跃队列中的位置。

有两种模式：

- 如果开启 `UseSrrScheduling`，则采用近似 FIFO 或 SRR 的顺序
- 否则按剩余字节排序，近似 SRPT

这说明 SIRD 并没有取代消息排序逻辑，而是在原有排序逻辑之外，又增加了一层“发送方预算”限制。

因此，一条消息能否被继续授权，要同时满足两件事：

- 它在接收端排序中足够靠前
- 它对应的发送方仍然有可用信用

## 2.8 `HomaRecvScheduler::SendAppropriateGrants`

这是整个 SIRD 实现中最核心的函数。

如果没有启用 SIRD，它走的是原始 Homa 风格的授权逻辑：

- 根据 overcommit
- 根据活跃消息顺序
- 根据优先级带宽
- 决定给哪些消息继续发 `GRANT`

但如果启用了 SIRD，这个函数就变成了“双反馈环加显式信用预算”的控制中心。

它的大致流程如下：

1. 遍历当前所有活跃入站消息。
2. 跳过已经完全授权的消息。
3. 跳过已经返回 `BUSY` 的发送方。
4. 为新出现的发送方初始化两个预算：
   - 网络侧预算 `m_sirdSenderBudgetNetPkts`
   - 主机侧预算 `m_sirdSenderBudgetHostPkts`
5. 根据 `CE` 调整网络侧预算：
   - 如果看到 `CE`，执行乘法减小
   - 如果没看到 `CE`，执行加法增加
6. 根据 `CSN` 调整主机侧预算：
   - 如果看到 `CSN`，执行乘法减小
   - 如果没看到 `CSN`，执行加法增加
7. 取两个预算的较小值，作为该发送方当前的有效预算。
8. 计算该发送方已经借出的信用。
9. 再计算系统全局剩余信用。
10. 调用 `CapGrantableWindow(1)`，强制每次最多只新增 1 个分组级信用。
11. 如果发送方本地预算和全局预算都还有剩余，就生成一个 `GRANT`。
12. 调用 `TraceSirdGrantDecision` 记录授权决策。
13. 发送这个 `GRANT`。
14. 把该发送方和全局的未归还信用都加 1。

这段逻辑可以概括为两句话：

- 原始 Homa 主要解决“下一步让谁发”
- SIRD 进一步解决“这个发送方此刻最多还能发多少”

你的修改：

- 这里加入了双反馈预算
- 加入了每发送方已借出 credit 的显式统计
- 并把授权动作和 credit 使用量绑定起来

这部分是你当前代码中最核心的 SIRD 主体改动之一。

## 2.9 `HomaInboundMsg::CapGrantableWindow`

这个函数虽然很短，但在 SIRD 中非常重要。

它的作用是：把可授权窗口限制在“当前已授权位置之后最多再向前推进指定数量的分片”。

在 `SendAppropriateGrants` 中，它被用成了 `CapGrantableWindow(1)`，这意味着：

- 每次新增授权基本只对应 1 个数据分片
- 每一个 `GRANT` 都近似对应一个分组级令牌
- 接收端维护的信用数量和实际允许发送的数据分片数是一一对应的

如果没有这一步，那么“预算按分组计数”在实现上就会变得不够精确。

你的修改：

- 这里把 `CapGrantableWindow` 固定用成 `1`
- 等于把一次授权明确压成“最多新增 1 个分片”
- 这是你把 credit 精确离散化成 packet token 的关键实现选择

## 2.10 接收端是如何把 credit 变成 `grantOffset` 的

这是 SIRD 实现里最关键的一条链路，必须单独说明。

接收端并不是简单地维护一个“credit 数”，然后直接把这个数原样塞进 `grantOffset`。当前代码真正做的事情是：

1. 先维护“该发送方还能不能继续拿信用”
2. 再把这份信用约束落实到“这条消息的可授权窗口”
3. 最后把“可授权窗口上界”写进 `grantOffset`

也就是说，在这份代码里：

- `credit` 表示接收端还愿不愿意继续放行
- `grantOffset` 表示接收端这一次具体允许发送端发到哪个分片偏移

两者不是同一个变量，但前者最终会约束后者。

### 第一步：接收端维护 credit

接收端在 `HomaRecvScheduler::SendAppropriateGrants` 中维护每个发送方的预算和已借出信用。

关键变量有：

- `m_sirdSenderBudgetNetPkts`
- `m_sirdSenderBudgetHostPkts`
- `m_sirdSenderCreditsInUsePkts`
- `m_sirdGlobalCreditsInUsePkts`

函数先计算：

- 某个发送方当前预算上限 `senderBudgetPkts`
- 这个发送方当前已经借出多少信用 `senderInUsePkts`
- 该发送方当前还剩多少可用信用 `senderAvailPkts`
- 全局当前还剩多少可用信用 `globalAvailPkts`

只有当：

- `senderAvailPkts > 0`
- `globalAvailPkts > 0`

时，接收端才允许继续发新的 `GRANT`。

### 第二步：credit 约束消息可授权窗口

即便预算允许继续授权，接收端也不会无界地向前推进授权窗口，而是调用：

- `currentMsg->CapGrantableWindow(1)`

这一步的含义是：

- 本轮最多只允许这条消息再新增 1 个分片级授权

因此，信用约束在这里被具体落实成了“授权窗口最多再向前推进多少”。

### 第三步：把窗口上界写进 `grantOffset`

真正写 `grantOffset` 的地方在 `HomaInboundMsg::GenerateGrantOrAck`。

这个函数里直接执行：

- `homaHeader.SetGrantOffset(m_maxGrantableIdx);`

然后紧接着执行：

- `m_maxGrantedIdx = m_maxGrantableIdx;`

这表示：

- `m_maxGrantableIdx` 是当前“理论上允许授权到哪里”
- 一旦 `GRANT` 真正生成并发送，`m_maxGrantedIdx` 就同步推进到这里

因此，这条链路可以严格写成：

```text
发送方 credit 是否可用
-> 是否允许继续授权
-> CapGrantableWindow(1) 限制本次新增授权幅度
-> GenerateGrantOrAck() 把 m_maxGrantableIdx 写入 grantOffset
-> 这个 grantOffset 随 GRANT 包发给发送端
```

### 第四步：为什么不是直接把 credit 数写进 `grantOffset`

因为 `credit` 和 `grantOffset` 的语义不同：

- `credit` 是“还能再放行多少”
- `grantOffset` 是“已经允许发送到哪里”

前者是预算概念，后者是消息内分片偏移概念。

所以当前实现采用的是：

- 用 credit 决定能否继续前推授权窗口
- 再把前推后的窗口上界编码到 `grantOffset`

这种实现方式更符合 Homa 原本按分片偏移授权的协议格式。

你的修改：

- 当前代码没有把“credit 数量”直接塞进报文
- 而是把 credit 的结果先转成授权窗口上界，再编码到 `grantOffset`

这是你当前实现中一个很重要的结构性选择。

## 2.11 `HomaRecvScheduler::ForwardUp`

当一条消息已经完整接收后，接收端会调用这个函数：

1. 用 `GetReassembledMsg` 重组完整消息
2. 调用 `HomaL4Protocol::ForwardUp` 上交应用层
3. 发送一个 `ACK`
4. 调用 `ClearStateForMsg` 清理这条消息的状态

这一步表示：这条受 SIRD 控制的消息在接收端生命周期结束了。

## 2.12 `HomaRecvScheduler::ExpireRtxTimeout`

SIRD 主要解决的是信用和拥塞控制，但可靠性仍然要靠重传机制。

这个函数负责：

- 判断消息是否长期没有进展
- 必要时发送 `RESEND`
- 重新设置接收端重传定时器
- 更新“连续多少次超时仍无进展”的计数

它不是 SIRD 控制算法本身，但它保证了 SIRD 不会因为某个分片丢失而永久停滞。

---

## 3. 从“一个包被发送”的流程来分析函数

这一部分按发送路径分析，也就是应用层的一条消息如何最终变成网络中的一个个包。

## 3.1 `HomaL4Protocol::Send`

这是发送路径的起点。

它负责：

1. 根据应用层给出的消息和五元组构造一个 `HomaOutboundMsg`
2. 保存可选的路由信息
3. 把该消息交给 `HomaSendScheduler::ScheduleNewMsg`
4. 如果成功分配到 `txMsgId`，记录消息开始的跟踪信息

这个函数本身不决定“当前立即发什么包”，它只是把应用层消息正式交给发送侧调度器。

## 3.2 `HomaOutboundMsg::HomaOutboundMsg`

这是发送端单条消息最重要的初始化函数。

它先完成常规 Homa 的工作：

- 保存地址和端口
- 计算消息总字节数
- 根据 MTU 对消息分片
- 建立待发送分片队列

然后进入 SIRD 相关的关键分支：

- 如果消息较短，则仍可保留较多的初始非调度发送能力
- 如果启用了 SIRD，且消息长度超过 `sirdUnschThresholdPkts`
- 则：
  - `m_maxGrantedIdx = 0`
  - `m_waitForFirstGrant = true`

这表示长消息在一开始不能直接发送真实数据，而必须先等待接收端授权。

这是 SIRD 与原始 Homa 在发送端最本质的区别之一。

你的修改：

- 对长消息引入 `m_waitForFirstGrant = true`
- 这使得长消息必须经历“请求包 -> 首个 GRANT -> 再发真实数据”的启动流程

## 3.3 `HomaSendScheduler::ScheduleNewMsg`

这个函数负责：

- 从空闲列表中分配一个 `txMsgId`
- 把新消息挂到 `m_outboundMsgs`
- 如果发送器当前空闲，则安排一次新的 `TxDataPacket`

它的意义在于：所有发送消息都要先登记到统一调度器中，再由调度器决定下一次到底发谁。

## 3.4 `HomaSendScheduler::GetNextMsgId`

这个函数遍历所有活跃发送消息，找出当前最适合发送的那一条。

它考虑的条件包括：

- 该消息没有过期
- 该消息剩余字节数尽量小
- 它要么已经有可发送的数据分片
- 要么虽然还不能发真实数据，但需要先发一个首轮信用请求包

SIRD 在这里带来的变化是：

在原始 Homa 中，调度器主要关心“谁现在有数据可发”；在 SIRD 中，调度器还必须把“零负载请求包”视为一种合法的发送对象。

## 3.5 `HomaOutboundMsg::NeedsInitialCreditRequest`

这个函数返回真，说明当前消息满足以下三个条件：

- 它是一条长消息
- 它还在等待第一个 `GRANT`
- 它的首个请求包还没有发过

这个函数的作用，就是把发送路径分成两种状态：

- 已经获得授权，可以发送真实数据
- 尚未获得授权，只能先发送请求包

## 3.6 `HomaOutboundMsg::GenerateInitialCreditRequest`

这个函数生成的是一个特殊的零负载 `DATA` 包。

它的特点是：

- 类型仍然是 `DATA`
- `payloadSize = 0`
- `pktOffset = 0`
- `grantOffset = 0`
- 优先级初始设为 0
- 设置 `ECT0`

它不是为了真正传输数据，而是为了告诉接收端：

- 有一条长消息到来了
- 请为这条长消息建立接收状态
- 然后开始显式发放信用

所以它本质上是“借用数据包格式实现启动控制语义”。

你的修改：

- 这里没有新定义单独的控制包类型
- 而是复用了 `DATA` 包格式，把零负载 `DATA` 当作首轮信用请求

## 3.7 `HomaSendScheduler::GetNextPktOfMsg`

这个函数负责把“选中的消息”转成“真正要发送的那个包”。

它分成两个分支。

### 分支一：发送首轮信用请求包

如果 `NeedsInitialCreditRequest()` 返回真，则：

- 调用 `GenerateInitialCreditRequest`
- 调用 `MarkInitialCreditRequestSent`
- 返回该零负载请求包

### 分支二：发送真实数据包

如果该消息已经获得授权，则：

- 调用 `GetNextPktOffset`
- 调用 `RemoveNextPktFromTxQ`
- 填写 Homa 头
- 设置 DSCP 和 ECN
- 必要时设置 `CSN`

这里有两个 SIRD 关键点。

#### 第一，发送端何时设置 `CSN`

代码通过 `m_homa->GetTimeToDrainTxQueue()` 判断本地发送队列是否已经积压。

如果队列排空时间超过 `SirdSenderCsnThreshold`，则在 Homa 头中置位：

- `FeedbackFlags_t::FEEDBACK_CSN`

这表示发送端主动告诉接收端：我本地主机侧也已经出现拥塞。

你的修改：

- 这里的 `CSN` 不是按“累计持有多少 credit”直接判定
- 而是按“本地发送队列预计排空时间是否超过阈值”来判定

#### 第二，数据包会带上 `ECT0`

启用 SIRD 时，数据包会带上 `ECT0`，这样网络设备就可以通过 ECN 标记反馈 `CE`，而接收端再利用 `CE` 调整网络侧预算。

你的修改：

- 这里把发送端主机侧反馈 `CSN` 和网络侧反馈 `ECT0/CE` 同时接入了控制环
- 这就是你当前代码中的双反馈基础

## 3.8 `HomaSendScheduler::TxDataPacket`

这是发送端真正的发包循环。

它的执行逻辑是：

1. 先看本地发送队列是否已经排空
2. 如果还未排空，则延后自己
3. 如果已经可以继续发送，则调用 `GetNextMsgId`
4. 再调用 `GetNextPktOfMsg`
5. 最后调用 `HomaL4Protocol::SendDown`
6. 再安排下一次发送事件

因此，无论是零负载请求包还是真实数据包，最终都必须经过这个函数。

## 3.9 `HomaSendScheduler::CtrlPktRecvdForOutboundMsg`

当发送端收到接收端发来的控制包时，会由这个函数处理。

它按类型分别执行：

- 收到 `GRANT` 时，调用 `HandleGrantOffset`
- 收到 `RESEND` 时，调用 `HandleGrantOffset` 和 `HandleResend`
- 收到 `ACK` 时，调用 `HandleAck` 并清理状态

SIRD 最关键的是 `GRANT` 分支，因为它会让发送端从“等待授权”进入“可以继续发送真实数据”的状态。

## 3.10 `HomaOutboundMsg::HandleGrantOffset`

这是发送端对接收端授权作出反应的核心函数。

它主要做三件事：

1. 更新 `m_maxGrantedIdx`
2. 记录接收端授予的优先级
3. 如果之前还在等待第一个 `GRANT`，则清除等待状态

从控制路径看：

- `GenerateInitialCreditRequest` 表示“请求启动”
- `HandleGrantOffset` 表示“收到许可”

这两步共同构成了 SIRD 长消息在发送端的启动握手过程。

## 3.11 发送端收到 `GRANT` 后，是如何处理 `grantOffset` 的

发送端对 `grantOffset` 的处理，同样是一条必须单独讲清楚的主链路。

它的处理分成三步。

### 第一步：在发送端入口识别出 `GRANT`

当发送端收到控制包时，会进入：

- `HomaSendScheduler::CtrlPktRecvdForOutboundMsg`

如果控制包类型是 `GRANT`，代码会执行：

- `targetMsg->HandleGrantOffset(homaHeader);`

也就是说，发送端并不是在调度器里直接展开处理，而是把 `grantOffset` 交给对应的 `HomaOutboundMsg` 去更新自己的发送状态。

### 第二步：`HandleGrantOffset` 把 `grantOffset` 变成发送端本地状态

在 `HomaOutboundMsg::HandleGrantOffset` 中，代码会：

1. 从 Homa 头中读出 `grantOffset`
2. 检查这个 `grantOffset` 是否比当前的 `m_maxGrantedIdx` 更大
3. 如果更大，则更新：
   - `m_maxGrantedIdx = grantOffset`
   - `m_prio = homaHeader.GetPrio()`
4. 如果这是第一笔有效授权，则清除：
   - `m_waitForFirstGrant = false`

因此，对发送端来说，`grantOffset` 最终被转化成了一个本地状态变量：

- `m_maxGrantedIdx`

它表示：

- 当前接收端已经允许本消息发送到哪个最大分片偏移

### 第三步：发送端只允许发送 `<= m_maxGrantedIdx` 的分片

真正消费这个授权的位置，不在 `HandleGrantOffset` 本身，而在后续选包时的：

- `HomaOutboundMsg::GetNextPktOffset`

这个函数会检查待发送队列头部的下一个分片偏移 `nextPktOffset`，只有在下面条件满足时才允许发：

- `nextPktOffset <= m_maxGrantedIdx`

否则即使队列里还有分片，也不能发送。

因此发送端的处理链路可以写成：

```text
收到 GRANT 包
-> CtrlPktRecvdForOutboundMsg()
-> HandleGrantOffset() 更新 m_maxGrantedIdx
-> GetNextPktOffset() 检查待发分片偏移是否 <= m_maxGrantedIdx
-> 只有被授权范围内的分片才允许发送
```

### 第四步：这条链路在 SIRD 中的意义

这说明 SIRD 并不是只在接收端“算预算”就结束了，而是最终通过 `grantOffset` 把接收端的控制意图落实成发送端的硬约束：

- 接收端决定最多授权到哪里
- 发送端严格按这个偏移上界放行

所以从机制上看：

- credit 决定是否继续放大授权窗口
- `grantOffset` 是这个授权窗口在协议报文中的具体表示
- `m_maxGrantedIdx` 是发送端收到后对应的本地执行状态

你的修改：

- 当前代码把接收端的 credit 控制最终落实成 sender 侧的 `m_maxGrantedIdx` 约束
- 只有 `<= m_maxGrantedIdx` 的分片才能真正发送

## 3.12 `HomaL4Protocol::SendDown`

所有最终要发出去的包都会进入这个函数。

它负责：

- 估计链路串行化时间
- 更新 `m_nextTimeTxQueWillBeEmpty`
- 对 `DATA` 包记录发送跟踪信息
- 调用下层 IP 发送回调

它虽然不是直接的 SIRD 控制函数，但它维护了发送端判断是否需要置位 `CSN` 所依赖的队列排空时间，因此也是 SIRD 闭环中的一个基础支撑点。

---

## 4. 单独总结：SIRD 的机制到底是什么

如果把前面的接收路径和发送路径压缩成一句话，那么 SIRD 的机制可以概括为：

接收端根据“网络拥塞”和“发送端主机拥塞”这两类反馈，为每个发送方维护一个按分组计数的信用预算，并通过显式 `GRANT` 逐个发放信用，从而控制长消息能够继续注入多少数据。

## 4.1 SIRD 比原始 Homa 多做了什么

原始 Homa 主要强调：

- 接收端驱动
- 用 `GRANT` 控制后续数据发送
- 根据消息大小和优先级进行调度

SIRD 在此基础上又多加了一层：

- 不仅决定“下一步给谁发 `GRANT`”
- 还决定“这个发送方当前最多还能持有多少在途信用”

因此，SIRD 的控制粒度比原始 Homa 更细。

你的修改：

- 你在代码里把这种更细的控制粒度具体实现成了按 packet token 发放和回收 credit
- 而不是只保留抽象的优先级或粗粒度窗口

## 4.2 SIRD 的两个反馈环

### 网络侧反馈环

输入信号是 `CE`。

基本逻辑是：

- 如果最近收到的包带 `CE`，说明网络路径出现拥塞
- 则对该发送方的网络侧预算做乘法减小
- 如果没有看到 `CE`，则做加法增加

对应变量主要有：

- `m_sirdSenderBudgetNetPkts`
- `GetSirdEcnMdFactor`
- `GetSirdEcnAiStep`

### 主机侧反馈环

输入信号是发送端在 Homa 头中携带回来的 `CSN`。

基本逻辑是：

- 如果发送端告诉接收端自己本地发送队列已经积压
- 则对该发送方的主机侧预算做乘法减小
- 如果没有看到 `CSN`，则做加法增加

对应变量主要有：

- `m_sirdSenderBudgetHostPkts`
- `GetSirdSenderMdFactor`
- `GetSirdSenderAiStep`

## 4.3 为什么要取两个预算的最小值

因为这两个反馈源反映的是不同维度的压力：

- `CE` 反映网络内部拥塞
- `CSN` 反映发送端主机本地拥塞

如果只看其中一个，就可能忽略另一侧的压力。因此代码中采用：

- `effective budget = min(netBudget, hostBudget)`

这意味着接收端给某个发送方的授权，必须同时满足网络和主机两方面都不紧张。

你的修改：

- 当前代码显式使用 `min(netBudget, hostBudget)` 作为有效预算
- 这是你这份实现中双反馈融合的核心决策规则

## 4.4 什么叫“已借出但未归还的信用”

当接收端发出一个 `GRANT` 时，只表示：

- 允许发送端再多发送一个数据分片

但这个分片此时还没有真正到达，所以接收端会先把它记成：

- 已借出
- 但尚未归还

对应变量主要有：

- `m_sirdSenderCreditsInUsePkts`
- `m_sirdGlobalCreditsInUsePkts`

后续当真实数据分片到达时，再把这一个信用回收。

这样做的意义是：预算不再是抽象的数值，而是和实际允许发送的数据分片数量严格对应。

你的修改：

- 当前代码通过 `m_sirdSenderCreditsInUsePkts` 和 `m_sirdGlobalCreditsInUsePkts` 显式维护“已借出但未归还的 credit”
- 这让整个 credit 机制从概念层面落实成了可执行的状态机

## 4.5 为什么长消息要先发零负载请求包

因为在 SIRD 下，长消息一开始不能像原始 Homa 那样直接发送一串真实数据。

但如果接收端什么都没收到，也就无法：

- 建立这条消息的接收状态
- 计算这条消息后续该如何授权
- 发出第一个 `GRANT`

因此必须先发一个零负载 `DATA` 包，作用是：

- 告诉接收端“有一条长消息到来了”
- 让接收端创建 `HomaInboundMsg`
- 然后由接收端正式开始发放显式信用

这就是 SIRD 长消息启动机制的关键。

## 4.6 SIRD 的完整闭环

SIRD 的闭环过程可以概括为以下 8 步：

1. 发送端创建长消息，但不立即发送真实数据。
2. 发送端先发一个零负载请求包。
3. 接收端收到后建立入站消息状态。
4. 接收端根据 `CE` 和 `CSN` 更新该发送方预算。
5. 接收端按预算发放 1 个分组级 `GRANT`。
6. 发送端收到 `GRANT` 后放行 1 个真实数据分片。
7. 接收端收到该真实分片后回收 1 个未归还信用。
8. 重复上述过程，直到整条消息完成。

这就是一个典型的“接收端显式信用控制加双反馈闭环”。

## 4.7 原文中的 `CSN` 判定，与当前代码实现是否一致

这一点在论文中需要单独说明，因为当前代码与原文并不是严格一一对应的实现。

原文中的表述是：

- 如果某个发送方当前累计持有的总信用大于等于阈值 `SThr`
- 就给该发送方发出的 scheduled `DATA` 包打上 `csn = 1`
- 否则 `csn = 0`

也就是说，原文中的 `CSN` 判定依据是：

- 发送方当前持有的信用数量

但你当前代码中的实现不是直接判断“持有多少信用”，而是判断：

- 当前发送队列的预计排空时间是否大于等于 `SirdSenderCsnThreshold`

对应实现见 `HomaSendScheduler::GetNextPktOfMsg`，其判断条件为：

- `m_homa->IsSirdEnabled()`
- `m_homa->GetTimeToDrainTxQueue() >= m_homa->GetSirdSenderCsnThreshold()`

因此，当前代码中的 `CSN` 判定依据是：

- 本地发送积压是否足够大

而不是：

- 当前持有的总信用是否达到固定阈值

所以从严格意义上说，这两者并不等价。

它们之间的区别可以概括为：

### 原文

- 判定量是“信用数量”
- 属于显式信用阈值判定
- 发送方需要明确知道自己当前累计持有多少信用

### 当前代码

- 判定量是“预计排空时间”
- 属于本地发送积压判定
- 发送方不直接按信用数置位 `CSN`

当前代码之所以还能在效果上与原文存在一定关联，是因为在一些较强假设下，二者可能近似相关。例如：

- 一个信用近似对应一个数据分片
- 发送方一拿到信用就很快转化成待发送分片
- 分片大小相对稳定
- 链路速率固定
- 发送方向上没有其他协议流量干扰

在这些假设下，可以粗略理解为：

- 持有的信用越多
- 本地待发数据越多
- 队列排空时间通常也越长

因此“信用数量大”与“排空时间长”在趋势上可能一致，但它们仍然不是同一个变量。

更准确地说，当前实现是：

- 用发送端本地发送积压作为 `CSN` 的代理量

而不是：

- 直接复现原文按累计信用数量阈值置位 `CSN` 的机制

如果论文中需要严谨表述，建议写成：

“本文代码并未在发送端直接以累计持有信用是否超过阈值 `SThr` 来置位 `CSN`，而是采用发送队列排空时间作为发送端主机侧拥塞的代理量。当预计排空时间大于等于 `SirdSenderCsnThreshold` 时，在发送的数据包头中置位 `CSN`。因此，该实现与原文的 `CSN` 判定机制并非严格等价，而是一种基于本地发送积压的近似实现。”

---

## 5. 除了 SIRD 本身之外，还应关注的其他重要函数

虽然论文重点是 SIRD，但下面这些函数仍然非常重要，因为它们构成了整个 Homa/SIRD 系统的基础支撑。

## 5.1 `HomaL4Protocol::GetTypeId`

这个函数负责把协议参数注册到 ns-3。

重要性在于：

- 所有 SIRD 参数都是在这里暴露出来的
- 例如 `SirdEnabled`、`SirdCreditBudgetPkts`、`SirdSenderCsnThreshold`
- 实验脚本能够调整这些参数，依赖的就是这里

如果论文需要说明“实验参数如何配置”，这个函数必须提到。

## 5.2 `HomaL4Protocol::TraceSirdGrantDecision`

这个函数本身只是一层跟踪封装，但它非常适合论文中说明“如何观测接收端决策过程”。

它会记录：

- 发送方地址
- `txMsgId`
- `grantOffset`
- 当前发送方预算
- `ecnEwma`
- `senderCsn`

如果后续要画“接收端授权行为”相关图表，这个跟踪点很重要。

## 5.3 `HomaL4Protocol::GetTimeToDrainTxQueue`

这个函数直接影响发送端什么时候置位 `CSN`。

因此它虽然只是一个辅助函数，但实际上参与了 SIRD 中主机侧反馈信号的产生。

## 5.4 `HomaInboundMsg::GenerateGrantOrAck`

这个函数负责真正构造 `GRANT` 或 `ACK` 包。

接收端所有的控制决策，最后都必须落实到这个函数构造出来的控制包上。

## 5.5 `HomaInboundMsg::GenerateResends`

这个函数体现的是可靠性恢复，而不是拥塞控制。

它不是 SIRD 的核心，但没有它，长消息在丢包场景下就很难顺利完成。

## 5.6 `PfifoHomaQueueDisc::DoEnqueue`

这个函数把 DS 字段映射到优先级队列。

其重要性在于：

- 接收端虽然可以在 `GRANT` 中授予优先级
- 但真正让高优先级包先走，依赖的是底层队列识别这个优先级

因此它是“控制决策落地到排队行为”的桥梁。

## 5.7 `PfifoHomaQueueDisc::DoDequeue`

这个函数保证高优先级队列先出队。

只有经过这里，接收端授予的优先级才会真正变成网络中的实际发送先后顺序。

---

## 6. 一段可以直接放进论文正文的总结

这份实现中，SIRD 并不是简单地在 Homa 上增加几个参数，而是系统性地修改了接收端授权逻辑和发送端启动逻辑。接收端通过 `HomaRecvScheduler` 为每个发送方维护双反馈预算，其中网络侧预算由 ECN/CE 调整，主机侧预算由发送端携带的 `CSN` 调整，实际可发预算取两者的较小值。随后，接收端通过分组级显式 `GRANT` 发放信用，并在真实数据分片到达后回收未归还信用，从而形成一个精确的闭环控制。发送端则通过零负载请求包启动长消息传输，并在本地发送队列积压时主动上报 `CSN`。最终，这套机制与 `PfifoHomaQueueDisc` 提供的优先级队列结合，使接收端的控制决策能够真正落实到网络中的转发顺序。

---

## 7. 后续还可以继续补什么

如果后面要把这份文档继续扩展成论文附录，建议再补三部分：

1. 每个关键函数对应的源码行号
2. 一张“接收流程图”和一张“发送流程图”
3. 一张“论文变量名”和“代码成员变量名”的对应表
