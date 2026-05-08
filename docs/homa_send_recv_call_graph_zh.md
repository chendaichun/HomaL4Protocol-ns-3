# Homa/SIRD 发送端与接收端函数调用图

这份文档按当前代码结构重写，和以下三个实现文件保持一致：

- `src/internet/model/homa-l4-protocol-core.cc`
- `src/internet/model/homa-l4-protocol-send.cc`
- `src/internet/model/homa-l4-protocol-recv.cc`

文档分成两张图：

- 发送端主流程
- 接收端主流程

每个节点统一给出三类信息：

- `函数名`
- `作用`
- `主要读写的长期状态`

## 1. 发送端主流程

```mermaid
flowchart TD
    A["App / HomaSocket<br/>提交完整 message"] --> B["HomaL4Protocol::Send()<br/>作用：发送入口，创建发送侧消息对象并交给发送调度器<br/>状态：触发 m_msgBeginTrace"]

    B --> C["HomaOutboundMsg::HomaOutboundMsg()<br/>作用：分片并初始化发送窗口<br/>状态：m_pktTxQ, m_packets/m_pktSizes, m_remainingBytes, m_maxGrantedIdx, m_waitForFirstGrant, m_initialCreditRequestSent"]
    B --> D["HomaSendScheduler::ScheduleNewMsg()<br/>作用：分配 txMsgId，并把消息挂到发送调度器<br/>状态：m_outboundMsgs, m_txMsgIdFreeList, m_txEvent"]

    D --> E["HomaSendScheduler::TxDataPacket()<br/>作用：发送侧主循环，在链路空闲时挑选下一包<br/>状态：m_txEvent"]
    E --> F["HomaSendScheduler::GetNextPacket()<br/>作用：决定下一条消息，以及下一包是 credit request 还是 DATA<br/>状态：读 m_outboundMsgs；可能消费 sender credit"]
    F --> G["HomaSendScheduler::SelectNextSendableMsgId()<br/>作用：从活跃消息中选最该发的一条，并清理过期消息<br/>状态：可能触发 ClearStateForMsg()"]
    F --> H["HomaOutboundMsg::NeedsInitialCreditRequest()<br/>作用：判断长消息是否先发零负载 DATA 请求首个 GRANT<br/>状态：读 m_waitForFirstGrant, m_initialCreditRequestSent"]
    F --> I["HomaOutboundMsg::TryGetNextPktOffset()<br/>作用：判断当前是否存在真正可发的数据分片<br/>状态：读 m_pktTxQ, m_maxGrantedIdx, m_sirdCreditAvailableTimes"]
    F --> J["HomaOutboundMsg::RemoveNextPktFromTxQ()<br/>作用：弹出一个待发送/重传分片<br/>状态：m_pktTxQ"]
    F --> K["HomaOutboundMsg::ConsumeSirdCredit()<br/>作用：消耗一笔已经成熟的 sender-side scheduled credit<br/>状态：m_sirdCreditAvailableTimes"]
    J --> L["HomaL4Protocol::SendDown()<br/>作用：下发到 IPv4，记录序列化占用和 trace<br/>状态：m_nextTimeTxQueWillBeEmpty"]
    L --> M["Network<br/>DATA 发往接收端"]

    M -. "GRANT / RESEND / ACK 返回" .-> N["HomaL4Protocol::Receive()<br/>作用：解析控制包并分发给发送调度器<br/>状态：无发送侧长期状态改动"]
    N --> O["HomaSendScheduler::HandleControlPacketForOutboundMsg()<br/>作用：处理 GRANT/RESEND/ACK 并推动发送状态机前进<br/>状态：必要时重启 m_txEvent"]
    O --> P["HomaOutboundMsg::HandleGrantOffset()<br/>作用：推进授权上界，记录接收端优先级，发放 sender-side credit<br/>状态：m_maxGrantedIdx, m_prio, m_prioSetByReceiver, m_waitForFirstGrant, m_sirdCreditAvailableTimes, m_remainingBytes"]
    O --> Q["HomaOutboundMsg::HandleResend()<br/>作用：把缺失分片重新加入发送队列<br/>状态：m_pktTxQ；必要时更新 m_maxGrantedIdx, m_prio"]
    O --> R["HomaOutboundMsg::HandleAck()<br/>作用：确认消息完成<br/>状态：m_remainingBytes = 0"]
    R --> S["HomaSendScheduler::ClearStateForMsg()<br/>作用：清理发送侧消息状态并回收 txMsgId<br/>状态：m_outboundMsgs, m_txMsgIdFreeList；可能记录 sender credit 清理 trace"]

    classDef sender fill:#eef6ff,stroke:#2f6fb3,stroke-width:1px,color:#111;
    classDef network fill:#fff6e8,stroke:#d88900,stroke-width:1px,color:#111;
    class A,B,C,D,E,F,G,H,I,J,K,L,N,O,P,Q,R,S sender;
    class M network;
```

### 发送端调用链说明

1. `HomaL4Protocol::Send()` 是应用层发送入口，只负责创建 `HomaOutboundMsg` 并把它交给 `HomaSendScheduler`。
2. `HomaOutboundMsg::HomaOutboundMsg()` 在消息级别完成初始化：分片、建立 `m_pktTxQ`、决定是否等待首个 `GRANT`。
3. `HomaSendScheduler::ScheduleNewMsg()` 给每条消息分配一个 `txMsgId`，随后由 `m_txEvent` 驱动 `TxDataPacket()` 进入包级发送循环。
4. `TxDataPacket()` 并不直接“自己挑包”，而是调用 `GetNextPacket()`。`GetNextPacket()` 再内部走两步：
   - `SelectNextSendableMsgId()`：从所有活跃消息里选最该发送的一条；
   - 对该消息调用 `NeedsInitialCreditRequest()` / `TryGetNextPktOffset()` / `RemoveNextPktFromTxQ()`。
5. 对长消息，第一步可能不是发送真实 payload，而是发送 `GenerateInitialCreditRequest()` 生成的零负载 DATA，请求接收端显式给第一笔 `GRANT`。
6. 一旦发出的是真实 DATA，SIRD 模式下会消耗一笔已经成熟的 sender-side credit，即 `ConsumeSirdCredit()`。
7. 控制包路径统一从 `HomaL4Protocol::Receive()` 进入发送调度器：
   - `GRANT` -> `HandleGrantOffset()`
   - `RESEND` -> `HandleGrantOffset()` + `HandleResend()`
   - `ACK` -> `HandleAck()` + `ClearStateForMsg()`
8. `HandleGrantOffset()` 是发送端最关键的状态推进点。它不仅推进 `m_maxGrantedIdx`，还把新增授权映射为 `m_sirdCreditAvailableTimes` 中未来可以真正起飞的 sender credit。

## 2. 接收端主流程

```mermaid
flowchart TD
    A["Network<br/>DATA / BUSY 到达接收端"] --> B["HomaL4Protocol::Receive()<br/>作用：解析 Homa 头并按 DATA/BUSY 与 GRANT/RESEND/ACK 分发<br/>状态：触发 data/control trace"]
    B --> C["HomaRecvScheduler::ReceivePacket()<br/>作用：接收端主入口，同时驱动 Homa 接收逻辑和 SIRD credit loop<br/>状态：m_busySenders, sender/global budget, credit-in-use, CE/CSN 统计"]

    C --> D["HomaRecvScheduler::ReceiveDataPacket()<br/>作用：把 DATA 归并到对应 inbound message<br/>状态：m_inboundMsgs 可能新增或重排"]
    D --> E["HomaRecvScheduler::FindInboundMsg()<br/>作用：按五元组 + txMsgId 查找活跃消息<br/>状态：只读 m_inboundMsgs"]
    D --> F["HomaInboundMsg::HomaInboundMsg()<br/>作用：首包到来时创建接收侧消息状态<br/>状态：m_receivedPackets, m_maxGrantableIdx, m_maxGrantedIdx, m_hasGrantedData, m_creditDrivenGrantWindow, m_lastRtxGrntIdx"]
    D --> G["HomaInboundMsg::ReceiveDataPacket()<br/>作用：记录已收到分片，必要时推进 grantable window<br/>状态：m_receivedPackets, m_remainingBytes, m_packets/m_pktSizes, m_maxGrantableIdx"]
    G --> H["HomaInboundMsg::IsFullyReceived()<br/>作用：判断消息是否已完整接收<br/>状态：只读 m_receivedPackets"]
    H -->|否| I["HomaRecvScheduler::RescheduleInboundMsg()<br/>作用：按 SRPT 或 SRR 重新排列活跃消息顺序<br/>状态：m_inboundMsgs"]
    I --> J["HomaRecvScheduler::EnsureCreditTickScheduled()<br/>作用：若仍有 grant opportunity，则安排 credit tick<br/>状态：m_creditTickEvent"]
    J --> K["HomaRecvScheduler::CreditTick()<br/>作用：按一个 full-data-packet 发送时间周期性尝试发放 credit<br/>状态：m_creditTickEvent"]
    K --> L["HomaRecvScheduler::IssuePendingGrants()<br/>作用：扫描活跃消息，决定当前允许给谁发 GRANT<br/>状态：m_sirdSenderCreditsInUsePkts, m_sirdGlobalCreditsInUsePkts, inboundMsg::m_currentlyScheduled, SRR cursor"]
    L --> M["HomaInboundMsg::AdvanceGrantableWindow()<br/>作用：把一笔新的 receiver credit 转化为更大的 grantable boundary<br/>状态：m_maxGrantableIdx"]
    L --> N["HomaInboundMsg::GenerateGrantOrAck(GRANT)<br/>作用：生成 GRANT 并同步已授权边界<br/>状态：m_prio, m_maxGrantedIdx, m_hasGrantedData"]
    N --> O["HomaL4Protocol::SendDown()<br/>作用：把 GRANT 发回发送端<br/>状态：m_nextTimeTxQueWillBeEmpty"]

    H -->|是| P["HomaRecvScheduler::ForwardUp()<br/>作用：重组后上交应用，并回 ACK<br/>状态：随后调用 RemoveInboundMsg() 清理"]
    P --> Q["HomaL4Protocol::ForwardUp()<br/>作用：把完整 message 交给 socket / 应用<br/>状态：触发 m_msgFinishTrace"]
    P --> R["HomaInboundMsg::GenerateGrantOrAck(ACK)<br/>作用：生成 ACK 控制包<br/>状态：不推进授权边界"]
    R --> S["HomaL4Protocol::SendDown()<br/>作用：把 ACK 发回发送端<br/>状态：m_nextTimeTxQueWillBeEmpty"]
    P --> T["HomaRecvScheduler::RemoveInboundMsg()<br/>作用：移除活跃消息并取消重传定时器<br/>状态：m_inboundMsgs, rtxEvent"]

    U["HomaRecvScheduler::ExpireRtxTimeout()<br/>作用：超时后生成 RESEND，或在长期无进展时删除消息<br/>状态：m_numRtxWithoutProgress, m_lastRtxGrntIdx"] -.-> V["HomaInboundMsg::GenerateResends()<br/>作用：根据缺口位图批量生成 RESEND<br/>状态：读 m_receivedPackets, m_maxGrantedIdx"]
    V -.-> S

    classDef receiver fill:#eefaf1,stroke:#2d8a57,stroke-width:1px,color:#111;
    classDef network fill:#fff6e8,stroke:#d88900,stroke-width:1px,color:#111;
    class A network;
    class B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,U,V receiver;
```

### 接收端调用链说明

1. `HomaL4Protocol::Receive()` 统一完成 Homa 头解析和分发：
   - `DATA/BUSY` 交给 `HomaRecvScheduler::ReceivePacket()`
   - `GRANT/RESEND/ACK` 交给发送端 `HomaSendScheduler`
2. `HomaRecvScheduler::ReceivePacket()` 不只是“收 DATA”。它还负责 SIRD 控制环：
   - 根据收到的 DATA 回收 credit
   - 更新每个 sender 的网络侧与主机侧预算
   - 维护 sender/global credit-in-use
   - 调度 `EnsureCreditTickScheduled()`
3. `ReceiveDataPacket()` 先尝试 `FindInboundMsg()`；如果找不到，说明这是该消息的首包，需要创建新的 `HomaInboundMsg`。
4. `HomaInboundMsg` 内部维护两条边界：
   - `m_maxGrantableIdx`：接收端当前“最多愿意放开到哪里”
   - `m_maxGrantedIdx`：已经通过 `GRANT` 真正告诉发送端“你可以发到哪里”
5. 消息未完成时，接收端并不会立刻 GRANT，而是先 `RescheduleInboundMsg()`，然后通过 `CreditTick()` -> `IssuePendingGrants()` 的闭环决定何时、给谁、发多少。
6. `IssuePendingGrants()` 是接收端 credit control 的核心：
   - 非 SIRD 模式：按 overcommit + busy sender 规则给 `GRANT`
   - SIRD 模式：同时受 sender budget、global budget、`m_sirdSenderCreditsInUsePkts`、`m_sirdGlobalCreditsInUsePkts` 限制
7. 消息完整后，`ForwardUp()` 会：
   - 调用 `HomaL4Protocol::ForwardUp()` 把重组结果交给应用
   - 发送 `ACK`
   - 调用 `RemoveInboundMsg()` 删除本地消息状态
8. 如果消息长期没有新的接收进展，`ExpireRtxTimeout()` 会生成 `RESEND`；如果超时次数过多，直接删除该消息状态。

## 3. 三条最重要的逻辑线

### 3.1 数据发送主线

`Send()` -> `ScheduleNewMsg()` -> `TxDataPacket()` -> `GetNextPacket()` -> `SendDown()`

这是“真正把 DATA 发出去”的主线。

### 3.2 授权控制主线

接收端：

`ReceivePacket()` -> `ReceiveDataPacket()` -> `RescheduleInboundMsg()` -> `EnsureCreditTickScheduled()` -> `CreditTick()` -> `IssuePendingGrants()` -> `GenerateGrantOrAck(GRANT)` -> `SendDown()`

发送端：

`Receive()` -> `HandleControlPacketForOutboundMsg()` -> `HandleGrantOffset()`

这是 receiver-driven Homa/SIRD 的核心闭环。

### 3.3 缺口恢复主线

接收端：

`ExpireRtxTimeout()` -> `GenerateResends()` -> `SendDown()`

发送端：

`Receive()` -> `HandleControlPacketForOutboundMsg()` -> `HandleResend()` -> `TxDataPacket()`

这是 “发现缺包 -> 请求重传 -> 重新发送” 的主线。

## 4. 论文或汇报里怎么引用

如果你想在正文里用一段话概括这两张图，可以直接写：

> 图 X 和图 Y 分别展示了当前 Homa/SIRD 实现中发送端与接收端的主函数调用链。发送端围绕 `HomaOutboundMsg` 与 `HomaSendScheduler` 展开，负责消息分片、授权窗口推进以及数据发送；接收端围绕 `HomaInboundMsg` 与 `HomaRecvScheduler` 展开，负责消息归并、credit 分配、GRANT/ACK 生成以及缺口恢复。

如果你口头讲图，建议只抓住三个词：

- `数据主线`
- `授权主线`
- `重传主线`

这样最容易讲清楚，不会陷进每个 helper 的细节里。
