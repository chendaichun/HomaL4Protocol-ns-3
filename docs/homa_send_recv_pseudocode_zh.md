# Homa/SIRD 论文风格伪代码（C++ 风格）

本文档将发送端和接收端逻辑压缩为两个主函数，但写法改成更接近 `C++` 的风格，便于直接放进论文中讲机制，同时也方便和实现代码对照。

设计目标：

- 形式上像 `C++` 伪代码，而不是纯算法表
- 内容上仍然保持论文里的抽象层次
- 不直接照抄原论文
- 不陷入 ns-3 的工程细节

建议在论文中将其命名为：

- `Algorithm 1 Sender-side main procedure`
- `Algorithm 2 Receiver-side main procedure`

---

## 1. 发送端主过程

```cpp
void SenderMain(const Event& event, SenderState& state)
{
    switch (event.type)
    {
    case NEW_MESSAGE: {
        MessageState msg;
        msg.Packetize(event.message);
        msg.remainingBytes = event.message.size();

        if (state.sirdEnabled && msg.IsLong(state.unschedThresholdPkts))
        {
            msg.maxGrantedOffset = 0;
            msg.waitForFirstGrant = true;
        }
        else
        {
            msg.maxGrantedOffset = msg.InitialUnscheduledWindow(state.bdpPkts);
        }

        msg.txMsgId = state.AllocateTxMsgId();
        state.activeMessages[msg.txMsgId] = msg;

        if (state.txEventIdle)
        {
            state.ScheduleTransmissionOpportunity();
        }
        return;
    }

    case GRANT: {
        MessageState& msg = state.activeMessages[event.txMsgId];

        if (event.grantOffset > msg.maxGrantedOffset)
        {
            uint32_t oldOffset = msg.maxGrantedOffset;
            msg.maxGrantedOffset = event.grantOffset;
            msg.priority = event.priority;
            msg.waitForFirstGrant = false;

            uint32_t newCredits =
                msg.ComputeNewCreditsAfterGrant(oldOffset, event.grantOffset);
            msg.AddScheduledCredits(newCredits);
        }

        if (state.txEventIdle)
        {
            state.ScheduleTransmissionOpportunity();
        }
        return;
    }

    case RESEND: {
        MessageState& msg = state.activeMessages[event.txMsgId];

        if (event.grantOffset > msg.maxGrantedOffset)
        {
            msg.maxGrantedOffset = event.grantOffset;
            msg.priority = event.priority;
        }

        msg.unsentPacketQueue.push(event.pktOffset);

        if (state.ExistsHigherPriorityMessageThan(event.txMsgId))
        {
            state.SendBusy(msg);
        }

        if (state.txEventIdle)
        {
            state.ScheduleTransmissionOpportunity();
        }
        return;
    }

    case ACK: {
        MessageState& msg = state.activeMessages[event.txMsgId];
        msg.remainingBytes = 0;
        state.RecycleTxMsgId(msg.txMsgId);
        state.activeMessages.erase(msg.txMsgId);
        return;
    }

    case TX_OPPORTUNITY: {
        if (!state.LocalTxQueueEmpty())
        {
            state.RescheduleAfterQueueDrain();
            return;
        }

        MessageState* chosen = nullptr;
        for (auto& [id, msg] : state.activeMessages)
        {
            if (msg.expired)
            {
                state.ClearMessage(id);
                continue;
            }

            bool needInitialRequest =
                msg.waitForFirstGrant && !msg.initialRequestSent;
            bool hasSendableData =
                msg.HasSendablePacket() &&
                msg.HeadOffset() <= msg.maxGrantedOffset &&
                (!state.sirdEnabled || msg.HasMatureScheduledCredit());

            if (!(needInitialRequest || hasSendableData))
            {
                continue;
            }

            if (chosen == nullptr || msg.remainingBytes < chosen->remainingBytes)
            {
                chosen = &msg;
            }
        }

        if (chosen == nullptr)
        {
            if (state.ExistsDelayedCredit())
            {
                state.RescheduleAtEarliestCreditMaturity();
            }
            return;
        }

        if (chosen->waitForFirstGrant && !chosen->initialRequestSent)
        {
            Packet request = chosen->GenerateInitialCreditRequest();
            state.SendDown(request, *chosen);
            chosen->initialRequestSent = true;
            state.RescheduleAfterQueueDrain();
            return;
        }

        uint32_t pktOffset = chosen->HeadOffset();
        if (pktOffset > chosen->maxGrantedOffset)
        {
            return;
        }

        if (state.sirdEnabled && !chosen->HasMatureScheduledCredit())
        {
            state.RescheduleAtEarliestCreditMaturity();
            return;
        }

        Packet p = chosen->PopHeadPacket();
        if (state.sirdEnabled)
        {
            chosen->ConsumeScheduledCredit();
        }

        p.AttachDataHeader(chosen->txMsgId,
                           pktOffset,
                           chosen->maxGrantedOffset,
                           chosen->priority);
        state.SendDown(p, *chosen);
        state.RescheduleAfterQueueDrain();
        return;
    }
    }
}
```

### 发送端伪代码强调的点

- 新消息到达时先分片，再决定是否等待首个 `GRANT`
- `GRANT` 负责推进授权窗口
- `RESEND` 负责把丢失分片重新放回待发送队列
- `ACK` 负责结束消息生命周期
- 真正的数据发送只在 `TX_OPPORTUNITY` 事件里发生

---

## 2. 接收端主过程

```cpp
void ReceiverMain(const Event& event, ReceiverState& state)
{
    switch (event.type)
    {
    case DATA: {
        state.UpdateFeedbackState(event);
        state.ReclaimCredits(event);
        state.UpdateBudgetsIfEpochEnds(event.sender);

        MessageState* msg =
            state.FindMessage(event.fiveTuple, event.txMsgId);

        if (msg == nullptr)
        {
            MessageState newMsg;
            newMsg.InitializeFromFirstPacket(event);

            if (state.sirdEnabled && newMsg.IsLong(state.unschedThresholdPkts))
            {
                newMsg.maxGrantedOffset = 0;
                newMsg.maxGrantableOffset = 0;
                newMsg.hasGrantedData = false;
                newMsg.creditDrivenWindow = true;
            }
            else
            {
                newMsg.maxGrantedOffset = newMsg.InitialUnscheduledWindow(state.bdpPkts);
                newMsg.maxGrantableOffset = newMsg.maxGrantedOffset + 1;
                newMsg.hasGrantedData = true;
            }

            state.InsertMessage(newMsg);
            msg = state.FindMessage(event.fiveTuple, event.txMsgId);
            state.ArmRtxTimeout(*msg);
        }

        if (event.hasPayload && !msg->PacketAlreadyReceived(event.pktOffset))
        {
            msg->MarkPacketReceived(event.pktOffset, event.payloadSize);

            if (!msg->creditDrivenWindow &&
                msg->maxGrantableOffset + 1 < msg->MessagePacketCount())
            {
                msg->maxGrantableOffset++;
            }
        }

        if (msg->FullyReceived())
        {
            Packet whole = msg->Reassemble();
            state.DeliverToApplication(whole, *msg);
            state.SendAck(*msg);
            state.RemoveMessage(*msg);
            return;
        }

        state.RescheduleActiveMessage(*msg);
        state.MarkSenderNotBusy(event.sender);

        if (state.creditTickIdle && state.HasGrantOpportunity())
        {
            state.ScheduleCreditTickNow();
        }
        return;
    }

    case BUSY: {
        state.MarkSenderBusy(event.sender);
        return;
    }

    case CREDIT_TICK: {
        state.creditTickIdle = true;
        state.ResetCurrentlyScheduledFlags();

        for (MessageState& msg : state.ActiveMessagesInSchedulingOrder())
        {
            SenderId sender = msg.sender;

            if (state.SenderBusy(sender))
            {
                continue;
            }
            if (msg.FullyGranted())
            {
                continue;
            }
            if (!state.SenderBudgetAvailable(sender) ||
                !state.GlobalBudgetAvailable())
            {
                continue;
            }

            if (state.sirdEnabled && !msg.Grantable())
            {
                msg.maxGrantableOffset++;
            }

            if (msg.Grantable())
            {
                Packet grant = msg.GenerateGrant(state.NextGrantPriority());
                state.SendGrant(grant, msg);

                msg.maxGrantedOffset = grant.grantOffset;
                msg.hasGrantedData = true;
                msg.currentlyScheduled = true;

                state.senderCreditsInUse[sender]++;
                state.globalCreditsInUse++;
            }
        }

        if (state.HasGrantOpportunity())
        {
            state.ScheduleNextCreditTick();
        }
        return;
    }

    case RTX_TIMEOUT: {
        MessageState* msg = state.LookupTimedOutMessage(event.messageId);
        if (msg == nullptr || msg->FullyReceived())
        {
            return;
        }

        if (msg->numRtxWithoutProgress >= state.maxRtxPerMsg)
        {
            state.RemoveMessage(*msg);
            return;
        }

        if (msg->currentlyScheduled && !state.SenderBusy(msg->sender))
        {
            for (uint32_t i = 0; i <= msg->maxGrantedOffset; ++i)
            {
                if (!msg->PacketReceived(i))
                {
                    Packet resend =
                        msg->GenerateResend(i, msg->maxGrantedOffset, msg->priority);
                    state.SendResend(resend, *msg);
                }
            }
        }

        uint32_t oldGrantable = msg->lastRtxGrantableOffset;
        state.RearmRtxTimeout(*msg);

        if (msg->maxGrantableOffset > oldGrantable)
        {
            msg->numRtxWithoutProgress = 0;
        }
        else if (msg->currentlyScheduled)
        {
            msg->numRtxWithoutProgress++;
        }

        msg->lastRtxGrantableOffset = msg->maxGrantableOffset;
        return;
    }
    }
}
```

### 接收端伪代码强调的点

- 接收端同时负责：
  - 消息重组
  - `GRANT` 分配
  - `ACK` 返回
  - `RESEND` 触发
- 在普通 Homa 中，数据到达自然推动可授权窗口
- 在 SIRD 长消息中，真正推进授权窗口的是 `CREDIT_TICK`
- `RTX_TIMEOUT` 表示“已经给过机会但迟迟没收到该来的数据”

---

## 3. 论文里可直接配的解释文字

你可以直接在正文里配下面这段话：

> 上述两个过程分别描述了发送端与接收端的事件驱动主逻辑。发送端在消息到达、发送机会以及 `GRANT/RESEND/ACK` 控制包到达时更新本地状态，并仅在授权边界与 sender-side credit 同时满足条件时发送数据。接收端在数据到达、`BUSY` 反馈、credit tick 与重传超时等事件触发下维护消息状态，并在 sender/global budget 约束下决定何时发放新的 `GRANT`、何时回送 `ACK` 以及何时请求 `RESEND`。

## 4. 和当前实现的大致对应关系

虽然这里写成了 `C++` 风格的论文伪代码，但它仍然对应当前实现中的核心路径：

- 发送端：
  - `Send()`
  - `ScheduleNewMsg()`
  - `SelectNextSendableMsgId()`
  - `GetNextPacket()`
  - `TxDataPacket()`
  - `HandleGrantOffset()`
  - `HandleResend()`
  - `HandleAck()`

- 接收端：
  - `ReceivePacket()`
  - `ReceiveDataPacket()`
  - `RescheduleInboundMsg()`
  - `IssuePendingGrants()`
  - `CreditTick()`
  - `ForwardUp()`
  - `GenerateResends()`
  - `ExpireRtxTimeout()`

## 5. 如果还要更像论文最终稿

再往前走一步，可以继续收紧成下面这种风格：

- 函数名改成英文小写伪代码风格
- 变量名再统一成论文符号
- 每个函数减少到 25 到 40 行
- 更接近 `algorithm2e` / `algorithmic` 可直接排版版本

如果你准备把它贴进 LaTeX，我下一步就直接给你改成 `algorithm2e` 兼容版。 
