# Homa/SIRD 发送端与接收端函数调用图

这份文档给出两张函数调用图：

- 发送端主流程图
- 接收端主流程图

每个节点统一写三类信息：

- `函数名`
- `作用`
- `主要修改的状态量`

图中约定：

- 蓝色箭头：DATA 主路径
- 橙色箭头：GRANT / ACK / RESEND 控制路径
- 绿色虚线：SIRD credit / CE / CSN 控制逻辑

## 1. 发送端函数调用图

```mermaid
flowchart TD
    A["App / HomaSocket<br/>提交完整 message"] --> B["HomaL4Protocol::Send()<br/>作用: 发送入口，创建发送侧消息对象<br/>修改: 无长期状态；触发 m_msgBeginTrace"]
    B --> C["HomaOutboundMsg::HomaOutboundMsg()<br/>作用: 分片、初始化发送窗口、设置是否等待首个 GRANT<br/>修改: m_msgSizeBytes, m_remainingBytes, m_pktTxQ, m_maxGrantedIdx, m_waitForFirstGrant, m_prioSetByReceiver, m_sirdCreditAvailableTimes"]
    B --> D["HomaSendScheduler::ScheduleNewMsg()<br/>作用: 分配 txMsgId，把消息挂到发送调度器<br/>修改: m_outboundMsgs, m_txMsgIdFreeList, m_txEvent"]
    D --> E["HomaSendScheduler::TxDataPacket()<br/>作用: 发送调度主循环，等待链路空闲后挑选下一包<br/>修改: m_txEvent"]
    E --> F["HomaSendScheduler::GetNextMsgId()<br/>作用: 从活跃 outbound messages 中选下一条可发送消息<br/>修改: 可能触发 ClearStateForMsg 清理过期消息"]
    F --> G["HomaSendScheduler::GetNextPktOfMsg()<br/>作用: 生成下一个 DATA 包或初始 credit request<br/>修改: HomaOutboundMsg::m_pktTxQ, m_sirdCreditAvailableTimes；TraceSirdSenderCreditState(event=3)"]
    G --> H["HomaL4Protocol::SendDown()<br/>作用: 封装并下发到 IPv4；设置 prio / ECN / CSN<br/>修改: m_nextTimeTxQueWillBeEmpty；触发 m_dataSendTrace"]
    H --> I["Network<br/>DATA 到达接收端"]

    J["HomaL4Protocol::Receive()<br/>作用: 收到 GRANT / RESEND / ACK 后分发给发送调度器<br/>修改: 无核心长期状态"] --> K["HomaSendScheduler::CtrlPktRecvdForOutboundMsg()<br/>作用: 处理发送侧控制包<br/>修改: 视控制包类型更新 outbound 状态；必要时重启 m_txEvent"]
    K --> L["HomaOutboundMsg::HandleGrantOffset()<br/>作用: 更新授权上界与优先级，发放 scheduled credit<br/>修改: m_maxGrantedIdx, m_prio, m_prioSetByReceiver, m_waitForFirstGrant, m_remainingBytes, m_sirdCreditAvailableTimes"]
    K --> M["HomaOutboundMsg::HandleResend()<br/>作用: 将缺失 packet 重新入队<br/>修改: m_pktTxQ；必要时更新 m_maxGrantedIdx, m_prio, m_prioSetByReceiver"]
    K --> N["HomaOutboundMsg::HandleAck()<br/>作用: 最终确认消息完成<br/>修改: m_remainingBytes = 0"]
    N --> O["HomaSendScheduler::ClearStateForMsg()<br/>作用: 清理发送侧消息状态并回收 txMsgId<br/>修改: m_outboundMsgs, m_txMsgIdFreeList；取消 rtxEvent；TraceSirdSenderCreditState(event=4)"]

    I -. "接收端发回 GRANT / RESEND / ACK" .-> J

    classDef sender fill:#eef6ff,stroke:#2f6fb3,stroke-width:1px,color:#111;
    classDef network fill:#fff6e8,stroke:#d88900,stroke-width:1px,color:#111;
    class A,B,C,D,E,F,G,H,J,K,L,M,N,O sender;
    class I network;
```

### 发送端图怎么读

1. 应用把一个完整 message 交给 `HomaL4Protocol::Send()`。
2. `Send()` 创建 `HomaOutboundMsg`，在这里完成分片、初始化 unscheduled/scheduled 发送窗口。
3. `ScheduleNewMsg()` 给消息分配 `txMsgId`，并挂到 `HomaSendScheduler::m_outboundMsgs`。
4. `TxDataPacket()` 是发送端主循环。它会先选消息，再选 packet，然后调用 `SendDown()` 发包。
5. 如果接收端发来 `GRANT/RESEND/ACK`，控制包会从 `Receive()` 进入 `CtrlPktRecvdForOutboundMsg()`。
6. `HandleGrantOffset()` 是发送端最关键的状态推进函数：它更新 `m_maxGrantedIdx` 和发送优先级，并把接收到的 scheduled credit 放进 `m_sirdCreditAvailableTimes`。
7. `HandleAck()` 后进入 `ClearStateForMsg()`，消息生命周期结束。

## 2. 接收端函数调用图

```mermaid
flowchart TD
    A["Network<br/>DATA / BUSY 到达接收端"] --> B["HomaL4Protocol::Receive()<br/>作用: 接收入口，解析 Homa 头并分发到接收调度器<br/>修改: 触发 m_dataRecvTrace / m_ctrlRecvTrace / TraceSirdPacketState"]
    B --> C["HomaRecvScheduler::ReceivePacket()<br/>作用: 接收侧主入口；处理 DATA/BUSY；驱动 SIRD credit loop<br/>修改: m_busySenders, m_sirdSenderBudgetNetPkts, m_sirdSenderBudgetHostPkts, m_sirdSenderCreditsInUsePkts, m_sirdGlobalCreditsInUsePkts, m_sirdSenderCsnState, m_sirdSenderCeState, m_sirdCeRatioEwma, 各类 epoch/统计计数"]
    C --> D["HomaRecvScheduler::ReceiveDataPacket()<br/>作用: 将 DATA 归并到对应 inbound message<br/>修改: m_inboundMsgs 列表可能新增或重排"]
    D --> E["HomaInboundMsg::HomaInboundMsg()<br/>作用: 首包到来时创建接收侧消息状态<br/>修改: m_msgSizePkts, m_remainingBytes, m_receivedPackets, m_maxGrantedIdx, m_maxGrantableIdx, m_hasGrantedData, m_creditDrivenGrantWindow, m_lastRtxGrntIdx"]
    D --> F["HomaInboundMsg::ReceiveDataPacket()<br/>作用: 记录收到的 packet，推进 grantable window<br/>修改: m_receivedPackets, m_remainingBytes, m_pktSizes / m_packets, m_maxGrantableIdx"]
    F --> G["HomaRecvScheduler::ScheduleMsgAtIdx()<br/>作用: 按 SRPT 或 SRR 重新排列活跃消息<br/>修改: m_inboundMsgs 顺序"]
    G --> H["HomaRecvScheduler::EnsureCreditTickScheduled()<br/>作用: 若存在 grant opportunity，则安排 credit tick<br/>修改: m_creditTickEvent"]
    H --> I["HomaRecvScheduler::CreditTick()<br/>作用: 按一个 full-data-packet serialization time 周期释放 credit<br/>修改: m_creditTickEvent"]
    I --> J["HomaRecvScheduler::SendAppropriateGrants()<br/>作用: 选择该给谁发 GRANT，是接收端 credit control 核心<br/>修改: m_sirdSenderCreditsInUsePkts, m_sirdGlobalCreditsInUsePkts, m_srrLastGrantedSender, m_srrHaveLastGrantedSender, inboundMsg::m_currentlyScheduled"]
    J --> K["HomaInboundMsg::AdvanceGrantableWindow()<br/>作用: 把一个 credit 暴露为新的 grantable window<br/>修改: m_maxGrantableIdx"]
    J --> L["HomaInboundMsg::GenerateGrantOrAck(GRANT)<br/>作用: 生成 GRANT 控制包<br/>修改: m_prio, m_maxGrantedIdx, m_hasGrantedData"]
    L --> M["HomaL4Protocol::SendDown()<br/>作用: 把 GRANT 发回发送端<br/>修改: m_nextTimeTxQueWillBeEmpty；触发 TraceSirdGrantDecision / TraceSirdReceiverCreditState"]

    F --> N["HomaInboundMsg::IsFullyReceived()<br/>作用: 判断消息是否完整接收<br/>修改: 无"]
    N -->|是| O["HomaRecvScheduler::ForwardUp()<br/>作用: 上交应用，并回 ACK<br/>修改: 调用 ClearStateForMsg 清理接收侧消息"]
    O --> P["HomaL4Protocol::ForwardUp()<br/>作用: 把重组后的完整 message 交给接收端应用<br/>修改: 触发 m_msgFinishTrace"]
    O --> Q["HomaInboundMsg::GenerateGrantOrAck(ACK)<br/>作用: 生成 ACK 控制包<br/>修改: 无额外核心状态"]
    Q --> R["HomaL4Protocol::SendDown()<br/>作用: 把 ACK 发回发送端<br/>修改: m_nextTimeTxQueWillBeEmpty"]
    O --> S["HomaRecvScheduler::ClearStateForMsg()<br/>作用: 删除 inbound message 并取消重传定时器<br/>修改: m_inboundMsgs；取消 rtxEvent"]

    T["HomaRecvScheduler::ExpireRtxTimeout()<br/>作用: 接收端超时重传逻辑<br/>修改: inboundMsg::m_numRtxWithoutProgress, m_lastRtxGrntIdx；必要时发送 RESEND 或清理消息"] -.-> Q
    S -. "若消息未完成则保留，等待后续 DATA 或超时" .-> T

    classDef receiver fill:#eefaf1,stroke:#2d8a57,stroke-width:1px,color:#111;
    classDef network fill:#fff6e8,stroke:#d88900,stroke-width:1px,color:#111;
    class A network;
    class B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T receiver;
```

### 接收端图怎么读

1. 所有 `DATA/BUSY` 包先进入 `HomaL4Protocol::Receive()`，再进入 `HomaRecvScheduler::ReceivePacket()`。
2. `ReceivePacket()` 里有两层逻辑：
   - 普通 Homa 接收流程：把 DATA 交给 `ReceiveDataPacket()`。
   - SIRD 控制流程：读取 `CE/CSN`，回收 credit，更新 sender/global budget 和 credit-in-use 计数。
3. `HomaInboundMsg::ReceiveDataPacket()` 负责更新“这个 message 已经收到了哪些 packet”。
4. 接收端随后通过 `ScheduleMsgAtIdx()` 决定 active message 的顺序。`UseSrrScheduling=false` 时偏 SRPT；`true` 时偏 SRR/FIFO。
5. `CreditTick()` 和 `SendAppropriateGrants()` 构成 receiver-driven credit 核心闭环。
6. `SendAppropriateGrants()` 决定是否给某个 sender 发一个新的 GRANT，同时增加：
   - `m_sirdSenderCreditsInUsePkts[sender]`
   - `m_sirdGlobalCreditsInUsePkts`
7. 消息完整后，`ForwardUp()` 会把重组结果交给应用，再发 ACK，并清理 `HomaInboundMsg`。
8. 如果消息长时间没有进展，`ExpireRtxTimeout()` 会发 `RESEND` 或最终清理状态。

## 3. 论文里怎么用这两张图

建议把这两张图分别叫：

- `图 X 发送端 Homa/SIRD 函数调用流程`
- `图 Y 接收端 Homa/SIRD 函数调用流程`

正文中的一句话可以这样写：

> 图 X 和图 Y 分别展示了 Homa/SIRD 在发送端和接收端的主函数调用链。发送端流程围绕 `HomaOutboundMsg` 与 `HomaSendScheduler` 展开，负责消息分片、授权窗口推进和数据发送；接收端流程围绕 `HomaInboundMsg` 与 `HomaRecvScheduler` 展开，负责消息重组、GRANT 分配以及 SIRD credit control 状态更新。

如果你后面要，我可以继续把这两张 Mermaid 图整理成更适合论文排版的 `draw.io` 风格分层框图版本。
