# 代码 3-1: Homa/SIRD 发送与接收逻辑

下面按论文代码框的风格重写，保留关键分支，省略不必要的 helper 细节。

注：`BUSY` 属于原始 Homa，不写进这份 SIRD 主伪代码。

---

## 3.2 算法实现

代码 3-1 到代码 3-3 分别给出了发送端、接收端以及 SIRD 授权节拍的核心实现逻辑。前两段描述的是消息在发送端和接收端的状态推进过程，后一段描述的是接收端如何按照预算逐步放行 `GRANT`。相比完整源码，这里的伪码去掉了很多工程细节，只保留协议状态变化中最关键的条件判断和动作。

## 代码 3-1: 发送端上的 Homa/SIRD 逻辑

代码 3-1 描述的是发送端上的消息生命周期：新消息如何进入活跃表、长消息如何先等待首个授权、收到 `GRANT/RESEND/ACK` 时如何推进状态，以及在 `TX_OPPORTUNITY` 到来时如何真正选择并发送分片。这里的核心思想是，发送端并不是“有包就发”，而是要等授权窗口、credit 成熟度和本地发送机会同时满足之后，才把数据真正下发。

```text
1:  SenderMain(event, state):
2:    if event.type == NEW_MESSAGE then
3:      // 先分片，再建立消息状态
4:      msg <- Packetize(event.message)
5:      msg.txMsgId <- AllocateTxMsgId()
6:      if state.sirdEnabled && msg.IsLong(unschedThresholdPkts) then
7:        // 长消息先不发真实 DATA
8:        msg.waitForFirstGrant <- true
9:        msg.maxGrantedOffset <- 0
10:     else
11:       // 普通消息直接使用初始 unscheduled 窗口
12:       msg.maxGrantedOffset <- InitialUnscheduledWindow(BDP)
13:     Insert(activeMsgs, msg)
14:     // 若发送器空闲，立刻安排下一次发包
15:     if txEventIdle then ScheduleTxOpportunity()
16:
17:   else if event.type == GRANT then
18:     msg <- activeMsgs[event.txMsgId]
19:     // 收到授权，推进可发送上界
20:     msg.HandleGrantOffset(event.homaHeader)
21:     if txEventIdle then ScheduleTxOpportunity()
22:
23:   else if event.type == RESEND then
24:     msg <- activeMsgs[event.txMsgId]
25:     msg.HandleGrantOffset(event.homaHeader)
26:     // 把丢失分片重新插回发送队列
27:     msg.HandleResend(event.homaHeader)
28:     if txEventIdle then ScheduleTxOpportunity()
29:
30:   else if event.type == ACK then
31:     msg <- activeMsgs[event.txMsgId]
32:     msg.HandleAck(event.homaHeader)
33:     // 消息结束，回收 txMsgId
34:     RecycleTxMsgId(msg.txMsgId)
35:     // 从活跃表删除该消息
36:     Erase(activeMsgs, event.txMsgId)
37:
38:   else if event.type == TX_OPPORTUNITY then
39:     // 先等本地队列排空
40:     if LocalTxQueueNotEmpty() then RescheduleAfterQueueDrain(); return
41:     // 选最“急”的消息
42:     chosen <- SelectSmallestSendable(activeMsgs)
43:     if chosen == null then if DelayedSirdCreditExists() then RescheduleAtEarliestCreditMaturity(); return
44:     if chosen.waitForFirstGrant && !chosen.initialRequestSent then
45:       // 零负载 DATA，请求首个显式授权
46:       SendDown(GenerateInitialCreditRequest(chosen))
47:       chosen.initialRequestSent <- true
48:       RescheduleAfterQueueDrain()
49:       return
50:     if !chosen.TryGetNextPktOffset(pktOffset) then if DelayedSirdCreditExists() then RescheduleAtEarliestCreditMaturity(); return
51:     // 取出当前最该发送的分片
52:     p <- chosen.RemoveNextPktFromTxQ(pktOffset)
53:     // 消耗一个已成熟 credit
54:     if state.sirdEnabled then chosen.ConsumeSirdCredit()
55:     AttachDataHeader(p, chosen.txMsgId, pktOffset, chosen.maxGrantedOffset, chosen.prio)
56:     SendDown(p, chosen)
57:     RescheduleAfterQueueDrain()
```

第 2-15 行描述的是新消息到达后的初始化过程。对于长消息，如果启用了 SIRD，发送端会先把消息放入 `waitForFirstGrant` 状态，不直接发送真实数据，而是等接收端返回第一个显式 `GRANT`。对于普通消息，则直接按初始 unscheduled 窗口设置可发送上界。

第 17-28 行对应控制包到达时的状态推进。`GRANT` 负责推进授权窗口，`RESEND` 除了更新授权窗口外，还会把缺失分片重新放回发送队列，`ACK` 则意味着消息生命周期结束，需要回收 `txMsgId` 并从活跃表删除该消息。

第 38-57 行是发送端真正发包的时刻。只有当本地发送队列已经排空、选出了当前最合适的消息，并且该消息要么已经拿到首个授权、要么已经具备可发送分片时，发送端才会构造 `DATA` 并下发；如果启用了 SIRD，还必须再检查对应的 `credit` 是否已经成熟。

---

## 代码 3-2: 接收端上的 Homa/SIRD 逻辑

代码 3-2 描述的是接收端处理 `DATA` 的主路径：到包后如何更新反馈信号、回收 credit、创建首包状态、完成重组与确认，并在消息未完成时重新安排后续调度。和发送端相比，接收端更像是控制环路的“决策中心”，因为它既要判断是否还有可发的 `GRANT`，也要维护预算和反馈状态。

```text
1:  ReceiverMain(event, state):
2:    if event.type == DATA then
3:      // 读取核心网拥塞反馈和发送端反馈位
4:      UpdateFeedbackState(event)
5:      // DATA 到达时，归还一个已占用的 credit
6:      ReclaimSirdCredits(event)
7:      // 到达一个 epoch 边界时更新预算
8:      UpdateBudgetsIfEpochEnds(event.sender)
9:      // 按四元组和 txMsgId 找到消息
10:     msg <- FindMsg(event.fiveTuple, event.txMsgId)
11:     // 首包到达，创建状态并挂超时
12:     if msg == null then msg <- InsertFirstInboundMsg(event); ArmRtxTimeout(msg)
13:     // 登记该分片已收到
14:     msg.ReceiveDataPacket(event.packet, event.pktOffset)
15:     if msg.IsFullyReceived() then
16:       DeliverToApplication(msg.GetReassembledMsg(), msg)
17:       SendAck(msg)
18:       RemoveMsg(msg)
19:       return
20:     // 重新插回活跃队列等待后续调度
21:     RescheduleInboundMsg(msg)
22:     // 该 sender 已能继续发 DATA
23:     MarkSenderNotBusy(event.sender)
24:     // 重新评估是否该发下一轮 GRANT
25:     EnsureCreditTickScheduled(true)
```

第 2-8 行表示接收端在处理数据包时，先更新 SIRD 相关的反馈状态，再回收一个已经占用的 credit，并在 epoch 边界时重新计算发送端预算。这部分对应的是“数据到达以后，控制环路先修正自身记账”的过程。

第 9-19 行描述消息首次到达和消息完成的两个边界情况。若这是首包，则接收端要先创建消息状态并挂上重传定时器；若所有分片都已收到，则直接重组消息、交付上层并发送 `ACK`，然后清理状态。

第 20-25 行对应消息尚未完成时的处理。接收端会把消息重新插回活跃队列，更新 sender 的忙闲状态，并重新评估是否需要启动下一轮 `CreditTick`，以决定是否可以继续发放 `GRANT`。

---

## 代码 3-3: SIRD 的授权节拍

代码 3-3 描述的是 SIRD 的授权节拍控制。接收端不是看到数据就立即连续发 `GRANT`，而是通过离散的 `CreditTick` 在满足预算和窗口条件时逐步放行授权。这样做的目的，是把“消息级授权”改造成“预算受限的节拍式授权”。

```text
1:  CreditTick(state):
2:    // 没有授权空间就不扫描
3:    if !HasGrantOpportunity() then return false
4:    issued <- false
5:    for each msg in activeMsgs do
6:      // 本轮 overcommit 已用完
7:      if overcommitDue == 0 then break
8:      if msg.IsFullyGranted() || !CanIssueGrant(msg) then continue
9:      grant <- msg.GenerateGrantOrAck(nextGrantPrio, GRANT)
10:     SendDown(grant, msg.dst, msg.src)
11:     // 记账：全局 credit 和 per-sender credit 同步更新
12:     AccountIssuedGrant(msg)
13:     overcommitDue <- overcommitDue - 1
14:     // 下一个 GRANT 使用更低优先级
15:     nextGrantPrio <- nextGrantPrio + 1
16:     issued <- true
17:   return issued
```

第 2-3 行先判断当前是否真的存在授权机会；如果全局预算已经耗尽，或者没有任何消息满足发放条件，那么这一轮 `CreditTick` 就直接结束。

第 4-16 行扫描活跃消息并尝试发放授权。每当某条消息仍然可继续授权、并且当前轮次的 `overcommit` 还没有用完时，接收端就生成一个新的 `GRANT`，将其发送给对应 sender，同时同步记账全局 credit 和 per-sender credit。这样，授权不是连续泄洪式地发出，而是被明确地限制在每一轮可用预算之内。

---

## 一句话总结

> 发送端负责“何时把已授权分片真正发出去”，接收端负责“何时根据预算再发下一笔授权”，SIRD 额外引入的 credit maturity 与 credit tick 则把这两个过程串成了闭环。
