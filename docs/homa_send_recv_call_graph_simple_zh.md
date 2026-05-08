# Homa/SIRD 发送端与接收端函数调用图（精简版）

这份文档只保留最适合讲解的主函数调用链，不展开内部状态变量。

另见：

- [Homa/SIRD 发送端与接收端状态机图](./homa_send_recv_state_machine_zh.md)

特点：

- 只保留主路径
- 使用当前代码里的函数名
- 强调 `DATA`、`GRANT/ACK/RESEND` 和 `SIRD credit` 三条线

## 1. 发送端精简图

```mermaid
flowchart TD
    A["App / HomaSocket<br/>提交完整 message"] --> B["HomaL4Protocol::Send()<br/>发送入口"]
    B --> C["HomaOutboundMsg::HomaOutboundMsg()<br/>分片并初始化发送状态"]
    B --> D["HomaSendScheduler::ScheduleNewMsg()<br/>把消息交给发送调度器"]
    D --> E["HomaSendScheduler::TxDataPacket()<br/>发送端主循环"]
    E --> F["HomaSendScheduler::GetNextPacket()<br/>决定下一包发什么"]
    F --> G["HomaSendScheduler::SelectNextSendableMsgId()<br/>选择下一条可发送消息"]
    F --> H["HomaOutboundMsg::NeedsInitialCreditRequest()<br/>判断是否先发 credit request"]
    F --> I["HomaOutboundMsg::TryGetNextPktOffset()<br/>判断是否已有可发 DATA"]
    F --> J["HomaOutboundMsg::RemoveNextPktFromTxQ()<br/>取出一个分片"]
    J --> K["HomaL4Protocol::SendDown()<br/>下发到网络"]
    K --> L["Network<br/>DATA 发往接收端"]

    L -. "GRANT / RESEND / ACK 返回" .-> M["HomaL4Protocol::Receive()<br/>接收控制包"]
    M --> N["HomaSendScheduler::HandleControlPacketForOutboundMsg()<br/>处理发送侧控制包"]
    N --> O["HomaOutboundMsg::HandleGrantOffset()<br/>处理 GRANT"]
    N --> P["HomaOutboundMsg::HandleResend()<br/>处理 RESEND"]
    N --> Q["HomaOutboundMsg::HandleAck()<br/>处理 ACK"]
    Q --> R["HomaSendScheduler::ClearStateForMsg()<br/>清理发送侧状态"]

    classDef sender fill:#eef6ff,stroke:#2f6fb3,stroke-width:1px,color:#111;
    classDef network fill:#fff6e8,stroke:#d88900,stroke-width:1px,color:#111;
    class A,B,C,D,E,F,G,H,I,J,K,M,N,O,P,Q,R sender;
    class L network;
```

### 发送端怎么讲

1. `Send()` 接收完整 message。
2. `HomaOutboundMsg` 负责消息级状态，`HomaSendScheduler` 负责发送调度。
3. `TxDataPacket()` 是发送端持续运行的主循环。
4. `GetNextPacket()` 决定此刻发送的是：
   - 零负载 DATA credit request，还是
   - 真正的 DATA 分片。
5. 接收端返回的 `GRANT / RESEND / ACK` 会重新进入发送端控制路径。

## 2. 接收端精简图

```mermaid
flowchart TD
    A["Network<br/>DATA / BUSY 到达接收端"] --> B["HomaL4Protocol::Receive()<br/>接收入口"]
    B --> C["HomaRecvScheduler::ReceivePacket()<br/>接收侧主入口"]
    C --> D["HomaRecvScheduler::ReceiveDataPacket()<br/>把 DATA 归并到对应消息"]
    D --> E["HomaRecvScheduler::FindInboundMsg()<br/>查找活跃消息"]
    D --> F["HomaInboundMsg::HomaInboundMsg()<br/>必要时创建接收侧消息状态"]
    D --> G["HomaInboundMsg::ReceiveDataPacket()<br/>记录分片接收进度"]
    G --> H["HomaRecvScheduler::RescheduleInboundMsg()<br/>按 SRPT 或 SRR 排列活跃消息"]
    H --> I["HomaRecvScheduler::EnsureCreditTickScheduled()<br/>安排 credit tick"]
    I --> J["HomaRecvScheduler::CreditTick()<br/>周期性尝试发 credit"]
    J --> K["HomaRecvScheduler::IssuePendingGrants()<br/>决定该给谁发 GRANT"]
    K --> L["HomaInboundMsg::AdvanceGrantableWindow()<br/>推进可授权窗口"]
    K --> M["HomaInboundMsg::GenerateGrantOrAck(GRANT)<br/>生成 GRANT"]
    M --> N["HomaL4Protocol::SendDown()<br/>把 GRANT 发回发送端"]

    G --> O["HomaInboundMsg::IsFullyReceived()<br/>判断消息是否收齐"]
    O -->|是| P["HomaRecvScheduler::ForwardUp()<br/>上交应用并回 ACK"]
    P --> Q["HomaL4Protocol::ForwardUp()<br/>把完整消息交给应用"]
    P --> R["HomaInboundMsg::GenerateGrantOrAck(ACK)<br/>生成 ACK"]
    R --> S["HomaL4Protocol::SendDown()<br/>把 ACK 发回发送端"]
    P --> T["HomaRecvScheduler::RemoveInboundMsg()<br/>清理接收侧状态"]

    U["HomaRecvScheduler::ExpireRtxTimeout()<br/>超时后发 RESEND 或清理消息"] -.-> V["HomaInboundMsg::GenerateResends()<br/>生成 RESEND"]
    V -.-> S

    classDef receiver fill:#eefaf1,stroke:#2d8a57,stroke-width:1px,color:#111;
    classDef network fill:#fff6e8,stroke:#d88900,stroke-width:1px,color:#111;
    class A network;
    class B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,U,V receiver;
```

### 接收端怎么讲

1. 所有 `DATA / BUSY` 先从 `Receive()` 进入接收端。
2. `ReceivePacket()` 一边处理普通 Homa 接收，一边驱动 SIRD credit 逻辑。
3. `ReceiveDataPacket()` 负责把 packet 归到某个 message 上。
4. `RescheduleInboundMsg()` 决定活跃消息顺序；`CreditTick()` 和 `IssuePendingGrants()` 决定何时把 credit 给谁。
5. 消息收齐后发 `ACK`；长期没有进展则 `ExpireRtxTimeout()` 触发 `RESEND`。

## 3. 最适合口头讲的 6 句话

1. 发送端先把完整 message 交给 `HomaL4Protocol::Send()`。
2. `HomaSendScheduler` 在 `TxDataPacket()` 里决定当前发哪条消息、哪一个分片。
3. 接收端收到 DATA 后，把分片归并到对应 `HomaInboundMsg`。
4. 接收端通过 `CreditTick()` 周期性触发调度，再由 `IssuePendingGrants()` 决定下一次把 credit 给谁。
5. 发送端收到 `GRANT` 后，在 `HandleGrantOffset()` 里推进授权窗口，再继续发 scheduled DATA。
6. 消息完成后，接收端回 `ACK`，发送端和接收端分别清理各自状态。

## 4. 引用建议

图名建议：

- `图 X Homa/SIRD 发送端主调用流程（精简版）`
- `图 Y Homa/SIRD 接收端主调用流程（精简版）`

正文一句话可以这样写：

> 图 X 和图 Y 展示了当前 Homa/SIRD 实现中发送端和接收端的主函数调用链。发送端围绕消息创建、发送调度和控制包处理展开；接收端围绕消息归并、credit 分配和完成确认展开。
