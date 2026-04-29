# Homa/SIRD 发送端与接收端函数调用图（精简版）

这份文档给出一版更适合讲解的调用图。

特点：

- 只保留主函数调用链；
- 每个节点只写 `函数名` 和 `作用`；
- 不展开“修改了哪些变量”；
- 重点突出 `DATA`、`GRANT/ACK/RESEND` 和 `SIRD credit` 这三条逻辑线。

## 1. 发送端函数调用图

```mermaid
flowchart TD
    A["App / HomaSocket<br/>提交完整 message"] --> B["HomaL4Protocol::Send()<br/>发送入口，创建发送侧消息对象"]
    B --> C["HomaOutboundMsg::HomaOutboundMsg()<br/>分片并初始化发送状态"]
    B --> D["HomaSendScheduler::ScheduleNewMsg()<br/>把消息挂到发送调度器"]
    D --> E["HomaSendScheduler::TxDataPacket()<br/>发送端主循环"]
    E --> F["HomaSendScheduler::GetNextMsgId()<br/>选择下一条可发送消息"]
    F --> G["HomaSendScheduler::GetNextPktOfMsg()<br/>选择下一包 DATA 或 credit request"]
    G --> H["HomaL4Protocol::SendDown()<br/>封装后下发到网络"]
    H --> I["Network<br/>DATA 发往接收端"]

    I -. "GRANT / RESEND / ACK 返回" .-> J["HomaL4Protocol::Receive()<br/>接收发送侧控制包"]
    J --> K["HomaSendScheduler::CtrlPktRecvdForOutboundMsg()<br/>分发并处理控制包"]
    K --> L["HomaOutboundMsg::HandleGrantOffset()<br/>处理 GRANT，推进授权窗口"]
    K --> M["HomaOutboundMsg::HandleResend()<br/>处理 RESEND，安排重传"]
    K --> N["HomaOutboundMsg::HandleAck()<br/>处理 ACK，确认消息完成"]
    N --> O["HomaSendScheduler::ClearStateForMsg()<br/>清理发送侧状态"]

    classDef sender fill:#eef6ff,stroke:#2f6fb3,stroke-width:1px,color:#111;
    classDef network fill:#fff6e8,stroke:#d88900,stroke-width:1px,color:#111;
    class A,B,C,D,E,F,G,H,J,K,L,M,N,O sender;
    class I network;
```

### 发送端怎么讲

1. 应用先调用 `HomaL4Protocol::Send()` 发一个完整 message。
2. 发送端创建 `HomaOutboundMsg`，完成分片和初始发送状态设置。
3. `HomaSendScheduler` 把消息纳入调度，然后在 `TxDataPacket()` 主循环里持续挑选“哪条消息、哪一个包”可以发。
4. 接收端回来的 `GRANT / RESEND / ACK` 会重新进入发送端控制路径。
5. 其中最关键的是 `HandleGrantOffset()`，因为它决定发送端拿到多少新的 scheduled 发送机会。

## 2. 接收端函数调用图

```mermaid
flowchart TD
    A["Network<br/>DATA / BUSY 到达接收端"] --> B["HomaL4Protocol::Receive()<br/>接收入口，解析 Homa 包"]
    B --> C["HomaRecvScheduler::ReceivePacket()<br/>接收侧主入口，驱动 Homa/SIRD 控制逻辑"]
    C --> D["HomaRecvScheduler::ReceiveDataPacket()<br/>把 DATA 归并到对应消息"]
    D --> E["HomaInboundMsg::HomaInboundMsg()<br/>首包到来时创建接收侧消息状态"]
    D --> F["HomaInboundMsg::ReceiveDataPacket()<br/>记录已收到的 packet"]
    F --> G["HomaRecvScheduler::ScheduleMsgAtIdx()<br/>按 SRPT 或 SRR 排列活跃消息"]
    G --> H["HomaRecvScheduler::EnsureCreditTickScheduled()<br/>安排 credit tick"]
    H --> I["HomaRecvScheduler::CreditTick()<br/>周期性释放 credit"]
    I --> J["HomaRecvScheduler::SendAppropriateGrants()<br/>决定该给谁发 GRANT"]
    J --> K["HomaInboundMsg::AdvanceGrantableWindow()<br/>推进可授权窗口"]
    J --> L["HomaInboundMsg::GenerateGrantOrAck(GRANT)<br/>生成 GRANT"]
    L --> M["HomaL4Protocol::SendDown()<br/>把 GRANT 发回发送端"]

    F --> N["HomaInboundMsg::IsFullyReceived()<br/>判断消息是否完整接收"]
    N -->|是| O["HomaRecvScheduler::ForwardUp()<br/>上交应用并准备 ACK"]
    O --> P["HomaL4Protocol::ForwardUp()<br/>把完整消息交给接收端应用"]
    O --> Q["HomaInboundMsg::GenerateGrantOrAck(ACK)<br/>生成 ACK"]
    Q --> R["HomaL4Protocol::SendDown()<br/>把 ACK 发回发送端"]
    O --> S["HomaRecvScheduler::ClearStateForMsg()<br/>清理接收侧状态"]

    T["HomaRecvScheduler::ExpireRtxTimeout()<br/>超时后发送 RESEND 或清理消息"] -.-> Q

    classDef receiver fill:#eefaf1,stroke:#2d8a57,stroke-width:1px,color:#111;
    classDef network fill:#fff6e8,stroke:#d88900,stroke-width:1px,color:#111;
    class A network;
    class B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T receiver;
```

### 接收端怎么讲

1. 所有 `DATA / BUSY` 都从 `HomaL4Protocol::Receive()` 进入接收端。
2. `HomaRecvScheduler::ReceivePacket()` 是接收端主入口，它既处理普通 Homa 收包，也驱动 SIRD 的 credit 控制。
3. `ReceiveDataPacket()` 负责把 packet 归到对应 message 上，`ScheduleMsgAtIdx()` 负责决定活跃消息的服务顺序。
4. `CreditTick()` 和 `SendAppropriateGrants()` 组成 receiver-driven credit 的核心闭环。
5. 消息收齐后，接收端把完整 message 上交应用，同时生成 `ACK`；如果长期没有进展，则由 `ExpireRtxTimeout()` 触发 `RESEND`。

## 3. 最适合口头讲解的主线

如果你在答辩或汇报里只想讲最核心的 6 句话，可以直接按下面这条线讲：

1. 发送端先把一个完整 message 交给 `HomaL4Protocol::Send()`。
2. 发送调度器负责决定当前能发哪条消息、哪一个 packet。
3. 接收端收到数据后，把 packet 归并到对应 message，并维护活跃消息顺序。
4. 接收端通过 `CreditTick()` 周期性触发调度，再由 `SendAppropriateGrants()` 决定下一次把 credit 给谁。
5. 发送端收到 `GRANT` 后，在 `HandleGrantOffset()` 中推进授权窗口，然后继续发送 scheduled data。
6. 消息完成后，接收端回 `ACK`，发送端和接收端分别清理各自状态。

## 4. 论文里可以怎么引用

图名建议：

- `图 X Homa/SIRD 发送端主调用流程（精简版）`
- `图 Y Homa/SIRD 接收端主调用流程（精简版）`

正文一句话可以写：

> 图 X 和图 Y 展示了 Homa/SIRD 在发送端和接收端的主函数调用链。发送端围绕消息创建、发送调度和控制包处理展开；接收端围绕消息重组、credit 分配和完成确认展开。
