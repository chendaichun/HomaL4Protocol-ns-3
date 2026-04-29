# Homa/SIRD 发送端与接收端伪代码

本文档给出一份适合论文初稿的 C++ 风格伪代码，描述当前 `HomaL4Protocol` 实现里的发送端和接收端主流程。

写法原则：

- 保留核心控制逻辑；
- 使用当前代码中的主要函数名；
- 省略 ns-3 细节、trace 细节和边角异常处理；
- 重点突出消息状态、GRANT、ACK、SIRD credit 控制和 SRPT/SRR 调度。

## 1. 发送端伪代码

### 1.1 应用层发送入口

```cpp
void HomaL4Protocol::Send(Message msg,
                          Address src,
                          Address dst,
                          Port sport,
                          Port dport)
{
    HomaOutboundMsg* outMsg = new HomaOutboundMsg(msg, src, dst, sport, dport, this);

    int txMsgId = m_sendScheduler->ScheduleNewMsg(outMsg);

    if (txMsgId >= 0)
    {
        TraceMsgBegin(msg, src, dst, sport, dport, txMsgId);
    }
}
```

作用：

- 创建发送侧消息状态对象；
- 把完整 message 转交发送调度器；
- 分配 `txMsgId`；
- 记录消息开始发送事件。

---

### 1.2 发送侧消息初始化

```cpp
HomaOutboundMsg::HomaOutboundMsg(Message msg, ...)
{
    m_msgSizeBytes = msg.size();
    m_remainingBytes = m_msgSizeBytes;

    PacketizeMessageIntoMtuSizedPackets(msg, m_packets, m_pktTxQ);

    if (SirdEnabled())
    {
        if (MessagePkts() > UnschedThresholdPkts())
        {
            // 长消息：必须等待显式 GRANT
            m_maxGrantedIdx = 0;
            m_waitForFirstGrant = true;
        }
        else
        {
            // 短消息：可以直接走 unscheduled path
            m_maxGrantedIdx = min(BdpPkts() - 1, MessagePkts() - 1);
        }
    }
    else
    {
        m_maxGrantedIdx = min(BdpPkts() - 1, MessagePkts() - 1);
    }
}
```

作用：

- 对 message 进行分片；
- 初始化发送窗口；
- 判断消息是 unscheduled 还是 scheduled；
- 决定是否要等待第一个 GRANT。

主要状态量：

- `m_msgSizeBytes`
- `m_remainingBytes`
- `m_packets` / `m_pktTxQ`
- `m_maxGrantedIdx`
- `m_waitForFirstGrant`

---

### 1.3 注册到发送调度器

```cpp
int HomaSendScheduler::ScheduleNewMsg(HomaOutboundMsg* outMsg)
{
    if (m_txMsgIdFreeList.empty())
    {
        return -1;
    }

    uint16_t txMsgId = m_txMsgIdFreeList.front();
    m_txMsgIdFreeList.pop_front();

    m_outboundMsgs[txMsgId] = outMsg;

    if (m_txEvent.IsExpired())
    {
        m_txEvent = Schedule(GetTimeToDrainTxQueue(),
                             &HomaSendScheduler::TxDataPacket,
                             this);
    }

    return txMsgId;
}
```

作用：

- 分配一个唯一的 `txMsgId`；
- 将消息插入发送端活跃消息表；
- 如果当前发送器空闲，则启动发送循环。

主要状态量：

- `m_txMsgIdFreeList`
- `m_outboundMsgs`
- `m_txEvent`

---

### 1.4 发送调度主循环

```cpp
void HomaSendScheduler::TxDataPacket()
{
    if (GetTimeToDrainTxQueue() != 0)
    {
        m_txEvent = Schedule(GetTimeToDrainTxQueue(),
                             &HomaSendScheduler::TxDataPacket,
                             this);
        return;
    }

    uint16_t nextTxMsgId;
    if (!GetNextMsgId(nextTxMsgId))
    {
        if (ExistsDelayedCredit())
        {
            m_txEvent = Schedule(MinDelayedCreditTime(),
                                 &HomaSendScheduler::TxDataPacket,
                                 this);
        }
        return;
    }

    Packet p;
    bool ok = GetNextPktOfMsg(nextTxMsgId, p);
    if (!ok)
    {
        return;
    }

    HomaOutboundMsg* msg = m_outboundMsgs[nextTxMsgId];
    m_homa->SendDown(p, msg->GetSrcAddress(), msg->GetDstAddress(), msg->GetRoute());

    m_txEvent = Schedule(GetTimeToDrainTxQueue(),
                         &HomaSendScheduler::TxDataPacket,
                         this);
}
```

作用：

- 在链路空闲时选择下一包发送；
- 若没有可发送数据，则等待未来 credit 生效；
- 这是发送端真正的“包级发送驱动器”。

主要状态量：

- `m_txEvent`
- `m_outboundMsgs`

---

### 1.5 选择下一条消息

```cpp
bool HomaSendScheduler::GetNextMsgId(uint16_t& txMsgId)
{
    uint32_t minRemainingBytes = INF;
    bool found = false;

    for (auto& [id, msg] : m_outboundMsgs)
    {
        if (msg->IsExpired())
        {
            ClearStateForMsg(id);
            continue;
        }

        if (!msg->NeedsInitialCreditRequest() &&
            !msg->HasSendablePacket())
        {
            continue;
        }

        if (msg->GetRemainingBytes() < minRemainingBytes)
        {
            minRemainingBytes = msg->GetRemainingBytes();
            txMsgId = id;
            found = true;
        }
    }

    return found;
}
```

作用：

- 在活跃发送消息中选出“当前最该发”的一条；
- 当前实现本质上更接近按 `remainingBytes` 选最短；
- 同时清理已过期消息。

---

### 1.6 生成一个要发送的包

```cpp
bool HomaSendScheduler::GetNextPktOfMsg(uint16_t txMsgId, Packet& p)
{
    HomaOutboundMsg* msg = m_outboundMsgs[txMsgId];

    if (msg->NeedsInitialCreditRequest())
    {
        p = msg->GenerateInitialCreditRequest(txMsgId);
        msg->MarkInitialCreditRequestSent();
        return true;
    }

    uint16_t pktOffset;
    if (!msg->GetNextPktOffset(pktOffset))
    {
        return false;
    }

    p = msg->RemoveNextPktFromTxQ(pktOffset);

    if (SirdEnabled())
    {
        msg->ConsumeSirdCredit();
        TraceSenderCreditState(...);
    }

    HomaHeader h;
    h.flags = DATA;
    h.txMsgId = txMsgId;
    h.pktOffset = pktOffset;
    h.grantOffset = msg->GetMaxGrantedIdx();
    h.prio = msg->GetPrio(pktOffset);

    if (SirdEnabled() && AccumulatedSenderCreditPkts() > SenderCsnThreshold())
    {
        h.feedbackFlags = FEEDBACK_CSN;
    }

    AttachPriorityAndEcnTag(p, h.prio, ECT0);
    p.AddHeader(h);
    return true;
}
```

作用：

- 若是长消息且还没拿到 grant，则先发送空 payload 的 credit request；
- 否则弹出下一个可发送分片；
- 若启用 SIRD，则消耗一份 sender 已持有 credit；
- 根据当前 sender credit 是否过高决定是否设置 `CSN`。

主要状态量：

- `m_pktTxQ`
- `m_sirdCreditAvailableTimes`
- `sender-held credit`

---

### 1.7 网络层下发

```cpp
void HomaL4Protocol::SendDown(Packet p, Address saddr, Address daddr, Route route)
{
    Time txTime = SerializeTime(p);

    if (Now() <= m_nextTimeTxQueWillBeEmpty)
        m_nextTimeTxQueWillBeEmpty += txTime;
    else
        m_nextTimeTxQueWillBeEmpty = Now() + txTime;

    DownTarget(p, saddr, daddr, route);
}
```

作用：

- 将包交给 IPv4；
- 更新“发送队列何时清空”的时间；
- 为发送调度器提供链路忙闲判断依据。

主要状态量：

- `m_nextTimeTxQueWillBeEmpty`

---

### 1.8 发送端接收控制包

```cpp
RxStatus HomaL4Protocol::Receive(Packet p, Ipv4Header ip, ...)
{
    HomaHeader h = PeekHomaHeader(p);

    if (h.flags contains DATA or BUSY)
    {
        m_recvScheduler->ReceivePacket(...);
    }
    else if (h.flags contains GRANT or RESEND or ACK)
    {
        m_sendScheduler->CtrlPktRecvdForOutboundMsg(ip, h);
    }

    return RX_OK;
}
```

作用：

- 区分 DATA 路径和控制路径；
- 对发送端来说，最重要的是把 `GRANT/RESEND/ACK` 转给发送调度器。

---

### 1.9 处理 GRANT / RESEND / ACK

```cpp
void HomaSendScheduler::CtrlPktRecvdForOutboundMsg(Ipv4Header ip, HomaHeader h)
{
    uint16_t txMsgId = h.txMsgId;
    HomaOutboundMsg* msg = m_outboundMsgs[txMsgId];

    if (h.flags contains GRANT)
    {
        msg->HandleGrantOffset(h);
    }
    else if (h.flags contains RESEND)
    {
        msg->HandleGrantOffset(h);
        msg->HandleResend(h);

        if (NotHighestPriorityMsg(txMsgId))
        {
            SendBusy(msg, txMsgId);
        }
    }
    else if (h.flags contains ACK)
    {
        msg->HandleAck(h);
        ClearStateForMsg(txMsgId);
    }

    if (m_txEvent.IsExpired())
    {
        m_txEvent = Schedule(GetTimeToDrainTxQueue(),
                             &HomaSendScheduler::TxDataPacket,
                             this);
    }
}
```

作用：

- `GRANT`：扩大发送窗口；
- `RESEND`：将缺失 packet 重新排入发送队列；
- `ACK`：释放消息状态；
- 控制包到来后，可能允许新的 DATA 继续发，因此要重启发送循环。

---

### 1.10 处理 GRANT

```cpp
void HomaOutboundMsg::HandleGrantOffset(HomaHeader h)
{
    uint16_t grantOffset = h.grantOffset;

    if (m_waitForFirstGrant && grantOffset >= m_maxGrantedIdx)
    {
        m_waitForFirstGrant = false;
    }

    if (grantOffset > m_maxGrantedIdx)
    {
        uint16_t oldMaxGrantedIdx = m_maxGrantedIdx;
        m_maxGrantedIdx = grantOffset;
        m_prio = h.prio;
        m_prioSetByReceiver = true;

        m_remainingBytes =
            m_msgSizeBytes - (m_maxGrantedIdx + 1 - BdpPkts()) * m_maxPayloadSize;

        uint16_t newCreditPkts =
            (old message waited for first grant) ? (grantOffset + 1)
                                                 : (grantOffset - oldMaxGrantedIdx);

        AddSirdCreditAvailability(newCreditPkts);
        TraceSenderCreditState(...);
    }
}
```

作用：

- 更新授权边界 `m_maxGrantedIdx`；
- 设置接收端指定的优先级；
- 将新授予的 credit 转换成 sender 可在未来发出的 packet 配额；
- 更新剩余字节估计。

主要状态量：

- `m_maxGrantedIdx`
- `m_prio`
- `m_prioSetByReceiver`
- `m_waitForFirstGrant`
- `m_remainingBytes`
- `m_sirdCreditAvailableTimes`

---

### 1.11 清理发送端状态

```cpp
void HomaSendScheduler::ClearStateForMsg(uint16_t txMsgId)
{
    HomaOutboundMsg* msg = m_outboundMsgs[txMsgId];

    Cancel(msg->GetRtxEvent());
    m_outboundMsgs.erase(txMsgId);
    m_txMsgIdFreeList.push_back(txMsgId);
}
```

作用：

- 删除完成或失效的发送消息；
- 回收 `txMsgId`；
- 取消对应超时事件。

## 2. 接收端伪代码

### 2.1 接收入口

```cpp
RxStatus HomaL4Protocol::Receive(Packet p, Ipv4Header ip, ...)
{
    HomaHeader h = PeekHomaHeader(p);

    if (h.flags contains DATA or BUSY)
    {
        m_recvScheduler->ReceivePacket(p, ip, h, iface);
    }
    else if (h.flags contains GRANT or RESEND or ACK)
    {
        m_sendScheduler->CtrlPktRecvdForOutboundMsg(ip, h);
    }

    return RX_OK;
}
```

作用：

- 对接收端来说，DATA/BUSY 会进入接收调度器；
- GRANT/ACK 等控制包不在这里处理，而是回到发送调度器。

---

### 2.2 接收端主入口

```cpp
void HomaRecvScheduler::ReceivePacket(Packet p, Ipv4Header ip, HomaHeader h, Interface iface)
{
    if (h.flags contains DATA)
    {
        if (SirdEnabled())
        {
            UpdateSenderBudgetAndCreditState(ip, h, p);
        }

        ReceiveDataPacket(p, ip, h, iface);
        m_busySenders.erase(ip.src);
    }
    else if (h.flags contains BUSY)
    {
        m_busySenders.insert(ip.src);
    }

    TracePerPacketState(...);
    EnsureCreditTickScheduled(true);
}
```

作用：

- 这是接收端最核心的入口；
- DATA 到来时，一边做正常的消息接收，一边更新 SIRD sender/global credit 状态；
- BUSY 到来时，表示该 sender 当前不宜继续 grant。

主要状态量：

- `m_busySenders`
- `m_sirdSenderBudgetNetPkts`
- `m_sirdSenderBudgetHostPkts`
- `m_sirdSenderCreditsInUsePkts`
- `m_sirdGlobalCreditsInUsePkts`
- `m_sirdSenderCsnState`
- `m_sirdSenderCeState`
- `m_sirdCeRatioEwma`

---

### 2.3 SIRD 状态更新

```cpp
void UpdateSenderBudgetAndCreditState(Ipv4Header ip, HomaHeader h, Packet p)
{
    sender = ip.src;

    InitializePerSenderStateIfNeeded(sender);

    bool senderCsn = h.feedbackFlags contains FEEDBACK_CSN;
    bool ceMarked = (ip.ecn == CE);

    UpdateObservedCounters(sender, senderCsn, ceMarked);

    if (p.size > 0)
    {
        // 一个 DATA 到达，说明之前借出去的一份 credit 被消费了
        ReclaimOneOutstandingCredit(sender);
    }

    if (EpochFinished(sender))
    {
        UpdateNetBudgetByCe(sender);
        UpdateHostBudgetByCsn(sender);
        ResetEpochCounters(sender);
    }
}
```

作用：

- 统计每个 sender 的 CE 和 CSN 信号；
- DATA 到达时回收已经借出的 scheduled credit；
- 周期性根据 CE/CSN 更新 sender budget。

---

### 2.4 归并 DATA 到消息对象

```cpp
void HomaRecvScheduler::ReceiveDataPacket(Packet p, Ipv4Header ip, HomaHeader h, Interface iface)
{
    HomaInboundMsg* inboundMsg;
    int msgIdx = FindInboundMsg(ip, h);

    if (msgIdx >= 0)
    {
        inboundMsg = m_inboundMsgs[msgIdx];
        inboundMsg->ReceiveDataPacket(p, h.pktOffset);
    }
    else
    {
        inboundMsg = new HomaInboundMsg(p, ip, h, iface, ...);
        inboundMsg->SetRtxEvent(Schedule(InboundRtxTimeout(), ExpireRtxTimeout, ...));
    }

    if (inboundMsg->IsFullyReceived())
    {
        ForwardUp(inboundMsg, msgIdx);
    }
    else
    {
        ScheduleMsgAtIdx(inboundMsg, msgIdx);
    }
}
```

作用：

- 找到或创建对应的接收侧 message；
- 把 DATA 归并进去；
- 若消息完成则上交应用，否则重新进入 active list。

---

### 2.5 接收侧消息初始化

```cpp
HomaInboundMsg::HomaInboundMsg(Packet firstPkt, Ipv4Header ip, HomaHeader h, ...)
{
    m_msgSizeBytes = h.msgSize;
    m_msgSizePkts = ComputeMsgPkts(...);
    m_remainingBytes = m_msgSizeBytes;

    InitPacketBufferAndBitmap();

    if (firstPkt.size > 0)
    {
        MarkPacketReceived(h.pktOffset, firstPkt);
        m_remainingBytes -= firstPkt.size;
    }

    bool longSirdMsg = SirdEnabled() && (m_msgSizePkts > UnschedThresholdPkts());
    if (longSirdMsg)
    {
        m_maxGrantedIdx = 0;
        m_maxGrantableIdx = 0;
        m_hasGrantedData = false;
        m_creditDrivenGrantWindow = true;
    }
    else
    {
        m_maxGrantedIdx = min(rttPackets - 1, m_msgSizePkts - 1);
        m_maxGrantableIdx = m_maxGrantedIdx + 1;
        m_hasGrantedData = true;
    }

    m_lastRtxGrntIdx = m_maxGrantableIdx;
}
```

作用：

- 初始化接收侧消息状态；
- 标记首个 packet 已收到；
- 决定该消息是否需要 credit-driven grant window。

主要状态量：

- `m_receivedPackets`
- `m_remainingBytes`
- `m_maxGrantedIdx`
- `m_maxGrantableIdx`
- `m_hasGrantedData`
- `m_creditDrivenGrantWindow`

---

### 2.6 更新接收进度

```cpp
void HomaInboundMsg::ReceiveDataPacket(Packet p, uint16_t pktOffset)
{
    if (p.size == 0)
    {
        // zero-payload DATA = initial credit request
        return;
    }

    if (!m_receivedPackets[pktOffset])
    {
        StorePacket(pktOffset, p);
        m_receivedPackets[pktOffset] = true;
        m_remainingBytes -= p.size;

        if (!m_creditDrivenGrantWindow && m_maxGrantableIdx < m_msgSizePkts - 1)
        {
            m_maxGrantableIdx++;
        }
    }
}
```

作用：

- 标记一个 packet 已到达；
- 减少剩余字节；
- 在普通 Homa 模式下，随着 DATA 到达推进可继续授权的窗口。

---

### 2.7 活跃消息排序

```cpp
void HomaRecvScheduler::ScheduleMsgAtIdx(HomaInboundMsg* inboundMsg, int msgIdx)
{
    if (msgIdx >= 0)
    {
        RemoveOldPosition(msgIdx);
    }

    if (UseSrrScheduling())
    {
        m_inboundMsgs.push_back(inboundMsg);
        return;
    }

    InsertByRemainingBytesAscending(inboundMsg);
}
```

作用：

- `UseSrrScheduling=false`：按剩余字节排序，偏 SRPT；
- `UseSrrScheduling=true`：按 FIFO/轮转思路保留顺序，偏 SRR。

主要状态量：

- `m_inboundMsgs`

---

### 2.8 安排 credit tick

```cpp
void HomaRecvScheduler::EnsureCreditTickScheduled(bool immediate)
{
    if (m_creditTickEvent.IsRunning())
        return;

    if (!HasGrantOpportunity())
        return;

    Time delay = immediate ? 0 : GetCreditTickInterval();
    m_creditTickEvent = Schedule(delay, &HomaRecvScheduler::CreditTick, this);
}
```

作用：

- 如果当前还有 sender 值得 grant，就启动或维持 receiver-driven credit 时钟。

主要状态量：

- `m_creditTickEvent`

---

### 2.9 credit tick 主循环

```cpp
void HomaRecvScheduler::CreditTick()
{
    m_creditTickEvent = EventId();

    bool issued = SendAppropriateGrants();
    if (!issued)
        return;

    if (HasGrantOpportunity())
    {
        m_creditTickEvent = Schedule(GetCreditTickInterval(),
                                     &HomaRecvScheduler::CreditTick,
                                     this);
    }
}
```

作用：

- 按“一个满长 DATA 包序列化时间”节拍持续发放 credit；
- 如果没有 grant 发出或没有剩余 grant 机会，则停止。

---

### 2.10 接收端发 GRANT 的核心逻辑

```cpp
bool HomaRecvScheduler::SendAppropriateGrants()
{
    ResetCurrentlyScheduledFlagForAllMsgs();

    for each inboundMsg in scheduling order
    {
        sender = inboundMsg->GetSrcAddress();

        if (SenderAlreadyGrantedThisRound(sender))
            continue;
        if (SenderIsBusy(sender))
            continue;
        if (inboundMsg->IsFullyGranted())
            continue;

        budget = min(NetBudget(sender), HostBudget(sender));
        senderAvailPkts = SenderBudget(sender) - SenderCreditsInUse(sender);
        globalAvailPkts = GlobalBudget() - GlobalCreditsInUse();

        if (senderAvailPkts > 0 && globalAvailPkts > 0 && !inboundMsg->IsGrantable())
        {
            inboundMsg->AdvanceGrantableWindow(1);
        }

        if (senderAvailPkts > 0 && globalAvailPkts > 0 && inboundMsg->IsGrantable())
        {
            Packet grant = inboundMsg->GenerateGrantOrAck(grantingPrio, GRANT);
            SendDown(grant, receiver, sender);

            inboundMsg->SetCurrentlyScheduled(true);
            SenderCreditsInUse(sender)++;
            GlobalCreditsInUse()++;

            TraceGrantDecision(...);
            TraceReceiverCreditState(...);
            UpdateSrrCursorIfNeeded(sender);

            return true;
        }
    }

    return false;
}
```

作用：

- 这是整个 receiver-driven credit control 的核心；
- 选择“当前谁应该被 grant”；
- 检查 sender budget、global budget、busy sender、SRPT/SRR 顺序；
- 发出一个新的 GRANT；
- 更新“已借出 credit”状态。

主要状态量：

- `m_sirdSenderCreditsInUsePkts`
- `m_sirdGlobalCreditsInUsePkts`
- `m_srrLastGrantedSender`
- `m_srrHaveLastGrantedSender`
- `inboundMsg->m_currentlyScheduled`

---

### 2.11 生成 GRANT / ACK

```cpp
Packet HomaInboundMsg::GenerateGrantOrAck(uint8_t grantedPrio, uint8_t pktTypeFlag)
{
    HomaHeader h;
    h.srcPort = m_dport;
    h.dstPort = m_sport;
    h.txMsgId = m_txMsgId;
    h.pktOffset = EarliestMissingPacketOffset();
    h.grantOffset = min(m_maxGrantableIdx, m_msgSizePkts - 1);
    h.prio = grantedPrio;
    h.flags = pktTypeFlag;

    if (pktTypeFlag == GRANT)
    {
        m_maxGrantedIdx = h.grantOffset;
        m_hasGrantedData = true;
    }

    return PacketWithHeader(h);
}
```

作用：

- 生成控制包；
- 对 GRANT 来说，还会同步推进 `m_maxGrantedIdx`。

主要状态量：

- `m_prio`
- `m_maxGrantedIdx`
- `m_hasGrantedData`

---

### 2.12 完成消息并上交应用

```cpp
void HomaRecvScheduler::ForwardUp(HomaInboundMsg* inboundMsg, int msgIdx)
{
    Message completeMsg = inboundMsg->GetReassembledMsg();

    m_homa->ForwardUp(completeMsg,
                      inboundMsg->GetIpv4Header(),
                      inboundMsg->GetSrcPort(),
                      inboundMsg->GetDstPort(),
                      inboundMsg->GetTxMsgId(),
                      inboundMsg->GetIpv4Interface());

    Packet ack = inboundMsg->GenerateGrantOrAck(HighestPriority(), ACK);
    m_homa->SendDown(ack, inboundMsg->GetDstAddress(), inboundMsg->GetSrcAddress());

    ClearStateForMsg(inboundMsg, msgIdx);
}
```

作用：

- 消息完整接收后重组；
- 上交给应用；
- 回发 ACK；
- 删除接收侧状态。

---

### 2.13 接收侧超时重传

```cpp
void HomaRecvScheduler::ExpireRtxTimeout(HomaInboundMsg* inboundMsg,
                                         uint16_t maxRsndPktOffset)
{
    if (inboundMsg->IsFullyReceived())
        return;

    if (inboundMsg->GetNumRtxWithoutProgress() >= MaxNumRtxPerMsg())
    {
        ClearStateForMsg(inboundMsg, msgIdx);
        return;
    }

    if (!SenderIsBusy(inboundMsg->GetSrcAddress()) &&
        inboundMsg->IsCurrentlyScheduled())
    {
        list<Packet> rsnds = inboundMsg->GenerateResends(maxRsndPktOffset);
        SendAll(rsnds);
    }

    inboundMsg->SetRtxEvent(Schedule(InboundRtxTimeout(), ExpireRtxTimeout, ...));

    if (inboundMsg->GetLastRtxGrntIdx() < inboundMsg->GetMaxGrantableIdx())
        inboundMsg->ResetNumRtxWithoutProgress();
    else if (inboundMsg->IsCurrentlyScheduled())
        inboundMsg->IncrNumRtxWithoutProgress();

    inboundMsg->SetLastRtxGrntIdx(inboundMsg->GetMaxGrantableIdx());
}
```

作用：

- 若接收端长时间没有看到新数据，则要求 sender 重发缺失 packet；
- 若多次超时仍无进展，则清理消息状态。

主要状态量：

- `m_numRtxWithoutProgress`
- `m_lastRtxGrntIdx`
- `rtxEvent`

## 3. 论文里怎么引用

可以把这部分叫作：

- `发送端伪代码`
- `接收端伪代码`

一句总述可以写：

> 发送端伪代码展示了 message 从应用提交、分片、等待 GRANT、发送 DATA 到处理 ACK 的完整过程；接收端伪代码展示了 message 从接收 DATA、更新 credit 状态、发放 GRANT、重组消息到返回 ACK 的完整过程。两者共同构成 Homa/SIRD 的 receiver-driven transport control loop。

